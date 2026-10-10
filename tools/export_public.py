"""Build a source-only ZIP from audited working-tree files, without Git history."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

from check_publication import ROOT, audit, is_local


def export(root, destination):
    destination = destination.resolve()
    output = (root / 'output').resolve()
    if not destination.is_relative_to(output):
        raise ValueError('Public archives must be written inside output/.')
    files, findings = audit(root)
    if findings:
        for name, reason in findings:
            print(f'FAIL: {name}: {reason}')
        raise ValueError('Publication audit failed; archive was not created.')
    manifest = {'formatVersion': 1, 'includesGitHistory': False, 'files': []}
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.zip.partial')
    try:
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name in files:
                if is_local(name) or name == 'PUBLICATION_MANIFEST.json':
                    continue
                data = (root / name).read_bytes()
                archive.writestr(name, data)
                manifest['files'].append({'path': name, 'bytes': len(data),
                                          'sha256': hashlib.sha256(data).hexdigest()})
            archive.writestr('PUBLICATION_MANIFEST.json',
                            json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return len(manifest['files'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/ledpanel-public.zip')
    args = parser.parse_args()
    count = export(ROOT, args.output)
    print(f'Created {args.output}: {count} source files; no Git history or local device data.')


if __name__ == '__main__':
    main()
