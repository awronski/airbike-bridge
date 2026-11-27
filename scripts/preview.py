"""Local UI test server. All workout, rider and sensor data is simulated."""
import argparse
import copy
import json
import socket
import time
import zlib
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
ROOT = Path(__file__).resolve().parent.parent
HR_FOCUS = False
mode = 'stopped'
setup_scenario = 'success'
setup_started = None
diagnostics_enabled = True
settings = dict(auto_start=True, auto_stop=True, start_seconds=3, stop_seconds=10, storage_ok=True)
riders = []
rider_create_keys = {}
selected_id = 0
ride_state = 'stopped'
current = {}
history = []
history_corrupt = False
next_ride_id = 1
scan_until = 0
sensor_selected = False
sensor_found = False
last_tick = time.monotonic()


rider_mode='manual'
auto_choice=None

def diagnostic_data(saved=False):
    return dict(schema=1,source='saved' if saved else 'current',firmware='SIMULATED-preview',enabled=diagnostics_enabled,
        setting_storage_ok=True,saved_available=True,save_status='saved',saved_this_boot=True,save_age_ms=92000,
        boot_reset_reason=1,report_reset_reason=1,session_id=1,has_session=True,active=not saved,partial=False,
        start_ms=12000,end_ms=0,captured_ms=750000,end_reason=0,events_omitted=0,
        stats=dict(bike_disconnects=1,bike_gaps=2,bike_max_gap_ms=5200),
        events=[dict(at_ms=730000,sequence=1,kind='disconnect',source='bike',a=0x208,b=0,c=0,d=0),
                dict(at_ms=734000,sequence=2,kind='connect',source='bike',a=0,b=0,c=0,d=0),
                dict(at_ms=735200,sequence=3,kind='gap_recovered',source='bike',a=5200,b=1,c=0,d=0)],
        rssi_ranges=[],health=[],rejected_bike_samples=[],notes='SIMULATED diagnostics for layout checks')


def fake_profiles():
    return [dict(id=i,name=name,sex=0,age=None,weight_kg=None,max_hr=190,ftp_w=235,band_address='aa:bb:cc:dd:ee:0'+str(i),band_type=0,band_name='Polar H10 '+name) for i,name in [(1,'Rider A'),(2,'Rider B')]]

def selection():
    if ride_state!='ready':
        return dict(mode=rider_mode,status='locked',ready=True,rider_id=current['rider_id'],rider_name=current['rider_name'],candidates=[])
    if rider_mode=='manual':
        r=next((r for r in riders if r['id']==selected_id),guest())
        return dict(mode='manual',status='manual',ready=True,rider_id=r['id'],rider_name=r['name'],candidates=[])
    if auto_choice is not None:
        r=next(r for r in riders if r['id']==auto_choice)
        return dict(mode='auto',status='selected',ready=True,rider_id=r['id'],rider_name=r['name'],candidates=[])
    choices=[r for r in riders if r.get('band_address')]
    if mode=='auto_one': choices=choices[1:2]
    if mode in ('auto_wait','auto_scan'): choices=[]
    status='scanning' if mode=='auto_scan' else 'choose' if len(choices)>1 else 'selected' if choices else 'waiting'
    r=choices[0] if len(choices)==1 else guest()
    return dict(mode='auto',status=status,ready=status=='selected',rider_id=r['id'],rider_name=r['name'],candidates=[dict(id=r['id'],name=r['name']) for r in choices])

def guest():
    return dict(id=0,name='Guest',sex=0,age=None,weight_kg=None,max_hr=190,ftp_w=None,band_address='',band_type=0,band_name='')


def selected():
    sid=selection()['rider_id'] if rider_mode=='auto' else selected_id
    return next((r for r in riders if r['id']==sid),guest())


def rider_data(**extra):
    return dict(selected_id=selected_id, mode=rider_mode, selection=selection(), max_riders=10, riders=riders, **extra)


def blank():
    return dict(id=0, duration_s=0, distance_m=0, calories_kcal=0, avg_power_w=None,
                max_power_w=0, avg_speed_kph=None, max_speed_kph=0, avg_hr_bpm=None,
                max_hr_bpm=None, rider_id=selected()['id'], rider_name=selected()['name'], started_at=None,
                partial=False, reason='Current ride')


