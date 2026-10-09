import os
import json
from pathlib import Path
import struct
import unittest

from imu_protocol import HEADER, Reassembler, TCPFrames, decode_packet, encode_control, tcp_frame


class ProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(os.environ["TIC3_WIRE_FIXTURE"]).read_bytes()
        decoder = TCPFrames()
        cls.frames = []
        for start in range(0, len(source), 31):
            cls.frames.extend(decoder.feed(source[start:start + 31]))
        assert not decoder.buffer
        decoder = TCPFrames()
        source = Path(os.environ["TIC3_JSON_FIXTURE"]).read_bytes()
        cls.json_frames = []
        for start in range(0, len(source), 127):
            cls.json_frames.extend(decoder.feed(source[start:start + 127]))
        assert not decoder.buffer

    def data_groups(self):
        groups = {}
        for frame in self.frames:
            h, _ = decode_packet(frame)
            if h["type"] == 2:
                key = h["boot_id"], h["stream"], h["window_seq"]
                groups.setdefault(key, []).append(frame)
        return groups

    def test_c_to_python_and_independent_intervals(self):
        assembler = Reassembler()
        results = []
        for group in self.data_groups().values():
            for frame in reversed(group):  # UDP can reorder packets
                result = assembler.push(frame, now=0)
                if result:
                    results.append(result)
        self.assertEqual(len(results), 4)
        self.assertEqual([r["sample_count"] for r in results], [400, 400, 2000, 500])
        self.assertEqual([r["value_count"] for r in results], [1200, 1200, 6000, 1500])
        for r in results:
            self.assertEqual(r["values"][:3], [-32768, 32767, 0])
            self.assertEqual(r["values"][-3:], [-32768, 32767, r["sample_count"] - 1])
            self.assertEqual(r["sample_seq"][-1], 999 + r["sample_count"])
            self.assertEqual(r["read_us"][0], r["window_start_us"])
            self.assertEqual(r["device_id"], 0x1122334455AB)
            self.assertEqual(r["output_dropped_blocks"], 3)

    def test_duplicates_and_replays(self):
        group = next(iter(self.data_groups().values()))
        assembler = Reassembler()
        self.assertIsNone(assembler.push(group[0], now=0))
        self.assertIsNone(assembler.push(group[0], now=0))
        result = None
        for frame in group[1:]:
            result = assembler.push(frame, now=0)
        self.assertFalse(result["duplicate"])
        for frame in group:
            result = assembler.push(frame, now=1)
        self.assertTrue(result["duplicate"])
        self.assertEqual(result["value_count"], 1200)

    def test_missing_fragment_timeout_and_limits(self):
        group = next(iter(self.data_groups().values()))
        assembler = Reassembler(timeout=2, max_pending=1)
        assembler.push(group[0], now=0)
        self.assertEqual(assembler.expire(now=1), [])
        losses = assembler.expire(now=2)
        self.assertEqual(len(losses), 1)
        self.assertEqual(losses[0][1], len(group) - 1)
        assembler.push(group[0], now=3)
        other = list(self.data_groups().values())[1][0]
        with self.assertRaises(ValueError):
            assembler.push(other, now=3)

    def test_crc_and_lengths(self):
        frame = next(iter(self.data_groups().values()))[0]
        for position in (8, 30, 95, 100):
            damaged = bytearray(frame)
            damaged[position] ^= 1
            with self.assertRaises(ValueError):
                decode_packet(damaged)
        with self.assertRaises(ValueError):
            decode_packet(frame[:-1])
        with self.assertRaises(ValueError):
            TCPFrames().feed(struct.pack("!I", 65535))

    def test_control_messages_and_coalesced_frames(self):
        hello, _ = decode_packet(self.frames[0])
        self.assertEqual(hello["type"], 1)
        ack = encode_control(4, hello["device_id"], hello["boot_id"])
        decoded, payload = decode_packet(ack)
        self.assertEqual(decoded["type"], 4)
        self.assertEqual(payload, b"")
        self.assertEqual(len(ack), HEADER.size)
        self.assertEqual(TCPFrames().feed(tcp_frame(ack) * 2), [ack, ack])
        ack_json = encode_control(4, hello["device_id"], hello["boot_id"], encoding="json")
        self.assertEqual(decode_packet(ack_json), decode_packet(ack))

    def test_json_matches_binary_and_worst_case_fits(self):
        def assemble(frames):
            assembler = Reassembler()
            results = []
            for frame in frames:
                if decode_packet(frame)[0]["type"] == 2:
                    result = assembler.push(frame, now=0)
                    if result:
                        results.append(result)
            return results
        binary, json_results = assemble(self.frames), assemble(self.json_frames)
        self.assertEqual(len(json_results), 5)
        for a, b in zip(binary, json_results):
            for key in ("values", "sample_seq", "read_us", "key", "interval_ms", "sample_hz"):
                self.assertEqual(a[key], b[key])
        self.assertEqual(json_results[-1]["device_id"], 2**64 - 1)
        self.assertEqual(json_results[-1]["values"], [-32768] * 18)
        self.assertTrue(all(len(frame) <= 1200 for frame in self.json_frames))
        broken = json.loads(self.json_frames[0])
        broken["values"][0] += 1
        with self.assertRaises(ValueError):
            decode_packet(json.dumps(broken, separators=(",", ":")).encode())
        broken["values"][0] = True
        with self.assertRaises(ValueError):
            decode_packet(json.dumps(broken, separators=(",", ":")).encode())


if __name__ == "__main__":
    unittest.main()
