# WiFi por prioridades y AP de Raspberry

> Diagnóstico AP resuelto: firmware con perfil1 habilitado y perfiles2/3
> temporalmente deshabilitados para esta prueba. AP configurado con RSN/CCMP
> (WPA2/AES), clave de ESP sincronizada con el archivo privado del AP de Raspberry.
> Servidor y GUI activos en segundo plano, independientes de SSH.
> ESP obtuvo 192.0.2.30 y confirmó HELLO con 192.168.50.1:5001.
> Prueba real: 250 XYZ de aceleración y 125 XYZ de giro guardadas, cero descartes
> y errores reportados. Evidencia: [ap_connection_verified.json](../raspi/ap_connection_verified.json).
> Mesh sigue deshabilitado temporalmente; reactivarlo requiere menuconfig/build/flash.



> Estado vigente: GUI con XYZ en vivo y configuración remota de ODR, rangos,
> objetivos por flujo, ventana, modo por core, TCP/UDP y binario/JSON.
> La prueba actual usa WiFi compartido; AP desactivado temporalmente por usuario.
> Manual y evidencia actualizados en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las notas inferiores conservan etapas previas de desarrollo.

> Actualización GUI/control: START/STOP y GUI web implementados. La ESP ahora
> espera START al encender; STOP cierra y guarda bloques parciales antes de
> confirmar. Estado y uso vigentes en [GUI_CONTROL_AP.md](../raspi/GUI_CONTROL_AP.md).
> Las secciones anteriores a esta actualización describen la etapa de envío continuo.

Estado: WiFi habilitado, firmware flasheado y envío TCP binario real verificado
por WIFI_COMPARTIDO. Servidor IMU activo en Raspberry. El script de AP está
preparado; su ejecución con sudo y activación siguen pendientes.
Guía vigente: [ENVIO_REAL.md](../raspi/ENVIO_REAL.md).

## Perfiles

| Prioridad | SSID | Servidor en esa red | Estado |
|---|---|---|---|
| 1 | TIC3-Raccoon | 192.168.50.1:5001 | Credenciales locales preparadas; AP pendiente de activar |
| 2 | Pendiente de SSID exacto del laboratorio | IP/nombre en laboratorio:5001 | Deshabilitado |
| 3 | WIFI_COMPARTIDO | 192.0.2.10:5001 | Conectado; envío real verificado |

El AP de Raspberry es el exigido por Tarea 1. Los demás sirven para desarrollo
y respaldo si la red permite comunicar clientes. Aislamiento de laboratorio
puede impedir alcanzar Raspberry aunque ambos tengan WiFi.

SSID actual verificado en PC (5805 MHz) y Raspberry (2412 MHz), mismo nombre.
ESP32-S3 utiliza 2,4 GHz y podrá encontrar la variante de esa banda. La clave proporcionada por el usuario quedó únicamente en configuración local. Los SSID distinguen mayúsculas y espacios, máximo 32 bytes.

## Comportamiento implementado

1. Validar perfiles habilitados con SSID, clave y host/puerto completos. Red
   abierta exige clave vacía; PSK WPA2/WPA3 admite 8…63 caracteres. Omitir perfiles
   incompletos/inválidos e informar sin imprimir claves.
2. Escanear SSID conocidos y elegir el visible de mayor prioridad que no esté
   en cooldown. La señal no cambia el orden de perfiles; dentro del mismo SSID
   el driver busca AP por señal.
3. Dos intentos por perfil detectado, máximo 15 s cada uno para autenticación y
   DHCP. Al fallar, cooldown de 60 s para ese perfil y probar el siguiente, para
   evitar que un AP visible con clave incorrecta bloquee todo el fallback.
4. Si ninguno funciona, repetir ronda con backoff 1/2/4/8/16/30 s y jitter,
   máximo 30 s. Rehabilitar el perfil fallido al cumplirse su cooldown.
5. Mantener una red sana: no interrumpirla solo porque reaparezca el AP preferido.
   Tras perder WiFi/IP o solicitar rescan entre sesiones, empezar por prioridades.
6. Publicar perfil, IP, endpoint, motivo de desconexión y generación. Una nueva
   generación obliga al cliente socket a invalidar conexiones anteriores.
7. La capa de socket reporta éxito/fallo de conexión/HELLO asociado a la
   generación. Tres fallos reportados consecutivos provocan cooldown del perfil
   actual y selección de otra red. Ignorar reportes de una generación antigua.