def scenario(name):
    global mode, ride_state, current, history, sensor_selected, sensor_found, last_tick, riders, rider_mode, auto_choice, selected_id, next_ride_id
    mode = name
    global history_corrupt
    history_corrupt = name == 'corrupt_history'
    if name.startswith('auto_') or name in ('band_locked','hr_focus','readouts','dated_history'):
        riders=fake_profiles();rider_mode='auto';auto_choice=None;selected_id=0
        rider_create_keys.clear()
    ride_state = 'ready' if name in ('waiting','ready','auto_both','auto_one','auto_wait','auto_scan') else 'recording' if name in ('recording', 'live', 'long','readouts') else 'stopped'
    current=dict(rider_id=selected_id,rider_name=next((r['name'] for r in riders if r['id']==selected_id),'Guest'))
    current = blank()
    if name in ('band_locked','hr_focus','readouts','dated_history'):current.update(rider_id=1,rider_name='Rider A')
    if ride_state != 'ready':
        current.update(duration_s=514, distance_m=3200, calories_kcal=48, avg_power_w=165,
                       max_power_w=380, avg_speed_kph=22.4, max_speed_kph=30.5, avg_hr_bpm=138, max_hr_bpm=160)
    if name=='hr_focus':current['duration_s']=1800
    if name=='readouts':current.update(duration_s=2820,distance_m=21450)
    if name=='dated_history':current.update(duration_s=2920,distance_m=24800)
    if ride_state!='ready':current['started_at']=int(time.time())-current['duration_s']
    total = 20 if name == 'history' else 0 if name in ('waiting','corrupt_history') else 2 if name=='dated_history' else 3
    history = []
    for i in range(total):
        r = blank()
        r.update(id=20-i, duration_s=1452+i*60, distance_m=8400+i*500, avg_power_w=165+i,
                 max_power_w=380+i, avg_speed_kph=20.8, max_speed_kph=30.5,
                 avg_hr_bpm=135, max_hr_bpm=165, reason='Saved by rider')
        if name == 'legacy_history':
            r.update(avg_hr_bpm=None, max_hr_bpm=None)
        if name == 'dated_history':
            day, duration, metres, speed, max_speed, power, max_power, hr, max_hr = [
                (24, 2892, 24700, 30.7, 44.8, 146, 291, 132, 163),
                (20, 1691, 13400, 28.5, 39.6, 126, 253, 128, 152),
            ][i]
            r.update(started_at=int(datetime(2026, 11, day, 12, tzinfo=timezone.utc).timestamp()),
                     duration_s=duration, distance_m=metres, avg_speed_kph=speed, max_speed_kph=max_speed,
                     avg_power_w=power, max_power_w=max_power, avg_hr_bpm=hr, max_hr_bpm=max_hr)
        history.append(r)
    next_ride_id = max(next_ride_id, max([r['id'] for r in history] + [0]) + 1)
    sensor_selected = name not in ('waiting', 'hr_missing')
    sensor_found = False
    last_tick = time.monotonic()


def sensor():
    sel=selection();r=selected();configured=sensor_selected and sel['ready'] and (not r['id'] or bool(r.get('band_address')))
    if rider_mode=='auto':configured=sel['ready'] and bool(r.get('band_address'))
    address=r.get('band_address') or ('aa:bb:cc:dd:ee:ff' if configured else '')
    devices=[dict(name='Polar H10 '+name,address='aa:bb:cc:dd:ee:0'+str(i),type=0,rssi=-45-i*4,available=True) for i,name in [(1,'Rider A'),(2,'Rider B')]] if sensor_found else []
    bpm = 126 if mode=='dated_history' else 135 if mode=='readouts' else 125 if HR_FOCUS or mode=='hr_focus' else 142
    return dict(configured=configured,connected=configured and mode!='band_wait',bpm=bpm if configured and mode!='band_wait' else None,
        contact=True,data_age_ms=100 if configured else None,name=r.get('band_name') or ('Polar H10 SIMULATED' if configured else ''),
        address=address,detail='Connected to simulated H10' if configured else 'No linked H10 connected',scanning=time.monotonic()<scan_until,
        rider_id=r['id'],max_hr=r.get('max_hr'),rider_name=r['name'] if sel['ready'] else '',selection=sel,rejected_packets=0,devices=devices)


