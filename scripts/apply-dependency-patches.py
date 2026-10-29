"""Apply reviewed fixes to pinned submodules; fail rather than build unpatched code."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def apply_patch(component, patch):
    def git(*args):
        return subprocess.run(['git', '-C', str(component), 'apply', *args, str(patch)],
                              capture_output=True, text=True)
    if git('--reverse', '--check').returncode == 0:
        return
    check = git('--check')
    if check.returncode:
        raise RuntimeError(f'{patch.name} does not apply cleanly; check the pinned submodule and local edits.\n{check.stderr}')
    result = git()
    if result.returncode:
        raise RuntimeError(result.stderr)

if __name__ == '__main__':
    for name in ('gdolib', 'nvs_wifi_connect'):
        apply_patch(ROOT / 'components' / name, ROOT / 'patches' / (name + '.patch'))
