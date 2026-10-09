# Tarea 1 — TIC 3

Adquisición con ESP32-S3 y BMI270, envío por WiFi y almacenamiento y visualización en Raspberry Pi. La GUI funciona en la red local, sin conexión a Internet.

## Organización

| Carpeta | Contenido |
|---|---|
| `esp/` | Proyecto ESP-IDF: firmware C, CMake, Kconfig, pruebas y configuración de VS Code y Dev Container. |
| `raspi/` | Servidor Python, GUI web, control remoto, configuración del AP, pruebas y reportes de verificación. |
| `docs/` | Diseño, protocolo, decisiones WiFi y registro de desarrollo. |
| `datos_ejemplo/` | Capturas reales y manifiestos de sesiones conservados como ejemplos. |
| `referencias/` | Enunciado, material ESP-IDF y demo original del BMI270. |
| `ejemplos_tcp/` | Cliente y servidor TCP iniciales; el sistema actual utiliza `raspi/imu_server.py`. |

Esta carpeta es una copia organizada del proyecto que se probó. Las notas históricas conservan rutas de la instalación original y algunas decisiones anteriores; este README describe cómo ejecutar esta distribución.

## Ejecutar el servidor y la GUI

Con Python 3 en la Raspberry, desde la raíz del repositorio:

```bash
python3 raspi/imu_server.py --host 0.0.0.0 --port 5001 --control-port 5000 --gui-port 8080 --output sensores
```

Abrir `http://localhost:8080` en la Raspberry o `http://IP_DE_LA_RASPBERRY:8080` en un PC de la misma red. El servidor usa la biblioteca estándar de Python. Iniciar la captura desde la GUI; **Detener y guardar** cierra la sesión y guarda los bloques pendientes. `Ctrl+C` termina el servidor en la terminal. Los archivos nuevos aparecen en `sensores/`, ignorado por Git; los ejemplos incluidos permanecen en `datos_ejemplo/`.

En VS Code se puede abrir la carpeta del repositorio en la Raspberry, o mediante Remote SSH, y ejecutar ese mismo comando en su terminal. El servidor debe estar activo antes de iniciar una captura en la ESP.

## Compilar y grabar la ESP

Instalar/activar ESP-IDF (la versión utilizada fue v6.1) y abrir `esp/` como carpeta de VS Code con la extensión ESP-IDF. Desde una terminal con ESP-IDF activado:

```bash
cd esp
idf.py -B build-tic3 set-target esp32s3
idf.py -B build-tic3 menuconfig
idf.py -B build-tic3 build
idf.py -B build-tic3 -p /dev/ttyACM0 flash monitor
```

Ajustar el puerto a la placa conectada. En **TIC3 - WiFi por prioridad**, habilitar los perfiles deseados y configurar SSID, contraseña, dirección del servidor y puerto 5001. Habilitar WiFi y envío a Raspberry en sus menús. Las credenciales se introducen localmente y quedan en `sdkconfig`, que no se publica. `sdkconfig.defaults` contiene valores iniciales sin contraseñas. El helper `esp/configurar_wifi_local.py` es específico del antiguo perfil Mesh 3; para otras redes utilizar `menuconfig`.

La ESP ejecuta firmware después del flasheo; no se ejecuta `main.c` como un programa del PC. Espera la orden de inicio de la GUI. Salir del monitor con `Ctrl+]` no detiene la placa.

## WiFi compartido o AP de la Raspberry

En una red común, ESP y Raspberry deben comunicarse dentro de esa red; el PC necesita acceso a la Raspberry para abrir la GUI. El cable USB no comparte automáticamente el WiFi del PC con la ESP.

Para preparar el AP en la Raspberry, copiar `raspi/.ap_credentials.example.json` a un archivo privado llamado `.ap_credentials.json`, cambiar su contraseña localmente y protegerlo con `chmod 600 .ap_credentials.json`. Después, desde la raíz del repositorio:

```bash
sudo python3 raspi/setup_ap.py --credentials "$PWD/.ap_credentials.json"
```

Añadir `--activate` desde una terminal local de la Raspberry o con acceso alternativo: activar el AP puede cortar la sesión SSH que utiliza su WiFi anterior. El AP utiliza WPA2/AES, 2,4 GHz, SSID `TIC3-Raccoon`, dirección `192.168.50.1/24` y DHCP. Configurar la misma contraseña en el perfil 1 de la ESP y servidor `192.168.50.1`. Conectar el PC al AP y abrir `http://192.168.50.1:8080`. No necesita Internet. El AP y el servidor no tienen inicio automático configurado por este repositorio.

## Decisiones de adquisición y envío

- Aceleración y velocidad angular tienen arreglos independientes y tasas de 25, 50, 100, 200 o 400 Hz. Una muestra XYZ contiene tres valores; 400 muestras XYZ equivalen a 1200 valores por sensor en una ventana nominal de 1 segundo.
- La GUI permite configurar tasas, rangos, ventana, cantidad objetivo, TCP/UDP, binario/JSON y modo de ejecución mientras la adquisición está detenida. La ESP valida memoria y configuración antes de aceptar cambios.
- Paralelo: adquisición y formación de arreglos en core 1, envío en core 0. Secuencial: adquisición, arreglos y envío bloqueante en core 1; las tareas del sistema siguen funcionando.
- TCP confirma el guardado mediante ACK y permite reintentos y deduplicación. UDP no confirma ni retransmite los datos. Los buffers son finitos y los descartes se contabilizan.
- La GUI muestra valores SI y previsualización; los archivos guardan las muestras completas recibidas. El timestamp del sensor representa la lectura de FIFO, no el instante físico exacto de conversión.

Detalles en [manual de GUI y control](raspi/GUI_CONTROL_AP.md), [protocolo](docs/PROTOCOLO_DATOS.md) y [diseño](docs/DISENO_ESP32_TAREA1.md). Los reportes de pruebas de hardware están en `raspi/*verified.json` y `raspi/runtime_hardware_check.json`; corresponden a sesiones distintas de los datos de ejemplo.

## Verificar sin hardware

```bash
bash scripts/verificar.sh
```

El script usa un compilador C (`cc`) y Python 3, genera fixtures temporales y comprueba interoperabilidad C→Python y sockets locales. Las pruebas de hardware requieren placa, sensor y Raspberry disponibles.

## Archivos generados y privados

Los directorios `build*`, el bootloader generado, CMakeFiles, cachés, `sdkconfig`, contraseñas, claves SSH y nuevas capturas no forman parte del repositorio. ESP-IDF vuelve a generar los archivos de compilación a partir de CMake y del código fuente. Se incluyen los archivos necesarios para reproducir el proyecto, no los binarios que contienen la configuración privada de la instalación.

Las direcciones `192.0.2.x`, el usuario `usuario` y las rutas `/ruta/...` son ejemplos para documentación; reemplazarlos por los valores locales. La subred `192.0.2.20/24` es la configuración del AP definida por el código. El historial publicado omite identificadores de red y rutas personales.
