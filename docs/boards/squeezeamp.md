# SqueezeAMP

The [SqueezeAMP](https://github.com/philippe44/SqueezeAMP) is an ESP32 board with a
TAS5756 DAC and a built-in Class-D amplifier. No external DAC is needed — connect speakers
directly to the board.

## Flashing

=== "Browser"

    Use the SqueezeAMP installer on the [flashing page](../getting-started/flashing.md).
    Prebuilt binaries exist for the Bluetooth build and the 4 MB variant.

=== "PlatformIO"

    ```bash
    # AirPlay only
    pio run -e squeezeamp -t upload
    pio run -e squeezeamp -t uploadfs

    # AirPlay + Bluetooth A2DP
    pio run -e squeezeamp-bt -t upload
    pio run -e squeezeamp-bt -t uploadfs
    ```

=== "ESP-IDF"

    ```bash
    idf.py set-target esp32
    idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.squeezeamp" build
    idf.py -p /dev/ttyUSB0 flash
    ```

    `idf.py flash` also writes the SPIFFS `storage` partition from `data/`, so the
    captive-portal pages are present on first boot.

## Build variants

| Environment | Flash | Bluetooth | Notes |
| --- | --- | :-: | --- |
| `squeezeamp` | 8 MB | — | AirPlay only |
| `squeezeamp-bt` | 8 MB | yes | Prebuilt binary published |
| `squeezeamp-4m` | 4 MB | — | Smaller partition table, prebuilt binary published |

Bluetooth does not fit alongside AirPlay in 4 MB of flash, which is why there is no
`squeezeamp-4m-bt`.

## What the build configures

The SqueezeAMP build selects the TAS57xx DAC driver automatically through Kconfig
(`CONFIG_DAC_TAS57XX`) and sets the correct I2S and I2C pins. Audio buffers are reduced
from 5000 to 2500 frames to fit the original ESP32's more limited PSRAM bandwidth.

## Hybrid flow DSP

The TAS575xM supports **hybrid flow** DSP programs that run on the chip's miniDSP core. At
boot the driver looks for `/spiffs/hf/tas57xx_fw.bin` and loads it automatically if
present. There is no menuconfig setting — place the file and reboot.

To add or update a hybrid flow:

1. Export a `.cfg` file from TI PurePath Console.
2. Convert it to binary:
   ```bash
   python3 components/dac_tas57xx/hybridflows/hybridflow_convert_cfg.py --bin my_flow.cfg
   ```
3. Rename the output to `tas57xx_fw.bin`.
4. Copy it to `data/hf/` for a serial flash, or upload it over WiFi:
   ```bash
   curl -X POST "http://<device-ip>/api/fs/upload?path=/spiffs/hf/tas57xx_fw.bin" \
        --data-binary @tas57xx_fw.bin
   ```
5. Reboot the device.

Delete the file to disable the hybrid flow.

!!! note

    Hybrid flows are only supported on TAS575xM chips. The driver detects the chip family
    at boot and skips hybrid flow loading on TAS578x devices.

See [SPIFFS filesystem](../reference/spiffs.md) for the file management API.

## Related

- [Bluetooth A2DP](../features/bluetooth.md)
- [Build environments](../reference/build-environments.md)
