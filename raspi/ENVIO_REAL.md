# Envío real ESP32-S3 → Raspberry

> Estado vigente: GUI con XYZ en vivo y configuración remota de ODR, rangos,
> objetivos por flujo, ventana, modo por core, TCP/UDP y binario/JSON.
> La prueba actual usa WiFi compartido; AP desactivado temporalmente por usuario.
> Manual y evidencia actualizados en [GUI_CONTROL_AP.md](GUI_CONTROL_AP.md).
> Las notas inferiores conservan etapas previas de desarrollo.

> Actualización GUI/control: START/STOP y GUI web implementados. La ESP ahora
> espera START al encender; STOP cierra y guarda bloques parciales antes de
> confirmar. Estado y uso vigentes en [GUI_CONTROL_AP.md](GUI_CONTROL_AP.md).
> Las secciones anteriores a esta actualización describen la etapa de envío continuo.

Verificado el 7 de octubre de 2026: firmware grabado en `/dev/ttyACM0`,
WIFI_COMPARTIDO, TCP binario a 192.0.2.10:5001. Raspberry guarda aceleración
y velocidad angular en directorios separados. No es una simulación del PC.

Después de ajustar el arranque, se comprobaron 162 ventanas consecutivas por
flujo, desde 0 hasta 161: cero ventanas descartadas y cero skips FIFO reportados.
Últimas 60 ventanas: promedio 403,483 muestras XYZ por ventana de un segundo
en ambos flujos, ODR nominal 400 Hz. El conteo físico puede variar; se transmite
el real. Esta prueba no garantiza ausencia de pérdidas ante cortes prolongados.

## Repetir y observar

En el PC:

```bash
ssh -i ~/.ssh/tic3_raspi usuario@192.0.2.10
```

En esa sesión de Raspberry:

```bash
tail -f ~/tic3/imu_server.log
```

Ctrl+C deja de observar, sin detener el servidor. Para revisar el resumen:

```bash
cat ~/tic3/sensores/status.json
ss -ltnp 'sport = :5001'
```

El servidor quedó en segundo plano, independiente de SSH, sin servicio de
arranque automático. Si Raspberry reinicia, comprobar que el puerto está libre
y arrancar una sola copia en una terminal que permanezca abierta:

```bash
cd ~/tic3
python3 -u raspi/imu_server.py --output sensores
```

Para volver a compilar/grabar desde otra terminal del PC:

```bash
source /ruta/a/.espressif/tools/activate_idf_v6.1.sh
cd /ruta/al/repositorio/esp
idf.py -B build-tic3 build
idf.py -B build-tic3 -p /dev/ttyACM0 flash monitor
```

`idf.py` existe después de activar el entorno. `main.c` se ejecuta en la ESP,
no con el botón Run de VSCode como un programa del PC. Tras grabar, arranca
automáticamente al reiniciar o alimentar la placa. USB sirve para grabar y
observar; los bloques viajan por WiFi. Salir de monitor con Ctrl+].

## Parámetros y decisiones implementadas

| Tema | Decisión y motivo |
|---|---|
| Sensor/bus | BMI270, I2C 0x68, SDA42/SCL2, 100 kHz |
| Parámetros | Aceleración XYZ ±8 g y velocidad angular XYZ ±2000 °/s; no mide velocidad lineal |
| Muestreo | ODR independientes: 25/50/100/200/400 Hz; ambas en 400 Hz actualmente |
| Lectura | FIFO cada 10 ms; el sensor toma las muestras, el bucle no imprime 400 veces/s |
| Ventanas | Un bloque por parámetro cada 1000 ms; 400 XYZ nominales equivalen a 1200 valores |
| Cambio a 5 s | Capacidad se recalcula: 2108 XYZ por bloque a 400 Hz; dos flujos/dos bloques consumen 118048 B de arrays |
| Límite de memoria | Presupuesto 128 KiB y margen de heap de 64 KiB; rechazar configuración inválida antes de medir. 10 s a 400 Hz no cabe con estos pools |
| Cores | Core1 adquiere; core0 envía; gestor WiFi también en core0. Colas transfieren bloques inmutables |
| Arranque | Esperar WiFi con IP y HELLO_ACK del servidor antes de activar FIFO/adquisición; evita llenar pools mientras conecta |
| Formato activo | Binario, big-endian, CRC32; registros raw XYZ int16 con secuencia y tiempo de lectura |
| Formato alternativo | JSON seleccionable en menuconfig; receptor acepta ambos, pero prueba en placa realizada con binario |
| Fragmentos | Máximo 1200 B; TCP delimita cada uno con longitud uint32. No se envía todo el array como un solo paquete IP |
| IP/identidad | Una dirección destino por perfil; MAC+boot+flujo+ventana identifica bloques. No se repite IP por muestra |
| Dirección actual | Raspberry 192.0.2.10, ESP recibe IP por DHCP. Si cambia la IP de Raspberry, actualizar endpoint y regrabar o reservarla en router |
| Puertos | Datos TCP/UDP5001; control TCP5000 para alternativa UDP. Cliente usa puerto local automático |
| Confirmación de servidor | connect y HELLO/HELLO_ACK verifican servicio/protocolo; no se escanean puertos ni se requiere confirmación manual |
| Puerto ocupado | Servidor informa error; no cambia silenciosamente de puerto. No ejecutar servidor de texto y receptor IMU simultáneamente en 5001 |
| TCP | Conservar bloque hasta ACK válido; ACK solo después de persistir. Timeout 2 s para connect/envío del bloque/espera de respuesta por operación |
| Reintento | Reconectar y reenviar mismo bloque con espera creciente 1/2/4/8/16/30 s y jitter; receptor deduplica también tras reinicio |
| Caída de WiFi | Invalidar sockets, reconectar automáticamente con claves guardadas; no necesita teclado |
| Falta de buffers | Conservar pendientes y descartar ventanas nuevas completas, contándolas. Dos cores y TCP no dan almacenamiento ilimitado |
| UDP alternativo | Probe por TCP5000 y envío una vez por datagrama, sin ACK de datos. Compilado, aún sin prueba real en placa |
| Persistencia | Raspberry `~/tic3/sensores/<device_boot>/{acceleration,angular_velocity}/window-*.json`; binario de red se decodifica a JSON en disco |
| Tiempos/unidades | Tiempo de lectura FIFO, no instante exacto de conversión; recepción UTC aparte. Escala guardada para m/s² y rad/s |
| Error I2C | Tres errores consecutivos detienen adquisición; recuperación automática del sensor pendiente |

