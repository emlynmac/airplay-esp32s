# HybridFlow host tools

These build the real firmware DSP sources (`tas57xx_cram.c`, `tas57xx_hf1.c`) on
the host against small ESP-IDF shims, so the coefficient generator can be tested
without flashing hardware. Building the actual firmware sources — rather than a
reimplementation — is the point: it makes generator/firmware drift impossible.

## Build and run

```bash
cd components/dac_tas57xx/hybridflows/tools
gcc -std=gnu11 -O1 -Wall -I. -I../.. test.c ../../tas57xx_cram.c \
    ../../tas57xx_hf1.c -lm -o hf1test
./hf1test ../DMA80/hf1-flat-441.bin
```

## What each tool does

| File | Purpose |
|---|---|
| `test.c` | Applies a config to a flow image, then re-applies it and diffs the coefficient RAM. Re-applying an identical config must change **no** words; changing one band must touch only that band's 4–5 words. |
| `mkcfg.c` | Writes an `hf1.cfg` matching a given flow, for upload to `/spiffs/hf/hf1.cfg`. |
| `dump.c` + `shim.c` | Prints designed biquad coefficients so the web UI's JavaScript `designBq()` can be diffed against the firmware's C `design()`. `shim.c` `#define static`s the file in to reach the static function. |
| `gen.c` | Standalone coefficient generation experiments. |
| `verify.py`, `pages.py` | Decode a flow binary / PPC2 I2C capture into page+register form. |

## Expected `test.c` output

```
re-apply identical config                  changed words: (none)
EQ band 1 -> peaking 100Hz Q1 +3dB         changed words: 84 85 86 87
EQ band 5 -> HP2 2kHz                      changed words: 103 104 105 106 107

values self-consistent
```

Any drift on the first line means the design path is not idempotent, which on
hardware shows up as coefficient RAM being rewritten every apply.
