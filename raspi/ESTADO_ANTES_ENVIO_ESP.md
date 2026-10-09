# Estado y decisiones antes de enviar datos ESP → Raspberry

> Registro histórico anterior al envío real. El servidor de texto fue detenido
> y sustituido por `raspi/imu_server.py`. Estado e instrucciones actuales en
> [ENVIO_REAL.md](ENVIO_REAL.md). Las secciones siguientes conservan la prueba anterior.

Verificado el 7 de octubre de 2026. Este resumen diferencia lo que ejecuta el
código de las políticas acordadas para la conexión futura.

Actualización WiFi: gestor ESP opcional de tres perfiles implementado; pendientes
claves/activación y pruebas en placa. AP Raspberry, sockets y servidor IMU siguen
pendientes. Política actualizada en ../WIFI_PERFILES.md.

## 1. Conexión PC/Raspberry verificada

| Parámetro | Valor actual |
|---|---|
| Usuario Raspberry | raccoon |
| Hostname | Raccoon |
| IPv4 Raspberry en WiFi actual | 192.0.2.10 |
| IPv4 PC vista por Raspberry | 192.0.2.20 |
| SSH | TCP 22, clave dedicada |
| Clave en PC | ~/.ssh/tic3_raspi |
| Python Raspberry | 3.13.5 |
| Directorio remoto | /ruta/al/proyecto |
| Servidor de prueba | clientservertcp.py, escucha 0.0.0.0:5001 |
| Cliente de prueba | servercleintetcp.py, ejecutado en PC |
| Proceso servidor observado | PID 3542, registrado en tcp_probe.pid |
| Log remoto | /ruta/al/proyecto/tcp_probe.log |
| Resultado | Tres mensajes confirmados en una conexión TCP |

El servidor quedó en segundo plano y no depende de mantener abierta la sesión
SSH. No es un servicio de arranque: tras reiniciar Raspberry puede ser necesario
iniciarlo de nuevo. PID/IP son valores de esta ejecución, no identidades permanentes.
Se dejó también una sesión SSH interactiva de trabajo abierta en esta ejecución.

Este servidor solo prueba texto con prefijo de longitud y ACK. No recibe aún el
protocolo de bloques IMU, ni guarda sensores ni configura la ESP. Atiende un
cliente a la vez, timeout de socket de 5 s, texto máximo de 16384 bytes y puerto
configurable. No confundir ese ACK de texto con el futuro ACK de bloque guardado.

## 2. Repetir manualmente

En terminal del PC:

```bash
ssh -i ~/.ssh/tic3_raspi usuario@192.0.2.10
```

Dentro de la Raspberry:

```bash
cd ~/tic3
ss -ltnp 'sport = :5001'
tail -f tcp_probe.log
```

Ctrl+C detiene la lectura de `tail`, no el servidor. Mientras se vea un proceso
escuchando en 5001, no iniciar otra copia. Si el puerto está libre después de
un reinicio, iniciar manualmente en primer plano:

```bash
cd ~/tic3
python3 -u clientservertcp.py --host 0.0.0.0 --port 5001
```

Dejar esa terminal abierta. Si se prefiere el mismo arranque en segundo plano
que se usó en la prueba, **solo cuando el puerto esté libre**:

```bash
cd ~/tic3
nohup python3 -u clientservertcp.py --host 0.0.0.0 --port 5001 >> tcp_probe.log 2>&1 < /dev/null &
printf '%s\n' "$!" > tcp_probe.pid
```

En otra terminal del PC:

```bash
python3 /ruta/al/proyecto/servercleintetcp.py --host 192.0.2.10 --port 5001 --count 3 --interval 1 --timeout 5
```

Resultado esperado: `[OK] 3 mensajes confirmados en una misma conexión TCP.`
SSH es acceso para administrar; los datos TCP usan otra conexión al puerto 5001.
No se crea un túnel SSH para esta prueba. No usar 0.0.0.0 o 127.0.0.1 como destino
desde el PC. Si cambia WiFi, consultar `hostname -I` y actualizar IP de conexión.

## 3. Parámetros del firmware comprobados en los archivos actuales

