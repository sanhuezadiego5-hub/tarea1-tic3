# Contrato ESP32 ↔ Raspberry: datos separados, binario o JSON

> Estado vigente: GUI con XYZ en vivo y configuración remota de ODR, rangos,
> objetivos por flujo, ventana, modo por core, TCP/UDP y binario/JSON.
> La prueba actual usa WiFi compartido; AP desactivado temporalmente por usuario.
> Manual y evidencia actualizados en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las notas inferiores conservan etapas previas de desarrollo.

> Actualización GUI/control: START/STOP y GUI web implementados. La ESP ahora
> espera START al encender; STOP cierra y guarda bloques parciales antes de
> confirmar. Estado y uso vigentes en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las secciones anteriores a esta actualización describen la etapa de envío continuo.

Estado: codecs, reensamblado, cliente socket ESP y servidor Raspberry
implementados. Envío real TCP binario verificado, ACK después de guardado durable
y deduplicación persistente. JSON probado en integración y UDP compilado;
UDP en placa, AP y comandos remotos START/STOP/CONFIG siguen pendientes.
[ENVIO_REAL.md](../raspi/ENVIO_REAL.md) describe lo que está activo. Las propuestas
de comandos remotos de este contrato no implican que estén implementadas.

## 1. Bloque lógico, fragmento y formato

Un bloque corresponde a un parámetro y una ventana temporal. `stream=0` es
aceleración XYZ; `stream=1` es velocidad angular XYZ. No se mezclan parámetros.
`sample_count` cuenta muestras XYZ, por lo que los valores escalares son
`3 × sample_count`. El conteo es real, no se rellena para alcanzar el esperado.

Ambos formatos guardan las mismas cuentas crudas `int16`, secuencias, tiempos
y metadatos. Binario v1 usa enteros big-endian con tamaños explícitos; JSON usa
UTF-8 y campos con nombre. Ninguno transmite el layout de una estructura C.

Cada fragmento mide **como máximo 1200 bytes**, sin incluir el prefijo TCP.
Binario lleva hasta 78 muestras XYZ; JSON hasta 6, para acotar el peor caso de
serialización. El tamaño de fragmento se comprobó también con contadores y
identificadores de anchura máxima. JSON tiene más overhead; ambos reconstruyen
el mismo arreglo. Un bloque vacío lleva un fragmento sin muestras.

Por ejemplo, un bloque de aceleración de 400 XYZ / 1200 valores requiere
6 fragmentos binarios o 67 fragmentos JSON. A 5 s y 400 Hz son nominalmente
2000 XYZ / 6000 valores, distribuidos en 26 fragmentos binarios o 334 JSON.
El intervalo controla el cierre del bloque, no obliga a enviarlo en un solo
paquete ni cambia la ODR física.

## 2. Delimitación prevista para la conexión

- **TCP:** prefijo uint32 big-endian con longitud del fragmento, seguido del
  fragmento completo. La longitud válida es 96…1200 bytes. Acumular recv
  parciales, separar recv con varios mensajes y enviar todos los bytes ante
  send parcial. No usar una llamada recv como límite de mensaje.
- **UDP:** un fragmento por datagrama, sin prefijo. Reensamblar por identificador,
  no por orden de llegada. No depender de fragmentación IP del arreglo completo.
- **Consola de prueba `TIC3_WIRE_CONSOLE`:** usa el prefijo TCP tanto para
  binario como para JSON; no establece un socket. Excluir logs de arranque y
  depuración de la captura de payload. La salida antigua de depuración
  `TIC3_PRINT_ARRAYS` sigue siendo NDJSON sin prefijo y no es este protocolo.

## 3. Layout binario v1

Cabecera fija de 96 bytes; todos los enteros multibyte son unsigned big-endian:

| Offset | Bytes | Campo |
|---:|---:|---|
| 0 | 4 | Magic ASCII `TIC3` |
| 4 | 1 | Versión = 1 |
| 5 | 1 | Tipo: HELLO=1, DATA=2, ACK=3, HELLO_ACK=4 |
| 6 | 1 | Stream: aceleración=0, giro=1; 255 para HELLO |
| 7 | 1 | Flags |
| 8 | 8 | device_id: MAC base en los 48 bits bajos |
| 16 | 8 | boot_id aleatorio nuevo por arranque |
| 24 | 4 | window_seq, creciente por ventana durante el arranque |
| 28 | 2 | sample_hz del flujo |
| 30 | 2 | Rango absoluto: g para aceleración, °/s para giro |
| 32 | 4 | interval_ms configurado |
| 36 | 8 | window_start_us, reloj monotónico de ESP |
| 44 | 8 | window_end_us |
| 52 | 4 | sample_count del bloque entero |
| 56 | 2 | fragment_index, comienza en cero |
| 58 | 2 | fragment_count |
| 60 | 4 | sample_offset dentro del bloque |
| 64 | 4 | fragment_samples |
| 68 | 4 | buffer_dropped, muestras descartadas en la ventana |
| 72 | 4 | fifo_skipped_frames, frames omitidos según FIFO |
| 76 | 4 | fifo_errors, pérdidas no cuantificadas / header inválido |
| 80 | 4 | i2c_errors en la ventana |
| 84 | 4 | output_dropped_blocks, acumulativo por flujo al abrir la ventana |
| 88 | 4 | payload_bytes: fragment_samples × 14 |
| 92 | 4 | CRC32 de bytes 0…91 concatenados con payload |

