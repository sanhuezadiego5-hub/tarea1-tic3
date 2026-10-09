# GUI, configuración remota y captura BMI270

> Actualización AP: conexión real y guardado verificados en TIC3-Raccoon.
> Se corrigieron seguridad WPA→WPA2/AES y diferencia de claves; servidor iniciado
> independiente de SSH. ESP temporalmente solo AP (Mesh deshabilitado).
> En esta prueba se guardaron 250/125 XYZ sin errores ni descartes.
> Ver ap_connection_verified.json. Las notas anteriores a esta actualización
> conservan el desarrollo y las pruebas por WiFi compartido.

La GUI vive en la Raspberry y se abre en `http://localhost:8080` desde ella o
`http://192.0.2.10:8080` desde el PC en WIFI_COMPARTIDO. El AP TIC3-Raccoon
quedó preparado y fue activado por el usuario, quien lo desactivó temporalmente.
Se mantiene como alternativa; no se reactiva sin una nueva indicación.

## Valores en tiempo real

La página muestra aceleración XYZ en m/s² y velocidad angular XYZ en rad/s,
valores numéricos, gráficas y hora de la última actualización. La ESP publica
una previsualización de los valores más recientes hasta 10 veces por segundo
por el canal de control; la página consulta cada 200 ms. La curva conserva hasta
200 puntos de esa previsualización y no sustituye a las muestras completas.
En secuencial, los valores pueden permanecer sin actualizar durante el envío:
no hay una tarea concurrente de lectura en ese modo y no se inventan muestras.

Los arreglos completos se envían aparte, con la frecuencia de cierre de ventana
seleccionada. Los archivos contienen todas las muestras recibidas, sus cuentas
raw, secuencias, tiempos de lectura y escala SI. El instante es de lectura FIFO,
no un timestamp exacto de conversión; UTC de recepción se guarda por separado.

## Configurar desde la GUI

| Campo | Valores / significado |
|---|---|
| Frecuencias independientes | Aceleración y giro: 25, 50, 100, 200 o 400 Hz |
| Rango de aceleración | ±2, ±4, ±8 o ±16 g |
| Rango de giro | ±125, ±250, ±500, ±1000 o ±2000 °/s |
| Ventana | 100…60000 ms, limitada por RAM; controla cierre de cada arreglo |
| Muestras XYZ a adquirir | Un objetivo por sensor, 0…10000000; XYZ cuenta como una muestra |
| Captura continua | Objetivo 0; si un flujo es continuo la sesión no termina automáticamente |
| Fin por cantidad | Ambos objetivos mayores que cero: detener después de adquirir ambos |
| Secuencial | Core1: leer sensor, armar arreglos y enviarlos de forma bloqueante |
| Paralelo | Core1: leer/armar; core0: enviar bloques inmutables mediante colas |
| Protocolo | TCP con ACK de guardado o UDP sin ACK/reenvío de datos |
| Formato | Binario o JSON; independiente de elegir TCP/UDP |
| Buffers | Se conservan 2 por sensor; el aumento a 4 no fue aprobado |

WiFi y control remoto siguen ejecutando las tareas del sistema necesarias;
secuencial se refiere a las funciones de adquisición, formación y envío de
arreglos en una sola tarea/core, no a desactivar físicamente el otro core.

La cantidad de la sesión y el tamaño de un arreglo son distintos. Por ejemplo,
100 Hz, ventana 500 ms y objetivo 250 XYZ produce nominalmente cinco arreglos de
50 XYZ; el último puede ser parcial. No reserva RAM para todas las 250 muestras
juntas. Los conteos reales dependen del reloj del sensor; no se rellena un array.
Se adquiere el objetivo exacto; descartes por buffers o pérdida UDP pueden hacer
que se guarde una cantidad menor. La GUI distingue adquiridas de guardadas.

Para cambiar parámetros:

1. **Detener y guardar**, y esperar la confirmación.
2. Editar campos y pulsar **Aplicar configuración**.
3. Esperar la configuración efectiva de ESP que aparece debajo del formulario.
4. **Iniciar captura**. No hace falta recompilar ni grabar de nuevo.

Cambios durante captura/parada pendiente se rechazan. CONFIG se envía por TCP5000
con CRC e identidad; solo se informa éxito después de validación de capacidad,
reserva de memoria, configuración del BMI270 y lectura de sus registros. Si no
cabe en RAM, la GUI informa error y conserva la configuración anterior. Si falla
el sensor, se intenta restaurar sus ajustes; si eso también falla, queda en fallo
hasta revisar/reiniciar la ESP. No existe una aplicación parcial silenciosa.

La reserva de nuevos buffers se realiza antes de liberar los anteriores para
hacer el cambio reversible; por eso puede rechazarse una combinación que entra
en 128 KiB de arrays si falta heap para la transición y el margen de 64 KiB.
Cinco segundos a 400 Hz y dos buffers requieren 118048 B de arrays; que entren en
el presupuesto no garantiza que haya memoria libre suficiente en ese instante.

## Sesiones y errores

Cada START nuevo crea una sesión. START repetido no vuelve a empezar una sesión
que ya terminó automáticamente. Una reconexión dentro de la misma sesión conserva
su contador; un START nuevo lo reinicia. TCP es el protocolo al arrancar; la
configuración remota queda en RAM de ESP y se pierde al reiniciar la placa.

