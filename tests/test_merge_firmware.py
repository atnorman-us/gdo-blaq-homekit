import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "merge-firmware.py"


class MergeFirmwareTests(unittest.TestCase):
    def test_uses_every_manifest_partition_in_offset_order(self):
        spec = importlib.util.spec_from_file_location("merge_firmware", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        with tempfile.TemporaryDirectory() as directory:
            build = pathlib.Path(directory).resolve()
            manifest = {
                "flash_settings": {
                    "flash_mode": "dio",
                    "flash_size": "4MB",
                    "flash_freq": "80m",
                },
                "flash_files": {
                    "0x10000": "app.bin",
                    "0x0": "bootloader.bin",
                    "0xf000": "ota_data_initial.bin",
                    "0x8000": "partition-table.bin",
                },
                "extra_esptool_args": {"chip": "esp32s3"},
            }
            (build / "flasher_args.json").write_text(json.dumps(manifest))
            output = build / "combined.bin"

            self.assertEqual(
                module.merge_args(build, output),
                [
                    sys.executable,
                    "-m",
                    "esptool",
                    "--chip",
                    "esp32s3",
                    "merge_bin",
                    "-o",
                    str(output),
                    "--flash_mode",
                    "dio",
                    "--flash_size",
                    "4MB",
                    "--flash_freq",
                    "80m",
                    "0x0",
                    str(build / "bootloader.bin"),
                    "0x8000",
                    str(build / "partition-table.bin"),
                    "0xf000",
                    str(build / "ota_data_initial.bin"),
                    "0x10000",
                    str(build / "app.bin"),
                ],
            )


if __name__ == "__main__":
    unittest.main()
