"""Receiver for TIC3 IMU v1: TCP/UDP, durable blocks, idempotent ACK."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import logging
import math
import os
from pathlib import Path
import socket
import tempfile
import threading
import time

from imu_control import CaptureControl
from imu_settings import decode_config, encode_config
from imu_protocol import Reassembler, TCPFrames, decode_packet, encode_control, tcp_frame

LOG = logging.getLogger("imu_server")


class BlockStore:
    def __init__(self, folder):
        self.folder = Path(folder)
        self.folder.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.started = time.time()
        self.blocks = self.duplicates = self.lost = self.errors = 0
        self.samples = {"acceleration": 0, "angular_velocity": 0}
        self.control = None

    @staticmethod
    def atomic_json(path, value):
        temporary = None
        try:
            with tempfile.NamedTemporaryFile("w", dir=path.parent, delete=False, encoding="utf-8") as file:
                temporary = file.name
                json.dump(value, file, separators=(",", ":"), allow_nan=False)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, path)
            temporary = None
            fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        finally:
            if temporary is not None:
                os.unlink(temporary)

    def save(self, block, transport, encoding):
        stream = "acceleration" if block["stream"] == 0 else "angular_velocity"
        stable = {key: value for key, value in block.items()
                  if key not in ("duplicate", "fragment_count", "version", "type")}
        digest = hashlib.sha256(json.dumps(stable, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        base = self.folder / f'{block["device_id"]:016x}_{block["boot_id"]:016x}' / stream
        with self.lock:
            base.mkdir(parents=True, exist_ok=True)
            path = base / f'window-{block["window_seq"]:010d}.json'
            if path.exists():
                previous = json.loads(path.read_text())
                if previous.get("sha256") != digest:
                    raise ValueError("conflicting replay of saved block")
                self.duplicates += 1
                self.update_status()
                return path, False
            data = dict(block)
            data.update(sha256=digest, transport=transport, encoding=encoding,
                        mode="sequential" if block["flags"] & 8 else "parallel", timestamp_kind="fifo_host_read",
                        received_utc=datetime.now(timezone.utc).isoformat(), stream_name=stream,
                        si_unit="m/s2" if block["stream"] == 0 else "rad/s",
                        scale_to_si=block["range"] * (9.80665 if block["stream"] == 0 else math.pi / 180) / 32768)
            data["session_id"] = self.control.session_id((block["device_id"], block["boot_id"]),block["window_seq"]) if self.control else None
            if data['session_id']:
                manifest=self.control.sessions / (data['session_id']+'.json')
                data['configuration']=json.loads(manifest.read_text()).get('configuration')
            duration = (block["window_end_us"] - block["window_start_us"]) / 1e6
            data["captured_samples_per_second"] = block["sample_count"] / duration if duration > 0 else None
            self.atomic_json(path, data)
            self.blocks += 1
            self.samples[stream] += block["sample_count"]
            self.update_status()
            if self.control:
                self.control.saved(data, path)
            return path, True

    def update_status(self):
        with self.lock:
            self.atomic_json(self.folder / "status.json", {
                "started_utc": datetime.fromtimestamp(self.started, timezone.utc).isoformat(),
                "uptime_s": round(time.time() - self.started, 3), "unique_blocks_this_run": self.blocks,
                "duplicate_blocks_this_run": self.duplicates, "samples_this_run": self.samples,
                "expired_blocks_this_run": self.lost, "errors_this_run": self.errors,
                "updated_utc": datetime.now(timezone.utc).isoformat(),
            })

    def expire(self, assembler):
        expired = assembler.expire()
        if expired:
            with self.lock:
                self.lost += len(expired)
                self.update_status()
            for key, missing in expired:
                LOG.warning("Incomplete block %s missing=%s", key, missing)


class IMUServer:
    def __init__(self, host="0.0.0.0", port=5001, control_port=5000, folder="sensores", max_clients=8):
        self.host, self.port, self.control_port = host, port, control_port
        self.store = BlockStore(folder)
        self.control = CaptureControl(self.store)
        self.store.control = self.control
        self.slots = threading.BoundedSemaphore(max_clients)
        self.stop = threading.Event()
        self.sockets = []
        self.threads = []

    def start(self):
        try:
            for sock_type, port, role in ((socket.SOCK_STREAM, self.port, "data"),
                                          (socket.SOCK_STREAM, self.control_port, "control"),
                                          (socket.SOCK_DGRAM, self.port, "udp")):
                sock = socket.socket(socket.AF_INET, sock_type)
                self.sockets.append(sock)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock.bind((self.host, port))
                sock.settimeout(0.5)
                if sock_type == socket.SOCK_STREAM:
                    sock.listen(8)
                target = self.udp_loop if role == "udp" else self.accept_loop
                thread = threading.Thread(target=target, args=(sock, role), daemon=True)
                self.threads.append(thread)
            for thread in self.threads:
                thread.start()
            self.store.update_status()
            LOG.info("Listening TCP/UDP=%s:%s control TCP=%s; store=%s", self.host, self.port,
                     self.control_port, self.store.folder.resolve())
        except Exception:
            self.close()
            raise

    def close(self):
        self.stop.set()
        for sock in self.sockets:
            sock.close()
        for thread in self.threads:
            thread.join(timeout=1)

    def accept_loop(self, listener, role):
        while not self.stop.is_set():
            try:
                conn, addr = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            if not self.slots.acquire(blocking=False):
                conn.close()
                continue
            threading.Thread(target=self.tcp_client, args=(conn, addr, role), daemon=True).start()

    def tcp_client(self, conn, addr, role):
        assembler, frames = Reassembler(timeout=2), TCPFrames()
        identity = None
        connected = last_data = time.monotonic()
        try:
            with conn:
                conn.settimeout(1)
                while not self.stop.is_set():
                    self.store.expire(assembler)
                    if identity is None and time.monotonic() - connected > 3:
                        raise TimeoutError("HELLO timeout")
                    if time.monotonic() - last_data > 180:
                        break
                    try:
                        chunk = conn.recv(4096)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    last_data = time.monotonic()
                    for packet in frames.feed(chunk):
                        header, payload = decode_packet(packet)
                        peer = header["device_id"], header["boot_id"]
                        if header["type"] == 1:
                            if identity is not None and identity != peer:
                                raise ValueError("identity changed within TCP connection")
                            identity = peer
                            if role == "control":
                                self.control.register(identity, addr)
                            conn.sendall(tcp_frame(encode_control(4, *identity)))
                            LOG.info("HELLO %s device=%016x boot=%016x role=%s", addr, *identity, role)
                        elif header["type"] == 7 and role == "control":
                            if identity != peer:
                                raise ValueError("STATE without matching HELLO")
                            kind, seq, state = self.control.report(identity, addr, header['window_seq'], header['stream'], payload)
                            extra=encode_config(self.control.peers[identity]['pending_config']) if kind==9 else b''
                            conn.sendall(tcp_frame(encode_control(kind, *identity, seq, state, payload=extra)))
                        elif header['type'] in (10,11) and role=='control':
                            if identity!=peer:raise ValueError('CONFIG result without HELLO')
                            config=decode_config(payload) if header['type']==10 else None
                            kind,seq,state=self.control.configuration_result(identity,addr,header['window_seq'],config,
                                0 if header['type']==10 else header['stream'])
                            extra=encode_config(self.control.peers[identity]['pending_config']) if kind==9 else b''
                            conn.sendall(tcp_frame(encode_control(kind,*identity,seq,state,payload=extra)))
                        elif header["type"] == 2 and role == "data":
                            if identity != peer:
                                raise ValueError("DATA received without matching HELLO")
                            block = assembler.push(packet)
                            if block is None:
                                continue
                            path, created = self.store.save(block, "tcp", "json" if packet.startswith(b"{") else "binary")
                            conn.sendall(tcp_frame(encode_control(3, *identity, block["window_seq"], block["stream"])))
                            if created:
                                LOG.info("Saved %s n=%d fs=%dHz drops=%d fifo_skip=%d", path,
                                         block["sample_count"], block["sample_hz"], block["output_dropped_blocks"],
                                         block["fifo_skipped_frames"])
                        else:
                            raise ValueError("unexpected message type/channel")
        except (OSError, ValueError, TimeoutError) as error:
            LOG.warning("Client %s closed: %s", addr, error)
            with self.store.lock:
                self.store.errors += 1
                self.store.update_status()
        finally:
            self.slots.release()

    def udp_loop(self, sock, role):
        del role
        assembler = Reassembler(timeout=2, max_pending=16)
        while not self.stop.is_set():
            self.store.expire(assembler)
            try:
                packet, addr = sock.recvfrom(1201)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                block = assembler.push(packet)
                if block:
                    path, created = self.store.save(block, "udp", "json" if packet.startswith(b"{") else "binary")
                    if created:
                        LOG.info("UDP saved %s n=%d", path, block["sample_count"])
            except (ValueError, OSError) as error:
                LOG.warning("UDP %s rejected: %s", addr, error)
                with self.store.lock:
                    self.store.errors += 1
                    self.store.update_status()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--control-port", type=int, default=5000)
    parser.add_argument("--output", default="sensores")
    parser.add_argument("--gui-port", type=int, default=8080, help="0 desactiva GUI")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    server = IMUServer(args.host, args.port, args.control_port, args.output)
    gui = None
    try:
        server.start()
        if args.gui_port:
            from imu_gui import start_gui
            gui = start_gui(server, args.host, args.gui_port)
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        if gui:
            gui.shutdown()
            gui.server_close()
        server.close()


if __name__ == "__main__":
    main()
