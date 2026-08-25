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
interface at `/bq`, one filter per section, per output and per amplifier. Each
section can be a peaking filter, a shelf, a low or high pass in six alignments, a band
pass, a notch, a phase shift, or five raw coefficients. The filter models match
PurePath Console 3, and coefficients are recomputed whenever the I2S sample rate
changes. The chip's two outputs are named A and B — which of them carries left, right
or a sum is the routing setting, not a fixed assignment. A and B can be ganged or
tuned separately. Editing only redraws the response graph: **Apply** sends the filters
to the amplifiers so you can hear them, and **Commit to flash** makes them survive a
reboot. **Revert** goes back to what is in flash. For plain tone shaping, **Load
15-band EQ** fills the chain with a flat graphic equaliser — one peaking section per
band from 20 Hz to 16 kHz — leaving only the gains to set. It fills in the form and
nothing more, so the amplifier hears it only once applied.

Crossovers are built from these same sections, so a two-way or subwoofer split is just
a high pass on one amplifier and a low pass on the other. **Build crossover** does that
for you: pick how the drivers are wired — both bands on one amplifier, one amplifier per
speaker, tweeters on one amplifier and woofers on the other, or satellites plus a
subwoofer — then a crossover frequency and an alignment: Linkwitz-Riley at 12 or
24 dB per octave, Butterworth at 6, 12 or 24, or Bessel at 12. It writes the filters
into every output the split touches, sets ganging and input routing to match, and
leaves the filters staged so nothing is heard until applied. Apply always pushes all
amplifiers, so a crossover spanning both can never go live by halves. Layouts the
wiring rules out are not offered — a bridged amplifier has no separate A and B to
split across.

Steeper alignments cost more slots, because sections cascade. A Linkwitz-Riley is two
cascaded Butterworths of half its order, so LR2 is two 1st-order Butterworths — real
poles, which collapse into a single biquad at Q 0.5 — while LR4 is two Butterworth 2
sections at Q 0.707 and cannot be folded into one. That is why a 24 dB per octave
split shows two Butterworth 2 sections rather than one Linkwitz-Riley 2: both would
slope at 24 dB per octave, but only the Butterworth pair sits 6 dB down at the corner,
which is what lets the two branches sum flat. Two Linkwitz-Riley 2 sections would be
12 dB down there and sum 6 dB short.

Each section also has an **Inv** box that flips its polarity. The builder sets this
itself and does not offer it as a choice, because the alignment decides it: at the
corner the branches sit 180° apart at 12 dB per octave and need opposite polarity to
sum flat, but they are back in phase at 24, where inverting would instead dig a notch.
Only one section of a branch ever carries it — polarity belongs to the chain, and
since sections multiply, inverting an even number of them cancels back to none. The
box is still there by hand for drivers wired out of phase. A section left on Bypass
with Inv ticked is a plain polarity flip and costs nothing else.

### Fitting to a measurement

**Fit to a measurement…** turns a measured response into a correction. Load a frequency
response export — REW text, or any file with a frequency and a level in its first two
columns — and the fitter searches for the peaking filters and shelves that flatten it,
then writes them into the chain. Only the shape is fitted, never the absolute level. The
response graph gains two overlays, the measurement as it was and as it would be once
corrected, so the fit can be judged before anything is applied.

Three settings matter more than the rest. The fit range decides where the effort goes: a
driver rolls off at its ends by more than any filter can undo, and leaving the range
wide spends filters fighting that instead of correcting the band the driver covers — the
page says so when the measurement is already well down at the low limit. Smoothing sets
how much detail is chased, and a sixth of an octave keeps the room modes while ignoring
the fine structure that moves when the microphone does. The boost and cut limits apply
to the summed correction rather than to any one filter, and the result reports how much
boost was used, so the same amount can come off that output's level to keep the
headroom.

**Keep first N sections** protects the head of the chain. The fit replaces everything
past that count, so on a bi-amped speaker keep the crossover, measure each way through
it, and fit into what is left; the count is filled in from any low or high passes
already sitting at the top of the chain. Kept sections are slots the fitter cannot have,
so **Filters to use** caps itself at what remains — a 24 dB per octave crossover leaves
13 of the 15. Fitted filters are staged like any other edit — nothing is heard until
applied and nothing survives a reboot until committed.

Each amplifier carries its own input routing: the stereo pair as-is, summed to
`(L+R)/2`, or one channel fed to both outputs. A bridged amplifier drives a single voice
coil, so it is always fed a single channel and defaults to the sum; ganging is implicit
and the stereo option is not offered. A combined response graph at the top of the page
plots every active output together, so a crossover spread across both amplifiers can be
read as one picture.

**Show what the outputs sum to** adds one more trace to that graph. Outputs feeding
separate drivers add as vectors and not as curves, so two branches that each look right
alone can still cancel where they overlap — and the two curves look identical either
way round, which is what makes it easy to miss. The sum is taken on the complex
response instead: a crossover is right when it runs flat through the corner, and a dip
there means the branches are fighting, so one of them needs **Inv**. The trace knows
only the filters, never the drivers, their spacing or the room, so it shows what the
crossover is aiming at rather than what a microphone would hear.

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
