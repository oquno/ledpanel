"""Local editor and in-memory API mock. No hardware transport or production auth."""
import argparse
import copy
import hashlib
import json
import webbrowser
import re
import struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Lock
from urllib.parse import unquote, urlsplit
from uuid import UUID, uuid4

ROOT = Path(__file__).resolve().parents[1]
MAX_BODY = 1024 * 1024
MIME = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
        '.mjs': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
        '.json': 'application/json; charset=utf-8', '.png': 'image/png', '.jpg': 'image/jpeg',
        '.webm': 'video/webm'}


class Simulator:
    def __init__(self, rig):
        self.lock = Lock()
        self.maintenance = False
        self.control_revision = 0
        self.rig = copy.deepcopy(rig)
        self.topology = dict(revision=0, hash='0' * 64, layoutId='horizontal')
        bindings = next((x['bindings'] for x in rig['layouts'] if x['layoutId'] == 'horizontal'), [])
        slots = {b['nodeId']: b['slotId'] for b in bindings}
        self.nodes = [dict(n, online=True, ready=False, slotId=slots.get(n['nodeId']),
                           firmwareVersion='simulator-0.1.0', activeHash=None,
                           candidateHash=None, syncState='fault',
                           faultCode='PANEL_PROFILE_UNVERIFIED', clockSynced=True,
                           clockOffsetUs=0, clockRttUs=0, clockSampleAgeMs=0)
                      for n in rig['nodes']]
        self.state = dict(revision=0, topologyRevision=self.topology['revision'], topologyHash=self.topology['hash'],
                          packageHash=None, layoutId='horizontal', mode='stopped', sceneId=None,
                          cueId=None, running=False, positionTicks='0', anchorUs='0', effectiveAtUs='0',
                          blackout=True, userBrightness=26, activePackageHash=None,
                          candidatePackageHash=None)

    def get(self, route):
        with self.lock:
            routes = {
                'health': dict(ok=True, apiMajor=2, bootId='simulator', networkMode='softap', simulator=True),
                'provisioning': dict(provisioned=True, setup='login', networkMode='softap'),
                'auth/csrf': dict(csrfToken='simulator-csrf'),
                'status': dict(revision=self.control_revision, state=self.state, topology=self.topology,
                               nodes=self.nodes, maintenance=self.maintenance,
                               maintenanceScope='simulated_fleet', simulator=True),
                'nodes': dict(nodes=self.nodes, topologyRevision=self.topology['revision'], topologyHash=self.topology['hash']),
                'rig': dict(rig=self.rig, controlRevision=self.control_revision,
                            topologyRevision=self.topology['revision'], rigRevision=self.rig['revision'],
                            editable=(self.maintenance and self.state['mode'] == 'stopped' and
                                      not self.state['running'] and self.state['blackout']),
                            maintenance=self.maintenance, maintenanceScope='simulated_fleet',
                            topologySynchronized=(self.rig['revision'] == self.topology['revision'])),
                'capabilities': dict(apiMajor=2, protocolMajor=2, renderer='lp-raster-1',
                                     maxActiveNodes=4, maxRegisteredNodes=64, maxLayouts=16,
                                     maxSlotsPerLayout=16, networkModes=['softap', 'infrastructure'],
                                     limits=dict(maxNodes=4, maxLayouts=16),
                                     features=dict(multiNodeSync=False, safeStateTransactions=False,
                                                   topologyDistribution=False,
                                                   packageDistribution=False,
                                                   panelDiagnosticTest=False, playback=False),
                                     panelReady=False, panelOutput='disabled_black',
                                     panelFault='PANEL_PROFILE_UNVERIFIED'),
            }
            return copy.deepcopy(routes.get(route))

    def change_maintenance(self, body, enabled):
        if not isinstance(body, dict) or set(body) != {'requestId', 'expectedRevision'}:
            raise ValueError('INVALID_REQUEST')
        try:
            if str(UUID(body['requestId'])) != body['requestId']:
                raise ValueError('INVALID_REQUEST')
        except (ValueError, TypeError, AttributeError):
            raise ValueError('INVALID_REQUEST')
        with self.lock:
            if type(body['expectedRevision']) is not int or body['expectedRevision'] != self.control_revision:
                raise ValueError('REVISION_CONFLICT')
            if self.state['mode'] != 'stopped' or self.state['running'] or not self.state['blackout']:
                raise ValueError('MAINTENANCE_REQUIRES_STOPPED_BLACKOUT')
            if self.maintenance == enabled:
                raise ValueError('MAINTENANCE_ALREADY_ACTIVE' if enabled else 'MAINTENANCE_NOT_ACTIVE')
            self.maintenance = enabled
            self.control_revision += 1
            return dict(status='maintenance' if enabled else 'normal', maintenance=enabled,
                        maintenanceScope='simulated_fleet', acceptedRevision=self.control_revision,
                        simulator=True)

    def stage_package(self, data, expected_revision, declared_hash):
        if len(data) < 64 or len(data) > 2621440 or data[:4] != b'LPK2':
            raise ValueError('PACKAGE_INVALID')
        major, minor, header_size, manifest_length, blob_length, total_length = struct.unpack_from('<HHIIII', data, 4)
        if (major, minor, header_size, total_length) != (2, 0, 64, len(data)) or \
                total_length != 64 + manifest_length + blob_length or manifest_length > 131072 or \
                data[56:64] != b'\0' * 8 or hashlib.sha256(data[64:]).digest() != data[24:56]:
            raise ValueError('PACKAGE_INVALID')
        package_hash = hashlib.sha256(data).hexdigest()
        if package_hash != declared_hash:
            raise ValueError('PACKAGE_HASH_MISMATCH')
        try:
            manifest = json.loads(data[64:64 + manifest_length])
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ValueError('PACKAGE_INVALID') from None
        if not isinstance(manifest, dict) or manifest.get('version') != 2 or \
                manifest.get('runtimeMinor') != 0 or manifest.get('renderer') != 'lp-raster-1':
            raise ValueError('PACKAGE_INVALID')
        with self.lock:
            if not self.maintenance:
                raise ValueError('MAINTENANCE_REQUIRED')
            if type(expected_revision) is not int or expected_revision != self.control_revision:
                raise ValueError('REVISION_CONFLICT')
            if self.state['mode'] != 'stopped' or self.state['running'] or not self.state['blackout']:
                raise ValueError('MAINTENANCE_STOPPED_BLACKOUT_REQUIRED')
            self.state['candidatePackageHash'] = package_hash
            self.control_revision += 1
            return dict(status='validated_candidate', packageHash=package_hash,
                        packageBytes=len(data), acceptedRevision=self.control_revision, simulator=True)

    def activate_package(self, body):
        if not isinstance(body, dict) or set(body) != {'packageHash', 'expectedRevision'} or \
                not isinstance(body['packageHash'], str) or not re.fullmatch(r'[0-9a-f]{64}', body['packageHash']):
            raise ValueError('INVALID_REQUEST')
        with self.lock:
            if not self.maintenance:
                raise ValueError('MAINTENANCE_STOPPED_BLACKOUT_REQUIRED')
            if type(body['expectedRevision']) is not int or body['expectedRevision'] != self.control_revision:
                raise ValueError('REVISION_CONFLICT')
            if self.state['mode'] != 'stopped' or self.state['running'] or not self.state['blackout']:
                raise ValueError('MAINTENANCE_STOPPED_BLACKOUT_REQUIRED')
            if self.state['candidatePackageHash'] != body['packageHash']:
                raise ValueError('CANDIDATE_NOT_FOUND')
            self.state['activePackageHash'] = body['packageHash']
            self.state['candidatePackageHash'] = None
            self.state['revision'] += 1
            self.control_revision += 1
            return dict(status='active_but_panel_dark', packageHash=body['packageHash'],
                        acceptedRevision=self.control_revision, stateRevision=self.state['revision'],
                        panelFault='PANEL_PROFILE_UNVERIFIED',
                        simulator=True)

    def command(self, body):
        if not isinstance(body, dict) or not isinstance(body.get('args', {}), dict) or \
                type(body.get('expectedRevision')) is not int:
            raise ValueError('Expected a command object and args object')
        op, args = body.get('op'), body.get('args', {})
        if op not in ('blackout', 'stop', 'brightness'):
            raise ValueError('Unsupported simulator command')
        if op == 'blackout' and args.get('enabled') is not True:
            raise ValueError('PANEL_PROFILE_UNVERIFIED')
        if op == 'brightness' and (type(args.get('value')) is not int or not 0 <= args['value'] <= 255):
            raise ValueError('brightness must be 0..255')
        with self.lock:
            s = self.state
            if body['expectedRevision'] != self.control_revision:
                raise ValueError('REVISION_CONFLICT')
            if op == 'blackout':
                s['blackout'] = True
            elif op == 'brightness':
                s['userBrightness'] = min(args['value'], 64)
            elif op == 'stop':
                s.update(mode='stopped', running=False, sceneId=None, cueId=None,
                         positionTicks='0', blackout=True)
            s['revision'] += 1
            self.control_revision += 1
            return dict(operationId=str(uuid4()), status='applied', acceptedRevision=self.control_revision,
                        revision=s['revision'], blackout=True, simulator=True)

    def update_rig(self, body):
        if not isinstance(body, dict) or set(body) != {
                'requestId', 'expectedRevision', 'expectedTopologyRevision', 'rig'}:
            raise ValueError('INVALID_REQUEST')
        try:
            if str(UUID(body['requestId'])) != body['requestId']:
                raise ValueError('INVALID_REQUEST')
        except (ValueError, TypeError, AttributeError):
            raise ValueError('INVALID_REQUEST')
        with self.lock:
            if not self.maintenance:
                raise ValueError('RIG_REQUIRES_MAINTENANCE_STOPPED_BLACKOUT')
            if type(body['expectedRevision']) is not int or body['expectedRevision'] != self.control_revision:
                raise ValueError('REVISION_CONFLICT')
            if type(body['expectedTopologyRevision']) is not int or \
                    body['expectedTopologyRevision'] != self.rig['revision']:
                raise ValueError('TOPOLOGY_REVISION_CONFLICT')
            if self.state['mode'] != 'stopped' or self.state['running'] or not self.state['blackout']:
                raise ValueError('RIG_REQUIRES_STOPPED_BLACKOUT')
            rig = body['rig']
            if not isinstance(rig, dict) or type(rig.get('revision')) is not int or \
                    rig.get('revision') != self.rig['revision']:
                raise ValueError('TOPOLOGY_REVISION_CONFLICT')
            if set(rig) != {'version', 'groupId', 'masterNodeId', 'revision', 'nodes', 'layouts'} or \
                    rig.get('version') != 2 or rig.get('groupId') != self.rig['groupId'] or \
                    rig.get('masterNodeId') != self.rig['masterNodeId'] or \
                    not isinstance(rig.get('nodes'), list) or not 1 <= len(rig['nodes']) <= 64 or \
                    not isinstance(rig.get('layouts'), list) or len(rig['layouts']) > 16:
                raise ValueError('RIG_INVALID')
            ids, hardware_ids, enabled_ids = set(), set(), set()
            master_found = False
            for node in rig['nodes']:
                if not isinstance(node, dict) or set(node) != {
                        'nodeId', 'hardwareId', 'label', 'role', 'enabled', 'boardProfile',
                        'rotation', 'brightnessCap'}:
                    raise ValueError('RIG_INVALID')
                node_id, hardware_id = node['nodeId'], node['hardwareId']
                if type(node_id) is not int or not 1 <= node_id <= 65535 or node_id in ids or \
                        not isinstance(hardware_id, str) or not re.fullmatch(
                            r'[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', hardware_id) or \
                        hardware_id in hardware_ids or not isinstance(node['label'], str) or \
                        not 1 <= len(node['label']) <= 80 or any(ord(ch) < 32 or 127 <= ord(ch) <= 159
                                                                  for ch in node['label']) or \
                        node['role'] not in ('master', 'follower') or type(node['enabled']) is not bool or \
                        not isinstance(node['boardProfile'], str) or not re.fullmatch(
                            r'[a-z][a-z0-9_-]{0,31}', node['boardProfile']) or \
                        node['rotation'] not in (0, 180) or type(node['rotation']) is not int or \
                        type(node['brightnessCap']) is not int or not 0 <= node['brightnessCap'] <= 255:
                    raise ValueError('RIG_INVALID')
                ids.add(node_id)
                hardware_ids.add(hardware_id)
                if node['enabled']:
                    enabled_ids.add(node_id)
                if node['role'] == 'master':
                    original_master = next(item for item in self.rig['nodes'] if item['role'] == 'master')
                    if master_found or node_id != rig['masterNodeId'] or not node['enabled'] or \
                            hardware_id != original_master['hardwareId']:
                        raise ValueError('RIG_INVALID')
                    master_found = True
            if not master_found or len(enabled_ids) > 16:
                raise ValueError('RIG_INVALID')
            layout_ids = set()
            for layout in rig['layouts']:
                if not isinstance(layout, dict) or set(layout) != {'layoutId', 'bindings'} or \
                        not isinstance(layout['layoutId'], str) or not re.fullmatch(
                            r'[a-z][a-z0-9_-]{0,31}', layout['layoutId']) or \
                        layout['layoutId'] in layout_ids or not isinstance(layout['bindings'], list) or \
                        len(layout['bindings']) > 16:
                    raise ValueError('RIG_INVALID')
                layout_ids.add(layout['layoutId'])
                slots, bound_nodes = set(), set()
                for binding in layout['bindings']:
                    if not isinstance(binding, dict) or set(binding) != {'slotId', 'nodeId'} or \
                            not isinstance(binding['slotId'], str) or not re.fullmatch(
                                r'[a-z][a-z0-9_-]{0,31}', binding['slotId']) or \
                            type(binding['nodeId']) is not int or binding['nodeId'] not in enabled_ids or \
                            binding['slotId'] in slots or binding['nodeId'] in bound_nodes:
                        raise ValueError('RIG_INVALID')
                    slots.add(binding['slotId'])
                    bound_nodes.add(binding['nodeId'])
            self.rig = copy.deepcopy(rig)
            self.rig['revision'] += 1
            self.control_revision += 1
            slot_map = {binding['nodeId']: binding['slotId'] for layout in self.rig['layouts']
                        if layout['layoutId'] == self.topology['layoutId']
                        for binding in layout['bindings']}
            old_nodes = {node['nodeId']: node for node in self.nodes}
            self.nodes = [dict(node, online=bool(node['enabled']), ready=False,
                               slotId=slot_map.get(node['nodeId']),
                               firmwareVersion=old_nodes.get(node['nodeId'], {}).get('firmwareVersion'),
                               activeHash=old_nodes.get(node['nodeId'], {}).get('activeHash'))
                          for node in self.rig['nodes']]
            return dict(status='saved_local_pending_topology', acceptedRevision=self.control_revision,
                        rigRevision=self.rig['revision'], topologyRevision=self.topology['revision'],
                        topologySynchronized=False, simulator=True)