En menuconfig: **TIC3 - bloques IMU**, **TIC3 - WiFi por prioridad** y
**TIC3 - envio a Raspberry**. Cambios de frecuencia, ventana, formato o endpoints
se aplican con compilación y reinicio; no existe CONFIG remoto todavía.

## WiFi por prioridades y AP

1. `TIC3-Raccoon`, destino 192.168.50.1:5001. Credenciales preparadas localmente;
   AP todavía no configurado con sudo ni activado.
2. Laboratorio: deshabilitado hasta conocer SSID exacto y configuración.
3. `WIFI_COMPARTIDO`, destino 192.0.2.10:5001. Usado en la prueba real.

Seleccionar la red visible de mayor prioridad; dos intentos de hasta 15 s por
perfil, cooldown 60 s si falla. Tres fallos de conexión/HELLO reportados hacen
probar alternativas. Mantener la red sana actual aunque aparezca otra preferida;
reevaluar prioridades después de desconexión. Reintentar sin intervención manual.
Claves en configuración local, excluidas de archivos versionados.

AP significa punto de acceso: Raspberry anuncia una red WiFi, autentica clientes
y les asigna IP por DHCP. Hotspot es ese uso como red local, con posibilidad de
compartir Internet si hay otra salida, por ejemplo Ethernet. Internet no hace
falta para medir/enviar. Raspberry será 192.168.50.1, banda 2,4 GHz, canal1.

Preparación manual desde una terminal de Raspberry:

```bash
sudo python3 ~/tic3/raspi/setup_ap.py
```

Introduce la contraseña de tu usuario de Raspberry cuando sudo la pida: no es
la contraseña de WiFi. El comando prepara el perfil sin cambiar la red actual.
Se dejó el script y su archivo privado de credenciales en Raspberry; sudo no
está disponible sin contraseña, por eso este paso sigue pendiente.

Para activar, con acceso local o Ethernet disponible:

```bash
sudo python3 ~/tic3/raspi/setup_ap.py --activate
```

La activación cambia wlan0 del WiFi actual al AP, puede cortar SSH a
192.0.2.10. Conectar PC al SSID nuevo y usar SSH a 192.168.50.1. La clave de
AP es diferente de la actual, guardada en el archivo privado
`~/tic3/.ap_credentials.json`; consultarla únicamente en tu terminal local si
necesitas conectar el PC. No se activa automáticamente al reiniciar todavía.

No se ha medido la estabilidad real del AP. El tráfico binario previsto es
aproximadamente 0,1 Mbit/s para ambos sensores; el volumen es pequeño, pero hay
que verificar señal, interferencias y cortes antes de asegurar funcionamiento.

## Archivos añadidos y pendientes

- `esp/main/imu_transport.c`: sockets, handshake, ACK y reintentos.
- `raspi/imu_server.py`: receptor TCP/UDP y guardado durable/idempotente.
- `raspi/test_imu_server.py`: prueba TCP, guardado, reenvío y deduplicación entre formatos/reinicio.
- `raspi/setup_ap.py`: preparación/activación local del hotspot.
- `configurar_wifi_local.py`: entrada de clave por terminal sin mostrarla ni necesitar idf.py.

Compilación ESP-IDF y grabación verificadas. Pruebas de codec y buffers e
integración TCP binario/JSON correctas. Pendientes AP real, UDP en placa,
cortes prolongados de red, configuración remota START/STOP/CONFIG, sesiones
finitas, modo secuencial y GUI; no presentar esta prueba como toda la Tarea1.
