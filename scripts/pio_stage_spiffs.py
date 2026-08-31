"""Point PlatformIO's filesystem targets at the same staged image CMake uses.

PlatformIO packs $PROJECT_DATA_DIR with its own mkspiffs rather than using the
storage.bin that spiffs_create_partition_image() produces, so without this the
two flows disagree: `idf.py flash` would write gzipped pages and
`pio run -t uploadfs` would write raw ones, which do not fit a 4 MB board.
"""

import os
import re
import subprocess
import sys

Import("env")  # noqa: F821 - injected by PlatformIO

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}


def selects_tas57xx(project_dir):
    """Whether this env's sdkconfig layers choose the HybridFlow DAC driver.

    Read from platformio.ini, not from a generated sdkconfig — a pre: script
    runs before CMake has configured, so no sdkconfig exists yet.
    """
    args = env.GetProjectOption("board_build.cmake_extra_args", "")
    if not isinstance(args, str):
        args = " ".join(args)
    layers = re.search(r"-DSDKCONFIG_DEFAULTS=([^\"\s]+)", args)
    if not layers:
        return True  # unrecognised config, so ship all of data/
    for layer in layers.group(1).split(";"):
        path = os.path.join(project_dir, layer)
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            if "CONFIG_DAC_TAS57XX=y" in f.read():
                return True
    return False


if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # noqa: F821 - SCons global
    project_dir = env.subst("$PROJECT_DIR")
    staged = os.path.join(env.subst("$BUILD_DIR"), "spiffs_image")
    cmd = [sys.executable,
           os.path.join(project_dir, "scripts", "gen_spiffs_image.py")]
    if not selects_tas57xx(project_dir):
        cmd += ["--exclude", "hf/base-hf*.bin"]
    cmd += [env.subst("$PROJECT_DATA_DIR"), staged]
    subprocess.check_call(cmd)
    env.Replace(PROJECT_DATA_DIR=staged)
