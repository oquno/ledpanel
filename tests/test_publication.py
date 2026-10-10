import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_publication import audit
from export_public import export


class PublicationTests(unittest.TestCase):
    def test_export_keeps_sources_and_reference_packages_without_local_data(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ('README.md', 'examples/demo.lpk', 'firmware/main.cpp',
                         'tmp/group.private.json', 'output/device-backups/flash.bin',
                         'firmware/build/firmware.bin', '.git/config'):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'reference')
            # Use a real repository to exercise tracked and untracked discovery.
            (root / '.git/config').unlink()
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / '.gitignore').write_text('tmp/\noutput/\nfirmware/build/\n')
            subprocess.run(['git', '-C', str(root), 'add', 'README.md'], check=True)
            archive_path = root / 'output/public.zip'
            export(root, archive_path)
            with zipfile.ZipFile(archive_path) as archive:
                names = archive.namelist()
                self.assertIn('README.md', names)
                self.assertIn('examples/demo.lpk', names)
                self.assertIn('firmware/main.cpp', names)
                self.assertFalse(any(name.startswith(('tmp/', 'output/', '.git/')) for name in names))
                self.assertNotIn('firmware/build/firmware.bin', names)
                manifest = json.loads(archive.read('PUBLICATION_MANIFEST.json'))
                self.assertFalse(manifest['includesGitHistory'])
                for item in manifest['files']:
                    data = archive.read(item['path'])
                    self.assertEqual(len(data), item['bytes'])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), item['sha256'])

    def test_known_private_secret_in_binary_blocks_export(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'tmp').mkdir()
            value = 'generated-only-for-this-test'
            (root / 'tmp/group.private.json').write_text(json.dumps({'groupKey': value}))
            (root / 'demo.lpk').write_bytes(b'LPK2' + value.encode())
            destination = root / 'output/public.zip'
            with self.assertRaises(ValueError):
                export(root, destination)
            self.assertFalse(destination.exists())

    def test_tracked_secret_file_is_rejected_even_if_gitignored(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            (root / 'group.private.json').write_text('{}')
            subprocess.run(['git', '-C', str(root), 'add', 'group.private.json'], check=True)
            (root / '.gitignore').write_text('*.private.json\n')
            _, findings = audit(root)
            self.assertIn(('group.private.json', 'local-only file is tracked'), findings)

    def test_credentials_json_and_personal_path_are_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'config.json').write_text(json.dumps({'adminPassword': 'example-password'}))
            (root / 'README.md').write_text('C:' + '/Users/private-person/document.txt')
            _, findings = audit(root)
            self.assertIn(('config.json', 'provisioning credentials in public JSON'), findings)
            self.assertIn(('README.md', 'personal Windows path'), findings)

    def test_archive_destination_must_stay_in_output(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with self.assertRaises(ValueError):
                export(root, root / 'source.zip')


if __name__ == '__main__':
    unittest.main()
