"""Point PlatformIO's filesystem targets at the same staged image CMake uses.

PlatformIO packs $PROJECT_DATA_DIR with its own mkspiffs rather than using the
storage.bin that spiffs_create_partition_image() produces, so without this the
two flows disagree: `idf.py flash` would write gzipped pages and
`pio run -t uploadfs` would write raw ones, which do not fit a 4 MB board.
"""

import os
import subprocess
import sys

Import("env")  # noqa: F821 - injected by PlatformIO

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}

if FS_TARGETS & set(COMMAND_LINE_TARGETS):  # noqa: F821 - SCons global
    staged = os.path.join(env.subst("$BUILD_DIR"), "spiffs_image")
    subprocess.check_call(
        [
            sys.executable,
            os.path.join(env.subst("$PROJECT_DIR"), "scripts",
                         "gen_spiffs_image.py"),
            env.subst("$PROJECT_DATA_DIR"),
            staged,
        ]
    )
    env.Replace(PROJECT_DATA_DIR=staged)
