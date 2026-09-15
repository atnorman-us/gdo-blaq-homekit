"""Merge exactly the images and offsets produced by ESP-IDF (including otadata)."""
import json
import pathlib
import subprocess
import sys

def merge_args(build, output):
    build = pathlib.Path(build).resolve()
    manifest = json.loads((build / 'flasher_args.json').read_text())
    args = [sys.executable, '-m', 'esptool', '--chip', manifest['extra_esptool_args']['chip'],
            'merge_bin', '-o', str(pathlib.Path(output).resolve())]
    for key, value in manifest['flash_settings'].items():
        args.extend(['--' + key, value])
    for offset, file in sorted(manifest['flash_files'].items(), key=lambda item: int(item[0], 0)):
        args.extend([offset, str(build / file)])
    return args

if __name__ == '__main__':
    subprocess.run(merge_args(sys.argv[1], sys.argv[2]), check=True)
