import json
import socket
import tempfile
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from imu_gui import start_gui
from imu_protocol import decode_packet, encode_control, tcp_frame
from imu_server import IMUServer
from test_imu_server import free_port, read_frame


class ControlTests(unittest.TestCase):
    def test_restart_marks_unconfirmed_and_udp_never_claims_saved(self):
        with tempfile.TemporaryDirectory() as folder:
            server = IMUServer('127.0.0.1', free_port(), free_port(), folder)
            identity = 10, 20
            self.assertEqual(server.control.report(identity, ('127.0.0.1', 1), 0, 4), (8, 0, 4))
            server.control.command('start')
            self.assertEqual(server.control.report(identity, ('127.0.0.1', 1), 1, 1), (8, 1, 1))
            server.control.command('stop')
            self.assertEqual(server.control.report(identity, ('127.0.0.1', 1), 2, 4), (8, 2, 4))
            session = server.control.snapshot()['peers'][0]['session']
            self.assertEqual(session['result'], 'stopped_udp_unconfirmed')
            server.control.command('start')
            pending = server.control.snapshot()['peers'][0]['session']['id']
            restarted = IMUServer('127.0.0.1', free_port(), free_port(), folder)
            manifest = json.loads((restarted.control.sessions / (pending + '.json')).read_text())
            self.assertEqual(manifest['result'], 'interrupted_unconfirmed')
            self.assertNotIn('ended_utc', manifest)

    def test_gui_requires_confirmation_and_session_survives_stop(self):
        with tempfile.TemporaryDirectory() as folder:
            server = IMUServer('127.0.0.1', free_port(), free_port(), folder)
            server.start()
            gui = start_gui(server, '127.0.0.1', 0)
            base = f'http://127.0.0.1:{gui.server_port}'
            def post(action, origin=None):
                headers = {'X-TIC3-Control': '1'}
                if origin:
                    headers['Origin'] = origin
                return urlopen(Request(base + '/api/' + action, data=b'', headers=headers), timeout=3)
            def state(client, seq, value):
                client.sendall(tcp_frame(encode_control(7, 10, 20, seq, value)))
                return decode_packet(read_frame(client))[0]
            try:
                with self.assertRaises(HTTPError) as error:
                    post('start')
                self.assertEqual(error.exception.code, 409)
                error.exception.close()
                with socket.create_connection(('127.0.0.1', server.control_port), timeout=3) as client:
                    client.sendall(tcp_frame(encode_control(1, 10, 20)))
                    self.assertEqual(decode_packet(read_frame(client))[0]['type'], 4)
                    self.assertEqual(state(client, 0, 0)['type'], 8)
                    with self.assertRaises(HTTPError) as error:
                        post('start', 'http://foreign.example')
                    self.assertEqual(error.exception.code, 403)
                    error.exception.close()
                    with post('start') as reply:
                        started = json.load(reply)['peers'][0]
                    session_id = started['session']['id']
                    command = state(client, 0, 0)
                    self.assertEqual((command['type'], command['window_seq']), (5, 1))
                    self.assertEqual(state(client, 1, 1)['type'], 8)
                    with post('start') as reply:
                        self.assertEqual(json.load(reply)['peers'][0]['session']['id'], session_id)
                    with post('stop') as reply:
                        self.assertEqual(json.load(reply)['peers'][0]['session']['result'], 'stop_pending')
                    self.assertEqual(state(client, 1, 1)['type'], 6)
                    self.assertEqual(state(client, 2, 2)['type'], 6)
                    with self.assertRaises(HTTPError) as error:
                        post('start')
                    self.assertEqual(error.exception.code, 409)
                    error.exception.close()
                    self.assertEqual(state(client, 2, 0)['type'], 8)
                    with urlopen(base + '/api/status') as reply:
                        status = json.load(reply)
                    self.assertEqual(status['peers'][0]['state'], 'Detenido y guardado')
                    self.assertEqual(status['sessions'][0]['result'], 'stopped_and_saved')
                    self.assertIn('ended_utc', status['sessions'][0])
                    with post('start') as reply:
                        self.assertNotEqual(json.load(reply)['peers'][0]['session']['id'], session_id)
            finally:
                gui.shutdown()
                gui.server_close()
                server.close()


if __name__ == '__main__':
    unittest.main()