class Handler(BaseHTTPRequestHandler):
    def reply(self, status, body, mime='application/json; charset=utf-8'):
        if not isinstance(body, bytes):
            body = json.dumps(body, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        if self.command != 'HEAD':
            try:
                self.wfile.write(body)
            except ConnectionError:
                pass  # A browser may cancel an in-flight request on navigation.

    def error(self, status, code):
        self.reply(status, {'error': {'code': code}})

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        route = unquote(urlsplit(self.path).path)
        if route.startswith('/api/v2/'):
            result = self.server.simulator.get(route[len('/api/v2/'):])
            return self.error(404, 'NOT_FOUND') if result is None else self.reply(200, result)
        direct = {'/': ROOT / 'apps/editor/index.html', '/app.js': ROOT / 'apps/editor/app.js',
                  '/style.css': ROOT / 'apps/editor/style.css',
                  '/apps/editor/animation.mjs': ROOT / 'apps/editor/animation.mjs',
                  '/apps/editor/deployment.mjs': ROOT / 'apps/editor/deployment.mjs',
                  '/apps/editor/media.mjs': ROOT / 'apps/editor/media.mjs'}
        target = direct.get(route)
        if target is None:
            # Only browser code, test pages, and public samples are served. Never serve repository secrets.
            for prefix in ('packages/', 'examples/', 'tests/browser/'):
                if route.startswith('/' + prefix):
                    base = (ROOT / prefix).resolve()
                    target = (base / route[len(prefix) + 1:]).resolve()
                    if not target.is_relative_to(base):
                        return self.error(404, 'NOT_FOUND')
                    break
        try:
            if target is None or not target.is_file() or target.suffix not in (*MIME, '.lpk', '.ledproj'):
                return self.error(404, 'NOT_FOUND')
            self.reply(200, target.read_bytes(), MIME.get(target.suffix, 'application/octet-stream'))
        except (OSError, ValueError):
            self.error(404, 'NOT_FOUND')

    def do_POST(self):
        route = urlsplit(self.path).path
        maintenance_routes = {'/api/v2/maintenance/enter': True,
                              '/api/v2/maintenance/exit': False}
        package_routes = {'/api/v2/deployments', '/api/v2/deployments/activate'}
        if route != '/api/v2/commands' and route not in maintenance_routes and route not in package_routes:
            return self.error(404, 'NOT_FOUND')
        origin = self.headers.get('Origin')
        if origin and origin != 'http://' + self.headers.get('Host', ''):
            return self.error(403, 'ORIGIN_MISMATCH')
        if self.headers.get('Transfer-Encoding'):
            return self.error(400, 'INVALID_REQUEST')
        if route == '/api/v2/deployments':
            if self.headers.get_content_type() != 'application/vnd.ledpanel.package':
                return self.error(415, 'PACKAGE_CONTENT_TYPE_REQUIRED')
        elif self.headers.get_content_type() != 'application/json':
            return self.error(415, 'JSON_REQUIRED')
        if route in maintenance_routes or route in package_routes:
            if self.headers.get('X-CSRF-Token') != 'simulator-csrf':
                return self.error(403, 'AUTH_OR_CSRF_REQUIRED')
        if route == '/api/v2/deployments':
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 64 <= length <= 2621440:
                    return self.error(413, 'PACKAGE_SIZE_LIMIT')
                expected_revision = int(self.headers.get('X-Expected-Revision', ''))
                declared_hash = self.headers.get('X-Content-SHA256', '')
                if not re.fullmatch(r'[0-9a-f]{64}', declared_hash):
                    return self.error(400, 'INVALID_PACKAGE_HEADERS')
                self.connection.settimeout(15)
                data = self.rfile.read(length)
                if len(data) != length:
                    return self.error(400, 'PACKAGE_BODY_INCOMPLETE')
                result = self.server.simulator.stage_package(data, expected_revision, declared_hash)
            except (ValueError, OSError) as error:
                code = str(error) or 'INVALID_REQUEST'
                status = 409 if code in ('REVISION_CONFLICT', 'MAINTENANCE_REQUIRED',
                                          'MAINTENANCE_STOPPED_BLACKOUT_REQUIRED') else \
                    422 if code in ('PACKAGE_INVALID', 'PACKAGE_HASH_MISMATCH') else 400
                return self.error(status, code)
            return self.reply(201, result)
        if route == '/api/v2/deployments/activate':
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 4096:
                    return self.error(413, 'BODY_SIZE')
                self.connection.settimeout(5)
                body = json.loads(self.rfile.read(length))
                result = self.server.simulator.activate_package(body)
            except (ValueError, OSError) as error:
                code = str(error) or 'INVALID_REQUEST'
                status = 409 if code in ('REVISION_CONFLICT', 'MAINTENANCE_STOPPED_BLACKOUT_REQUIRED',
                                          'CANDIDATE_NOT_FOUND') else 400
                return self.error(status, code)
            return self.reply(200, result)
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= MAX_BODY:
                return self.error(413, 'BODY_SIZE')
            self.connection.settimeout(5)
            body = json.loads(self.rfile.read(length))
            result = (self.server.simulator.command(body) if route == '/api/v2/commands' else
                      self.server.simulator.change_maintenance(body, maintenance_routes[route]))
        except (ValueError, OSError) as error:
            code = str(error) or 'INVALID_REQUEST'
            status = 503 if code == 'PANEL_PROFILE_UNVERIFIED' else \
                409 if code.endswith('CONFLICT') or code.startswith('MAINTENANCE_') else 400
            return self.error(status, code)
        self.reply(202, result)

    def do_PUT(self):
        if urlsplit(self.path).path != '/api/v2/rig':
            return self.error(404, 'NOT_FOUND')
        origin = self.headers.get('Origin')
        if origin and origin != 'http://' + self.headers.get('Host', ''):
            return self.error(403, 'ORIGIN_MISMATCH')
        if self.headers.get('X-CSRF-Token') != 'simulator-csrf':
            return self.error(403, 'AUTH_OR_CSRF_REQUIRED')
        if self.headers.get('Transfer-Encoding') or self.headers.get_content_type() != 'application/json':
            return self.error(415, 'JSON_REQUIRED')
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 40 * 1024:
                return self.error(413, 'RIG_BODY_SIZE')
            self.connection.settimeout(5)
            body = json.loads(self.rfile.read(length))
            result = self.server.simulator.update_rig(body)
        except (ValueError, OSError) as error:
            code = str(error) or 'INVALID_REQUEST'
            status = 409 if code.endswith('CONFLICT') or code.startswith('RIG_REQUIRES_') else \
                422 if code == 'RIG_INVALID' else 400
            return self.error(status, code)
        self.reply(200, result)


def create_server(port=4173, rig_path=None):
    rig = json.loads(Path(rig_path or ROOT / 'examples/demo.ledrig.json').read_text(encoding='utf-8'))
    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.simulator = Simulator(rig)
    return server


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=4173)
    parser.add_argument('--open', action='store_true', help='Open the editor in the default browser')
    parser.add_argument('--rig', type=Path, help='Secret-free node registry JSON; default is the 4-node sample')
    args = parser.parse_args()
    with create_server(args.port, args.rig) as server:
        print(f'Editor (mock devices): http://127.0.0.1:{server.server_port}', flush=True)
        if args.open:
            webbrowser.open(f'http://127.0.0.1:{server.server_port}')
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
