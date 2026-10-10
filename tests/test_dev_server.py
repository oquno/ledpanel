import http.client
import hashlib
import json
import threading
import unittest
from uuid import uuid4
from contextlib import closing
from tools.dev_server import create_server


class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = create_server(0)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        with self.server.simulator.lock:
            self.server.simulator.maintenance = False
            self.server.simulator.control_revision = 0
            self.server.simulator.state.update(revision=0, mode='stopped', running=False,
                                              blackout=True, packageHash=None,
                                              activePackageHash=None,
                                              candidatePackageHash=None)

    def request(self, path, body=None, headers=None, method=None):
        with closing(http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=5)) as conn:
            conn.request(method or ('GET' if body is None else 'POST'), path, body=body,
                         headers=headers or {'Content-Type': 'application/json'})
            r = conn.getresponse()
            return r.status, r.getheader('Content-Type'), r.read()

    def maintenance_request(self, enabled, expected_revision=None, csrf='simulator-csrf'):
        if expected_revision is None:
            expected_revision = json.loads(self.request('/api/v2/status')[2])['revision']
        payload = json.dumps(dict(requestId=str(uuid4()), expectedRevision=expected_revision))
        headers = {'Content-Type': 'application/json', 'X-CSRF-Token': csrf,
                   'Origin': f'http://127.0.0.1:{self.server.server_port}'}
        route = '/api/v2/maintenance/enter' if enabled else '/api/v2/maintenance/exit'
        return self.request(route, payload, headers)

    def test_editor_assets(self):
        for path, mime in [('/', 'text/html'), ('/app.js', 'text/javascript'), ('/style.css', 'text/css'),
                           ('/packages/formats/browser.mjs', 'text/javascript'),
                           ('/apps/editor/animation.mjs', 'text/javascript'),
                           ('/apps/editor/media.mjs', 'text/javascript'),
                           ('/tests/browser/index.html', 'text/html')]:
            with self.subTest(path=path):
                status, content_type, body = self.request(path)
                self.assertEqual(status, 200)
                self.assertTrue(content_type.startswith(mime))
                self.assertGreater(len(body), 0)

    def test_no_repository_or_traversal_access(self):
        for path in ['/tools/dev_server.py', '/.git/config', '/tmp/group.private.json',
                     '/packages/../README.md', '/examples/%2e%2e/README.md', '/packages/%2e%2e%5cREADME.md']:
            with self.subTest(path=path):
                self.assertEqual(self.request(path)[0], 404)

    def test_registry_keeps_arbitrary_ids(self):
        status, _, body = self.request('/api/v2/nodes')
        self.assertEqual(status, 200)
        self.assertEqual([n['nodeId'] for n in json.loads(body)['nodes']], [7, 12, 42, 100])

    def test_simulator_reports_only_capabilities_it_implements(self):
        status, _, body = self.request('/api/v2/capabilities')
        self.assertEqual(status, 200)
        features = json.loads(body)['features']
        self.assertFalse(features['multiNodeSync'])
        self.assertFalse(features['safeStateTransactions'])
        self.assertFalse(features['topologyDistribution'])
        self.assertFalse(features['packageDistribution'])
        self.assertFalse(features['playback'])

    def test_rig_update_saves_only_local_draft(self):
        self.assertEqual(self.maintenance_request(True)[0], 202)
        status, _, body = self.request('/api/v2/rig')
        self.assertEqual(status, 200)
        current = json.loads(body)
        self.assertNotIn('groupKey', body.decode())
        self.assertNotIn('apPsk', body.decode())
        rig = current['rig']
        payload = json.dumps(dict(requestId=str(uuid4()), expectedRevision=current['controlRevision'],
                                  expectedTopologyRevision=current['rigRevision'], rig=rig))
        headers = {'Content-Type': 'application/json', 'X-CSRF-Token': 'simulator-csrf',
                   'Origin': f'http://127.0.0.1:{self.server.server_port}'}
        status, _, result_bytes = self.request('/api/v2/rig', payload, headers, method='PUT')
        self.assertEqual(status, 200)
        result = json.loads(result_bytes)
        self.assertEqual(result['status'], 'saved_local_pending_topology')
        self.assertFalse(result['topologySynchronized'])
        updated = json.loads(self.request('/api/v2/rig')[2])
        self.assertEqual(updated['rigRevision'], current['rigRevision'] + 1)
        self.assertEqual(updated['topologyRevision'], current['topologyRevision'])
        self.assertFalse(updated['topologySynchronized'])
        self.assertEqual(json.loads(self.request('/api/v2/status')[2])['state']['revision'], 0)
        nodes = json.loads(self.request('/api/v2/nodes')[2])['nodes']
        self.assertTrue(all(not node['ready'] for node in nodes))

    def test_rig_update_rejects_stale_revisions_and_bad_csrf(self):
        self.assertEqual(self.maintenance_request(True)[0], 202)
        current = json.loads(self.request('/api/v2/rig')[2])
        payload = json.dumps(dict(requestId=str(uuid4()), expectedRevision=99,
                                  expectedTopologyRevision=current['rigRevision'], rig=current['rig']))
        headers = {'Content-Type': 'application/json', 'X-CSRF-Token': 'simulator-csrf',
                   'Origin': f'http://127.0.0.1:{self.server.server_port}'}
        self.assertEqual(self.request('/api/v2/rig', payload, headers, method='PUT')[0], 409)
        headers['X-CSRF-Token'] = 'wrong'
        self.assertEqual(self.request('/api/v2/rig', payload, headers, method='PUT')[0], 403)

    def test_maintenance_requires_stopped_blackout_and_updates_revision(self):
        status, _, body = self.request('/api/v2/status')
        self.assertEqual(status, 200)
        current_revision = json.loads(body)['revision']
        self.assertEqual(self.maintenance_request(True, csrf='wrong')[0], 403)
        self.assertEqual(self.maintenance_request(True, current_revision + 1)[0], 409)
        with self.server.simulator.lock:
            self.server.simulator.state.update(mode='cue', running=True, blackout=False)
        self.assertEqual(self.maintenance_request(True)[0], 409)
        with self.server.simulator.lock:
            self.server.simulator.state.update(mode='stopped', running=False, blackout=True)
        status, _, result_bytes = self.maintenance_request(True)
        self.assertEqual(status, 202)
        result = json.loads(result_bytes)
        self.assertTrue(result['maintenance'])
        self.assertEqual(result['acceptedRevision'], current_revision + 1)
        state = json.loads(self.request('/api/v2/status')[2])
        self.assertTrue(state['maintenance'])
        self.assertEqual(state['revision'], current_revision + 1)
        self.assertEqual(state['state']['revision'], 0)
        self.assertTrue(json.loads(self.request('/api/v2/rig')[2])['editable'])
        status, _, result_bytes = self.maintenance_request(False)
        self.assertEqual(status, 202)
        self.assertFalse(json.loads(result_bytes)['maintenance'])
        self.assertFalse(json.loads(self.request('/api/v2/status')[2])['maintenance'])

    def test_package_stage_and_activation_are_gated_by_maintenance(self):
        status, _, package = self.request('/examples/minimal.lpk')
        self.assertEqual(status, 200)
        package_hash = hashlib.sha256(package).hexdigest()
        headers = {'Content-Type': 'application/vnd.ledpanel.package',
                   'X-CSRF-Token': 'simulator-csrf',
                   'X-Expected-Revision': str(json.loads(self.request('/api/v2/status')[2])['revision']),
                   'X-Content-SHA256': package_hash,
                   'Origin': f'http://127.0.0.1:{self.server.server_port}'}
        self.assertEqual(self.request('/api/v2/deployments', package, headers, method='POST')[0], 409)
        self.assertEqual(self.maintenance_request(True)[0], 202)
        headers['X-Expected-Revision'] = str(json.loads(self.request('/api/v2/status')[2])['revision'])
        status, _, result_bytes = self.request('/api/v2/deployments', package, headers, method='POST')
        self.assertEqual(status, 201)
        self.assertEqual(json.loads(result_bytes)['packageHash'], package_hash)
        state = json.loads(self.request('/api/v2/status')[2])
        self.assertEqual(state['state']['candidatePackageHash'], package_hash)
        self.assertEqual(state['state']['revision'], 0)
        payload = json.dumps(dict(packageHash=package_hash, expectedRevision=state['revision']))
        headers = {'Content-Type': 'application/json', 'X-CSRF-Token': 'simulator-csrf',
                   'Origin': f'http://127.0.0.1:{self.server.server_port}'}
        status, _, result_bytes = self.request('/api/v2/deployments/activate', payload,
                                                headers, method='POST')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(result_bytes)['status'], 'active_but_panel_dark')
        state = json.loads(self.request('/api/v2/status')[2])
        self.assertEqual(state['state']['activePackageHash'], package_hash)
        self.assertIsNone(state['state']['candidatePackageHash'])
        self.assertEqual(state['state']['revision'], 1)

    def test_commands_do_not_implicitly_release_blackout(self):
        for op, args in [('blackout', {'enabled': True}), ('stop', {}), ('brightness', {'value': 40})]:
            state = json.loads(self.request('/api/v2/status')[2])
            payload = json.dumps(dict(op=op, args=args, expectedRevision=state['revision']))
            self.assertEqual(self.request('/api/v2/commands', payload)[0], 202)
        state = json.loads(self.request('/api/v2/status')[2])['state']
        self.assertTrue(state['blackout'])
        self.assertEqual(state['userBrightness'], 40)

    def test_unsafe_playback_commands_stay_rejected_and_dark(self):
        for op, args, expected_status in [
                ('go', {'cueId': 'cue_1'}, 400),
                ('blackout', {'enabled': False}, 503)]:
            state = json.loads(self.request('/api/v2/status')[2])
            payload = json.dumps(dict(op=op, args=args, expectedRevision=state['revision']))
            status, _, body = self.request('/api/v2/commands', payload)
            self.assertEqual(status, expected_status, body)
            after = json.loads(self.request('/api/v2/status')[2])
            self.assertTrue(after['state']['blackout'])
            self.assertEqual(after['revision'], state['revision'])

    def test_commands_require_current_revision(self):
        before = json.loads(self.request('/api/v2/status')[2])
        payload = json.dumps(dict(op='brightness', args={'value': 40}, expectedRevision=99))
        self.assertEqual(self.request('/api/v2/commands', payload)[0], 409)
        state = json.loads(self.request('/api/v2/status')[2])
        self.assertEqual(state['state']['userBrightness'], before['state']['userBrightness'])
        self.assertTrue(state['state']['blackout'])

    def test_invalid_commands_and_external_origin(self):
        for body in ['{', '[]', '{"op":"unknown"}', '{"op":"blackout","args":{"enabled":1}}']:
            self.assertEqual(self.request('/api/v2/commands', body)[0], 400)
        self.assertEqual(self.request('/api/v2/commands', '{}',
                                     {'Content-Type': 'application/json', 'Origin': 'http://example.com'})[0], 403)


if __name__ == '__main__':
    unittest.main()