Cada muestra del payload ocupa 14 bytes:

| Offset relativo | Bytes | Campo |
|---:|---:|---|
| 0 | 4 | sample_seq |
| 4 | 4 | read_offset_us respecto al inicio de ventana |
| 8 | 2 | raw_x signed int16 en complemento a dos |
| 10 | 2 | raw_y signed int16 |
| 12 | 2 | raw_z signed int16 |

CRC32 estándar compatible con `zlib.crc32`: polinomio reflejado 0xEDB88320,
estado inicial 0xffffffff y XOR final 0xffffffff. CRC no incluye su propio campo.

Flags: bit 0 indica timestamps de lectura del host, bit 1 indica pérdida de
magnitud desconocida (por ejemplo skip saturado o FIFO inválida), bit 2 indica
ventana parcial al detener adquisición por error. Otros bits se rechazan en v1.
Un skip de 255 es un mínimo reportable; no equivale necesariamente a 255
muestras perdidas de cada parámetro si tienen tasas diferentes.

`read_us = window_start_us + read_offset_us`. Es hora de vaciado FIFO por ESP,
no hora exacta de conversión. Muestras de un burst pueden compartirla. Para
medir tiempos exactos de sensor falta integrar sensortime, con campo/versión
explícitos; no reinterpretar el campo existente.

## 4. JSON v1

JSON lleva los campos anteriores con los mismos nombres y significado, además
de los arreglos `values`, `sample_seq` y `read_offset_us`. Identidades se
codifican como cadenas de **16 dígitos hexadecimales minúsculos** para evitar
redondeo de enteros de 64 bits en clientes web. `magic` es la cadena `TIC3`.

```text
values = [x0,y0,z0,x1,y1,z1,...]  # longitud = 3 × fragment_samples
sample_seq = [seq0,seq1,...]      # longitud = fragment_samples
read_offset_us = [dt0,dt1,...]    # longitud = fragment_samples
```

Las cuentas y tiempos deben ser enteros, no floats ni booleanos. Se rechazan
NaN, claves duplicadas, cantidades/rangos inválidos y arreglos con longitud
incorrecta. `payload_bytes` indica bytes del payload canónico binario equivalente,
no longitud del texto JSON. CRC32 se calcula sobre la cabecera binaria canónica
0…91 y las muestras canónicas de 14 bytes; así se verifica también JSON sin
depender de espacios u orden de las claves. El helper Python hace esa conversión.

Conversiones: `acc = raw × range × 9.80665 / 32768` m/s² y
`gyro = raw × range × pi / (180 × 32768)` rad/s. No se confunde giro con
velocidad lineal. El perfil de filtro actual es el del firmware existente;
el servidor debe guardar el resto de configuración de sesión al integrar control.

## 5. Identidad, reensamblado y deduplicación

Clave del bloque: `(device_id, boot_id, stream, window_seq)`. Identidad no depende
de la IP. No reiniciar window_seq dentro de un mismo boot_id al añadir futuras
sesiones; si se requiere hacerlo, extender el contrato con session_id antes.

El receptor valida versión, CRC, rangos, conteos, offset y metadatos coherentes
entre fragmentos. Usa índices para aceptar fuera de orden y descarta duplicados
idénticos. Fragmentos distintos con la misma clave/índice son un error.

Límite inicial: 16 bloques incompletos, máximo 2 s desde el primer fragmento
para completarlos. Un duplicado no prolonga el plazo. El servidor debe llamar
`expire()` incluso mientras no recibe datos y registrar cuántos fragmentos
faltaron. Si la red exige otro timeout, cambiarlo conscientemente en ambos lados.

El helper mantiene 128 claves completadas recientes. **Eso no sustituye la
deduplicación persistente del servidor:** guardar cada bloque de forma idempotente
con su clave para que un reenvío tras pérdida de ACK o reinicio del servidor no
duplique registros. Guardar bloques completos; conservar/reportar los parciales
de UDP como incompletos si se desea, sin rellenar huecos con cero.

## 6. Política de conexión a implementar