| Parámetro | Configuración |
|---|---|
| Target | ESP32-S3, dual core; CPU configurada a 160 MHz |
| Framework usado para compilar | ESP-IDF v6.1 instalado en PC |
| Entrada de aplicación | esp/main/main.c: app_main |
| Sensor | BMI270, acelerómetro y giroscopio habilitados |
| Bus | I2C, SDA GPIO42, SCL GPIO2, 100 kHz, dirección 0x68 |
| Aceleración | ODR 400 Hz, rango ±8 g, ACC_AVG=4, filtro performance del driver |
| Velocidad angular | ODR 400 Hz, rango ±2000 °/s, configuración GYR_CONF del driver |
| ODR habilitadas por aplicación | 25, 50, 100, 200 y 400 Hz, independientes por sensor |
| Vaciado FIFO | Cada 10 ms, burst de hasta 512 bytes |
| FIFO | Cabeceras, entrada filtrada por ODR, stop-on-full, sensortime deshabilitado |
| Tick FreeRTOS | 100 Hz = 10 ms; no se usa como reloj de muestreo de 400 Hz |
| Adquisición | Core 1, prioridad 8, stack 4096 bytes |
| Procesamiento/salida | Core 0, prioridad 4, stack 4096 bytes |
| Stack app_main | 16384 bytes |
| Timeout de lectura I2C | 100 ms; escritura 2000 ms, en milisegundos |
| Reintentos CHIPID/soft-reset | Hasta 3 |
| Intervalo actual | TIC3_INTERVAL_MS=1000, cierre de bloques cada 1 s |
| Rango de intervalos | 100…60000 ms, condicionado a memoria |
| Bloques por flujo | 2: activo + pendiente/en consumo; configurable 2…4 |
| Formato seleccionado | Binario; JSON también soportado |
| Salida actual | Resumen por consola; WIRE_CONSOLE y PRINT_ARRAYS desactivados |

Core 1 lee y deposita muestras; core 0 forma la salida de bloques cerrados. Las
colas llevan punteros a bloques cuya propiedad se transfiere, sin compartir un
arreglo mutable. La tarea de salida no modifica el activo. Un core no queda
reservado exclusivamente: FreeRTOS y tareas de sistema siguen compartiéndolo.

## 4. Arreglos, capacidad y tiempo

Se almacenan arreglos separados: stream 0 aceleración, stream 1 velocidad
angular. XYZ de un instante cuentan como una muestra y tres valores. Orden:
`[x0,y0,z0,x1,y1,z1,...]`. No hay medición directa de velocidad lineal/posición.

| Caso por flujo | XYZ nominales | Valores nominales |
|---|---:|---:|
| 400 Hz × 1 s | 400 | 1200 |
| 400 Hz × 5 s | 2000 | 6000 |
| 100 Hz × 5 s | 500 | 1500 |

Enviar conteos reales, sin repetir ni rellenar muestras. Ventanas se asignan por
hora de vaciado FIFO; no hay aún timestamp exacto de conversión del sensor.
`read_us` usa reloj monotónico de ESP; muestras de un burst pueden compartirlo.
La recepción UTC en Raspberry deberá guardarse aparte. Identidades y ventanas
permiten reconstruir datos aunque los fragmentos lleguen fuera de orden.

Capacidad por bloque = ceil(fs × intervalo_s) + ceil(N/20) + 8 XYZ.
Muestra en RAM: raw XYZ int16 (6 bytes), secuencia (4), offset de lectura (4),
total 14 bytes. Cada bloque conserva metadatos y punteros por separado.

Dos flujos a 400 Hz, dos bloques por flujo:

| Intervalo | Capacidad por bloque | RAM de arrays | Presupuesto |
|---|---:|---:|---|
| 1 s | 428 XYZ | 23968 bytes | Admite |
| 5 s | 2108 XYZ | 118048 bytes | Admite |
| 10 s | 4208 XYZ | 235648 bytes | Rechaza |

Presupuesto arrays 128 KiB; reservar al menos 64 KiB de heap interno de margen.
Validar tamaños, alineación y cada reserva contigua antes de empezar. No depender
de PSRAM. Que una combinación quepa en el presupuesto no prueba que la placa tenga
la RAM libre necesaria ni que la red sostenga el rendimiento.

## 5. Errores ya manejados por firmware

- Valor de fs/intervalo inválido o RAM insuficiente: informar y no adquirir.
- Falta de bloque libre: conservar pendientes y descartar la ventana nueva;
  incrementar contador de ventanas perdidas.
- Capacidad de muestras llena: descartar la nueva muestra y contarla.
- FIFO reporta skip: contar frames omitidos sin inventar pérdida exacta por flujo.
- Frame parcialmente leído: no concatenar cola; BMI270 repite íntegro ese frame.
- Lectura FIFO fallida/header inválido: flush para recuperar frontera, declarar
  pérdida desconocida. Una lectura de longitud fallida no consume muestras FIFO.
- Tres errores I2C consecutivos: parar adquisición, intentar publicar ventana
  parcial si la cola tiene sitio y desactivar FIFO. Recuperación automática pendiente.