def ride_data(known_history=None):
    global last_tick
    now = time.monotonic()
    if ride_state == 'recording' and mode not in ('stale', 'offline'):
        dt = now-last_tick
        speed_kph = 30.4 if mode=='readouts' else 22.4
        current['duration_s'] += dt
        current['distance_m'] += dt*speed_kph/3.6
        current['avg_power_w'] = 168 if mode=='readouts' else 185
        current['avg_speed_kph'] = speed_kph
        current['avg_hr_bpm'] = (135 if mode=='readouts' else 142) if sensor_selected else None
        current['max_hr_bpm'] = 160 if sensor_selected else None
    last_tick = now
    version = format(zlib.crc32(json.dumps(history).encode()), 'x')
    data = dict(active=ride_state != 'ready', state=ride_state, stop_reason='manual',
                saving=history_corrupt and ride_state == 'stopped',
                can_start=mode not in ('waiting','stale','offline') and selection()['ready'],
                can_clear=ride_state != 'ready',
                storage_ok=not history_corrupt, history_corrupt=history_corrupt, history_version=version,
                selected_rider=selected(), selection=selection(), current=copy.deepcopy(current),
                calories_kcal=current['calories_kcal'])
    if known_history != version:
        data['history'] = history
    return data


def trend_data():
    # Only this development server fabricates chart history. The embedded page
    # consumes this same API shape from the ESP32's bounded live sensor buffer.
    hr_anchors=[100,104,110,123,130,141,145,148,160,151,144,141,132,129,125]
    power_anchors=[65,110,95,160,125,210,180,260,350,195,230,155,210,160,185]
    if mode=='readouts':
        hr_anchors=[124,126,130,126,131,132,136,134,140,136,133,130,128,132,135]
        power_anchors=[164,169,165,158,170,172,167,174,179,174,168,173,164,169,168]
    if mode=='dated_history':
        hr_anchors=[122,127,133,130,135,139,138,135,133,146,158,155,141,139,141,136,135,130,126]
        power_anchors=[105,125,140,152,165,148,145,150,142,185,220,233,192,130,153,151,148,148,0]
    def interpolate(values,i):
        at=i/60*(len(values)-1);a=int(at);b=min(len(values)-1,a+1)
        return round(values[a]+(values[b]-values[a])*(at-a))
    sel=selection()
    points=[]
    if sel['ready'] and mode not in ('waiting','auto_one','auto_wait','auto_scan','auto_both'):
        heart_rate_ready=ride_state!='ready' and sensor()['connected']
        for i in range(61):
            age=(60-i)*30
            hr_value=interpolate(hr_anchors,i) if heart_rate_ready and age<=current['duration_s'] else None
            power=interpolate(power_anchors,i) if mode!='stale' else None
            points.append([age,hr_value,power])
    return dict(window_s=1800,interval_s=30,rider_id=sel['rider_id'],ready=sel['ready'],max_hr=selected().get('max_hr'),points=points)