La Raspberry será AP y servidor, ESP cliente. Propuesta: IP fija de la interfaz
AP `192.168.50.1/24` si no hay conflicto, ESP por DHCP; SSID/clave configurados
una vez, sin teclado al reconectar. AP de Raspberry obligatorio para la tarea;
lista de redes por prioridad opcional para desarrollo. No guardar credenciales
en el repositorio ni cambiar de red durante una sesión sana.

Puertos propuestos: TCP 5001 para datos TCP; UDP 5001 para datos UDP; TCP 5000
para control/probe cuando los datos usan UDP. Usar el mismo número en TCP/UDP
es válido. Múltiples clientes TCP comparten servidor; puerto local de ESP lo
asigna el sistema. Raspberry debe fallar claramente ante puerto ocupado y no
escoger silenciosamente otro.

**No se reserva ni se escanea el puerto desde la ESP.** Para TCP, connect con
timeout de 2 s comprueba disponibilidad; luego HELLO y HELLO_ACK de v1 comprueban
que el proceso habla el protocolo esperado. Validar tipo, versión e identidades
de respuesta. Para UDP, sendto exitoso no comprueba servicio/receptor: usar el
canal TCP 5000 con el mismo handshake antes de iniciar el flujo. Ese handshake
no promete que todos los datagramas siguientes lleguen.

HELLO/HELLO_ACK/ACK tienen cabecera sin muestras; el cliente implementado y
el receptor usan controles binarios, incluso cuando DATA está en JSON. El receptor compatible con v1 debe aceptar ambos formatos de datos;
si solo admite uno, rechazar antes de iniciar. El formato puede detectarse por
magic binario `TIC3` o inicio de objeto JSON. ACK identifica dispositivo, boot,
flujo y ventana; el resto de sus campos se pone en cero.

Al perder WiFi: invalidar sockets, esperar nueva IP y recrearlos. Reintentos
de 1, 2, 4, 8, 16 y 30 s con jitter, máximo 30 s entre intentos. No exigir
teclado para el funcionamiento normal; clave incorrecta requiere corregirla,
sin un bucle de intentos a máxima velocidad.

TCP: conservar el bloque inmutable mientras se envía y hasta ACK de aplicación,
con plazo de 2 s para enviar todo el bloque y 2 s para ACK. Ante fallo, cerrar,
reconectar/handshake y reenviar la misma clave y fragmentos. Raspberry confirma
solo después de guardar idempotentemente. Reintentar hasta recuperar conexión;
STOP remoto sigue pendiente; nunca borrar pendientes solo porque send devolvió éxito. La
adquisición sigue por el otro core y puede descartar ventanas nuevas por RAM.

UDP: enviar cada fragmento una vez, sin ACK ni retransmisión de muestras en la
variante inicial; contabilizar errores locales y continuar. Raspberry registra
ausencias/duplicados/reordenamiento. Un protocolo UDP confiable sería otra
variante a medir separadamente.

El transporte TCP implementado conserva el bloque hasta ACK. UDP está compilado,
pendiente de prueba en placa. La alternativa de consola libera el bloque tras
fallo, sin reintento. El servidor guarda antes de ACK y deduplica persistentemente.

## 7. Cambios de configuración

Actualmente ODR/intervalo/formato se configuran por menuconfig, build y arranque.
No hay comando remoto que cambie parámetros durante adquisición. Para la futura
configuración de Raspberry: STOP → cerrar/drenar o declarar descarte → validar
y aplicar CONFIG → ACK con valores efectivos → START. Cambiar formato, tasa o
intervalo solo entre sesiones, sin mezclar configuraciones dentro de un bloque.

Si falta RAM o el valor no está admitido, responder ERROR y no iniciar con una
configuración parcial. La aplicación remota deberá gestionar esa transición;
un timeout nunca cuenta como confirmación. TCP es la opción inicial de transporte
exigida por la tarea, con cambio posterior TCP/UDP desde Raspberry pendiente.

## 8. Integración con tu servidor

`raspi/imu_protocol.py` es un helper sin sockets ni GUI:

```python
from imu_protocol import TCPFrames, Reassembler

frames = TCPFrames()
blocks = Reassembler(timeout=2)
# En tu bucle TCP: chunks recv de <=4096 bytes.
for packet in frames.feed(chunk):
    block = blocks.push(packet)  # solo DATA; tratar HELLO/control aparte
    if block is not None:
        # Guardar idempotentemente por block['key']; después enviar ACK en TCP.
        acceleration_or_gyro_values = block['values']
# En UDP: blocks.push(datagram) directamente, sin prefijo.
# En ambos: llamar blocks.expire() periódicamente y registrar pérdidas.
```

El helper devuelve el arreglo completo, secuencias y tiempos; valida CRC y ambos
formatos. `encode_control` permite generar respuesta HELLO_ACK/ACK y `tcp_frame`
añade delimitación. La creación del socket, gestión de varios clientes, ACK,
persistencia, configuración remota y GUI pertenecen a tu servidor.
