from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from test_browser import wait_for_browser_port


class BrowserStartupTests(unittest.TestCase):
    @patch('test_browser.time.sleep')
    def test_retries_missing_locked_and_incomplete_port_file(self, sleep):
        port_file = Mock()
        port_file.read_text.side_effect = [
            FileNotFoundError(), PermissionError(), '', 'not-a-port\n',
            '0\n', '65536\n', '43210\n/devtools/browser/test\n',
        ]
        process = Mock()
        process.poll.return_value = None

        self.assertEqual(wait_for_browser_port(port_file, process), 43210)
        self.assertEqual(sleep.call_count, 6)

    def test_stops_when_browser_exits(self):
        port_file = Mock()
        process = Mock()
        process.poll.return_value = 1

        with self.assertRaisesRegex(RuntimeError, 'exit code 1'):
            wait_for_browser_port(port_file, process)
        port_file.read_text.assert_not_called()

    @patch('test_browser.time.monotonic', side_effect=[0, 15])
    def test_locked_file_times_out_with_original_error(self, monotonic):
        port_file = Mock()
        error = PermissionError('File is locked')
        port_file.read_text.side_effect = error
        process = Mock()
        process.poll.return_value = None

        with self.assertRaisesRegex(RuntimeError, 'Timed out') as raised:
            wait_for_browser_port(port_file, process)
        self.assertIs(raised.exception.__cause__, error)


if __name__ == '__main__':
    unittest.main()