class Handler(BaseHTTPRequestHandler):
    def send(self, body, status=200, content_type='application/json'):
        if not isinstance(body, (str, bytes)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global setup_scenario, setup_started
        path = urlsplit(self.path)
        query = parse_qs(path.query)
        if 'scenario' in query:
            scenario(query['scenario'][0])
        if path.path == '/':
            page = (ROOT/('web/hr-mock.html' if HR_FOCUS else 'web/index.html')).read_text()
            banner = '<aside style="padding:24px 0;color:#f4ce8e;font-size:12px">SIMULATED UI PREVIEW · No hardware data<nav style="display:flex;flex-wrap:wrap;gap:14px;margin-top:12px">'
            for name in ('dated_history','readouts','hr_focus','auto_both','auto_one','auto_wait','auto_scan','band_locked','band_wait','ready', 'recording', 'stopped', 'history', 'legacy_history', 'corrupt_history', 'hr_missing', 'garmin', 'blocked', 'stale', 'waiting', 'long', 'offline'):
                banner += f'<a href="/?scenario={name}">{name}</a>'
            page = page.replace('</main>', banner+'</nav></aside></main>', 1)
            self.send(page, content_type='text/html; charset=utf-8')
        elif path.path == '/api/live':
            if mode == 'offline':
                self.send('Simulated unreachable ATOM', 503, 'text/plain'); return
            connected = mode != 'waiting'
            watch = mode not in ('garmin', 'waiting')
            stopped_example = mode=='dated_history' and ride_state=='stopped'
            self.send(dict(connected=connected, has_data=connected, status='connected' if connected else 'scanning',
                detail='SIMULATED UI TEST — not hardware readings', bike_name='Assault AirBike SIMULATED' if connected else '',
                bike_address='00:00:00:00:00:00', programme='Manual', console_state=10 if connected else None,
                notifications=100, data_packets=int(time.monotonic()), rejected_packets=0,
                data_age_ms=9000 if mode == 'stale' else 100 if connected else None,
                last_data_hex='SIMULATED', last_notification='SIMULATED', wifi_connected=True, wifi_rssi=-55, ip='SIMULATED',
                relay=dict(ready=True, connected=watch, power_subscribed=watch, csc_subscribed=False, active=connected,
                    power_notifications=100 if mode == 'blocked' else int(time.monotonic()) if watch else 0,
                    csc_notifications=0, send_failures=0, advertising=True, output_connections=1 if watch else 0, wheel_mm=2000),
                hr=sensor(), ftp_w=selected().get('ftp_w'),
                metrics=dict(power_w=0 if stopped_example else 1999 if mode == 'long' else 168 if mode == 'readouts' else 185,
                    speed_kph=0 if stopped_example else 123.4 if mode == 'long' else 30.4 if mode == 'readouts' else 22.4,
                    cadence_rpm=0 if stopped_example else 140 if mode == 'long' else 48 if mode == 'readouts' else 72,
                    distance_m=3200, calories_kcal=48, elapsed_s=514, remaining_s=None)))
        elif path.path == '/api/rides':
            if mode == 'offline': self.send('Simulated unreachable ATOM', 503, 'text/plain')
            else: self.send(ride_data(query.get('history_version',[None])[0]))
        elif path.path == '/api/trend':
            if mode=='offline':self.send('Simulated unreachable ATOM',503,'text/plain')
            else:self.send(trend_data())
        elif path.path in ('/screen-awake.js', '/vendor/nosleep-media.js', '/diagnostics.js'):
            self.send((ROOT/'web'/path.path.lstrip('/')).read_text(), content_type='text/javascript; charset=utf-8')
        elif path.path == '/api/diagnostics':
            self.send(diagnostic_data())
        elif path.path == '/api/diagnostics/export':
            if query.get('source', [''])[0] not in ('current','saved'): self.send('Choose current or saved report',400,'text/plain')
            else: self.send(diagnostic_data(query['source'][0]=='saved'))
        elif path.path == '/api/settings':
            self.send(dict(**settings, selected_rider=selected(), hr_address=sensor()['address'], hr_name=sensor()['name']))
        elif path.path == '/api/riders':
            self.send(rider_data())
        elif path.path == '/api/hr/sensor':
            self.send(sensor())
        elif path.path == '/setup':
            setup_scenario = query.get('case', ['success'])[0]
            setup_started = None
            page = (ROOT/'web/setup.html').read_text()
            self.send(page.replace('<body>', '<body><p>SIMULATED SETUP · No real Wi-Fi changes</p>'), content_type='text/html; charset=utf-8')
        elif path.path == '/api/setup':
            done = setup_started is not None and time.monotonic()-setup_started >= 2
            phase = 'idle' if setup_started is None else ('failed' if setup_scenario == 'failure' else 'connected') if done else 'joining'
            self.send(dict(connected=phase == 'connected', failed=phase == 'failed', phase=phase, ip='198.51.100.42'))
        else:
            self.send('Not found', 404, 'text/plain')

    def do_POST(self):
        global diagnostics_enabled, history_corrupt
        global ride_state, current, history, selected_id, scan_until, sensor_selected, sensor_found, last_tick, setup_started, rider_mode, auto_choice, next_ride_id
        raw = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if self.path == '/wifi':
            setup_started = time.monotonic()
            if setup_scenario == 'lost_ack':
                self.connection.shutdown(socket.SHUT_RDWR); self.connection.close(); return
            self.send(dict(ok=True)); return
        fields = {key: values[0] for key, values in parse_qs(raw.decode(), keep_blank_values=True).items()}
        expected = {'/api/diagnostics/settings':'diagnostics', '/api/rides/control':'ride-control',
                    '/api/rides/clear':'clear-ride', '/api/rides/delete':'delete-ride',
                    '/api/rides/recover':'recover-history', '/api/retry':'retry', '/api/settings':'settings',
                    '/api/riders':'riders', '/api/riders/select':'rider-select', '/api/riders/resolve':'rider-select', '/api/hr/sensor':'hr-sensor'}
        if self.path not in expected or self.headers.get('X-AirBike-Action') != expected[self.path]:
            self.send('Use the dashboard', 403, 'text/plain'); return
        if self.path == '/api/diagnostics/settings':
            value=fields.get('enabled')
            if value not in ('0','1'): self.send('Use enabled=0 or enabled=1',400,'text/plain'); return
            diagnostics_enabled=value=='1'; self.send(diagnostic_data()); return
        ride_data()
        if self.path == '/api/retry':
            self.send(dict(ok=True))
        elif self.path == '/api/rides/recover':
            if fields.get('confirm') != '1':
                self.send('Confirm starting a fresh history',400,'text/plain');return
            if not history_corrupt or ride_state == 'recording':
                self.send('Recovery unavailable in the current state',409,'text/plain');return
            if ride_state == 'stopped' and (current['duration_s'] or current['distance_m']):
                history = [dict(current, id=next_ride_id, reason='Saved by rider')]
                next_ride_id += 1
            history_corrupt = False;ride_state = 'ready';current = blank()
            self.send(ride_data())
        elif self.path == '/api/rides/control':
            action = fields.get('action')
            if action == 'start' and ride_state == 'ready':
                current = blank(); current['started_at'] = int(time.time()); ride_state = 'recording'
            elif action == 'stop' and ride_state == 'recording': ride_state = 'stopped'
            elif action == 'resume' and ride_state == 'stopped': ride_state = 'recording'
            elif action == 'finish' and ride_state != 'ready':
                if history_corrupt:
                    ride_state = 'stopped'
                    self.send('History is unreadable. Use Recover history.',503,'text/plain');return
                item = copy.deepcopy(current); item['reason'] = 'Saved by rider'; item['id'] = next_ride_id
                next_ride_id += 1
                history = [item]+history[:19]; ride_state = 'ready'; current = blank()
            else:
                self.send('Action unavailable in current ride state', 409, 'text/plain'); return
            last_tick = time.monotonic(); self.send(ride_data())
        elif self.path == '/api/rides/delete':
            ride_id = fields.get('id', '')
            if not ride_id.isascii() or not ride_id.isdigit() or int(ride_id) <= 0:
                self.send('Invalid ride ID', 400, 'text/plain'); return
            ride_id = int(ride_id)
            if not any(item['id'] == ride_id for item in history):
                self.send('This ride is no longer in history.', 404, 'text/plain'); return
            history = [item for item in history if item['id'] != ride_id]
            self.send(ride_data())
        elif self.path == '/api/rides/clear':
            if ride_state == 'ready':
                self.send('There is no current dashboard ride to clear', 409, 'text/plain'); return
            ride_state = 'ready'; current = blank(); self.send(dict(ok=True))
        elif self.path == '/api/riders/select':
            if ride_state != 'ready': self.send('Save or clear the current ride before switching riders',409,'text/plain'); return
            if fields['id']=='auto':rider_mode='auto';auto_choice=None
            else:rider_mode='manual';selected_id=int(fields['id']);auto_choice=None
            self.send(rider_data())
        elif self.path == '/api/riders/resolve':
            if ride_state!='ready':self.send('Rider locked',409,'text/plain');return
            auto_choice=int(fields['id']);self.send(rider_data())
        elif self.path == '/api/riders':
            rid = int(fields.get('id', 0)); name = fields.get('name', '').strip()
            create_key = fields.get('create_key') if not rid else None
            if create_key is not None:
                if (len(create_key) != 16 or create_key == '0' * 16 or
                        any(digit not in '0123456789abcdef' for digit in create_key)):
                    self.send('Invalid rider creation key', 400, 'text/plain'); return
                saved_id = rider_create_keys.get(create_key)
                if saved_id is not None:
                    self.send(rider_data(saved_id=saved_id, already_saved=True)); return
            if not name or len(name.encode()) > 48: self.send('Invalid name',400,'text/plain'); return
            if not rid and len(riders) >= 10: self.send('Up to 10 riders',409,'text/plain'); return
            rid = rid or max([r['id'] for r in riders]+[0])+1
            r = dict(id=rid, name=name, sex=int(fields['sex']), age=int(fields['age']) if fields.get('age') else None,
                     weight_kg=float(fields['weight_kg']) if fields.get('weight_kg') else None,
                     max_hr=int(fields['max_hr']) if fields.get('max_hr') else None)
            existing = next((i for i,p in enumerate(riders) if p['id'] == rid), None)
            if 'ftp_w' in fields:
                ftp = fields['ftp_w']
                if ftp and (not ftp.isascii() or not ftp.isdecimal() or not 1<=int(ftp)<=2000):
                    self.send('Enter cycling FTP from 1 to 2000 watts, or leave it blank',400,'text/plain'); return
                r['ftp_w'] = int(ftp) if ftp else None
            else:
                r['ftp_w'] = riders[existing].get('ftp_w') if existing is not None else None
            if existing is None:r.update(band_address='',band_type=0,band_name='');riders.append(r)
            else:r.update({k:riders[existing].get(k,'' if k!='band_type' else 0) for k in ['band_address','band_type','band_name']});riders[existing]=r
            if create_key is not None:
                rider_create_keys[create_key] = rid
            self.send(rider_data(saved_id=rid))
        elif self.path == '/api/settings':
            if ride_state != 'ready': self.send('Save or clear current ride first',409,'text/plain'); return
            try:
                start, stop = int(fields['start_seconds']), int(fields['stop_seconds'])
                assert 1 <= start <= 600 and 1 <= stop <= 600
            except (ValueError, AssertionError): self.send('Use whole seconds from 1 to 600',400,'text/plain'); return
            settings.update(auto_start=fields['auto_start']=='1', auto_stop=fields['auto_stop']=='1', start_seconds=start, stop_seconds=stop)
            self.send(dict(**settings, selected_rider=selected()))
        elif self.path == '/api/hr/sensor':
            if fields.get('action')=='scan':scan_until=time.monotonic()+2;sensor_found=True
            else:
                if ride_state!='ready':self.send('Finish or clear the current ride first',409,'text/plain');return
                rider=next((r for r in riders if str(r['id'])==fields.get('rider_id')),None)
                if not rider:self.send('Choose a saved rider',400,'text/plain');return
                if fields.get('action')=='unlink':rider.update(band_address='',band_type=0,band_name='')
                else:
                    address=fields['address'];owner=next((r for r in riders if r.get('band_address')==address and r['id']!=rider['id']),None)
                    if owner:self.send('That band is already linked to another rider',409,'text/plain');return
                    device=next(d for d in sensor()['devices'] if d['address']==address)
                    rider.update(band_address=address,band_type=int(fields['type']),band_name=device['name'])
            self.send(sensor())

    def log_message(self, *_):
        pass


scenario('stopped')
if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port',type=int,default=18771)
    parser.add_argument('--hr-focus',action='store_true',help='Serve the separate HR-first design mock')
    args=parser.parse_args()
    HR_FOCUS=args.hr_focus
    if HR_FOCUS:scenario('hr_focus')
    print(f'Simulated UI preview: http://127.0.0.1:{args.port}/',flush=True)
    HTTPServer(('127.0.0.1',args.port),Handler).serve_forever()
