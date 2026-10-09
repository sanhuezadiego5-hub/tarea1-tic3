# SSH PC ↔ Raspberry y prueba TCP

> Registro histórico anterior al envío real. El servidor de texto fue detenido
> y sustituido por `raspi/imu_server.py`. Estado e instrucciones actuales en
> [ENVIO_REAL.md](ENVIO_REAL.md). Las secciones siguientes conservan la prueba anterior.

Esta prueba valida acceso remoto y mensajes TCP desde el PC. No es aún el
servidor final de datos BMI270, ni utiliza los arreglos binarios/JSON del codec IMU.
Los nombres originales se conservan: `clientservertcp.py` es el servidor y
`servercleintetcp.py` es el cliente. Requiere Python 3 estándar, sin instalar paquetes.

## Estado actual tras cambiar WiFi

Acceso verificado ahora como `usuario@192.0.2.10` con la misma clave. PC visto
por Raspberry: `192.0.2.20`. El puerto 5001 estaba libre y se reinició el
servidor de prueba en segundo plano, PID observado 3542. Tres mensajes y sus
ACK comprobados en ambos equipos. Sesión SSH de trabajo abierta.

```bash
ssh -i ~/.ssh/tic3_raspi usuario@192.0.2.10
python3 /ruta/al/proyecto/servercleintetcp.py --host 192.0.2.10 --port 5001 --count 3
```

Ejecutar el cliente en el PC, no dentro de la sesión SSH. Manual completo y
resumen de parámetros en `ESTADO_ANTES_ENVIO_ESP.md`. Los datos de IP/PID de la
sección siguiente son históricos y no se usan para la conexión actual.

## Prueba anterior del 7 de octubre de 2026 — red anterior

Acceso SSH y prueba TCP realizados con éxito:

- Raspberry: `usuario@192.0.2.10`, hostname `Raccoon`, Python 3.13.5.
- PC observado por Raspberry: `192.0.2.20`.
- Clave dedicada del PC: `~/.ssh/tic3_raspi`, autorizada por el usuario.
- Scripts copiados a `/ruta/al/proyecto/` y helper a su subcarpeta `raspi/`.
- Servidor de prueba escuchando en `0.0.0.0:5001`.
- Tres mensajes confirmados en una misma conexión; verificados también en el log remoto.
- Proceso arrancado en segundo plano, PID inicial 10295, guardado en
  `/ruta/al/proyecto/tcp_probe.pid`; log `/ruta/al/proyecto/tcp_probe.log`.

Para conectarse desde este PC:

```bash
ssh -i ~/.ssh/tic3_raspi usuario@192.0.2.10
```

Para volver a probar desde el PC, mientras el proceso siga activo:

```bash
python3 /ruta/al/proyecto/servercleintetcp.py --host 192.0.2.10 --port 5001 --count 3
```

No arrancar otra copia en 5001: el servidor de prueba ya ocupa ese puerto.
El PID es un registro de esa ejecución, no una identidad permanente. No se
instaló un servicio de arranque; tras reiniciar Raspberry habrá que arrancarlo
de nuevo. Para sustituirlo por el servidor IMU, detener primero este proceso
identificado y después iniciar el nuevo en el puerto acordado.

Los pasos siguientes documentan el procedimiento de instalación inicial.

## 1. En la Raspberry, con su teclado/terminal

Conectar PC y Raspberry a la misma red con comunicación entre equipos. Puede ser
el mismo router, Raspberry por Ethernet y PC por WiFi, o un AP de Raspberry ya
configurado. No cambiar ahora la red si aún no es necesario. Una red de laboratorio
puede aislar clientes; en ese caso usar una red donde puedan comunicarse.

```bash
sudo raspi-config
```

Seleccionar **Interface / Interfacing Options → SSH → Yes → Finish**.
Luego ejecutar:

```bash
whoami
hostname -I
systemctl is-active ssh
python3 --version
```

Anotar usuario e IP de la interfaz conectada al PC. No asumir que el usuario se
llama `pi` ni que la IP es 192.168.50.1. SSH debe informar `active`. Si no está
activo después de habilitarlo:

```bash
sudo systemctl enable --now ssh
```

No hace falta reinstalar Raspberry Pi OS. Si el sistema no tiene raspi-config,
confirmar distribución y servicio SSH antes de adaptar estos pasos.

## 2. En una terminal del PC

Sustituir los valores siguientes por los anotados en Raspberry:

```bash
RASPI_USER='tu_usuario_real'
RASPI_IP='tu_ip_real'
ssh "$RASPI_USER@$RASPI_IP"
```

