"""Binary v1 codec/reassembly helper. No sockets, WiFi, server or GUI here."""
from collections import OrderedDict
import hashlib
import json
import struct
import time
import zlib

HEADER = struct.Struct("!4sBBBBQQIHHIQQIHH9I")
RECORD = struct.Struct("!IIhhh")
MAX_PACKET = 1200
SAMPLES_PER_FRAGMENT = 78
JSON_SAMPLES_PER_FRAGMENT = 6
FIELDS = (
    "magic", "version", "type", "stream", "flags", "device_id", "boot_id",
    "window_seq", "sample_hz", "range", "interval_ms", "window_start_us",
    "window_end_us", "sample_count", "fragment_index", "fragment_count",
    "sample_offset", "fragment_samples", "buffer_dropped", "fifo_skipped_frames",
    "fifo_errors", "i2c_errors", "output_dropped_blocks", "payload_bytes", "crc32",
)
assert HEADER.size == 96 and RECORD.size == 14


def decode_packet(data):
    if not HEADER.size <= len(data) <= MAX_PACKET:
        raise ValueError("invalid packet length")
    is_json = data.startswith(b"{")
    if is_json:
        def reject_constant(value):
            raise ValueError(f"invalid JSON constant: {value}")

        def unique_object(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("duplicate JSON key")
                result[key] = value
            return result

        try:
            obj = json.loads(data, parse_constant=reject_constant, object_pairs_hook=unique_object)
            h = {key: obj[key] for key in FIELDS}
            if h["magic"] != "TIC3":
                raise ValueError("wrong JSON magic")
            h["magic"] = b"TIC3"
            for key in ("device_id", "boot_id"):
                text = h[key]
                if not isinstance(text, str) or len(text) != 16 or any(c not in "0123456789abcdef" for c in text):
                    raise ValueError("identity must be 16 lowercase hexadecimal digits")
                h[key] = int(text, 16)
            if any(type(h[key]) is not int for key in FIELDS[1:]):
                raise ValueError("header fields must be integers")
            rows = []
            if h["type"] == 2:
                values, sequences, offsets = obj["values"], obj["sample_seq"], obj["read_offset_us"]
                n = h["fragment_samples"]
                if not all(isinstance(array, list) for array in (values, sequences, offsets)):
                    raise ValueError("sample fields must be arrays")
                if len(values) != n * 3 or len(sequences) != n or len(offsets) != n:
                    raise ValueError("array lengths mismatch")
                for i in range(n):
                    row = [sequences[i], offsets[i], *values[3 * i:3 * i + 3]]
                    if any(type(value) is not int for value in row):
                        raise ValueError("samples must be integers")
                    rows.append(RECORD.pack(*row))
            payload = b"".join(rows)
            crc_header = HEADER.pack(*(h[key] for key in FIELDS))[:92]
        except (KeyError, TypeError, struct.error, UnicodeError, json.JSONDecodeError) as error:
            raise ValueError("invalid JSON frame") from error
    else:
        h = dict(zip(FIELDS, HEADER.unpack_from(data)))
        payload = bytes(data[HEADER.size:])
        crc_header = data[:92]
    if h["magic"] != b"TIC3" or h["version"] != 1 or h["type"] not in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11):
        raise ValueError("wrong service, protocol version or message type")
    if len(payload) != h["payload_bytes"]:
        raise ValueError("payload length mismatch")
    crc = zlib.crc32(payload, zlib.crc32(crc_header)) & 0xFFFFFFFF
    if crc != h["crc32"]:
        raise ValueError("CRC mismatch")
    if h["type"] != 2:
        allowed_payload = h["type"] in (7, 9, 10) and len(payload) == 32
        if (payload and not allowed_payload) or h["fragment_samples"] or h["sample_count"]:
            raise ValueError("control message contains samples")
        if h["stream"] not in (0, 1, 2, 3, 4, 5, 6, 7, 255):
            raise ValueError("unknown control stream")
        if h["type"] in (9,10) and len(payload)!=32:
            raise ValueError("CONFIG requires 32 bytes")
        return h, payload
    hz, interval, total = h["sample_hz"], h["interval_ms"], h["sample_count"]
    if hz not in (25, 50, 100, 200, 400) or not 100 <= interval <= 60000:
        raise ValueError("unsupported acquisition configuration")
    expected = (hz * interval + 999) // 1000
    capacity = expected + (expected + 19) // 20 + 8
    per_fragment = JSON_SAMPLES_PER_FRAGMENT if is_json else SAMPLES_PER_FRAGMENT
    parts = max(1, (total + per_fragment - 1) // per_fragment)
    index = h["fragment_index"]
    if h["stream"] not in (0, 1) or h["flags"] & ~15 or total > capacity:
        raise ValueError("invalid stream, flags or sample count")
    if h["range"] not in ((2, 4, 8, 16) if h["stream"] == 0 else (125, 250, 500, 1000, 2000)):
        raise ValueError("invalid sensor range")
    if h["fragment_count"] != parts or index >= parts:
        raise ValueError("invalid fragment count/index")
    offset = index * per_fragment
    n = min(per_fragment, total - offset)
    if h["sample_offset"] != offset or h["fragment_samples"] != n or len(payload) != n * RECORD.size:
        raise ValueError("invalid fragment layout")
    duration = h["window_end_us"] - h["window_start_us"]
    if duration < 0 or duration > (60000000 if h["flags"] & 4 else interval * 1000):
        raise ValueError("invalid window duration")
    if not h["flags"] & 4 and duration != interval * 1000:
        raise ValueError("non-partial window has wrong duration")
    for _, read_offset, *_ in RECORD.iter_unpack(payload):
        if read_offset > duration:
            raise ValueError("sample timestamp outside window")
    return h, payload


def encode_control(message_type, device_id, boot_id, window_seq=0, stream=255, encoding="binary", payload=b""):
    if message_type not in (1, 3, 4, 5, 6, 7, 8, 9, 10, 11) or stream not in (0, 1, 2, 3, 4, 5, 6, 7, 255):
        raise ValueError("invalid control message")
    fields = [b"TIC3", 1, message_type, stream, 0, device_id, boot_id, window_seq]
    fields.extend([0] * (len(FIELDS) - len(fields)))
    if payload and (message_type not in (7,9,10) or len(payload)!=32 or encoding!="binary"):
        raise ValueError("invalid extended control payload")
    fields[FIELDS.index("payload_bytes")] = len(payload)
    packet = HEADER.pack(*fields)
    fields[-1] = zlib.crc32(payload,zlib.crc32(packet[:92])) & 0xFFFFFFFF
    if encoding == "binary":
        return HEADER.pack(*fields)+payload
    if encoding != "json":
        raise ValueError("unknown encoding")
    obj = dict(zip(FIELDS, fields))
    obj.update(magic="TIC3", device_id=f"{device_id:016x}", boot_id=f"{boot_id:016x}")
    return json.dumps(obj, separators=(",", ":")).encode()


def tcp_frame(packet):
    if not HEADER.size <= len(packet) <= MAX_PACKET:
        raise ValueError("invalid frame length")
    return struct.pack("!I", len(packet)) + packet


class TCPFrames:
    """Incremental decoder: one recv may contain partial or several frames."""
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data):
        # Call with bounded recv chunks (<=4096 bytes) to bound temporary storage.
        if len(data) > 4096:
            raise ValueError("feed chunks must be <=4096 bytes")
        self.buffer.extend(data)
        frames = []
        while len(self.buffer) >= 4:
            n = struct.unpack_from("!I", self.buffer)[0]
            if not HEADER.size <= n <= MAX_PACKET:
                self.buffer.clear()
                raise ValueError("invalid TCP frame prefix")
            if len(self.buffer) < n + 4:
                break
            frames.append(bytes(self.buffer[4:n + 4]))
            del self.buffer[:n + 4]
        return frames


