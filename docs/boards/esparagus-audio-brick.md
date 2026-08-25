# Esparagus Audio Brick

The [Esparagus Audio Brick](https://github.com/sonocotta/esparagus-media-center/?tab=readme-ov-file#esparagus-audio-brick-prototype)
is an ESP32 board built around a TI **TAS5825M** Class-D DAC and amplifier. Like the
SqueezeAMP, it needs no external DAC — connect speakers directly.

## Features

- TAS5825M with on-chip DSP and a 15-band parametric EQ (25 Hz – 16 kHz)
- Hardware volume control with a configurable maximum level
- Speaker fault detection with automatic mute and recovery
- Automatic power state management (deep sleep / standby / play) driven by AirPlay session state
- 8 MB flash
- Optional [Bluetooth A2DP](../features/bluetooth.md)
- Optional [W5500 SPI Ethernet](../features/ethernet.md) with automatic WiFi failover

## Flashing

=== "Browser"

    Use the Esparagus Audio Brick installer on the
    [flashing page](../getting-started/flashing.md). The published binary is the
    Bluetooth + Ethernet build.

=== "PlatformIO"

    ```bash
    # AirPlay only
    pio run -e esparagus-audio-brick -t upload
    pio run -e esparagus-audio-brick -t uploadfs

    # AirPlay + Bluetooth + Ethernet
    pio run -e esparagus-audio-brick-bt -t upload
    pio run -e esparagus-audio-brick-bt -t uploadfs

    # Serial monitor
    pio run -e esparagus-audio-brick -t monitor
    ```

=== "ESP-IDF"

    ```bash
    idf.py set-target esp32
    idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esparagus-audio-brick" build
    idf.py -p /dev/ttyUSB0 flash
    ```

## Default GPIO assignments

| Function | GPIO | Notes |
| --- | --- | --- |
| I2S BCK | 26 | Bit clock |
| I2S WS | 25 | Word select (LRCLK) |
| I2S DO | 22 | Serial audio data |
| I2C SDA | 21 | DAC control (TAS5825M) |
| I2C SCL | 27 | DAC control (TAS5825M) |
| Jack detect | 34 | Headphone jack insertion, input |
| DAC warning | 36 | TAS5825M warning output, input |
| Speaker fault | 39 | TAS5825M fault output, input |

!!! note "GPIOs 34–39 are input-only"

    On the ESP32, GPIOs 34–39 are input-only and have no internal pull-up. The board
    provides external pull-ups on the fault and warning lines.

The build selects the TAS58xx driver automatically (`CONFIG_DAC_TAS58XX`). The driver
auto-detects the TAS5825M I2C address in the range 0x4C–0x4F at startup.

## Equaliser

TAS5825M boards expose the DAC's 15 cascaded biquad sections through the device's web
interface at `/bq.html`, one filter per section, per output and per amplifier. Each
section can be a peaking filter, a shelf, a low or high pass in six alignments, a band
pass, a notch, a phase shift, or five raw coefficients. The filter models match
PurePath Console 3, and coefficients are recomputed whenever the I2S sample rate
changes. The chip's two outputs are named A and B — which of them carries left, right
or a sum is the routing setting, not a fixed assignment. A and B can be ganged or
tuned separately; edits take effect immediately and are written to flash only when
committed. For plain tone shaping, **Load 15-band EQ** fills the chain with a flat
graphic equaliser — one peaking section per band from 20 Hz to 16 kHz — leaving only
the gains to set. It fills in the form and nothing more, so the amplifier hears it
only once applied.

Crossovers are built from these same sections, so a two-way or subwoofer split is just
a high pass on one amplifier and a low pass on the other. Each amplifier also carries
its own input routing: the stereo pair as-is, summed to `(L+R)/2`, or one channel fed
to both outputs. A bridged amplifier drives a single voice coil, so it is always fed a
single channel and defaults to the sum; ganging is implicit and the stereo option is
not offered. A combined response graph at the top of the page plots every active
output together, so a crossover spread across both amplifiers can be read as one
picture.

Levels are set in the Volume section. Master is the AirPlay volume and moves
everything together. Below it each output has its own level and mute, applied in the
DSP input mixer ahead of the filters — so they only ever attenuate and cost no filter
headroom. Use them to match drivers of differing sensitivity. Levels and mutes take
effect immediately and are stored in NVS, independently of the filter commit.

## Variants

| Environment | Chip | Notes |
| --- | --- | --- |
| `esparagus-audio-brick` | ESP32 | AirPlay only |
| `esparagus-audio-brick-bt` | ESP32 | Bluetooth + Ethernet, prebuilt binary published |
| `esparagus-audio-brick-s3` | ESP32-S3 | S3-based revision, no Bluetooth |
| `esparagus-audio-brick-dual-dac` | ESP32-S3 | Rev D with two TAS5825M: stereo at 0x4C, second at 0x4D, bridged mono or stereo |

### Esparagus Louder

The Esparagus Louder is the same TAS5825M design with additional gain.

| Environment | Chip | Bluetooth | Prebuilt |
| --- | --- | :-: | :-: |
| `esparagus-louder` | ESP32 | — | — |
| `esparagus-louder-bt` | ESP32 | yes | — |
| `esparagus-louder-s3` | ESP32-S3 | — | yes |

```bash
pio run -e esparagus-louder-s3 -t upload
pio run -e esparagus-louder-s3 -t uploadfs
```

## Related

- [Bluetooth A2DP](../features/bluetooth.md)
- [Ethernet (W5500)](../features/ethernet.md)
- [Build environments](../reference/build-environments.md)