En el primer acceso aparecerá la huella de la clave SSH. Confirmar que se trata
de la Raspberry esperada y aceptar la clave. Si se desea comprobarla en la
Raspberry, `ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub` muestra la huella
Ed25519. Introducir la contraseña del usuario de Raspberry (no se muestra al
escribir). Si se configuró autenticación por clave, usar esa clave existente.

Cuando aparezca el prompt de Raspberry, ya hay acceso SSH. Para comprobar:

```bash
hostname
whoami
```

Volver al PC para copiar archivos:

```bash
exit
```

## 3. Copiar los scripts desde el PC

En la misma terminal del PC, las variables siguen definidas:

```bash
ssh "$RASPI_USER@$RASPI_IP" 'mkdir -p ~/tic3/raspi'
scp /ruta/al/proyecto/clientservertcp.py /ruta/al/proyecto/servercleintetcp.py "$RASPI_USER@$RASPI_IP:tic3/"
scp /ruta/al/proyecto/raspi/tcp_probe_protocol.py "$RASPI_USER@$RASPI_IP:tic3/raspi/"
```

El helper `raspi/tcp_probe_protocol.py` debe conservar esa subcarpeta; ambos
scripts lo importan. No copiar el build ESP-IDF ni instalar Codex para esta prueba.

## 4. Arrancar servidor en Raspberry desde SSH

Desde el PC:

```bash
ssh "$RASPI_USER@$RASPI_IP"
```

Ya dentro de la Raspberry:

```bash
cd ~/tic3
python3 clientservertcp.py --host 0.0.0.0 --port 5001
```

Resultado esperado:

```text
[+] Servidor de prueba escuchando en 0.0.0.0:5001
```

Dejar esa terminal abierta. El servidor funciona en primer plano: Ctrl+C lo
detiene; no es todavía un servicio persistente de arranque.

## 5. Ejecutar cliente en otra terminal del PC

Volver a definir la IP, porque variables de una terminal no se comparten con otra:

```bash
RASPI_IP='tu_ip_real'
python3 /ruta/al/proyecto/servercleintetcp.py --host "$RASPI_IP" --port 5001 --count 3
```

Esperar:

```text
[OK] 3 mensajes confirmados en una misma conexión TCP.
```

Raspberry mostrará cada mensaje recibido. Usar la IP real del servidor, no
127.0.0.1 ni 0.0.0.0 como destino. SSH usa puerto 22; esta prueba usa TCP 5001.

## 6. Qué cambió en los ejemplos

- IP y puerto configurables por argumentos, sin editar código para cada prueba.
- Servidor escucha en todas las interfaces por defecto y atiende un cliente a
  la vez; después de desconexión acepta otro. No es todavía multi-cliente concurrente.
- Prefijo uint32 big-endian + texto UTF-8, con recepción acumulativa. No se supone
  que un recv contenga un mensaje completo ni que el mensaje mida <=1024 bytes.
- Varios mensajes por conexión, ACK verificado y timeout de 5 s por operación de socket.
- Longitud máxima de texto de 16384 bytes y recuperación ante cliente malformado.
- Prueba local verificada con mensajes partidos, texto de 1800 bytes, cliente
  malformado y reconexión posterior. Prueba en Raspberry todavía pendiente.

Estos cambios son para conectividad; el servidor final deberá usar
`imu_protocol.py`, HELLO/ACK de bloques, persistencia y configuración remota
definidos en `../PROTOCOLO_DATOS.md`.

## Problemas comunes

| Resultado | Revisar |
|---|---|
| SSH: Connection refused | SSH activo y puerto correcto; ver `systemctl status ssh --no-pager` en Raspberry |
| SSH: timeout / No route to host | IP correcta, conectividad de red, aislamiento de clientes |
| SSH: Permission denied | Usuario y contraseña/clave configurados; no usar un usuario supuesto |
| TCP: Connection refused | Servidor arrancado, IP correcta y mismo puerto |
| TCP: timeout | Red/firewall y servidor; no desactivar firewall a ciegas |
| Address already in use | Ya hay proceso en 5001; identificar con `ss -ltnp` antes de detenerlo |
| No module named raspi | Copiar helper en `~/tic3/raspi/` y ejecutar desde `~/tic3` |

Referencias oficiales: [acceso remoto / SSH](https://www.raspberrypi.com/documentation/computers/remote-access.html)
y [configuración Raspberry Pi OS](https://www.raspberrypi.com/documentation/computers/configuration.html).
