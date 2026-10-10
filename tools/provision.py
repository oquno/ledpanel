#!/usr/bin/env python3
"""USB provisioning helper; secrets are never printed by default."""
import argparse, getpass, json, secrets, subprocess, time, uuid
from pathlib import Path

PASSWORD_ALPHABET='ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'

def short_password(length=12):
    return ''.join(secrets.choice(PASSWORD_ALPHABET) for _ in range(length))

def validate_wifi_credentials(ssid,password):
    if not isinstance(ssid,str) or not 1 <= len(ssid.encode('utf-8')) <= 32 or \
            any(ord(char)<32 or ord(char)==127 for char in ssid):
        raise SystemExit('SSID must contain 1..32 UTF-8 bytes without control characters')
    if not isinstance(password,str) or not 8 <= len(password.encode('utf-8')) <= 63 or \
            any(ord(char)<32 or ord(char)==127 for char in password):
        raise SystemExit('Wi-Fi password must contain 8..63 UTF-8 bytes without control characters')

def group_file(path, network_mode='softap', ssid=None):
    path=Path(path)
    if path.exists(): raise SystemExit(f'file already exists; choose a new path: {path}')
    if network_mode not in ('softap','infrastructure'):
        raise SystemExit('network-mode must be softap or infrastructure')
    data={'version':2,'groupId':str(uuid.uuid4()),'apPsk':short_password(),'adminPassword':short_password(),'groupKey':secrets.token_hex(32),'networkMode':network_mode,'createdBy':'ledpanel-provision'}
    if network_mode == 'infrastructure':
        validate_wifi_credentials(ssid, 'placeholder')
        password=getpass.getpass('Wi-Fi password (hidden): ')
        confirmation=getpass.getpass('Confirm Wi-Fi password (hidden): ')
        if password != confirmation:
            raise SystemExit('Wi-Fi passwords do not match')
        validate_wifi_credentials(ssid, password)
        data['stationSsid']=ssid
        data['stationPassword']=password
    with path.open('x',encoding='utf-8') as target: target.write(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
    try: path.chmod(0o600)
    except OSError: pass
    print(f'created private provisioning file: {path}')

def flash(port,release):
    release=Path(release);image=release/'firmware-merged.bin'
    if not image.exists(): raise SystemExit(f'missing {image}')
    cmd=['esptool','--chip','esp32s3','--port',port,'write-flash','0x0',str(image)]
    print('running:', ' '.join(cmd));raise SystemExit(subprocess.call(cmd))

def is_hardware_id(value):
    try:
        return str(uuid.UUID(value)) == value
    except (ValueError, AttributeError):
        return False

def validate_registration(node_id,hardware_id,label):
    if not 1 <= node_id <= 65535:
        raise SystemExit('node-id must be in 1..65535')
    if not is_hardware_id(hardware_id):
        raise SystemExit('hardware-id must be a lowercase UUID returned by inspect')
    if not label or len(label) > 80 or any(ord(c) < 32 or 0x7f <= ord(c) <= 0x9f for c in label):
        raise SystemExit('label must contain 1..80 printable characters')

def inspect(port, display=True):
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('Install pyserial in the active Python environment: python -m pip install pyserial') from exc
    connection=serial.Serial()
    connection.port=port
    connection.baudrate=115200
    connection.timeout=0.25
    connection.write_timeout=2
    # Opening a USB CDC port with DTR/RTS asserted can reset the ESP32-S3.
    # Preserve the running display mode while inspecting device state.
    connection.dtr=False
    connection.rts=False
    try:
        with connection:
            connection.reset_input_buffer()
            end=time.monotonic()+20
            next_hello=0
            while time.monotonic()<end:
                now=time.monotonic()
                if now>=next_hello:
                    connection.write(b'{"id":1,"op":"inspect"}\n')
                    next_hello=now+1
                line=connection.readline()
                if not line.startswith(b'{'):
                    continue
                try:
                    response=json.loads(line)
                except (UnicodeDecodeError,json.JSONDecodeError):
                    continue
                if response.get('id') != 1 or not response.get('ok'):
                    continue
                fields=('provisioned','configurationOpen','boardProfile','hardwareId',
                        'nodeId','masterNodeId','role','groupId','networkMode',
                        'panelOutput','outputEnableLevel')
                result={k:response[k] for k in fields if k in response}
                if display:
                    print(json.dumps(result,ensure_ascii=False,indent=2))
                return result
            raise SystemExit('No USB inspect response. Check that the device is running its application.')
    except serial.SerialException as exc:
        raise SystemExit(f'Could not use {port}: {exc}') from exc

def _read_usb_response(connection, expected_id, timeout):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        line=connection.readline()
        if not line.startswith(b'{'):
            continue
        try:
            response=json.loads(line)
        except (UnicodeDecodeError,json.JSONDecodeError):
            continue
        if isinstance(response,dict) and response.get('id')==expected_id:
            return response
    raise SystemExit(f'USB response {expected_id} timed out.')

def test_panel(port):
    current=inspect(port,display=False)
    if current.get('panelOutput')=='dim_nightlight':
        print('The steady dim nightlight is already running. Use nightlight --mode off to turn it off.')
        return current
    if current.get('panelOutput')=='diagnostic_rgb_bars':
        print('Startup RGB-bar demo is already running and stays on until the board resets.')
        return current
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('Install pyserial in the active Python environment: python -m pip install pyserial') from exc
    connection=serial.Serial()
    connection.port=port
    connection.baudrate=115200
    connection.timeout=0.25
    connection.write_timeout=2
    connection.dtr=False
    connection.rts=False
    try:
        connection.open()
        connection.reset_input_buffer()
        connection.write(b'{"id":1,"op":"panel_test"}\n')
        started=_read_usb_response(connection,1,5)
        if not started.get('ok'):
            raise SystemExit(f"Master rejected the panel test: {started.get('error','UNKNOWN')}")
        if (started.get('status')!='diagnostic_started' or
                started.get('pattern')!='RGB_BARS' or started.get('durationMs')!=30000 or
                started.get('panelCoveragePercent')!=100 or started.get('brightness')!=16):
            raise SystemExit('Master returned an unexpected panel diagnostic response.')
        print('Full-panel RGB bars started: 30 seconds, pulsing at 16/255. Check the panel now.')
        connection.write(b'{"id":2,"op":"inspect"}\n')
        active=_read_usb_response(connection,2,3)
        if (not active.get('ok') or active.get('panelOutput')!='diagnostic_rgb_bars'):
            raise SystemExit('Master did not report the diagnostic scan as active.')
        print('Master reports diagnostic_rgb_bars active.')
        time.sleep(30.25)
        connection.write(b'{"id":3,"op":"inspect"}\n')
        finished=_read_usb_response(connection,3,3)
        if (not finished.get('ok') or finished.get('panelOutput')!='disabled_black' or
                finished.get('outputEnableLevel')!=1):
            raise SystemExit('Panel output did not return to OE HIGH blackout after the test.')
        print('Master confirmed automatic blackout: OE GPIO13 HIGH.')
        return finished
    except serial.SerialException as exc:
        raise SystemExit(f'Could not use {port}: {exc}') from exc
    finally:
        if connection.is_open:
            connection.close()

def nightlight(port, mode):
    desired='dim_nightlight' if mode=='on' else 'disabled_black'
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('Install pyserial in the active Python environment: python -m pip install pyserial') from exc
    operation='nightlight_on' if mode=='on' else 'nightlight_off'
    connection=serial.Serial()
    connection.port=port
    connection.baudrate=115200
    connection.timeout=0.25
    connection.write_timeout=2
    connection.dtr=False
    connection.rts=False
    try:
        with connection:
            connection.reset_input_buffer()
            connection.write(json.dumps({'id':1,'op':operation}).encode('utf-8')+b'\n')
            response=_read_usb_response(connection,1,5)
            if not response.get('ok'):
                raise SystemExit(f"Master rejected nightlight {mode}: {response.get('error','UNKNOWN')}")
            result=None
            end=time.monotonic()+3
            request_id=2
            while time.monotonic()<end:
                connection.write(json.dumps({'id':request_id,'op':'inspect'}).encode('utf-8')+b'\n')
                status=_read_usb_response(connection,request_id,2)
                if status.get('ok') and status.get('panelOutput')==desired:
                    result=status
                    break
                request_id+=1
                time.sleep(0.1)
    except serial.SerialException as exc:
        raise SystemExit(f'Could not use {port}: {exc}') from exc
    if not result or result.get('panelOutput')!=desired:
        observed=status.get('panelOutput') if 'status' in locals() and status else 'NO_RESPONSE'
        raise SystemExit(f'Master did not confirm nightlight {mode}: {observed}')
    print('Steady warm nightlight '+('on at 8/255.' if mode=='on' else 'off; panel is black.'))
    return result

def register_node(port,group,node_id,hardware_id,label):
    validate_registration(node_id,hardware_id,label)
    data=json.loads(Path(group).read_text(encoding='utf-8'))
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('Install pyserial in the active Python environment: python -m pip install pyserial') from exc
    try:
        with serial.Serial(port,115200,timeout=0.25,write_timeout=2) as connection:
            connection.reset_input_buffer()
            print(f'Checking the Master on {port}; then hold its BOOT button for 3 seconds.')
            end=time.monotonic()+300
            next_hello=0
            hello=None
            while time.monotonic()<end:
                now=time.monotonic()
                if now>=next_hello:
                    connection.write(b'{"id":0,"op":"hello"}\n')
                    next_hello=now+2
                line=connection.readline()
                if not line.startswith(b'{'):
                    continue
                try:
                    response=json.loads(line)
                except (UnicodeDecodeError,json.JSONDecodeError):
                    continue
                if response.get('id')==0 and response.get('ok'):
                    hello=response
                    if response.get('role') != 'master':
                        raise SystemExit('The connected board is not configured as a Master.')
                    if response.get('groupId') != data.get('groupId'):
                        raise SystemExit('The private group file does not match the connected Master.')
                if b'"event":"usbConfigOpened"' in line:
                    if hello is None:
                        raise SystemExit('Could not verify the Master group before opening USB setup.')
                    break
            else:
                raise SystemExit('Timed out. Check USB and hold BOOT for 3 seconds after the Master is running.')
            request={'id':1,'op':'register-node','groupId':data['groupId'],
                     'nodeId':node_id,'hardwareId':hardware_id,'label':label}
            connection.write(json.dumps(request,separators=(',',':')).encode('utf-8')+b'\n')
            end=time.monotonic()+30
            while time.monotonic()<end:
                line=connection.readline()
                if not line.startswith(b'{'):
                    continue
                try:
                    response=json.loads(line)
                except (UnicodeDecodeError,json.JSONDecodeError):
                    continue
                if response.get('id')==1:
                    if not response.get('ok'):
                        raise SystemExit(f"Master rejected registration: {response.get('error','UNKNOWN')}")
                    print(f'Follower node {node_id} registered as {label!r}; it is still unassigned and cannot join playback yet.')
                    return
            raise SystemExit('Master registration response timed out.')
    except serial.SerialException as exc:
        raise SystemExit(f'Could not use {port}: {exc}') from exc

def build_device_config(data,node_id,role,network_mode,master_address=None,master_node_id=None):
    if network_mode not in ('softap','infrastructure'):
        raise SystemExit('network-mode must be softap or infrastructure')
    if data.get('networkMode','softap') != network_mode:
        raise SystemExit('network mode must match the private group file')
    if not 1 <= node_id <= 65535:
        raise SystemExit('node-id must be in 1..65535')
    if role not in ('master','follower'):
        raise SystemExit('role must be master or follower')
    if role == 'follower':
        if master_node_id is None or not 1 <= master_node_id <= 65535 or master_node_id == node_id:
            raise SystemExit('a follower requires a distinct --master-node-id in 1..65535')
        if not master_address and network_mode == 'softap':
            master_address='192.168.4.1'
        if not master_address:
            raise SystemExit('infrastructure followers require --master-address (use a DHCP reservation)')
        if len(master_address) > 64 or not all(c.isascii() and (c.isalnum() or c in '.-:') for c in master_address):
            raise SystemExit('master-address must be an IP address or hostname up to 64 ASCII characters')
    else:
        if master_address or master_node_id is not None:
            raise SystemExit('master-address and master-node-id apply to followers only')
        master_node_id=node_id
    config={
        'version':2,
        'nodeId':node_id,
        'masterNodeId':master_node_id,
        'role':role,
        'groupId':data['groupId'],
        'networkMode':network_mode,
        'apPsk':data['apPsk'],
        'adminPassword':data['adminPassword'],
        'groupKey':data['groupKey'],
        'channel':6,
        'brightnessCap':64,
    }
    if network_mode == 'infrastructure':
        ssid=data.get('stationSsid','')
        password=data.get('stationPassword','')
        validate_wifi_credentials(ssid,password)
        config['stationSsid']=ssid
        config['stationPassword']=password
    if role == 'follower': config['masterAddress']=master_address
    return config

def plan_follower_pair(master,follower,data,node_id,label,network_mode=None,master_address=None,
                       master_port=None,follower_port=None):
    if master_port and follower_port and master_port.strip().casefold() == follower_port.strip().casefold():
        raise SystemExit('master-port and follower-port must identify different USB devices')
    mode=network_mode or data.get('networkMode','softap')
    if not master.get('provisioned') or master.get('role') != 'master':
        raise SystemExit('The connected Master port must be provisioned as role=master.')
    if not is_hardware_id(master.get('hardwareId')) or not is_hardware_id(follower.get('hardwareId')):
        raise SystemExit('Both boards must return a valid hardwareId from USB inspect.')
    if master['hardwareId'] == follower['hardwareId']:
        raise SystemExit('Master and Follower hardware IDs must be different.')
    if master.get('boardProfile') != follower.get('boardProfile'):
        raise SystemExit('Master and Follower board profiles do not match.')
    if master.get('groupId') != data.get('groupId'):
        raise SystemExit('The private group file does not match the connected Master.')
    if master.get('networkMode') != mode or data.get('networkMode','softap') != mode:
        raise SystemExit('The selected network mode must match both the Master and private group file.')
    master_node_id=master.get('nodeId')
    if not isinstance(master_node_id,int) or not 1 <= master_node_id <= 65535:
        raise SystemExit('The connected Master did not report a valid nodeId.')
    validate_registration(node_id,follower['hardwareId'],label)
    if node_id == master_node_id:
        raise SystemExit('Follower node-id must be different from the Master node-id.')
    already_configured=bool(follower.get('provisioned'))
    if already_configured and (follower.get('role') != 'follower' or
            follower.get('nodeId') != node_id or follower.get('masterNodeId') != master_node_id or
            follower.get('groupId') != data.get('groupId') or follower.get('networkMode') != mode):
        raise SystemExit('The Follower is already configured for another group or node. Refusing to overwrite it.')
    config=build_device_config(data,node_id,'follower',mode,master_address,master_node_id)
    return {'hardwareId':follower['hardwareId'],'masterNodeId':master_node_id,
            'networkMode':mode,'masterAddress':config.get('masterAddress'),
            'needsConfiguration':not already_configured}

def pair_follower(master_port,follower_port,group,node_id,label,network_mode=None,master_address=None,
                  dry_run=False):
    if master_port.strip().casefold() == follower_port.strip().casefold():
        raise SystemExit('master-port and follower-port must identify different USB devices')
    try:
        data=json.loads(Path(group).read_text(encoding='utf-8'))
    except (OSError,json.JSONDecodeError) as exc:
        raise SystemExit(f'Could not read the private group file: {exc}') from exc
    master=inspect(master_port,display=False)
    follower=inspect(follower_port,display=False)
    plan=plan_follower_pair(master,follower,data,node_id,label,network_mode,master_address,
                            master_port,follower_port)
    print(f"Verified Master node {plan['masterNodeId']} and Follower hardware {plan['hardwareId']}.")
    if dry_run:
        print('Dry run complete. No device settings or registry entries were changed.')
        print('A real run will request BOOT on the Follower, then on the Master.')
        return plan
    if plan['needsConfiguration']:
        configure(follower_port,group,node_id,'follower',plan['networkMode'],
                  master_address,plan['masterNodeId'])
    else:
        print(f'Follower node {node_id} is already configured for this Master; skipping USB configuration.')
    register_node(master_port,group,node_id,plan['hardwareId'],label)
    return plan

def configure(port,group,node_id,role,network_mode,master_address=None,master_node_id=None):
    try:
        import serial
    except ImportError as exc:
        raise SystemExit('Install pyserial in the active Python environment: python -m pip install pyserial') from exc
    data=json.loads(Path(group).read_text(encoding='utf-8'))
    config=build_device_config(data,node_id,role,network_mode,master_address,master_node_id)
    try:
        with serial.Serial(port,115200,timeout=0.25,write_timeout=2) as connection:
            connection.reset_input_buffer()
            print(f'USB setup on {port}. Hold the board BOOT button for 3 seconds.')
            print('Waiting up to 5 minutes for the physical setup window; secrets will not be printed.')
            end=time.monotonic()+300
            next_hello=0
            while time.monotonic()<end:
                now=time.monotonic()
                if now>=next_hello:
                    connection.write(b'{"id":0,"op":"hello"}\n')
                    next_hello=now+3
                line=connection.readline()
                if b'"event":"usbConfigOpened"' in line:
                    break
            else:
                raise SystemExit('Timed out. Check USB connection and press BOOT while the application is running.')
            payload=json.dumps({'id':1,'op':'configure','config':config},separators=(',',':')).encode('utf-8')+b'\n'
            connection.write(payload)
            end=time.monotonic()+30
            while time.monotonic()<end:
                line=connection.readline()
                if not line.startswith(b'{'):
                    continue
                try:
                    response=json.loads(line)
                except (UnicodeDecodeError,json.JSONDecodeError):
                    continue
                if response.get('id')==1:
                    if not response.get('ok'):
                        raise SystemExit(f"Device rejected configuration: {response.get('error','UNKNOWN')}")
                    print('Configuration saved. The device is restarting.')
                    if role == 'master' and network_mode == 'softap':
                        print(f"On the unit's private WPA2 network use http://192.168.4.1 (SSID suffix: {data['groupId'][-6:]}).")
                    elif role == 'master':
                        print('The Master obtains its address from the external AP; reserve that IP for this board and use it for the editor URL and Follower master-address.')
                    else:
                        print(f"Follower {node_id} is configured for {network_mode}; master address {master_address or '192.168.4.1'} is saved for synchronization setup.")
                    print('Use the management password stored in the private group file to log in.')
                    return
            raise SystemExit('Device configuration response timed out.')
    except serial.SerialException as exc:
        raise SystemExit(f'Could not use {port}: {exc}') from exc

def main():
    p=argparse.ArgumentParser(prog='ledpanel-provision');s=p.add_subparsers(dest='cmd',required=True)
    g=s.add_parser('create-group');g.add_argument('--output',required=True);g.add_argument('--network-mode',choices=['softap','infrastructure'],default='softap');g.add_argument('--ssid')
    f=s.add_parser('flash');f.add_argument('--port',required=True);f.add_argument('--release',required=True)
    i=s.add_parser('inspect');i.add_argument('--port',required=True)
    t=s.add_parser('test-panel',help='run the low-brightness pulsing RGB panel test over USB')
    t.add_argument('--port',required=True)
    nl=s.add_parser('nightlight',help='set a steady warm panel nightlight over USB')
    nl.add_argument('--port',required=True)
    nl.add_argument('--mode',choices=['on','off'],required=True)
    r=s.add_parser('register-node');r.add_argument('--port',required=True);r.add_argument('--group',required=True);r.add_argument('--node-id',type=int,required=True);r.add_argument('--hardware-id',required=True);r.add_argument('--label',required=True)
    c=s.add_parser('configure');c.add_argument('--port',required=True);c.add_argument('--group',required=True);c.add_argument('--node-id',type=int,required=True);c.add_argument('--role',choices=['master','follower'],required=True);c.add_argument('--network-mode',choices=['softap','infrastructure'],default='softap');c.add_argument('--master-address');c.add_argument('--master-node-id',type=int)
    pair=s.add_parser('pair-follower',help='verify two boards, configure a Follower, then register it on the Master')
    pair.add_argument('--master-port',required=True);pair.add_argument('--follower-port',required=True);pair.add_argument('--group',required=True)
    pair.add_argument('--node-id',type=int,required=True);pair.add_argument('--label',required=True)
    pair.add_argument('--network-mode',choices=['softap','infrastructure']);pair.add_argument('--master-address')
    pair.add_argument('--dry-run',action='store_true',help='inspect and validate both boards without writing settings')
    a=p.parse_args()
    if a.cmd=='create-group': group_file(a.output,a.network_mode,a.ssid)
    elif a.cmd=='flash': flash(a.port,a.release)
    elif a.cmd=='inspect': inspect(a.port)
    elif a.cmd=='test-panel': test_panel(a.port)
    elif a.cmd=='nightlight': nightlight(a.port,a.mode)
    elif a.cmd=='register-node': register_node(a.port,a.group,a.node_id,a.hardware_id,a.label)
    elif a.cmd=='configure': configure(a.port,a.group,a.node_id,a.role,a.network_mode,a.master_address,a.master_node_id)
    else: pair_follower(a.master_port,a.follower_port,a.group,a.node_id,a.label,a.network_mode,a.master_address,a.dry_run)

if __name__=='__main__': main()
