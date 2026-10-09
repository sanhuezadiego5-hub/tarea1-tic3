"""Local socket integration test for the SSH/TCP connectivity probe."""
import socket
import struct
import threading
import unittest
from unittest.mock import patch

from clientservertcp import atender_cliente
from servercleintetcp import ejecutar_cliente
from raspi.tcp_probe_protocol import receive_message, send_message


class TCPProbeTests(unittest.TestCase):
    def test_partial_large_messages_bad_peer_and_reconnection(self):
        errors = []
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(5)
            listener.settimeout(3)
            port = listener.getsockname()[1]

            def serve():
                try:
                    for _ in range(3):
                        conn, addr = listener.accept()
                        with conn:
                            atender_cliente(conn, addr, timeout=1)
                except Exception as error:
                    errors.append(error)

            with patch("clientservertcp.print", create=True), patch("servercleintetcp.print", create=True):
                worker = threading.Thread(target=serve, daemon=True)
                worker.start()
                with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
                    text = "á" * 900  # 1800 bytes: more than the original recv(1024).
                    payload = text.encode()
                    frame = struct.pack("!I", len(payload)) + payload
                    for offset in range(0, len(frame), 3):
                        client.sendall(frame[offset:offset + 3])
                    self.assertEqual(receive_message(client), "Servidor ACK: recibido (1800 bytes)")
                    send_message(client, "segunda muestra")
                    self.assertEqual(receive_message(client), "Servidor ACK: recibido (15 bytes)")
                # Malformed peer must not prevent a later connection.
                with socket.create_connection(("127.0.0.1", port), timeout=2) as bad:
                    bad.sendall(struct.pack("!I", 999999))
                    self.assertEqual(bad.recv(1), b"")
                ejecutar_cliente("127.0.0.1", port, message="prueba", count=3, interval=0, timeout=2)
                worker.join(timeout=3)
                self.assertFalse(worker.is_alive())
                self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