class Reassembler:
    """Bounded, unordered fragments. Persist deduplication in the actual server.

    The 128-key completed cache only detects recent in-process replays. An ACK
    must be issued by the server AFTER idempotently saving a complete result.
    """
    def __init__(self, timeout=2.0, max_pending=16, completed_cache=128):
        if timeout <= 0 or max_pending < 1 or completed_cache < 1:
            raise ValueError("invalid reassembly limits")
        self.timeout = timeout
        self.max_pending = max_pending
        self.completed_cache = completed_cache
        self.pending = {}
        self.completed = OrderedDict()

    def expire(self, now=None):
        now = time.monotonic() if now is None else now
        lost = []
        for key, state in list(self.pending.items()):
            if now - state["started"] >= self.timeout:
                lost.append((key, state["header"]["fragment_count"] - len(state["parts"])))
                del self.pending[key]
        return lost

    def push(self, packet, now=None):
        now = time.monotonic() if now is None else now
        # Call expire() in the server loop, including idle time, and log losses.
        h, payload = decode_packet(packet)
        if h["type"] != 2:
            raise ValueError("expected DATA")
        key = tuple(h[k] for k in ("device_id", "boot_id", "stream", "window_seq"))
        varying = {"fragment_index", "sample_offset", "fragment_samples", "payload_bytes", "crc32"}
        invariant = tuple(h[k] for k in FIELDS if k not in varying)
        if key not in self.pending:
            if len(self.pending) >= self.max_pending:
                raise ValueError("reassembly queue full; expire/drop explicitly")
            self.pending[key] = {"header": h, "invariant": invariant, "parts": {}, "started": now}
        state = self.pending[key]
        if state["invariant"] != invariant:
            del self.pending[key]
            raise ValueError("conflicting metadata for same block")
        index = h["fragment_index"]
        previous = state["parts"].get(index)
        if previous is not None and previous != payload:
            del self.pending[key]
            raise ValueError("conflicting duplicate fragment")
        state["parts"][index] = payload
        if len(state["parts"]) != h["fragment_count"]:
            return None
        raw = b"".join(state["parts"][i] for i in range(h["fragment_count"]))
        digest = hashlib.sha256(repr(invariant).encode() + raw).digest()
        duplicate = key in self.completed
        if duplicate and self.completed[key] != digest:
            del self.pending[key]
            raise ValueError("conflicting replay of completed block")
        self.completed[key] = digest
        self.completed.move_to_end(key)
        while len(self.completed) > self.completed_cache:
            self.completed.popitem(last=False)
        del self.pending[key]
        rows = list(RECORD.iter_unpack(raw))
        result = {k: v for k, v in state["header"].items() if k not in varying | {"magic"}}
        result.update({
            "key": key, "duplicate": duplicate,
            "sample_seq": [row[0] for row in rows],
            "read_us": [h["window_start_us"] + row[1] for row in rows],
            "values": [value for row in rows for value in row[2:]],
            "value_count": 3 * len(rows),
        })
        return result
