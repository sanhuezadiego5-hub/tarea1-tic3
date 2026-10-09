> Para el estado vigente y los comandos de esta copia organizada, consulta [README general](../README.md). Las notas de este archivo documentan también etapas anteriores.

# ESP32-S3 + BMI270: arreglos por parámetro

> Estado vigente: GUI con XYZ en vivo y configuración remota de ODR, rangos,
> objetivos por flujo, ventana, modo por core, TCP/UDP y binario/JSON.
> La prueba actual usa WiFi compartido; AP desactivado temporalmente por usuario.
> Manual y evidencia actualizados en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las notas inferiores conservan etapas previas de desarrollo.

> Actualización GUI/control: START/STOP y GUI web implementados. La ESP ahora
> espera START al encender; STOP cierra y guarda bloques parciales antes de
> confirmar. Estado y uso vigentes en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las secciones anteriores a esta actualización describen la etapa de envío continuo.

Firmware en `main/`; demo original conservada en `../referencias/bmi270_standalone.c`.
**Envío real comprobado:** ESP flasheada y conectada a WIFI_COMPARTIDO,
TCP binario a Raspberry 192.0.2.10:5001. Servidor IMU guarda bloques separados
y responde ACK después de persistirlos. JSON y UDP son opciones; la prueba en
placa se hizo con TCP binario. AP Raspberry preparado mediante script, aún no
activado. Guía actual: [ENVIO_REAL.md](../raspi/ENVIO_REAL.md).

## Qué hace

- Lee FIFO filtrada cada 10 ms en core 1 y separa aceleración / velocidad angular.
- Core 0 procesa bloques cerrados, uno por parámetro y ventana configurable.
- Por defecto: 400 Hz en ambos sensores, ventana de 1000 ms, dos bloques por
  parámetro (activo + pendiente/en consumo). Con NETWORK_SEND habilitado, salida por red.
- Se envía el conteo real; 400 XYZ equivalen nominalmente a 1200 valores.
- Frecuencias independientes: 25, 50, 100, 200 o 400 Hz.
- Raw int16 con rango activo para convertir a m/s² / rad/s. read_us representa
  vaciado FIFO, no instante físico exacto de cada muestra.

## Intervalo y capacidad

`TIC3_INTERVAL_MS`: de 100 a 60000 ms, sujeto al presupuesto de RAM.
1000 significa 1 segundo; 5000 significa 5 segundos. Capacidad por flujo:

```text
N = ceil(fs × interval_ms / 1000)
capacidad = N + ceil(N / 20) + 8 muestras XYZ
storage por bloque = capacidad × 14 bytes
```

Los 14 bytes de RAM por muestra son XYZ raw (6), secuencia (4) y offset de
lectura en microsegundos (4). El offset permite conservar el tiempo de host sin
repetir uint64 por cada muestra. La ventana máxima de 60 s cabe en ese offset.

| Dos sensores a 400 Hz, dos bloques por sensor | 1 s | 5 s | 10 s |
|---|---:|---:|---:|
| XYZ nominales por sensor | 400 | 2000 | 4000 |
| Valores por sensor | 1200 | 6000 | 12000 |
| Capacidad XYZ por bloque | 428 | 2108 | 4208 |
| RAM total de arrays | 23968 B | 118048 B | 235648 B |
| Presupuesto de arrays de 128 KiB | Admite | Admite | Rechaza |

Además debe quedar al menos 64 KiB de heap interno después de reservar arrays,
y cada malloc debe conseguir memoria contigua. Validación antes de inicializar
adquisición; si no cabe, informa error y no arranca con buffers insuficientes.
El límite de 60 s no promete aceptar todas las combinaciones de fs/intervalo.
Por ejemplo 60 s a 25 Hz sí cabe en el presupuesto. Para intervalos largos a
400 Hz, reducir tasas / bloques, ampliar almacenamiento o enviar subbloques con
más frecuencia; no cambiar solo un delay para hacerlo caber.

Si no queda bloque libre al cerrar ventana, descarta la ventana nueva completa,
incrementa el contador y conserva los bloques pendientes. La salida nunca pisa
un arreglo activo. Si una muestra excede capacidad, se descarta y se cuenta.
Tres errores I2C consecutivos detienen adquisición y publican un último bloque
parcial si hay espacio en la cola; si no, registran el descarte. No hay aún
reinicialización automática del sensor.

