"""Combine the built firmware into one image for flashing a new device.

A new device needs the bootloader, the partition table and boot_app0 as well
as the app, at the addresses PlatformIO uses for this board. This writes them
all into firmware-full.bin, to be flashed at offset 0x0 (by the web installer
in install/, or any esptool). Run it after `pio run`, from the project root.
"""
import json
import subprocess
import sys

ENV = "seeed_xiao_esp32s3"
OUTPUT = "firmware-full.bin"
PIO = [sys.executable, "-m", "platformio"]

metadata = json.loads(subprocess.check_output(PIO + ["project", "metadata", "-e", ENV, "--json-output"]))
extra = metadata[ENV]["extra"]
parts = [(image["offset"], image["path"]) for image in extra["flash_images"]]
parts.append((extra["application_offset"], f".pio/build/{ENV}/firmware.bin"))

args = []
for offset, path in parts:
    args += [offset, path]

# Same flash settings as PlatformIO's upload for this board (qio board -> dio)
subprocess.check_call(PIO + [
    "pkg", "exec", "-p", "tool-esptoolpy", "--",
    "esptool.py", "--chip", "esp32s3", "merge_bin", "-o", OUTPUT,
    "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "8MB",
] + args)
