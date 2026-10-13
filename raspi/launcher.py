#!/usr/bin/env python3
"""Entrada gráfica Tkinter: arranca el servidor local y abre la GUI web.

No sustituye el canal de control: los parámetros y la captura se manejan
con la página existente. Solo detiene el proceso que este lanzador creó.
"""
import json
import signal
import subprocess
import sys
import tkinter as tk
from tkinter import filedialog, messagebox
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen
import webbrowser

ROOT = Path(__file__).resolve().parents[1]
SERVER = Path(__file__).with_name('imu_server.py')


class Launcher:
    def __init__(self, window):
        self.window = window
        self.process = None
        self.log = None
        self.started_url = None
        window.title('TIC3 — Servidor y GUI')
        window.resizable(False, False)
        self.output = tk.StringVar(value=str(ROOT / 'sensores'))
        self.status = tk.StringVar(value='Pulsa Iniciar para abrir el sistema.')
        self.ports = {}
        frame = tk.Frame(window, padx=20, pady=20)
        frame.pack()
        tk.Label(frame, text='Captura BMI270', font=('', 16, 'bold')).grid(row=0, columnspan=3, pady=8)
        for row, (name, default) in enumerate((('Datos', 5001), ('Control', 5000), ('Interfaz', 8080)), 1):
            tk.Label(frame, text=f'Puerto {name.lower()}').grid(row=row, column=0, sticky='w')
            var = tk.StringVar(value=str(default))
            self.ports[name] = var
            tk.Entry(frame, textvariable=var, width=12).grid(row=row, column=1, sticky='w')
        tk.Label(frame, text='Carpeta de datos').grid(row=4, column=0, sticky='w')
        tk.Entry(frame, textvariable=self.output, width=40).grid(row=4, column=1)
        tk.Button(frame, text='Elegir…', command=self.choose).grid(row=4, column=2)
        tk.Button(frame, text='Iniciar y abrir interfaz', command=self.start).grid(row=5, columnspan=3, pady=12)
        tk.Button(frame, text='Volver a abrir interfaz', command=self.open_browser).grid(row=6, columnspan=3)
        tk.Button(frame, text='Cerrar servidor', command=self.close).grid(row=7, columnspan=3, pady=12)
        tk.Label(frame, textvariable=self.status, wraplength=440).grid(row=8, columnspan=3)
        tk.Label(frame, text='En la interfaz: esperar ESP → configurar → iniciar captura.\nAntes de cerrar: Detener y guardar, y esperar confirmación.', wraplength=440).grid(row=9, columnspan=3, pady=10)
        window.protocol('WM_DELETE_WINDOW', self.close)

    def choose(self):
        folder = filedialog.askdirectory()
        if folder:
            self.output.set(folder)

    def start(self):
        if self.process and self.process.poll() is None:
            self.open_browser()
            return
        if self.log:
            self.log.close()
        try:
            ports = {name: int(var.get()) for name, var in self.ports.items()}
            if any(not 1 <= p <= 65535 for p in ports.values()) or len(set(ports.values())) != 3:
                raise ValueError('Usa tres puertos distintos entre 1 y 65535.')
            folder = Path(self.output.get()).expanduser().resolve()
            folder.mkdir(parents=True, exist_ok=True)
            self.log = (folder / 'servidor.log').open('a')
            self.process = subprocess.Popen(
                [sys.executable, str(SERVER), '--port', str(ports['Datos']),
                 '--control-port', str(ports['Control']), '--gui-port', str(ports['Interfaz']),
                 '--output', str(folder)], cwd=ROOT, stdout=self.log, stderr=self.log)
            self.started_url = f'http://localhost:{ports["Interfaz"]}'
            self.status.set('Iniciando servidor…')
            self.window.after(200, lambda: self.wait_ready(0))
        except (ValueError, OSError) as error:
            messagebox.showerror('No se pudo iniciar', str(error))

    def get_status(self):
        with urlopen(self.started_url + '/api/status', timeout=1) as response:
            return json.load(response)

    def wait_ready(self, attempt):
        if self.process.poll() is not None:
            self.status.set('El servidor terminó. Revisa los puertos y servidor.log en la carpeta elegida.')
            return
        try:
            self.get_status()
        except (URLError, OSError, ValueError):
            if attempt < 40:
                self.window.after(200, lambda: self.wait_ready(attempt + 1))
            else:
                self.status.set('El servidor aún no responde; revisa servidor.log. Puedes volver a abrir la interfaz.')
            return
        # Give an occupied GUI port time to make the new child exit: do not
        # silently take ownership of a server that was already running.
        self.window.after(300, self.open_browser)

    def open_browser(self):
        if not self.process or self.process.poll() is not None:
            messagebox.showinfo('Servidor detenido', 'Primero pulsa Iniciar. Si falló, revisa los puertos y servidor.log.')
            return
        webbrowser.open(self.started_url)
        self.status.set('Servidor activo. Usa la interfaz para configurar, iniciar y detener la captura.')

    def close(self):
        if self.process and self.process.poll() is None:
            try:
                status = self.get_status()
                if any(p['desired'] or p['reported'] in (1, 2) or not p.get('confirmed')
                       for p in status['peers']):
                    messagebox.showwarning('Captura pendiente', 'Pulsa Detener y guardar en la interfaz y espera confirmación. El servidor permanece activo.')
                    return
            except (URLError, OSError, ValueError):
                if not messagebox.askyesno('Estado sin confirmar', 'No se pudo confirmar el estado. ¿Cerrar el servidor igualmente? Podrían quedar datos pendientes.'):
                    return
            self.process.send_signal(signal.SIGINT)
            self.status.set('Cerrando servidor…')
            self.finish_close(0)
            return
        self.window.destroy()

    def finish_close(self, attempt):
        if self.process.poll() is None:
            if attempt >= 50:
                self.status.set('El servidor sigue cerrándose. Consulta servidor.log; no se forzó su terminación.')
                return
            self.window.after(100, lambda: self.finish_close(attempt + 1))
            return
        if self.log:
            self.log.close()
        self.window.destroy()


if __name__ == '__main__':
    window = tk.Tk()
    Launcher(window)
    window.mainloop()
