"""Local web GUI for IMU status, XYZ charts and confirmed START/STOP."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import logging
from pathlib import Path
import threading

PAGE = Path(__file__).with_name('imu_gui.html')


def start_gui(receiver, host='0.0.0.0', port=8080):
    class Handler(BaseHTTPRequestHandler):
        def respond(self, value, code=200, content_type='application/json; charset=utf-8'):
            data = value if isinstance(value, bytes) else json.dumps(value, allow_nan=False).encode()
            self.send_response(code)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == '/':
                return self.respond(PAGE.read_bytes(), content_type='text/html; charset=utf-8')
            if self.path == '/api/status':
                status = receiver.control.snapshot()
                with receiver.store.lock:
                    status['totals'] = dict(blocks=receiver.store.blocks, samples=dict(receiver.store.samples),
                        errors=receiver.store.errors, incomplete=receiver.store.lost)
                status['storage'] = str(receiver.store.folder.resolve())
                status['data_port'], status['control_port'] = receiver.port, receiver.control_port
                manifests = sorted(receiver.control.sessions.glob('*.json'), key=lambda p:p.stat().st_mtime, reverse=True)[:20]
                live={p['session']['id']:p['session'] for p in status['peers'] if p['session'] and p['online']}
                status['sessions'] = [live.get(p.stem) or json.loads(p.read_text()) for p in manifests]
                return self.respond(status)
            self.respond({'error': 'No encontrado'}, 404)

        def do_POST(self):
            # Local network only. Custom header + strict Origin prevents browser CSRF.
            origin = self.headers.get('Origin')
            if self.headers.get('X-TIC3-Control') != '1' or (origin and origin != 'http://' + self.headers.get('Host', '')):
                return self.respond({'error': 'Origen de control no permitido'}, 403)
            if self.path not in ('/api/start', '/api/stop', '/api/config'):
                return self.respond({'error': 'No encontrado'}, 404)
            try:
                if self.headers.get('Transfer-Encoding'):
                    raise ValueError('Transfer-Encoding no admitido')
                length=int(self.headers.get('Content-Length','0'))
                if not 0<=length<=2048:
                    raise ValueError('Solicitud demasiado grande')
                if self.path=='/api/config':
                    self.connection.settimeout(2)
                    config=json.loads(self.rfile.read(length))
                    result=receiver.control.configure(config)
                else:
                    if length:raise ValueError('Este comando no admite cuerpo')
                    result=receiver.control.command(self.path.rsplit('/', 1)[1])
                self.respond(result)
            except (ValueError, TimeoutError) as error:
                self.respond({'error': str(error)}, 409)

        def log_message(self, format, *args):
            if self.path != '/api/status':
                logging.getLogger('imu_gui').info(format, *args)

    gui = ThreadingHTTPServer((host, port), Handler)
    gui.daemon_threads = True
    threading.Thread(target=gui.serve_forever, daemon=True).start()
    logging.getLogger('imu_gui').info('GUI HTTP %s:%s', host, gui.server_port)
    return gui
