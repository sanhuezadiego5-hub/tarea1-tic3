"""Framing for a text connectivity probe; this is not the IMU data protocol."""
import struct

MAX_BYTES = 16384


def receive_exact(sock, length, allow_clean_eof=False):
    data = bytearray()
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            if allow_clean_eof and not data:
                return None
            raise ConnectionError("Conexión cerrada a mitad del mensaje")
        data.extend(chunk)
    return bytes(data)


def receive_message(sock):
    prefix = receive_exact(sock, 4, allow_clean_eof=True)
    if prefix is None:
        return None
    length = struct.unpack("!I", prefix)[0]
    if not 1 <= length <= MAX_BYTES:
        raise ValueError(f"Longitud inválida: {length}; máximo {MAX_BYTES} bytes")
    return receive_exact(sock, length).decode("utf-8")


def send_message(sock, text):
    payload = text.encode("utf-8")
    if not 1 <= len(payload) <= MAX_BYTES:
        raise ValueError(f"El texto debe ocupar entre 1 y {MAX_BYTES} bytes UTF-8")
    sock.sendall(struct.pack("!I", len(payload)) + payload)
