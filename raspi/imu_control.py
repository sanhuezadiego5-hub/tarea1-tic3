"""Thread-safe session control shared by TCP control channel and local web GUI."""
from datetime import datetime, timezone
from pathlib import Path
import threading
import time
import uuid
import struct
from imu_settings import DEFAULTS, validate_config, encode_config


def utc():
    return datetime.now(timezone.utc).isoformat()


class CaptureControl:
    def __init__(self, store):
        self.store = store
        self.lock = threading.RLock()
        self.peers = {}
        self.sessions = self.store.folder / 'sessions'
        self.sessions.mkdir(exist_ok=True)
        # An unconfirmed session from another server run must never appear saved.
        import json
        for path in self.sessions.glob('*.json'):
            session = json.loads(path.read_text())
            if session.get('result') in ('recording', 'start_pending', 'stop_pending'):
                session.update(result='interrupted_unconfirmed', server_restart_utc=utc())
                self.store.atomic_json(path, session)

    def register(self, identity, address):
        with self.lock:
            if identity not in self.peers:
                self.peers[identity] = dict(device=f'{identity[0]:016x}', boot=f'{identity[1]:016x}',
                    address=address[0], desired=False, command=0, confirmed_command=None,
                    reported=None, delivery_ack=True, effective_config=None, pending_config=None, config_error=None, preview={}, next_window=0, last_seen=time.monotonic(), session=None, latest={})
            return self.peers[identity]

    def report(self, identity, address, command, state, payload=b""):
        if state not in (0, 1, 2, 3, 4, 6, 7):
            raise ValueError('invalid acquisition state')
        with self.lock:
            peer = self.register(identity, address)
            peer.update(reported=state, confirmed_command=command, last_seen=time.monotonic())
            if payload:
                self.preview(peer, payload)
            if peer['pending_config']:
                return 9, peer['command'], 255
            automatic=state in (6,7) and peer['desired']
            if automatic:
                peer['desired']=False
                if peer['session']:
                    peer['session'].update(requested_stop_utc=utc(), automatic_stop=True)
            if state in (0,6):peer['delivery_ack']=True
            if state in (4,7):
                peer['delivery_ack'] = False
            expected = 1 if peer['desired'] else (0 if peer['delivery_ack'] else 4)
            if command == peer['command'] and (state == expected or (not peer['desired'] and state in (6,7))):
                if state == 1 and peer['session'] and 'started_confirmed_utc' not in peer['session']:
                    peer['session']['started_confirmed_utc'] = utc()
                    self.write_session(peer)
                if state in (0, 4, 6, 7) and peer['session'] and not peer['session'].get('ended_utc'):
                    peer['session'].update(ended_utc=utc(), window_end_exclusive=peer['next_window'], result=('target_reached_and_saved' if peer['session'].get('automatic_stop') else 'stopped_and_saved') if state in (0,6) else 'stopped_udp_unconfirmed')
                    self.write_session(peer)
                return 8, command, state
            return (5 if peer['desired'] else 6), peer['command'], 255

    def preview(self, peer, payload):
        if len(payload)!=32:
            raise ValueError('STATE telemetry inválida')
        a,g,window,_=struct.unpack_from('!IIII',payload)
        raw=struct.unpack_from('!hhhhhhI',payload,16)
        peer['next_window']=window
        c=peer['effective_config'] or DEFAULTS
        for name,count,offset,scale,unit in [('acceleration',a,0,c['accel_range']*9.80665/32768,'m/s²'),
                                           ('angular_velocity',g,3,c['gyro_range']*3.141592653589793/(180*32768),'rad/s')]:
            preview=peer['preview'].setdefault(name,dict(points=[],last_count=None))
            if preview['last_count']==count or count==0:
                continue
            if preview['last_count'] is not None and count<preview['last_count']:
                preview['points']=[]
            preview.update(last_count=count,xyz=[round(raw[offset+i]*scale,6) for i in range(3)],
                           read_ms=raw[6],unit=unit,received_utc=utc())
            preview['points'].append([raw[6],*preview['xyz']])
            preview['points']=preview['points'][-200:]

    def configure(self, settings):
        config=validate_config(settings)
        with self.lock:
            peers=[p for p in self.peers.values() if time.monotonic()-p['last_seen']<4]
            if not peers:raise ValueError('No hay ESP conectada')
            for p in peers:
                if p['desired'] or p['reported'] not in (0,4,6,7) or p['command']!=p['confirmed_command'] or p['pending_config']:
                    raise ValueError('Detener y esperar el guardado antes de cambiar configuración')
                if config['blocks'] != (p['effective_config'] or DEFAULTS)['blocks']:
                    raise ValueError('La ampliación de buffers requiere decisión separada; mantener la cantidad actual')
            for p in peers:
                p['command']=(p['command']+1)&0xffffffff
                p['pending_config']=dict(config)
                p['config_error']=None
            return self.snapshot()

    def configuration_result(self, identity, address, revision, config=None, error=0):
        with self.lock:
            peer=self.register(identity,address)
            peer['last_seen']=time.monotonic()
            if revision!=peer['command']:
                if peer['pending_config']:
                    return 9,peer['command'],255
                if peer['effective_config'] is None and not error:
                    peer['command']=revision
                else:
                    raise ValueError('CONFIG ACK no corresponde al comando actual')
            if not error:
                config=validate_config(config)
                if peer['pending_config'] is not None and config!=peer['pending_config']:
                    raise ValueError('ESP confirmó una configuración diferente')
                peer['effective_config']=dict(config)
                peer['delivery_ack']=config['transport']=='tcp'
                peer['reported']=0 if peer['delivery_ack'] else 4
                peer['preview']={}
            else:
                peer['config_error']={1:'Configuración inválida',2:'ESP ocupada; detener primero',
                    3:'RAM insuficiente; reducir ventana o frecuencia',4:'Fallo de sensor y recuperación; reiniciar ESP',
                    5:'No se aplicó configuración del sensor; configuración anterior restaurada'}.get(error,'Error CONFIG')
            peer['pending_config']=None
            peer['confirmed_command']=revision
            return 8,revision,255 if not error else error

    def write_session(self, peer):
        session = peer['session']
        if session:
            self.store.atomic_json(self.sessions / (session['id'] + '.json'), session)

    def command(self, action):
        if action not in ('start', 'stop'):
            raise ValueError('unknown command')
        with self.lock:
            peers = [p for p in self.peers.values() if time.monotonic() - p['last_seen'] < 4]
            if not peers:
                raise ValueError('No hay ESP conectada al canal de control')
            if action == 'start':
                for peer in peers:
                    if peer['pending_config']:
                        raise ValueError('Esperar confirmación de configuración de la ESP')
                    if peer['reported'] == 3:
                        raise ValueError('La ESP reporta fallo de sensor; revisar y reiniciar')
                    if not peer['desired'] and (peer['reported'] not in (0, 4, 6, 7) or peer['confirmed_command'] != peer['command']):
                        raise ValueError('Esperar la confirmación de parada y guardado antes de iniciar')
            for peer in peers:
                desired = action == 'start'
                if peer['desired'] == desired:
                    continue
                peer['desired'] = desired
                peer['command'] = (peer['command'] + 1) & 0xffffffff
                if desired:
                    peer['latest'] = {}
                    peer['preview'] = {}
                    peer['session'] = dict(id=uuid.uuid4().hex, device=peer['device'], boot=peer['boot'],
                        requested_start_utc=utc(), result='start_pending', samples={}, blocks={},
                        first_window=None, last_window=None, window_begin=peer['next_window'], configuration=dict(peer['effective_config'] or DEFAULTS))
                elif peer['session']:
                    peer['session'].update(requested_stop_utc=utc(), result='stop_pending')
                self.write_session(peer)
            return self.snapshot()

    def session_id(self, identity, window=None):
        with self.lock:
            peer = self.peers.get(identity)
            if not peer or not peer['session']:
                return None
            if window is None or window >= peer['session'].get('window_begin',0):
                return peer['session']['id']
            import json
            for path in self.sessions.glob('*.json'):
                session=json.loads(path.read_text())
                if session['device']==peer['device'] and session['boot']==peer['boot'] and session.get('window_begin',0)<=window<session.get('window_end_exclusive',0):
                    return session['id']
            return None

    def saved(self, block, path):
        identity = block['device_id'], block['boot_id']
        with self.lock:
            peer = self.peers.get(identity)
            if not peer:
                return
            stream = block['stream_name']
            count = block['sample_count']
            step = max(1, (count + 255) // 256)
            rows = [block['values'][i:i+3] for i in range(0, len(block['values']), 3)]
            peer['latest'][stream] = dict(sample_count=count, sample_hz=block['sample_hz'],
                window_seq=block['window_seq'], partial=bool(block['flags'] & 4),
                unit=block['si_unit'], captured_hz=block['captured_samples_per_second'],
                values=[[round(v * block['scale_to_si'], 6) for v in row] for row in rows[::step]],
                read_us=block['read_us'][::step], path=str(path.resolve()),
                drops=block['output_dropped_blocks'], fifo_errors=block['fifo_errors'],
                i2c_errors=block['i2c_errors'], received_utc=block['received_utc'])
            session = peer['session']
            if session and block.get('session_id') == session['id']:
                session['samples'][stream] = session['samples'].get(stream, 0) + count
                session['blocks'][stream] = session['blocks'].get(stream, 0) + 1
                session['first_window'] = block['window_seq'] if session['first_window'] is None else min(session['first_window'], block['window_seq'])
                session['last_window'] = block['window_seq']
                if 'requested_stop_utc' not in session:
                    session['result'] = 'recording'
                self.write_session(peer)

    def snapshot(self):
        with self.lock:
            result = []
            names = {0: 'Detenido y guardado', 1: 'Midiendo', 2: 'Cerrando y guardando', 3: 'Fallo de sensor', 4: 'Detenido; recepción UDP sin confirmar', 6:'Cantidad alcanzada y guardada', 7:'Cantidad alcanzada; UDP sin confirmar', None: 'Esperando estado'}
            for peer in self.peers.values():
                online = time.monotonic() - peer['last_seen'] < 4
                confirmed = peer['command'] == peer['confirmed_command']
                label = names[peer['reported']]
                if not online:
                    label = 'Sin conexión de control; estado sin confirmar'
                elif not confirmed or (peer['desired'] and peer['reported'] != 1) or (not peer['desired'] and peer['reported'] not in (0, 4, 6, 7)):
                    label = 'Inicio pendiente' if peer['desired'] else 'Parada pendiente de confirmación'
                if peer['pending_config'] and online:
                    label='Configuración pendiente de ESP'
                if peer['reported'] == 3 and online:
                    label = names[3]
                result.append({k: peer[k] for k in ('device', 'boot', 'address', 'desired', 'command', 'reported', 'session', 'latest', 'effective_config', 'pending_config', 'config_error', 'preview')} | dict(online=online, confirmed=confirmed, state=label))
            return dict(peers=result, data_port=5001, control_port=5000)
