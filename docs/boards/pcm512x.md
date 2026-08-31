# PCM5121 / PCM5122

TI's PCM512x is the DAC on most of the well-known Raspberry Pi audio HATs. Unlike the
[PCM5102A](esp32s3-pcm5102a.md), which has no control interface at all, a PCM5121 or
PCM5122 is configured over I2C — so the AirPlay volume control lands in the DAC's own
32-bit digital attenuator instead of being applied in software.

There is no fixed board here: the `pcm512x` and `pcm512x-s3` environments are a generic
ESP32 or ESP32-S3 dev board with a PCM512x module wired to it. Edit the GPIOs in the
matching config file to suit your wiring.

## Wiring

| Function | ESP32 GPIO | ESP32-S3 GPIO | DAC pin |
| --- | --- | --- | --- |
| Bit clock | 26 | 11 | BCK |
| Audio data | 22 | 12 | DIN |
| Word select (LRCLK) | 25 | 13 | LRCK |
| Master clock | not wired | 8 | SCK |
| I2C data | 21 | 1 | SDA |
| I2C clock | 27 | 2 | SCL |

Pull SDA and SCL up to 3.3 V — the internal pull-ups are enabled but are weak, and most
PCM512x breakout boards already carry 4.7 kΩ resistors.

Strap ADR1/ADR2 for any of the four addresses; the driver probes 0x4C through 0x4F and
uses the first device that answers.

### MCLK is optional

The driver reads P0-R94 once the I2S clocks are live and points the PLL at the SCK pin
when it sees a clock there, or at BCK when it does not. Set `CONFIG_I2S_SCK_IO` to `-1`
if you would rather keep the pin, and the DAC will run from the bit clock alone — the
same arrangement the SqueezeAMP has always used. Look for this line at boot:

```
I (1234) PCM512X: PLL reference = MCLK/SCK
```

Running without MCLK also masks the clock-halt and LRCK/BCK-missing detectors, because
otherwise every gap between tracks would drop the part into powerdown.

## Building

=== "PlatformIO"

    ```bash
    pio run -e pcm512x-s3 -t upload      # ESP32-S3
    pio run -e pcm512x-s3 -t uploadfs

    pio run -e pcm512x -t upload         # ESP32
    pio run -e pcm512x -t uploadfs
    ```

    The `uploadfs` step is required — it writes the web UI to SPIFFS.

=== "ESP-IDF"

    ```bash
    idf.py set-target esp32s3
    idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.pcm512x-s3" build
    idf.py -p /dev/ttyUSB0 flash
    ```

## Configuration

Under **Component config → DAC Configuration** in `menuconfig`:

| Option | Default | Effect |
| --- | --- | --- |
| `PCM512X_MAX_VOLUME` | `0` | dB the DAC is set to at AirPlay volume 0. Lower it if whatever the line output feeds clips before the slider reaches the top. |
| `PCM512X_ANALOG_GAIN_MINUS_6DB` | off | Halves the output to roughly 1 Vrms, in the analogue domain, for inputs that expect consumer line level. |
| `PCM512X_FORCE_BCK_PLL` | off | Keeps the PLL on BCK even when MCLK is present. Only useful for testing. |

## What this driver does not do

The PCM5121 and PCM5122 have no user-programmable miniDSP — P0-R43 only accepts the ROM
process flows, and the "program in RAM" setting the
[TAS57xx HybridFlow](../features/hybridflow.md) support relies on is reserved on this
part. The `/hf` and `/bq` tuning pages are therefore not available, and no DSP images
are flashed to SPIFFS.

Everything else works as usual: hardware volume, mute, standby and powerdown, clock
detection and the read-only status registers.

!!! warning "PCM512x and TAS57xx cannot be auto-detected apart"

    Both families answer on 0x4C-0x4F and neither has a device ID register, so the
    driver is chosen at build time. Enabling `CONFIG_DAC_PCM512X` against a TAS57xx
    board gives you a DAC that appears to work but never leaves the amplifier muted.
    The driver checks P1-R5 at startup and logs a warning when the reset value looks
    like the wrong part.
