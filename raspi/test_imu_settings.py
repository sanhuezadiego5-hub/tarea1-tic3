import json
import os
from pathlib import Path
import socket
import struct
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from imu_gui import start_gui
from imu_protocol import decode_packet, encode_control, tcp_frame
from imu_settings import DEFAULTS, validate_config, encode_config, decode_config
from imu_server import IMUServer
from test_imu_server import free_port, read_frame


class SettingsTests(unittest.TestCase):
    def test_config_codec_validation_and_c_fixture(self):
        c=DEFAULTS | dict(accel_hz=200,gyro_hz=100,accel_range=4,gyro_range=500,
                         accel_samples=500,gyro_samples=200,mode='sequential',transport='udp',format='json')
        self.assertEqual(decode_config(encode_config(c)),c)
        if 'TIC3_CONFIG_FIXTURE' in os.environ:
            h,payload=decode_packet(Path(os.environ['TIC3_CONFIG_FIXTURE']).read_bytes())
            self.assertEqual((h['type'],h['window_seq']),(9,3))
            self.assertEqual(decode_config(payload),c)
        for change in [dict(accel_hz=30),dict(interval_ms=60000),dict(accel_samples=-1),dict(gyro_samples=1.5),dict(transport='http')]:
            with self.assertRaises(ValueError):validate_config(DEFAULTS | change)

    def test_config_ack_telemetry_and_finite_finish_through_real_sockets(self):
        with tempfile.TemporaryDirectory() as folder:
            server=IMUServer('127.0.0.1',free_port(),free_port(),folder);server.start()
            gui=start_gui(server,'127.0.0.1',0);base=f'http://127.0.0.1:{gui.server_port}'
            def exchange(client,kind,seq,state=255,payload=b''):
                client.sendall(tcp_frame(encode_control(kind,10,20,seq,state,payload=payload)))
                return decode_packet(read_frame(client))
            def post(action,body=None):
                headers={'X-TIC3-Control':'1'}
                data=json.dumps(body).encode() if body else b''
                return urlopen(Request(base+'/api/'+action,data=data,headers=headers),timeout=3)
            try:
                with socket.create_connection(('127.0.0.1',server.control_port),timeout=3) as client:
                    self.assertEqual(exchange(client,1,0)[0]['type'],4)
                    self.assertEqual(exchange(client,10,0,payload=encode_config(DEFAULTS))[0]['type'],8)
                    exchange(client,7,0,0)
                    config=DEFAULTS | dict(accel_hz=100,gyro_hz=50,accel_samples=123,gyro_samples=45,
                        mode='sequential',transport='udp',accel_range=4)
                    with post('config',config) as r:self.assertIsNotNone(json.load(r)['peers'][0]['pending_config'])
                    with self.assertRaises(HTTPError) as e:post('start')
                    self.assertEqual(e.exception.code,409);e.exception.close()
                    header,payload=exchange(client,7,0,0)
                    self.assertEqual(header['type'],9);self.assertEqual(decode_config(payload),config)
                    exchange(client,10,1,payload=encode_config(config))
                    with post('start') as r:self.assertEqual(json.load(r)['peers'][0]['session']['configuration'],config)
                    exchange(client,7,1,4)
                    telemetry=struct.pack('!IIIIhhhhhhI',12,5,0,0,100,200,300,-100,0,100,234)
                    exchange(client,7,2,1,telemetry)
                    with urlopen(base+'/api/status') as r:status=json.load(r)
                    preview=status['peers'][0]['preview']['acceleration']
                    self.assertEqual(preview['last_count'],12)
                    self.assertAlmostEqual(preview['xyz'][0],100*4*9.80665/32768,places=5)
                    exchange(client,7,2,7,struct.pack('!IIIIhhhhhhI',123,45,1,0,100,200,300,0,0,0,1234))
                    with urlopen(base+'/api/status') as r:status=json.load(r)
                    self.assertFalse(status['peers'][0]['desired'])
                    self.assertEqual(status['sessions'][0]['result'],'stopped_udp_unconfirmed')
                    self.assertTrue(status['sessions'][0]['automatic_stop'])
            finally:
                gui.shutdown();gui.server_close();server.close()