## Construcción / ajustes

Activar el ESP-IDF instalado; desde esta carpeta:

`source /ruta/a/.espressif/tools/activate_idf_v6.1.sh`

```bash
idf.py -B build-tic3 menuconfig
idf.py -B build-tic3 build
```

En **TIC3 - bloques IMU**:

- `TIC3_ACCEL_HZ`, `TIC3_GYRO_HZ`: frecuencias de cada parámetro.
- `TIC3_INTERVAL_MS`: cierre de ventana / periodicidad nominal de salida.
- `TIC3_BLOCKS_PER_STREAM`: 2…4 bloques por parámetro; influye en memoria.
- **Formato del protocolo de datos**: binario (default) o JSON.
- `TIC3_WIRE_CONSOLE`: imprime fragmentos del protocolo con longitud uint32
  big-endian seguida del payload, como futura delimitación TCP.
- `TIC3_PRINT_ARRAYS`: salida antigua de depuración en un JSON por bloque,
  sin prefijo; solo se usa si WIRE_CONSOLE está desactivado.

En **TIC3 - envio a Raspberry**, NETWORK_SEND activa la red y permite elegir
TCP o UDP. Si se desactiva, la salida por defecto es un resumen de consola.
WIRE_CONSOLE controla únicamente la alternativa por consola. Para captura del
protocolo por serial, excluir logs de arranque/ESP y comprobar capacidad UART/USB:
JSON fragmentado tiene bastante más overhead que binario y 115200 baud no basta
para dos flujos de 400 Hz. Preferir USB / velocidad serial medida suficiente.
Una salida lenta se detecta por descarte de bloques, sin cambiar la ODR.

Los cambios son locales por menuconfig/build/reinicio; no hay configuración
remota durante adquisición. Al integrar esa parte, cerrar sesión y aplicar
configuración validada antes de START, según el contrato.

## Integración con tu conexión y servidor

- `imu_output.h`: punto de salida `imu_send_block`, helpers
  `imu_output_fragment_count` e `imu_output_encode_fragment` que usan el formato
  seleccionado e identidad real de ESP. `imu_transport.c` implementa sockets y
  utiliza `imu_console_send_block` cuando NETWORK_SEND está desactivado.
- El bloque es inmutable mientras dura la llamada. Para TCP debe retenerse hasta
  ACK; no devolverlo al pool mientras el módulo de red lo use.
- `imu_wire.h/.c`: codecs independientes de WiFi y sockets.
- `../raspi/imu_protocol.py`: decodificación y reensamblado para tu servidor,
  sin sockets, WiFi ni GUI. Maneja binario / JSON y validación de integridad.
- `../docs/PROTOCOLO_DATOS.md`: layout, delimitación, puertos propuestos, handshake,
  timeout, reconexión, ACK/reintento y deduplicación.

## Verificación

Con compilador C local y Python estándar, sin dependencias nuevas:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main main/imu_stream.c tests/test_imu_stream.c -lm -o /tmp/test_imu_stream
/tmp/test_imu_stream
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main main/imu_stream.c main/imu_wire.c tests/wire_fixture.c -o /tmp/tic3_wire_fixture
/tmp/tic3_wire_fixture /tmp/tic3-wire.bin /tmp/tic3-json.bin
TIC3_WIRE_FIXTURE=/tmp/tic3-wire.bin TIC3_JSON_FIXTURE=/tmp/tic3-json.bin \
  python3 -m unittest discover -s ../raspi -p 'test_*.py' -v
```

Se verifican períodos 1/5 s, tasas independientes, rechazo por memoria,
capacidad, parser FIFO, signo/orden XYZ, conversión, binario/JSON C→Python,
CRC, payloads máximos, fragmentación, duplicados, reordenamiento y timeout.
Compilación con ESP-IDF v6.1, flasheo y recepción real TCP binaria verificados.
Más de 160 ventanas por flujo desde secuencia 0 sin descartes de salida ni
skips FIFO en la sesión posterior al ajuste de arranque. Pendientes pruebas
de cortes prolongados de red, UDP en placa, salida lenta y fallos I2C inducidos.
Modo secuencial, sesiones finitas y configuración remota siguen pendientes
para cumplir la totalidad de Tarea 1.
