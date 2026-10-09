"""Validated acquisition settings and fixed binary CONFIG payload."""
import struct

CONFIG = struct.Struct('!HHHHIIIBBBB8x')
DEFAULTS = dict(accel_hz=400, gyro_hz=400, accel_range=8, gyro_range=2000,
                interval_ms=1000, accel_samples=0, gyro_samples=0,
                mode='parallel', transport='tcp', format='binary', blocks=2)


def validate_config(config):
    if not isinstance(config, dict) or set(config) != set(DEFAULTS):
        raise ValueError('La configuración debe incluir todos los campos admitidos')
    c = dict(config)
    for name in ('accel_hz','gyro_hz','accel_range','gyro_range','interval_ms','accel_samples','gyro_samples','blocks'):
        if type(c[name]) is not int:
            raise ValueError(name + ': se requiere un entero')
    if c['accel_hz'] not in (25,50,100,200,400) or c['gyro_hz'] not in (25,50,100,200,400):
        raise ValueError('Frecuencias admitidas: 25, 50, 100, 200 y 400 Hz')
    if c['accel_range'] not in (2,4,8,16) or c['gyro_range'] not in (125,250,500,1000,2000):
        raise ValueError('Rango de sensor inválido')
    if not 100 <= c['interval_ms'] <= 60000 or not 2 <= c['blocks'] <= 4:
        raise ValueError('Ventana o cantidad de buffers fuera de rango')
    if any(not 0 <= c[k] <= 10000000 for k in ('accel_samples','gyro_samples')):
        raise ValueError('Cantidad de muestras fuera de rango: 0 a 10000000')
    if c['mode'] not in ('parallel','sequential') or c['transport'] not in ('tcp','udp') or c['format'] not in ('binary','json'):
        raise ValueError('Modo, transporte o formato inválido')
    storage = 0
    for name in ('accel_hz','gyro_hz'):
        n=(c[name]*c['interval_ms']+999)//1000
        storage+=(n+(n+19)//20+8)*14*c['blocks']
    if storage>128*1024:
        raise ValueError(f'Los arrays requieren {storage} B y exceden 128 KiB; reducir ventana o frecuencia')
    return c


def encode_config(config):
    c=validate_config(config)
    return CONFIG.pack(c['accel_hz'],c['gyro_hz'],c['accel_range'],c['gyro_range'],c['interval_ms'],
        c['accel_samples'],c['gyro_samples'],int(c['mode']=='parallel'),int(c['transport']=='udp'),int(c['format']=='json'),c['blocks'])


def decode_config(payload):
    if len(payload)!=32 or any(payload[24:]):
        raise ValueError('Payload CONFIG inválido')
    a,g,ar,gr,i,an,gn,parallel,udp,json_format,blocks=CONFIG.unpack(payload)
    if any(x not in (0,1) for x in (parallel,udp,json_format)):
        raise ValueError('Opciones CONFIG inválidas')
    return validate_config(dict(accel_hz=a,gyro_hz=g,accel_range=ar,gyro_range=gr,interval_ms=i,
        accel_samples=an,gyro_samples=gn,mode='parallel' if parallel else 'sequential',
        transport='udp' if udp else 'tcp',format='json' if json_format else 'binary',blocks=blocks))
