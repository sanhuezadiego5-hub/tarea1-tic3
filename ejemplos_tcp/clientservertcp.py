"""Servidor de prueba TCP. Ejecutar en la Raspberry; nombre original conservado."""
import argparse
import socket
import sys

from raspi.tcp_probe_protocol import receive_message, send_message


def atender_cliente(conn, addr, timeout=5):
    conn.settimeout(timeout)
    print(f"[+] Cliente conectado: {addr[0]}:{addr[1]}", flush=True)
    try:
        while True:
            message = receive_message(conn)
            if message is None:
                break
            print(f"[<] Recibido: {message!r}", flush=True)
            send_message(conn, f"Servidor ACK: recibido ({len(message.encode('utf-8'))} bytes)")
    except (OSError, ValueError, UnicodeError, ConnectionError) as error:
        print(f"[!] Cliente desconectado/error: {error}", flush=True)
    finally:
        print(f"[-] Conexión cerrada: {addr[0]}:{addr[1]}", flush=True)


def iniciar_servidor(host="0.0.0.0", port=5001, timeout=5):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(5)
        print(f"[+] Servidor de prueba escuchando en {host}:{server.getsockname()[1]}", flush=True)
        print("[+] Detener con Ctrl+C. Atiende un cliente a la vez.", flush=True)
        while True:
            conn, addr = server.accept()
            with conn:
                atender_cliente(conn, addr, timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0", help="IP de escucha; default: todas las interfaces")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--timeout", type=float, default=5, help="Timeout de cliente en segundos")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0 < args.timeout < float("inf"):
        parser.error("Puerto 1…65535 y timeout positivo finito requeridos")
    try:
        iniciar_servidor(args.host, args.port, args.timeout)
    except KeyboardInterrupt:
        print("\n[-] Servidor detenido.")
    except OSError as error:
        print(f"[X] No se pudo iniciar/continuar el servidor: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
