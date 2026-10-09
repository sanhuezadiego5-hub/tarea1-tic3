"""Explicit hardware check via the same API used by GUI. Run with capture stopped."""
import argparse
import json
from pathlib import Path
import time
from urllib.request import Request, urlopen


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url',default='http://127.0.0.1:8080')
    parser.add_argument('--output',default='runtime_hardware_check.json')
    args=parser.parse_args()
    def get():
        with urlopen(args.url+'/api/status',timeout=4) as r:return json.load(r)
    def peer():
        online=[p for p in get()['peers'] if p['online']]
        if len(online)!=1:raise RuntimeError('This check needs exactly one online ESP')
        return online[0]
    def post(action,body=None):
        with urlopen(Request(args.url+'/api/'+action,data=json.dumps(body).encode() if body else b'',
            headers={'X-TIC3-Control':'1','Content-Type':'application/json'}),timeout=4) as r:return json.load(r)
    def wait(predicate):
        until=time.monotonic()+30
        while time.monotonic()<until:
            p=peer()
            if p['config_error']:raise RuntimeError(p['config_error'])
            if predicate(p):return p
            time.sleep(.15)
        raise TimeoutError('ESP did not confirm expected state')
    initial=peer()['effective_config']
    if not initial or peer()['desired']:raise RuntimeError('Stop capture and wait for effective configuration first')
    configurations=[
        dict(accel_hz=100,gyro_hz=50,accel_range=4,gyro_range=500,interval_ms=500,accel_samples=250,gyro_samples=125,mode='parallel',transport='tcp',format='binary',blocks=initial['blocks']),
        dict(accel_hz=200,gyro_hz=100,accel_range=2,gyro_range=250,interval_ms=500,accel_samples=300,gyro_samples=150,mode='sequential',transport='udp',format='json',blocks=initial['blocks']),
        dict(accel_hz=50,gyro_hz=25,accel_range=16,gyro_range=1000,interval_ms=1000,accel_samples=120,gyro_samples=60,mode='sequential',transport='tcp',format='binary',blocks=initial['blocks']),
        dict(accel_hz=100,gyro_hz=100,accel_range=8,gyro_range=2000,interval_ms=1000,accel_samples=50,gyro_samples=50,mode='parallel',transport='udp',format='binary',blocks=initial['blocks']),
    ]
    results=[]
    try:
        for config in configurations:
            post('config',config)
            wait(lambda p:p['effective_config']==config and p['pending_config'] is None and p['confirmed'])
            post('start')
            finished=wait(lambda p:p['session'] and p['session'].get('ended_utc') and not p['desired'])
            time.sleep(.5) # Allow late UDP packets before inspecting durable files.
            session=peer()['session'];root=Path(get()['storage'])
            base=root/(finished['device']+'_'+finished['boot'])
            blocks=[json.loads(f.read_text()) for f in base.rglob('window-*.json')]
            blocks=[b for b in blocks if b.get('session_id')==session['id']]
            counts={name:sum(b['sample_count'] for b in blocks if b['stream_name']==name) for name in ['acceleration','angular_velocity']}
            assert all(b['mode']==config['mode'] and b['transport']==config['transport'] and b['encoding']==config['format'] for b in blocks)
            targets=dict(acceleration=config['accel_samples'],angular_velocity=config['gyro_samples'])
            assert all(counts[k]<=targets[k] for k in counts), 'Repeated acquisition exceeded target'
            if config['transport']=='tcp':assert counts==targets, 'TCP saved counts differ from targets'
            acquired={name:finished['preview'][name]['last_count'] for name in ['acceleration','angular_velocity']}
            assert acquired==dict(acceleration=config['accel_samples'],angular_velocity=config['gyro_samples'])
            result=dict(session=session,acquired=acquired,saved=counts,blocks=len(blocks),
                counters_max={k:max((b[k] for b in blocks),default=0) for k in ['output_dropped_blocks','buffer_dropped','fifo_errors','i2c_errors']})
            print(json.dumps(result),flush=True);results.append(result)
    finally:
        try:
            p=peer()
            if p['desired']:
                post('stop');wait(lambda p:not p['desired'] and p['reported'] in (0,4,6,7))
            post('config',initial);wait(lambda p:p['effective_config']==initial and p['pending_config'] is None)
        finally:
            Path(args.output).write_text(json.dumps(results,indent=2))


if __name__=='__main__':main()
