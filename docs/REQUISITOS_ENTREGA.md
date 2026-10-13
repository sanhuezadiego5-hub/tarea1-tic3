# Requisitos y estado de entrega

Revisión: 13 de octubre de 2026. Fuente: código y sesiones del repositorio.
Este archivo es documentación de entrega; no sustituye el informe corto.

| Requisito | Implementación / evidencia | Estado |
|---|---|---|
| Comparación TCP/UDP observable | Puerto datos 5001 TCP/UDP, control TCP 5000 | Implementado; captura/análisis Wireshark a cargo del grupo |
| Capas modulares y comentadas | Sensor `bmi270_sensor`, adquisición `imu_acquisition`, red `imu_wifi`/`imu_transport`, GUI Python/HTML/Tkinter | Implementado |
| GUI para usuario sin programación | Lanzador Tkinter + acceso de escritorio + GUI web local sin Internet | Implementado; requiere instalar Tkinter una vez |
| Cliente desconectado sin caer servidor | Hilo por conexión, límites de clientes, cierre de cliente sin cerrar listeners | Prueba de EOF/RST/mensaje inválido y reconexión incluida |
| Tiempo/configuración en datos | `read_us`, `received_utc`, `mode`, `sample_hz`, `range`, `scale_to_si`, `configuration` | Verificado en los 44 bloques incluidos |
| Nombre con apellidos | Renombrado en GitHub y ajuste de remote | Lo hará el usuario |
| Firmware secuencial/paralelo | Selección remota, core 1 adquisición, core 0 envío paralelo | Implementado y compilado; nuevo reparto de archivos no flasheado aún |
| Servidor + GUI Raspberry | Carpeta `raspi/` | Incluido |
| `sensores/` con >=3 sesiones variadas | Cinco sesiones, distintos Hz, rangos y objetivos; manifiestos en `sessions/` | Incluido; cantidades coherentes con bloques |
| README instalación/ejecución | README raíz y documentación por módulo | Actualizado |
| `prompts.md` raíz con etapa/texto/modelo/resultado/uso | 58 prompts y aclaraciones del historial original; modelos de metadatos | Incluido, salvo una credencial omitida explícitamente |
| Capturas Wireshark y análisis | Carpeta `wireshark/` con instrucciones | Pendiente del grupo |
| Informe <=5–6 páginas | Carpeta `informe/` con alcance | Pendiente del grupo |

## Contratos de las capas

- **Sensor:** owns I2C, chip ID, imagen de inicialización Bosch, registros de ODR/rangos, lectura FIFO y verificación por lectura de retorno. No usa sockets, WiFi ni GUI. Los pines y dirección I2C están en `bmi270_sensor.c`.
- **Adquisición:** es dueña de la configuración runtime, arrays, tiempos de ventana y colas. Solo su tarea modifica el FIFO durante captura. Reserva pools antes de aplicar cambios y restaura configuración anterior si falla una modificación del sensor.
- **Red:** elige perfiles, reconecta y codifica fragmentos. Recibe bloques inmutables de adquisición. TCP confirma guardado y reenvía; UDP no confirma datos. El control siempre usa TCP.
- **Servidor/control:** valida y reconstruye, guarda atómicamente, deduplica, controla sesiones y aplica configuración confirmada por ESP.
- **GUI:** recibe previsualización de memoria del servidor mediante HTTP; no reconstruye datos del sensor ni relee continuamente archivos de ventanas. La previsualización proviene del canal TCP de control. Los contadores reflejan guardados completos.

## Uso sin programación

Preparación única en Raspberry con escritorio:

```bash
sudo apt install python3-tk
bash scripts/instalar_acceso_gui.sh
```

En adelante: menú de aplicaciones → TIC3 - Captura BMI270 → Iniciar y abrir
interfaz → esperar ESP → configurar → Aplicar → esperar configuración efectiva
→ Iniciar captura → Detener y guardar → esperar confirmación → Cerrar servidor.
El lanzador solo administra su propio proceso, no termina otros servidores.
Los fallos de arranque quedan en `servidor.log` de la carpeta seleccionada.
Los puertos deben coincidir con los configurados en firmware.

## Semántica de timestamps y sensibilidad

Cada XYZ tiene secuencia y tiempo monotónico de **lectura** FIFO (`read_us`).
Varias muestras leídas juntas pueden compartir ese tiempo; no es un timestamp
hardware individual ni UTC sincronizado. `received_utc` es la fecha de recepción
del bloque en Raspberry. No se afirma sincronización de relojes.

Sensibilidad digital: 32768/rango LSB/g (acelerómetro) o LSB/(°/s) (giro).
`scale_to_si` guarda el factor por LSB: rango*9.80665/32768 para aceleración y
rango*pi/(180*32768) para giro. No se necesitan valores físicos duplicados por
muestra para conservar la sensibilidad. Configuración y escala se comparten
por bloque y sus muestras se interpretan con esos metadatos.

## Límites reales

- Dos cores no garantizan cero pérdidas; buffers finitos y errores FIFO/I2C se contabilizan. Secuencial pausa FIFO durante el envío.
- TCP no recupera muestras descartadas antes del envío. Un bloque sin ACK puede reintentarse indefinidamente mientras vive en RAM; cortar energía lo pierde.
- UDP incompleto expira; no se pide recuperación. Previsualización viva no demuestra entrega completa de arrays.
- La tolerancia probada es a desconexiones/mensajes inválidos de clientes; no es garantía frente a disco lleno, fallo de hardware o corte eléctrico.
- El lanzador se prueba a nivel de ciclo de proceso/HTTP y guardia de cierre sin pantalla. La ventana Tkinter debe comprobarse visualmente en Raspberry; en el PC de desarrollo no está instalado Tkinter.
- Cinco sesiones variadas no aíslan causalmente cada parámetro. El informe debe reconocerlo o usar pruebas controladas adicionales.
- El registro público conserva textos originales salvo una contraseña. No se presentan las versiones antiguas editadas como originales; no se inventan conversaciones de otros integrantes.

## Verificación reproducible

```bash
bash scripts/verificar.sh
```

Incluye fixtures C→Python, parser/CRC, formatos, configuración, sockets, ACK,
deduplicación, reconexión, desconexión abrupta, sesiones entregadas y ciclo del
lanzador sin pantalla. Para firmware, activar ESP-IDF y ejecutar en `esp/`:

```bash
idf.py -B build-tic3 build
```

Compilación probada en la instalación original `sample_project/` con los mismos
fuentes C. La configuración privada no se publica. El grupo debe volver a grabar
si quiere ejecutar esta versión modular en la placa.
