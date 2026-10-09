# Decisiones de diseño ESP32-S3 / BMI270 — Tarea 1

> Estado vigente: GUI con XYZ en vivo y configuración remota de ODR, rangos,
> objetivos por flujo, ventana, modo por core, TCP/UDP y binario/JSON.
> La prueba actual usa WiFi compartido; AP desactivado temporalmente por usuario.
> Manual y evidencia actualizados en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las notas inferiores conservan etapas previas de desarrollo.

> Actualización GUI/control: START/STOP y GUI web implementados. La ESP ahora
> espera START al encender; STOP cierra y guarda bloques parciales antes de
> confirmar. Estado y uso vigentes en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las secciones anteriores a esta actualización describen la etapa de envío continuo.

Actualizado el 7 de octubre de 2026. Referencias de implementación:
`esp/README.md` y `PROTOCOLO_DATOS.md`.

Actualización: envío TCP binario real comprobado por WIFI_COMPARTIDO,
servidor Raspberry con persistencia y ACK. AP aún pendiente de activar.
Guía y decisiones de red vigentes: [ENVIO_REAL.md](../raspi/ENVIO_REAL.md).

## Estado real

| Área | Estado |
|---|---|
| Arreglos separados por parámetro | Implementado |
| Frecuencias independientes para aceleración / velocidad angular | Implementado, configuración local |
| Lectura FIFO y adquisición / salida en cores distintos | Implementado |
| Intervalo variable con buffers dimensionados y límites de memoria | Implementado |
| Codificación binaria y JSON, fragmentación y verificación CRC | Implementado |
| Salida de prueba por consola | Implementado |
| Helper Python para tu servidor | Implementado, no abre sockets |
| Selección/reconexión WiFi por prioridades | Habilitado y probado en WiFi actual |
| Conexión socket a servidor y puertos | Implementado y probado con TCP binario |
| ACK, reintentos y reconexión | Implementado y probado con TCP binario |
| Configuración remota, sesiones finitas, modo secuencial y GUI | Pendiente |

La adquisición espera WiFi y HELLO_ACK del servidor antes de arrancar la FIFO.
El AP y las funciones remotas pendientes no se consideran implementados.

## 1. Un arreglo por parámetro

Aceleración XYZ y velocidad angular XYZ tienen bloques, frecuencia, conteo y
secuencia propios. Con ventana de 1 s y 400 Hz: 400 XYZ / 1200 valores por
parámetro. Con 5 s: 2000 XYZ / 6000 valores. Si gyro usa 100 Hz, su bloque de
5 s contiene nominalmente 500 XYZ / 1500 valores aunque aceleración use 400 Hz.

Esto conserva el significado de cada flujo y permite variar sus tasas sin
inventar o repetir lecturas. El sensor no mide velocidad lineal ni posición;
esas serían estimaciones con corrección de gravedad/orientación y deriva.

## 2. ODR, lectura, envío e impresión son independientes

El sensor genera según ODR. La ESP vacía FIFO cada 10 ms en core 1. Cada ventana
configurada cierra bloques; core 0 los procesa. Imprimir lento no cambia ODR,
pero puede agotar pools de salida. FIFO filtrada sigue ODR de cada sensor y
cabeceras permiten tasas distintas. Una ventana contiene la cantidad real
recuperada; jitter/reloj/latencia pueden causar variaciones respecto al nominal.

La hora guardada es de lectura del host, compartida por muestras de un burst.
No es la hora exacta de conversión ni debe usarse cada diferencia nula para
inferir fs. La integración de sensortime para precisión sigue pendiente.

## 3. Cambiar de 1 a 5 segundos

`TIC3_INTERVAL_MS=1000` o `5000`; rango local 100…60000 ms, sujeto a RAM.
Capacidad: ceil(fs × intervalo) + 5% redondeado hacia arriba + 8 XYZ.
Cada muestra ocupa 14 bytes en RAM; cada flujo tiene dos bloques por defecto.

| Dos flujos a 400 Hz | 1 s | 5 s | 10 s |
|---|---:|---:|---:|
| Valores nominales por flujo | 1200 | 6000 | 12000 |
| RAM de arrays | 23968 B | 118048 B | 235648 B |
| Presupuesto de arrays de 128 KiB | Admite | Admite | Rechaza |

La inicialización comprueba presupuesto, heap libre dejando 64 KiB de margen,
alineación y mallocs. Si no cabe, informa error y no comienza adquisición.
Así no se desborda el buffer al cambiar un período. No se garantiza aceptar
cualquier intervalo con cualquier fs; los límites son explícitos y verificables.
Más tiempo de ventana reduce frecuencia de bloques y aumenta RAM y latencia.

## 4. Concurrencia y pérdidas

Core 1 solo produce; core 0 solo consume bloques cerrados. Colas transfieren
propiedad mediante punteros. No bloquear adquisición esperando a la salida,
ni escribir un bloque que ésta está utilizando. Dos cores reducen interferencia,
pero no garantizan cero pérdidas de FIFO, RAM o transporte.

