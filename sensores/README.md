# Sesiones de prueba incluidas

Cinco sesiones reales, 44 bloques JSON, aceleración y giro en carpetas separadas.
Los manifiestos en `sessions/` vinculan configuración, bloques y cantidades.
Las cantidades XYZ son muestras, no valores de eje: cada muestra tiene X,Y,Z.

| Sesión (prefijo) | Hz acc/giro | Rangos ±g/±°/s | Ventana ms | Objetivos y recibidas acc/giro | Modo / protocolo / formato | Bloques |
|---|---:|---:|---:|---:|---|---:|
| `6f481649` | 100/50 | 4/500 | 500 | 250/125 | paralelo/TCP/binario | 10 |
| `70833adb` | 50/25 | 16/1000 | 1000 | 120/60 | secuencial/TCP/binario | 6 |
| `881331db` | 100/100 | 8/2000 | 1000 | 50/50 | paralelo/UDP/binario | 2 |
| `ec15ed5a` | 200/100 | 2/250 | 500 | 300/150 | secuencial/UDP/JSON | 6 |
| `f8b45ad9` | 100/50 | 4/500 | 1000 | 1000/500 | secuencial/TCP/JSON | 20 |

Las sesiones UDP indican `stopped_udp_unconfirmed`: recibir y verificar estos
archivos no convierte UDP en un protocolo con confirmación de entrega.

Cada bloque guarda `received_utc`, `read_us` por muestra, secuencias, modo,
frecuencia, rango, `scale_to_si`, unidad, errores y `configuration` completa.
Sensibilidad digital: aceleración = 32768/rango LSB/g; giro = 32768/rango
LSB/(°/s). `scale_to_si` expresa la conversión inversa en m/s² o rad/s por LSB.
`read_us` pertenece al reloj monotónico ESP y al instante de lectura FIFO;
no es una fecha UTC ni un timestamp de conversión exacto independiente.

Se verifica coherencia, no una causalidad aislada: las sesiones cambian varios
parámetros a la vez. El análisis del efecto de frecuencia/rango corresponde al
informe del grupo y debe considerar esta limitación.

Las capturas nuevas se ignoran por defecto. Para publicar otra sesión revisada,
agregar sus bloques y manifiesto explícitamente con `git add -f`, comprobando
antes que no contengan credenciales ni información que no se quiera publicar.
