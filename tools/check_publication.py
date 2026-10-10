"""Audit publishable working-tree files without printing secret contents."""
import argparse
import fnmatch
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LOCAL_DIRS = {'.git', 'tmp', 'output', 'provisioning-secrets', 'device-backups',
              'node_modules', '__pycache__', '.venv', 'venv', '.pytest_cache',
              '.vscode', '.idea', 'managed_components', 'build'}
LOCAL_NAMES = ('*.private.json', '*.log', '*.pyc', '*.bin', '*.elf', '*.map',
               '*.zip', '*.pem', '*.key', '*.p12', '*.pfx', '.env', '.env.*',
               'sdkconfig', 'sdkconfig.old', '.DS_Store', 'Thumbs.db')
PATTERNS = {
    'private key': re.compile(rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'GitHub token': re.compile(rb'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b'),
    'AWS access key': re.compile(rb'\b(?:AKIA|ASIA)[A-Z0-9]{16}\b'),
    'personal Windows path': re.compile(rb'C:[/\\]Users[/\\](?!YOUR_USER|USERNAME|<)[A-Za-z0-9_.-]+[/\\]', re.I),
}


def is_local(name):
    path = Path(name)
    return (any(part in LOCAL_DIRS or part.startswith('build-') for part in path.parts)
            or (path.name != '.env.example' and
                any(fnmatch.fnmatch(path.name, pattern) for pattern in LOCAL_NAMES)))


def source_files(root=ROOT):
    if (root / '.git').exists():
        result = subprocess.run(
            ['git', '-C', str(root), 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
            check=True, capture_output=True)
        return sorted({name.decode('utf-8') for name in result.stdout.split(b'\0') if name})
    # Also usable inside an exported archive before git init.
    return sorted(p.relative_to(root).as_posix() for p in root.rglob('*')
                  if p.is_file() and not is_local(p.relative_to(root).as_posix()))


def local_secrets(root):
    """Read only provisioning JSON; never read full flash dumps or print values."""
    values = set()
    for folder in ('tmp', 'provisioning-secrets'):
        base = root / folder
        if not base.exists():
            continue
        for path in base.rglob('*.private.json'):
            if path.is_symlink() or path.stat().st_size > 1024 * 1024:
                continue
            try:
                data = json.loads(path.read_text(encoding='utf-8'))
            except (ValueError, OSError):
                continue
            def collect(item):
                if isinstance(item, dict):
                    for key, value in item.items():
                        if isinstance(value, str) and len(value) >= 8 and re.search(
                                r'password|psk|groupkey|cookie|token', key, re.I):
                            values.add(value.encode('utf-8'))
                        else:
                            collect(value)
                elif isinstance(item, list):
                    for child in item:
                        collect(child)
            collect(data)
    return values


def audit(root=ROOT):
    files = source_files(root)
    secrets = local_secrets(root)
    findings = []
    for name in files:
        path = root / name
        if is_local(name):
            findings.append((name, 'local-only file is tracked'))
            continue
        if path.is_symlink():
            findings.append((name, 'symlink is not allowed in the public export'))
            continue
        if not path.is_file():
            findings.append((name, 'tracked file missing from working tree'))
            continue
        data = path.read_bytes()
        if any(value in data for value in secrets):
            findings.append((name, 'matches local provisioning secret'))
        for label, pattern in PATTERNS.items():
            if pattern.search(data):
                findings.append((name, label))
        if path.suffix == '.json' and not name.startswith(('examples/', 'tests/')):
            try:
                payload = json.loads(data)
            except (ValueError, UnicodeError):
                continue
            if isinstance(payload, dict) and any(payload.get(key) for key in
                    ('apPsk', 'adminPassword', 'groupKey', 'stationPassword')):
                findings.append((name, 'provisioning credentials in public JSON'))
    return files, findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    files, findings = audit()
    for name, reason in findings:
        print(f'FAIL: {name}: {reason}')
    if findings:
        return 1
    print(f'Publication audit passed: {len(files)} files. Git history is not included in this check.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
