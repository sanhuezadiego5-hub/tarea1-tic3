"""Exercise launcher process lifecycle without a display or Tk installation.

The widgets are not rendered here. The server is real, bound to loopback,
and the close guard is exercised against a capture-pending status.
"""
import heapq
import importlib.util
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest.mock import patch
from test_imu_server import free_port


class Value:
    def __init__(self, value): self.value = str(value)
    def get(self): return self.value
    def set(self, value): self.value = str(value)


class Window:
    def __init__(self): self.events = []; self.closed = False
    def after(self, milliseconds, function): heapq.heappush(self.events, (time.monotonic()+milliseconds/1000, function))
    def destroy(self): self.closed = True
    def run_until(self, condition, seconds=10):
        until = time.monotonic()+seconds
        while not condition() and time.monotonic()<until:
            if self.events and self.events[0][0] <= time.monotonic(): heapq.heappop(self.events)[1]()
            else: time.sleep(0.01)
        if not condition(): raise AssertionError('El lanzador no completó la operación')


class LauncherTests(unittest.TestCase):
    def test_start_web_status_and_refuse_close_while_capture_pending(self):
        warnings, opened = [], []
        dialog = types.SimpleNamespace(showerror=lambda *a: warnings.append(a), showwarning=lambda *a: warnings.append(a),
                                       showinfo=lambda *a: warnings.append(a), askyesno=lambda *a: False)
        tk = types.ModuleType('tkinter'); tk.messagebox = dialog; tk.filedialog = types.SimpleNamespace()
        with patch.dict(sys.modules, {'tkinter': tk}):
            spec = importlib.util.spec_from_file_location('tic3_launcher_test', Path(__file__).with_name('launcher.py'))
            module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        actual_popen = subprocess.Popen
        def local_server(args, **kwargs):
            return actual_popen([*args, '--host', '127.0.0.1'], **kwargs)
        with tempfile.TemporaryDirectory() as folder, patch.object(module.subprocess, 'Popen', local_server), patch.object(module.webbrowser, 'open', lambda url: opened.append(url)):
            app = module.Launcher.__new__(module.Launcher)
            app.window = Window(); app.process = None; app.log = None; app.started_url = None
            app.status = Value(''); app.output = Value(folder)
            ports = set()
            while len(ports)<3: ports.add(free_port())
            app.ports = dict(zip(('Datos','Control','Interfaz'), map(Value, ports)))
            try:
                app.start(); app.window.run_until(lambda: bool(opened))
                self.assertEqual(app.get_status()['storage'], str(Path(folder).resolve()))
                get_status = app.get_status
                app.get_status = lambda: {'peers': [{'desired': True, 'reported': 1, 'confirmed': True}]}
                app.close()
                self.assertIsNone(app.process.poll()); self.assertFalse(app.window.closed); self.assertTrue(warnings)
                app.get_status = get_status
                app.close(); app.window.run_until(lambda: app.window.closed)
                self.assertEqual(app.process.returncode, 0)
            finally:
                if app.process and app.process.poll() is None:
                    app.process.send_signal(signal.SIGINT); app.process.wait(timeout=10)
                if app.log and not app.log.closed: app.log.close()
