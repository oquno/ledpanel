import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import provision


GROUP = {
    'groupId': '2a68e6b7-a111-4c8c-a000-123456789abc',
    'apPsk': 'not-a-real-secret',
    'adminPassword': 'not-a-real-secret',
    'groupKey': '0' * 64,
    'networkMode': 'softap',
}

MASTER = {
    'provisioned': True,
    'boardProfile': 'seengreat_hub75_s3_n16r8_v1',
    'hardwareId': '00000000-0000-4000-8000-aabbccddeeff',
    'nodeId': 1,
    'masterNodeId': 1,
    'role': 'master',
    'groupId': GROUP['groupId'],
    'networkMode': 'softap',
}
FOLLOWER = {
    'provisioned': False,
    'boardProfile': 'seengreat_hub75_s3_n16r8_v1',
    'hardwareId': '00000000-0000-4000-8000-112233445566',
}


class ProvisioningTests(unittest.TestCase):
    def test_usb_response_reader_skips_logs_and_unrelated_ids(self):
        class FakeSerial:
            def __init__(self):
                self.lines=iter((b'I (123) usb-setup: ready\r\n',
                                b'{"id":2,"ok":true}\r\n',
                                b'{"id":3,"ok":true,"panelOutput":"disabled_black"}\r\n'))
            def readline(self):
                return next(self.lines,b'')
        response=provision._read_usb_response(FakeSerial(),3,1)
        self.assertEqual(response['panelOutput'],'disabled_black')

    def test_hardware_id_is_canonical_lowercase_uuid(self):
        self.assertTrue(provision.is_hardware_id('00000000-0000-4000-8000-aabbccddeeff'))
        self.assertFalse(provision.is_hardware_id('00000000000040008000aabbccddeeff'))
        self.assertFalse(provision.is_hardware_id('00000000-0000-4000-8000-AABBCCDDEEFF'))

    def test_registration_rejects_invalid_ids_and_control_characters(self):
        with self.assertRaises(SystemExit):
            provision.validate_registration(0, '00000000-0000-4000-8000-aabbccddeeff', 'Panel')
        with self.assertRaises(SystemExit):
            provision.validate_registration(2, 'not-a-uuid', 'Panel')
        with self.assertRaises(SystemExit):
            provision.validate_registration(2, '00000000-0000-4000-8000-aabbccddeeff', 'bad\nlabel')
        provision.validate_registration(2, '00000000-0000-4000-8000-aabbccddeeff', 'パネルB')

    def test_master_config_uses_its_own_node_id(self):
        config = provision.build_device_config(GROUP, 7, 'master', 'softap')
        self.assertEqual(config['nodeId'], 7)
        self.assertEqual(config['masterNodeId'], 7)
        self.assertNotIn('masterAddress', config)

    def test_follower_config_targets_explicit_master_and_private_network(self):
        config = provision.build_device_config(GROUP, 42, 'follower', 'softap', master_node_id=7)
        self.assertEqual(config['masterNodeId'], 7)
        self.assertEqual(config['masterAddress'], '192.168.4.1')
        self.assertEqual(config['groupId'], GROUP['groupId'])
        self.assertEqual(config['apPsk'], GROUP['apPsk'])
        custom = provision.build_device_config(
            GROUP, 42, 'follower', 'softap', 'panel-master.local', 7)
        self.assertEqual(custom['masterAddress'], 'panel-master.local')

    def test_follower_requires_distinct_master_id(self):
        with self.assertRaises(SystemExit):
            provision.build_device_config(GROUP, 7, 'follower', 'softap', master_node_id=7)
        with self.assertRaises(SystemExit):
            provision.build_device_config(GROUP, 42, 'follower', 'softap')

    def test_infrastructure_mode_includes_network_credentials(self):
        group = {**GROUP, 'networkMode': 'infrastructure',
                 'stationSsid': 'studio-wifi', 'stationPassword': 'eightchars'}
        master = provision.build_device_config(group, 7, 'master', 'infrastructure')
        self.assertEqual(master['networkMode'], 'infrastructure')
        self.assertEqual(master['stationSsid'], 'studio-wifi')
        self.assertEqual(master['stationPassword'], 'eightchars')
        follower = provision.build_device_config(
            group, 42, 'follower', 'infrastructure',
            master_address='192.168.1.50', master_node_id=7)
        self.assertEqual(follower['masterAddress'], '192.168.1.50')
        self.assertEqual(follower['stationSsid'], 'studio-wifi')

    def test_infrastructure_requires_matching_group_credentials(self):
        with self.assertRaises(SystemExit):
            provision.build_device_config(GROUP, 7, 'master', 'infrastructure')
        group = {**GROUP, 'networkMode': 'infrastructure', 'stationSsid': 'wifi',
                 'stationPassword': 'short'}
        with self.assertRaises(SystemExit):
            provision.build_device_config(group, 7, 'master', 'infrastructure')

        valid = {**GROUP, 'networkMode': 'infrastructure', 'stationSsid': 'wifi',
                 'stationPassword': 'long-enough'}
        with self.assertRaises(SystemExit):
            provision.build_device_config(valid, 42, 'follower', 'infrastructure',
                                          master_node_id=7)

    def test_private_group_creation_can_capture_external_wifi_secret(self):
        import tempfile
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'group.private.json'
            with patch.object(provision.getpass, 'getpass', return_value='router-secret'):
                provision.group_file(path, 'infrastructure', 'studio-wifi')
            group = json.loads(path.read_text(encoding='utf-8'))
        self.assertEqual(group['networkMode'], 'infrastructure')
        self.assertEqual(group['stationSsid'], 'studio-wifi')
        self.assertEqual(group['stationPassword'], 'router-secret')

    def test_pair_preflight_derives_follower_identity_and_master_address(self):
        plan = provision.plan_follower_pair(MASTER, FOLLOWER, GROUP, 2, 'Panel B',
                                            master_port='COM3', follower_port='COM4')
        self.assertEqual(plan, {
            'hardwareId': FOLLOWER['hardwareId'],
            'masterNodeId': 1,
            'networkMode': 'softap',
            'masterAddress': '192.168.4.1',
            'needsConfiguration': True,
        })

    def test_pair_preflight_rejects_mismatched_group_or_board_profile(self):
        with self.assertRaises(SystemExit):
            provision.plan_follower_pair(MASTER, FOLLOWER,
                {**GROUP, 'groupId': '3a68e6b7-a111-4c8c-a000-123456789abc'},
                2, 'Panel B')
        with self.assertRaises(SystemExit):
            provision.plan_follower_pair(MASTER,
                {**FOLLOWER, 'boardProfile': 'another_profile'}, GROUP, 2, 'Panel B')

    def test_pair_preflight_refuses_overwriting_another_follower_configuration(self):
        configured = {**FOLLOWER, 'provisioned': True, 'role': 'follower',
                      'nodeId': 9, 'masterNodeId': 1, 'groupId': GROUP['groupId'],
                      'networkMode': 'softap'}
        with self.assertRaises(SystemExit):
            provision.plan_follower_pair(MASTER, configured, GROUP, 2, 'Panel B')

    def test_pair_command_configures_then_registers_the_inspected_hardware_id(self):
        import tempfile
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'group.private.json'
            path.write_text(json.dumps(GROUP), encoding='utf-8')
            with patch.object(provision, 'inspect', side_effect=[MASTER, FOLLOWER]) as inspect, \
                    patch.object(provision, 'configure') as configure, \
                    patch.object(provision, 'register_node') as register:
                provision.pair_follower('COM3', 'COM4', str(path), 2, 'Panel B')
        self.assertEqual([call.args[0] for call in inspect.call_args_list], ['COM3', 'COM4'])
        configure.assert_called_once_with('COM4', str(path), 2, 'follower', 'softap', None, 1)
        register.assert_called_once_with('COM3', str(path), 2, FOLLOWER['hardwareId'], 'Panel B')

    def test_pair_command_skips_configuration_for_an_already_matching_follower(self):
        import tempfile
        from unittest.mock import patch
        configured = {**FOLLOWER, 'provisioned': True, 'role': 'follower',
                      'nodeId': 2, 'masterNodeId': 1, 'groupId': GROUP['groupId'],
                      'networkMode': 'softap'}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'group.private.json'
            path.write_text(json.dumps(GROUP), encoding='utf-8')
            with patch.object(provision, 'inspect', side_effect=[MASTER, configured]), \
                    patch.object(provision, 'configure') as configure, \
                    patch.object(provision, 'register_node') as register:
                provision.pair_follower('COM3', 'COM4', str(path), 2, 'Panel B')
        configure.assert_not_called()
        register.assert_called_once_with('COM3', str(path), 2, FOLLOWER['hardwareId'], 'Panel B')

    def test_pair_command_dry_run_never_configures_or_registers(self):
        import tempfile
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'group.private.json'
            path.write_text(json.dumps(GROUP), encoding='utf-8')
            with patch.object(provision, 'inspect', side_effect=[MASTER, FOLLOWER]), \
                    patch.object(provision, 'configure') as configure, \
                    patch.object(provision, 'register_node') as register:
                result = provision.pair_follower(
                    'COM3', 'COM4', str(path), 2, 'Panel B', dry_run=True)
        self.assertEqual(result['hardwareId'], FOLLOWER['hardwareId'])
        configure.assert_not_called()
        register.assert_not_called()


if __name__ == '__main__':
    unittest.main()
