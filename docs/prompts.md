# Registro de prompts

## 2026-10-07 — Arreglos independientes y adquisición BMI270

Herramienta: Codex (GPT-6).

Prompt original, sin corregir:

```text
Me parece que lo mejor es almacenar y enviar arreglos por separado, es decir, las aceleraciones en un arreglo considerando sus muestras, correspondientes a la frecuencia de muestreo definida para ese parámetro, en otro arreglo la velocidad angular y así con todos los parámetros que se midan, considerando que si hacemos el envío se hace cada 1 segundo y medimos a 400 de frecuencia,para el arreglo de aceleraciones que se enviará a la raspi, se tengan 1200 muestras, considerando que las aceleraciones en los tres ejes. Te doy el permiso para modificar mi código según lo que te estoy pidiendo.
```

Resultado: firmware modificado con bloques separados por parámetro, ODR independientes,
ventanas de un segundo, lectura FIFO en core 1 y salida en core 0. Se aclaró que
400 muestras XYZ equivalen a 1200 valores escalares. La salida inicial es consola
(resumen o JSON), con punto de extensión para transporte WiFi. No se flasheó la
placa. Se agregaron pruebas de parser, almacenamiento y conversión; la adquisición
real y la integración con Raspberry requieren validación posterior.

Este registro cubre este prompt de implementación. Los prompts anteriores y los
usados fuera de este chat también deben agregarse para completar el entregable.

## 2026-10-07 — Intervalo variable, formatos y contrato de conexión

Herramienta: Codex (GPT-6). Mensajes originales consecutivos:

```text
¿Entonces ahora la esp arma arreglos separados por el tipo de dato, los cuales tienen distintas muestras o frecuencias de muestreo, y luego el envío de los arreglos a la raspi (que se hace enviando en binario) cada 1 segundo? ¿Indicaste que decisión toma el código si es que la conexión falla, o si verifica el puerto antes de conectarse o cuidaste lo que pasa si cambio la frecuencia de envío de datos de 1 segundo a 5 por ejemplo u otro valor cualquiera, para que no se desconfigure? Todo ese tipo de decisiones tienen que estar establecidas. Explícame después lo que se definió y las decisiones que se tomaron y por que
```

```text
La conexión es aún una tarea pendiente, estoy haciendo el código de servidor en la raspberry pi para hacer eso
```

```text
El envío hacia la raspberry puede ser mediante binario o json
```

Resultado: intervalo local configurable, capacidad dinámica con validación de
presupuesto/heap y margen, codec binario y JSON y helper de decodificación para
integrar al servidor. Políticas de puerto, handshake, reconexión, ACK/reintentos
y cambios de configuración especificadas en PROTOCOLO_DATOS.md. La conexión
se mantiene pendiente, respetando la aclaración del usuario. Pruebas de C→Python
para ambos formatos y ventanas de 1/5 s; no se flasheó la placa.

## 2026-10-07 — SSH y prueba TCP PC/Raspberry

Herramienta: Codex (GPT-6). Prompt original:

```text
Hazlo entonces, dime lo que debo hacer en mi raspi para establecer una conexión ssh y poder hacer la prueba
```

Resultado: ejemplos TCP adaptados con IP/puerto configurables, framing,
conexión persistente y errores por cliente; helper y prueba local de sockets.
Guía de habilitación SSH y copia/ejecución en Raspberry. SSH al dispositivo real
pendiente de habilitación y datos del usuario/IP. Esto es una prueba de texto
de conectividad, no el servidor IMU final.

## 2026-10-07 — Conexión SSH y prueba real autorizadas

Herramienta: Codex (GPT-6). Mensajes originales:

```text
Haz tú la conexión, acá están los datos de mi raspi
Usuario:raccoon
ip:192.0.2.10
```

```text
Listo, me dice esto: Now try logging into the machine, with: "ssh -i ~/.ssh/tic3_raspi 'usuario@192.0.2.10'"
and check to make sure that only the key(s) you wanted were added.
```

Resultado: usuario autorizó la clave dedicada mediante ssh-copy-id en su
terminal. Acceso SSH verificado; scripts copiados y servidor de prueba
arrancado en segundo plano en Raspberry (TCP 5001). Cliente en PC confirmó
tres mensajes y el log de Raspberry los registró. Servidor de prueba quedó
activo; no se instaló un servicio de arranque ni el servidor IMU final.

## 2026-10-07 — Reconexión en nueva red y resumen previo al envío ESP

Herramienta: Codex (GPT-6). Prompt original:

```text
Mi raspi al ejecutar hostname -I me dice que la ip es `192.0.2.10`, conéctate tú por ssh con la raspi, sin embargo, dime como podría yo hacer de forma manual lo mismo que harás tú para probar el servidor. Deja la conexión hecha y dime todos los parámetros, decisiones lógicas, configuraciones y demás que establecimos antes de iniciar el envío de datos de la esp hacia la raspi
```

