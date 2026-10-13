# Evidencia pendiente — a completar por el grupo

Guardar aquí `prueba_tcp.pcap` y `prueba_udp.pcap` (o pcapng), las capturas de
pantalla y `analisis.md`. No existen mediciones Wireshark en esta carpeta aún.

Capturar en Raspberry, no en el WiFi de un PC que no es extremo del tráfico.
Mantener iguales red, ODR, rangos, ventana, cantidad, modo de cores y formato;
cambiar únicamente el protocolo de datos. El control sigue usando TCP 5000.
Capturar antes de conectar la ESP para observar el establecimiento TCP.

Filtro de captura (tcpdump): `tcp port 5001 or udp port 5001 or tcp port 5000`.
Filtros de visualización: `tcp.port == 5001`, `udp.port == 5001`,
`tcp.port == 5000`. Reemplazar números si se modificaron.

Evidencias: handshake TCP, transferencia/ACK, datagramas UDP, control TCP en
modo UDP, Conversations e I/O Graphs con igual escala. Anotar configuración,
sesión, muestras adquiridas/guardadas, bytes/paquetes, duración y descartes.
Distinguir ACK TCP de ACK de guardado. No inventar pérdidas o retransmisiones.
No confundir fragmentos del protocolo TIC3 con segmentos TCP o fragmentación IP.
