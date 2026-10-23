"""Synchronize the current local handoff documents to repository-facing paths."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAPPING = {
    'DEVELOPMENT.md': 'DEVELOPMENT.md',
    'BUILD-current.md': 'BUILD.md',
    'USAGE-current.md': 'USAGE.md',
    'TROUBLESHOOTING.md': 'TROUBLESHOOTING.md',
    'UDP多网卡回包修复说明.md': 'UDP多网卡回包修复说明.md',
    'UDP与连接质量排查.md': 'UDP与连接质量排查.md',
}

def sync():
    if not (ROOT/'文档').exists():
        return
    (ROOT/'docs').mkdir(exist_ok=True)
    for source, target in MAPPING.items():
        (ROOT/'docs'/target).write_bytes((ROOT/'文档'/source).read_bytes())

if __name__ == '__main__':
    sync()
