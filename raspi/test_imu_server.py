import json
import os
from pathlib import Path
import socket
import tempfile
import unittest

from imu_protocol import TCPFrames, decode_packet, encode_control, tcp_frame
from imu_server import IMUServer


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_frame(sock):
    decoder = TCPFrames()
    while True:
        data = sock.recv(4096)
        if not data:
            raise ConnectionError("unexpected EOF")
        frames = decoder.feed(data)
        if frames:
            return frames[0]


class IMUServerTests(unittest.TestCase):
    def test_saved_block_ack_dedup_after_restart_and_json(self):
        groups = []
        for name in ("TIC3_WIRE_FIXTURE", "TIC3_JSON_FIXTURE"):
            decoder = TCPFrames()
            raw = Path(os.environ[name]).read_bytes()
            frames = []
            for i in range(0, len(raw), 4096):
                frames.extend(decoder.feed(raw[i:i + 4096]))
            selected = [f for f in frames if decode_packet(f)[0]["type"] == 2
                        and decode_packet(f)[0]["window_seq"] == 0
                        and decode_packet(f)[0]["stream"] == 0]
            groups.append(selected)
        header, _ = decode_packet(groups[0][0])
        with tempfile.TemporaryDirectory() as folder:
            port, control = free_port(), free_port()
            server = IMUServer("127.0.0.1", port, control, folder)
            server.start()
            try:
                for group in (groups[0], groups[0]):
                    with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                        client.sendall(tcp_frame(encode_control(1, header["device_id"], header["boot_id"])))
                        self.assertEqual(decode_packet(read_frame(client))[0]["type"], 4)
                        for frame in group:
                            client.sendall(tcp_frame(frame))
                        ack, _ = decode_packet(read_frame(client))
                        self.assertEqual((ack["type"], ack["window_seq"], ack["stream"]), (3, 0, 0))
                self.assertEqual(len(list(Path(folder).rglob("window-*.json"))), 1)
                saved = json.loads(next(Path(folder).rglob("window-*.json")).read_text())
                self.assertEqual(saved["sample_count"], 400)
                self.assertEqual(saved["value_count"], 1200)
            finally:
                server.close()
            server = IMUServer("127.0.0.1", port, control, folder)
            server.start()
            try:
                # Different encoding/fragment count, same logical block is idempotent.
                with socket.create_connection(("127.0.0.1", port), timeout=3) as client:
                    client.sendall(tcp_frame(encode_control(1, header["device_id"], header["boot_id"])))
                    read_frame(client)
                    for frame in groups[1]:
                        client.sendall(tcp_frame(frame))
                    self.assertEqual(decode_packet(read_frame(client))[0]["type"], 3)
                self.assertEqual(len(list(Path(folder).rglob("window-*.json"))), 1)
            finally:
                server.close()


if __name__ == "__main__":
    unittest.main()