Si no hay bloque libre, conservar pendientes, descartar la ventana nueva y
contabilizarlo. Si se excede capacidad, descartar muestra y contarla. Frames
FIFO omitidos se registran sin presentarlos como pérdida exacta por sensor.
Frames parciales de I2C se repiten íntegros por BMI270; no concatenar colas.
Error FIFO/header inválido obliga a flush y declara pérdida desconocida. Tras
tres errores I2C consecutivos, detener adquisición y cerrar parcial si hay
espacio, para no seguir generando datos falsos. Recuperación del sensor pendiente.

## 5. JSON o binario

Ambos formatos ya tienen codec y pruebas C→Python. Binario por defecto reduce
bytes y carga de serialización; JSON facilita inspección. El formato se selecciona
localmente y el servidor puede usar el mismo decoder para ambos.

No enviar directamente structs C: padding, signo y endianness quedarían
ambiguos. Separar arreglo lógico de fragmentos de transporte, máximo 1200 bytes
por fragmento. Binario agrupa 78 XYZ y JSON 6 XYZ; el reensamblado conserva orden,
valores e identidad. La IP no se repite en cada valor: identificar por dispositivo,
arranque, flujo y ventana. Más detalles/offsets en `PROTOCOLO_DATOS.md`.

## 6. Decisiones de conexión implementadas; AP pendiente

El enunciado fija Raspberry como AP y servidor y ESP como cliente. Propuesta:
IP fija de AP 192.168.50.1/24 si no hay conflicto, ESP por DHCP, SSID/clave una
vez, sin teclado para recuperar una caída. Redes alternativas por prioridad son
opcionales; el laboratorio no reemplaza el AP exigido.

TCP 5001 / UDP 5001 para datos; TCP 5000 de control/probe con UDP. Varios
clientes pueden compartir servidor. Un puerto ocupado en Raspberry se reporta;
no cambiarlo silenciosamente ni pedir confirmación en cada conexión.

TCP comprueba servicio con connect acotado y HELLO/HELLO_ACK de v1. No hacer un
escaneo previo que no reserve nada. UDP no comprueba recepción con sendto:
usar control TCP para confirmar servicio disponible y seguir detectando pérdidas.
Puerto abierto no demuestra protocolo correcto: handshake evita mandar al servicio
equivocado.

Caída WiFi: cerrar sockets, reconectar con 1/2/4/8/16/30 s y jitter, esperar IP,
recrear socket y handshake. Adquisición paralela continúa; los buffers no guardan
indefinidamente. No se promete conservación de una caída larga sin más RAM o disco.

TCP: conservar lote hasta ACK de aplicación después de guardado idempotente;
si falla envío/ACK, reconectar y reenviar misma clave. Plazos implementados de 2 s
por conexión, 2 s por envío del bloque y 2 s por ACK. STOP remoto sigue pendiente.
Raspberry deduplica de manera persistente y vuelve a confirmar reenvíos.

UDP inicial: envío una vez, sin retransmisión/ACK de datos; registrar pérdidas,
duplicados y paquetes tardíos. Expirar bloques incompletos a los 2 s; máximo
16 pendientes. La confiabilidad adicional sería otra variante para Wireshark.

El transporte TCP implementado retiene la propiedad hasta ACK. La salida
alternativa de consola libera el bloque al fallar, sin reintento.

## 7. Configuración y sesión

Cambios locales actuales: menuconfig → build → arranque; no mutación simultánea
mientras un arreglo se llena. La futura aplicación de comandos remotos será
STOP → cierre/drenaje o descarte explícito → CONFIG validada → ACK → START.
No mezclar fs, rango, intervalo o formato dentro de un bloque.

Al implementar sesiones finitas con tasas distintas, se propone duración común
más conteos XYZ efectivos por flujo; si el profesor exige N muestras, definir N
por flujo y condición de término. El enunciado dice cantidad de envíos/muestras:
confirmar la interpretación antes de finalizar ese control, sin confundir N
con fragmentos de red. RF2 exige modo secuencial adicional para comparar.

Raspberry debe agregar UTC de recepción, configuración/filtro/mode de sesión y
estadísticas al guardar en sensores/. Un timestamp de llegada de red no sustituye
al de adquisición. Cambios de transporte deben ser seleccionables desde GUI sin
reescribir código; esa parte remota sigue pendiente.

## 8. Evidencia y límites

Pruebas host y codecs: 1/5 s, tasas distintas, límites de RAM, signo/XYZ, FIFO,
frames parciales, conversión, CRC, máximos de serialización, reordenamiento,
duplicados y expiración. Compilación ESP-IDF y flasheo verificados. Recepción
real TCP binaria por WiFi, más de 160 ventanas consecutivas de cada flujo sin
descartes de salida/skips FIFO. Ver ENVIO_REAL.md y verificacion_envio_real.json.
AP y pruebas de cortes prolongados siguen pendientes.

Referencias: enunciado local Tarea1_TIC3.pdf;
[Bosch BMI270](https://www.bosch-sensortec.com/en/products/motion-sensors/imus/bmi270);
[datasheet FIFO](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmi270-ds000.pdf);
[ESP-IDF sockets](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/lwip.html);
[ESP-IDF memoria](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/system/mem_alloc.html).
Capacidades, puertos y plazos son decisiones del proyecto, no garantías del montaje.