STOP desactiva FIFO, drena lo ya capturado, cierra un bloque parcial por flujo y
espera resolver los bloques pendientes. TCP conserva cada bloque hasta ACK de
persistencia; la GUI no anuncia guardado mientras falte esa confirmación. En UDP
la parada confirma el fin de adquisición/envío, no entrega durable de todos los
datagramas. Los estados lo indican explícitamente, aunque una prueba concreta
haya recibido todas las muestras UDP.

Si se corta el control, la ESP solicita parada local. La GUI muestra desconexión
sin asumir cierre. TCP reintenta y conserva bloques pendientes; puede esperar
indefinidamente si el servidor no vuelve. Si se reinicia el servidor, las sesiones
sin cierre confirmado quedan marcadas interrumpidas. ESP anuncia su configuración
efectiva de nuevo al reconectar; el servidor parte solicitando parada, no captura.
Los buffers finitos siguen pudiendo causar descartes durante demoras de salida.

Los archivos se guardan durante la sesión, no todos al final:

```text
~/tic3/sensores/<device_boot>/acceleration/window-*.json
~/tic3/sensores/<device_boot>/angular_velocity/window-*.json
~/tic3/sensores/sessions/<session_id>.json
```

Cada bloque nuevo incluye session_id, modo real, transporte, formato y
configuración. Los manifiestos registran objetivos, conteos, tiempos y rango de
ventanas, para distinguir sesiones aunque compartan arranque de ESP. Los bloques
UDP tardíos se vinculan mediante los rangos de ventanas de sus sesiones.

## Ejecución manual

Servidor y GUI juntos, cuando no haya otra copia escuchando:

```bash
cd ~/tic3
python3 -u raspi/imu_server.py --output sensores --gui-port 8080
```

La copia desplegada quedó en segundo plano, independiente de SSH. No se instaló
un servicio de arranque automático. TCP/UDP datos5001; control TCP5000; GUI8080.
La interfaz es para la red local; no se configuraron autenticación/TLS ni
redirecciones de router. POST usa cabecera y verificación de origen contra CSRF.

Firmware ya grabado. Para posteriores cambios de código desde PC:

```bash
source /ruta/a/.espressif/tools/activate_idf_v6.1.sh
cd /ruta/al/repositorio/esp
idf.py -B build-tic3 build
idf.py -B build-tic3 -p /dev/ttyACM0 flash monitor
```

## AP disponible cuando se necesite

SSID TIC3-Raccoon, banda2,4 GHz, canal1, Raspberry192.168.50.1/24, DHCP por
NetworkManager. Credenciales locales ya coinciden con el perfil1 de la ESP.
Para activarlo desde una terminal de Raspberry:

```bash
sudo python3 ~/tic3/raspi/setup_ap.py --activate
```

Puede cortar SSH por la IP antigua. Conectar PC al AP y usar
`ssh -i ~/.ssh/tic3_raspi usuario@192.168.50.1`; GUI
`http://192.168.50.1:8080`. Si no hay salida Internet en Raspberry, el PC al
conectarse a ese AP puede perder Internet/chat. Puede usarse la GUI local de
Raspberry manteniendo PC en su red original. No se necesita Internet para medir.
Una red sana no se abandona solo por aparecer el AP; reiniciar ESP para elegir
perfil1 al cambiar a esa topología. El AP no se activa automáticamente al arrancar.

## Pruebas y archivos

Doce pruebas Python correctas, fixture CONFIG C→Python verificado con sanitizers,
compilación ESP-IDF y grabación correctas. Pruebas reales por WiFi compartido:

| Modo | Protocolo/formato | Hz acc / giro | Rango acc / giro | Objetivos XYZ | XYZ guardadas |
|---|---|---|---|---|---|
| Paralelo | TCP binario | 100 / 50 | ±4 g / ±500 °/s | 250 / 125 | 250 / 125 |
| Secuencial | UDP JSON | 200 / 100 | ±2 g / ±250 °/s | 300 / 150 | 300 / 150 |
| Secuencial | TCP binario | 50 / 25 | ±16 g / ±1000 °/s | 120 / 60 | 120 / 60 |
| Paralelo | UDP binario | 100 / 100 | ±8 g / ±2000 °/s | 50 / 50 | 50 / 50 |

También se aplicó configuración desde la propia GUI y se comprobaron valores
XYZ, gráficas y parada automática en una sesión secuencial TCP JSON de 1000/500
muestras, todas guardadas. Cero descartes/errores reportados en estas pruebas;
no constituye una garantía para todas las condiciones de red.

Los datos de estas sesiones se copiaron a `../sensores/`. Resumen de validación:
`runtime_hardware_check.json`; captura: `gui_configuracion_tiempo_real.jpg`.
Dos pruebas anteriores al arreglo de parada automática se conservan en Raspberry
marcadas no válidas; no se presentan como evidencia de funcionamiento correcto.

Código principal: `imu_settings.py` valida/serializa CONFIG; `imu_control.py`
gestiona estados y previsualización; `imu_server.py` recibe/persiste;
`imu_gui.py` y `imu_gui.html` sirven la interfaz. ESP: `imu_config.c` define
configuración dinámica, `main.c` configura/adquiere y `imu_transport.c` conecta,
controla y envía. `check_runtime_hardware.py` ejecuta pruebas con una sola ESP,
con captura detenida, y restaura ajustes al terminar.

## Mejoras pendientes de aprobación

Aumentar buffers para ventanas de 1 s, arranque automático del servidor/AP,
autenticación, exportación CSV, almacenamiento ante cortes largos. Comparación
Wireshark y pruebas de estrés prolongado siguen pendientes; no se declara toda
la Tarea1 finalizada por estas sesiones de validación.