WiFi ready significa autenticación y DHCP, no servidor confirmado ni datos
guardados. El cliente socket verifica HELLO/HELLO_ACK antes de iniciar adquisición. No se necesitan teclado ni
contraseña manual para reconectar cuando las credenciales guardadas son válidas.

Gestor: core 0, prioridad 3, stack 4096 bytes, cola de ocho eventos; no lee I2C
ni bloquea la tarea de adquisición. WiFi inicia antes de reservar los pools
para incluir su consumo en la validación de RAM. Si no se pudo iniciar, se
informa y queda disponible adquisición por consola. No se borra NVS para ocultar
un error; el driver guarda configuración WiFi en RAM.

## Configuración local

Desde sample_project: `idf.py -B build-tic3 menuconfig`, menú **TIC3 - WiFi por
prioridad**. Activar gestor y completar perfiles/clave/endpoint. Mantener lab
deshabilitado hasta conocer su nombre. Después compilar y flashear para aplicar cambios. La clave actual está configurada
localmente; no debe copiarse a documentación ni archivos versionados.

Las claves locales quedan en sdkconfig y firmware; sdkconfig/build se excluyen de
Git. No incluirlas en sdkconfig.defaults o prompts. Esta versión no implementa
802.1X enterprise: si laboratorio exige usuario/certificado, adaptar ese perfil.

Cada perfil tiene su endpoint. Una IP de casa no vale necesariamente en lab.
Si Raspberry cambia IP DHCP, actualizar ese campo, reservar dirección en router
o usar un nombre resoluble al integrar sockets. No hay todavía cambios remotos
de claves ni de lista mientras la ESP está adquiriendo.

## AP de Raspberry

Funciona como una red WiFi local: anuncia SSID, permite autenticación con clave
y ofrece una red a ESP/PC. NetworkManager permite crear y activar hotspot y
configurar DHCP/red local; puede prepararse autoconexión al arrancar. Internet
no es necesario para enviar a Raspberry. Para dar Internet a clientes hace
falta una salida compartida, por ejemplo Ethernet.

Equipo verificado: Raspberry Pi 5 Model B Rev 1.1; WiFi-PROPERTIES.AP=yes.
Plan: TIC3-Raccoon en 2,4 GHz, clave local preparada, IP AP estable 192.168.50.1/24 si
no hay conflicto; ESP por DHCP. Script `raspi/setup_ap.py` preparado; no ejecutado con sudo ni activado todavía.

La interfaz wlan0 sigue conectada a WIFI_COMPARTIDO. La configuración normal
de hotspot sustituye el uso cliente de esa interfaz. Antes de activarla preparar
Ethernet o conectar PC al nuevo AP: SSH por 192.0.2.10 puede cortarse y cambiar
su IP. No se alteró la red de Raspberry durante esta consulta.

## Capacidad y estabilidad

Fixtures reales del codec para dos flujos de 400 XYZ/s y ventanas de 1 s:

- Binario: 12400 bytes/s, aproximadamente 0,099 Mbit/s.
- JSON: 92011 bytes/s, aproximadamente 0,736 Mbit/s.

Incluyen framing y metadatos de aplicación; no incluyen overhead TCP/IP, WiFi,
ACK o retransmisiones. Son tamaños calculados, no throughput medido de la red.
Por volumen hay margen razonable para un AP local, pero estabilidad depende de
señal, interferencia, alimentación y servidor. Confirmar con datos reales,
contadores de pérdidas, latencia, memoria y recuperación de desconexión.

## Archivos y pruebas

- main/Kconfig.wifi: SSID, claves locales, endpoint y timeout.
- main/wifi_policy.h/.c: validación, prioridad, cooldown, backoff.
- main/imu_wifi.h/.c: scan, asociación, DHCP, estado y recuperación.
- API: imu_wifi_get_status, imu_wifi_report_server_result, request_rescan.
- tests/test_wifi_policy.c: prioridad 1→2→3, lab omitido, perfiles inválidos,
  SSID de 32 bytes, cooldown para no bloquear fallback y backoff acotado.

Compilación completa con gestor deshabilitado, compilación de rama habilitada y
pruebas host pasaron. Asociación real y recuperación en placa no verificadas.

Referencias: [NetworkManager hotspot](https://networkmanager.pages.freedesktop.org/NetworkManager/NetworkManager/nmcli.html),
[ESP-IDF WiFi](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-reference/network/esp_wifi.html),
[ESP32-S3 2,4 GHz](https://www.espressif.com/en/products/socs/esp32-s3).
