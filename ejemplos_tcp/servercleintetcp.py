"""Cliente de prueba TCP. Ejecutar en el PC; nombre original conservado."""
import argparse
import socket
import sys
import time

from raspi.tcp_probe_protocol import receive_message, send_message


def ejecutar_cliente(host, port=5001, message="Hola Raspberry, prueba TIC3", count=3,
                      interval=1, timeout=5):
    with socket.create_connection((host, port), timeout=timeout) as client:
        print(f"[+] Conectado a {host}:{port}", flush=True)
        for index in range(count):
            text = f"{index + 1}/{count}: {message}"
            send_message(client, text)
            reply = receive_message(client)
            expected = f"Servidor ACK: recibido ({len(text.encode('utf-8'))} bytes)"
            if reply != expected:
                raise ValueError(f"Respuesta inesperada del servicio: {reply!r}")
            print(f"[>] {text}\n[<] {reply}", flush=True)
            if index + 1 < count:
                time.sleep(interval)
        print(f"[OK] {count} mensajes confirmados en una misma conexión TCP.", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="IP real de la Raspberry")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--message", default="Hola Raspberry, prueba TIC3")
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--timeout", type=float, default=5)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.count < 1:
        parser.error("Puerto 1…65535 y count >= 1 requeridos")
    if not 0 <= args.interval < float("inf") or not 0 < args.timeout < float("inf"):
        parser.error("Intervalo no negativo y timeout positivo, ambos finitos")
    try:
        ejecutar_cliente(args.host, args.port, args.message, args.count, args.interval, args.timeout)
    except (OSError, ValueError, UnicodeError, ConnectionError) as error:
        print(f"[X] Prueba fallida: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\n[-] Prueba cancelada.")
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
