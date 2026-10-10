"""Regenerate the distribution copy from the normative split documents."""
from pathlib import Path
import posixpath
import re

ROOT = Path(__file__).resolve().parents[1]
sources = ['IMPLEMENTATION_HANDOFF.md'] + [str(p.relative_to(ROOT)).replace('\\', '/') for p in sorted((ROOT / 'docs').glob('[0-9][0-9]-*.md'))]
sources += ['docs/VALIDATION.md', 'docs/IMPLEMENTATION_STATUS.md', 'docs/EDITOR_GUIDE.md', 'schemas/README.md', 'examples/README.md']
parts = ['# ライブ表示パネル ソフトウェア 統合仕様 v1.1\n\n分割仕様を正本とする配布用コピー。現在の実装範囲・検証結果は「実装状況」を参照。設計上の将来要件も含む。\n']
for source in sources:
    text = (ROOT / source).read_text(encoding='utf-8').rstrip()
    def relocate_link(match):
        target = match.group(1)
        if target.startswith(('#', '/', 'http:', 'https:', 'mailto:')):
            return match.group(0)
        path, separator, fragment = target.partition('#')
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source), path))
        relative = posixpath.relpath(resolved, 'docs')
        return '](' + relative + (separator + fragment if separator else '') + ')'
    text = re.sub(r'\]\(([^)]+)\)', relocate_link, text)
    parts.append(f'\n---\n\n<!-- Source: {source} -->\n\n' + text + '\n')
parts.append('\n---\n\n# 付録: JSON Schema\n')
for path in sorted((ROOT / 'schemas').glob('*.schema.json')):
    parts.append(f'\n## {path.name}\n\n```json\n{path.read_text(encoding="utf-8").rstrip()}\n```\n')
parts.append('\n---\n\n# 付録: 参照JSON\n')
for source in ['examples/demo-project/project.json', 'examples/minimal.runtime.json', 'examples/state.json', 'examples/command.json', 'examples/udp-test-vector.json', 'examples/demo.ledrig.json', 'examples/topology.json']:
    parts.append(f'\n## {source}\n\n```json\n{(ROOT / source).read_text(encoding="utf-8").rstrip()}\n```\n')
(ROOT / 'docs/COMPLETE_SPEC.md').write_text(''.join(parts), encoding='utf-8')
print('Regenerated docs/COMPLETE_SPEC.md')