Resultado: SSH con clave verificado en nueva IP; servidor de prueba reiniciado
en TCP 5001 libre y tres mensajes/ACK comprobados en PC y log remoto. Se dejó
sesión SSH de trabajo abierta y servidor en segundo plano. Guía actualizada y
snapshot ESTADO_ANTES_ENVIO_ESP.md con parámetros reales, políticas implementadas
y políticas de conexión IMU todavía pendientes. No se modificó ni flasheó ESP.

## 2026-10-07 — WiFi por prioridades y explicación de AP

Herramienta: Codex (GPT-6). Prompts originales:

```text
¿Qué wifise usará entonces, el que emite la raspi o al cual están conectados ambos dispositivos? En caso de que se pueda usar cualquiera sería bueno que hicieras una lista de wifi a los que el cliente puede conectarse, estableciendo prioridades, por ejemplo el de la misma raspi si es detectado prioridad 1, si no el wifi llamado lab sistemas de comunicaciones y así
```

Respuesta a aclaración:

```text
Está bien, pero el lab de sistemad de comunicaciones no se llama exactamente así, tendría que buscar el ssid cuando esté allá, pon también en la lista al wifi actual que estoy conectado
```

```text
¿Cómo funciona el wifi que emite la raspi? ¿Se debe activar y funciona como cualquier otra red wifi? ¿Es este lo suficientemente bueno o estable para lo que requiero?
```

Resultado: gestor WiFi de perfiles y pruebas agregados. TIC3-Raccoon prioridad 1,
laboratorio pendiente/deshabilitado prioridad 2 y SSID verificado WIFI_COMPARTIDO
prioridad 3. Claves pendientes de introducción local. Modelo/soporte AP de
Raspberry verificados sin activar hotspot ni cambiar red. Tamaños de tráfico
calculados de fixtures, sin afirmar rendimiento medido. No se flasheó ESP.


## GUI configurable y definición de cores

**Herramienta/modelo:** Codex; modelo exacto no expuesto en esta sesión.

**Prompt del usuario:**

> Recuerda que la idea es que la GUI muestre los datos en tiempo real, incluyendo los valores de aceleración y otros, además el código en la raspi así como la GUI deben tener la opción para cambiar cosas como la frecuencia de muestreo,cantidad  de muestras, envío secuencial o paralelo, elegir el protocolo de conexión tcp o udp

**Aclaración del usuario:**

> Secuencial y paralelo se refiere a hacer todo en un solo core o dividirse en dos, secuencial sería en un solo core procesar los datos del sensor hacer los arreglos y enviarlos, en paralelo se dividirían las funciones de los arreglos y el envío entre los dos cores

**Red del usuario durante esta etapa:**

> Desactivé momentáneamente el hotspot de la raspi, lo activaré de nuevo cuando tenga que conectarme a la raspi con ese método

**Resultado:** CONFIG binario validado/confirmado, objetivos independientes con
parada automática, modo secuencial en core1 y paralelo core1/core0, TCP/UDP y
binario/JSON elegibles en GUI. Previsualización hasta10Hz y página cada200ms,
valores XYZ SI y gráficas. Se ajustó el código tras detectar repetición indebida
al terminar por cantidad y valores antiguos tras cambio de rango. Las pruebas
fallidas se conservaron marcadas; las cuatro combinaciones reales posteriores
guardaron objetivos exactos, sin descartes reportados. AP permanece disponible,
pruebas por WiFi compartido. Las claves no se transcriben a este registro.


## Diagnóstico de conexión ESP al AP

**Prompt del usuario:**

> Toma control de mi pc y hazlo tú

**Herramienta/modelo:** Codex; modelo exacto no expuesto en esta sesión.

**Resultado:** Se deshabilitaron temporalmente los perfiles2/3, se compiló y
se grabó la ESP. El monitor identificó incompatibilidad de seguridad WPA/WPA2,
no un problema de prioridad. Se corrigió setup_ap.py para fijar RSN/CCMP y se
copió a Raspberry. La modificación remota del perfil fue rechazada por
NetworkManager (Insufficient privileges), por lo que se solicitó ejecución
local con sudo. También se encontró el servidor apagado; se arrancó en segundo
plano, independiente de SSH, y se verificó GUI HTTP200 y puertos5000/5001/8080.
Las contraseñas no se transcriben al registro.


**Cierre del diagnóstico AP:** Usuario aplicó el script WPA2/AES con sudo local.
Se comprobó que la clave del firmware difería de la del archivo privado del AP;
se sincronizó sin mostrarla, se recompiló y grabó. ESP obtuvo 192.0.2.30,
confirmó HELLO a192.168.50.1 y guardó 250/125 XYZ en una sesión real TCP binaria.
Cero errores/descartes reportados. Se restauraron parámetros de muestreo y el
WiFi original del PC; la ESP quedó detenida, temporalmente con solo AP habilitado.