- Error de salida por consola: informar y devolver bloque al pool, sin reintento.

Los contadores distinguen muestras descartadas, frames FIFO, errores I2C/FIFO y
ventanas de salida descartadas. Dos cores no garantizan conservación ilimitada.

## 6. Formato de datos implementado

- Binario: enteros big-endian, header 96 bytes, payload 14 bytes por muestra,
  CRC32 de cabecera sin campo CRC y payload; nunca copiar structs C directamente.
- JSON: mismos datos y CRC sobre representación binaria canónica, identidades
  como hex de 16 caracteres; arrays de valores, secuencias y offsets de lectura.
- Máximo fragmento: 1200 bytes; hasta 78 XYZ en binario y 6 XYZ en JSON.
- Bloque de 400 XYZ: 6 fragmentos binarios o 67 JSON, conservando el mismo arreglo.
- TCP futuro: prefijo uint32 big-endian de longitud + fragmento, manejar send/recv
  parciales. UDP futuro: un fragmento por datagrama, sin prefijo.
- Identificación: MAC base como device_id, boot_id nuevo por arranque, stream y
  window_seq; fragment_index, fragment_count, sample_offset y conteos reales.
- IP no se repite por muestra; usar identidad de dispositivo aunque cambie IP.
- Helper Python implementado: CRC, dimensiones, rangos, duplicados, reordenamiento,
  cache de 128 completados, máximo 16 incompletos, expiración a 2 s.
- La cache del helper no sustituye deduplicación persistente del servidor.
- Conversiones: acc = raw × rango_g × 9.80665/32768;
  gyro = raw × rango_deg_s × pi/(180 × 32768).

Ver `../PROTOCOLO_DATOS.md` para offsets, flags, validación y API del helper.

## 7. Política de red acordada, aún por implementar en ESP/servidor IMU

- Topología final exigida: Raspberry emite AP; servidor siempre en Raspberry,
  cliente siempre en ESP. El WiFi actual es entorno de desarrollo.
- Propuesta final de AP: IP fija 192.168.50.1/24 si no hay conflictos; ESP DHCP.
  No es la IP actual de SSH ni una configuración ya aplicada. SSID/clave pendientes.
- TCP por defecto, UDP seleccionable; datos TCP/UDP 5001, control TCP 5000 con UDP.
- Puerto local del cliente automático; varios clientes pueden compartir servidor.
  Ante puerto ocupado en Raspberry: error explícito, no elegir otro silenciosamente.
- TCP: connect de máximo 2 s y HELLO/HELLO_ACK para verificar protocolo; no escanear
  ni reservar un puerto previamente. UDP: probe/control TCP antes del flujo, sin
  confundir sendto exitoso con recepción real.
- WiFi caído: invalidar sockets, reintentos 1/2/4/8/16/30 s con jitter, esperar IP,
  recrear conexión/handshake. No teclado para reconectar con credenciales válidas.
- TCP: retener bloque hasta ACK después de guardado idempotente; envío del bloque
  máximo 2 s, ACK máximo 2 s; reenviar misma clave tras reconexión, deduplicar.
  STOP cancela espera. La RAM finita puede causar pérdidas en ventanas nuevas.
- UDP inicial: enviar una vez, sin ACK/retransmisión de datos; informar errores
  locales y registrar pérdidas/reordenamiento en receptor.
- Cambios remotos: STOP → cierre/drenaje o descarte explícito → CONFIG validada
  → ACK → START. No mezclar configuraciones dentro de un bloque.
- Actualmente se cambia config con menuconfig/build/arranque, no remotamente.
- Sesiones finitas con fs distintas: duración común y conteos por flujo como
  propuesta; confirmar interpretación docente de N muestras/envíos. Modo secuencial
  adicional exigido por la tarea todavía pendiente.

## 8. Qué falta para iniciar ESP → Raspberry con datos reales

Servidor IMU que integre helper, persistencia `sensores/`, HELLO/ACK y control;
cliente WiFi/TCP/UDP de ESP; SSID/clave/endpoint; AP de Raspberry; modo secuencial,
sesiones/configuración remota y GUI. Sustituir el proceso de prueba del puerto
5001 de forma controlada, sin arrancar dos servidores que compitan por él.

Verificación ya hecha: compilación firmware y pruebas de FIFO/buffers/codecs
C→Python para 1/5 s, binario/JSON, CRC y errores; ahora prueba de red real
PC/Raspberry con tres ACK. No se flasheó la ESP en esta etapa. Validar en placa
400 Hz sostenidos, RAM real y pérdidas antes de prometer adquisición continua.
