"""Validate the delivered sessions and their per-sample metadata."""
from collections import defaultdict
import json
from pathlib import Path
import unittest


class DeliveryDataTests(unittest.TestCase):
    def test_sessions_and_every_sample_have_consistent_metadata(self):
        root = Path(__file__).resolve().parents[1] / 'sensores'
        manifests = {p.stem: json.loads(p.read_text()) for p in (root / 'sessions').glob('*.json')}
        self.assertGreaterEqual(len(manifests), 3)
        combinations = set()
        totals = defaultdict(lambda: defaultdict(int))
        windows = list(root.glob('**/window-*.json'))
        self.assertGreater(len(windows), 0)
        for path in windows:
            d = json.loads(path.read_text())
            config = manifests[d['session_id']]['configuration']
            self.assertEqual(d['configuration'], config)
            self.assertEqual(len(d['read_us']), d['sample_count'])
            self.assertEqual(len(d['sample_seq']), d['sample_count'])
            self.assertEqual(len(d['values']), d['sample_count'] * 3)
            self.assertTrue(d['received_utc'])
            self.assertEqual(d['timestamp_kind'], 'fifo_host_read')
            self.assertEqual(d['mode'], config['mode'])
            prefix = 'accel' if d['stream'] == 0 else 'gyro'
            self.assertEqual(d['sample_hz'], config[prefix + '_hz'])
            self.assertEqual(d['range'], config[prefix + '_range'])
            self.assertGreater(d['scale_to_si'], 0)
            self.assertTrue(all(d['window_start_us'] <= t <= d['window_end_us'] for t in d['read_us']))
            totals[d['session_id']][d['stream_name']] += d['sample_count']
        for sid, manifest in manifests.items():
            c = manifest['configuration']
            combinations.add(tuple(c[k] for k in ('accel_hz','gyro_hz','accel_range','gyro_range','accel_samples','gyro_samples')))
            self.assertEqual(dict(totals[sid]), manifest['samples'])
        self.assertGreaterEqual(len(combinations), 3)
