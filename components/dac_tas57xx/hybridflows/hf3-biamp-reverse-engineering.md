# HybridFlow 3 (Bi-Amp) — PPC2 capture analysis

Reverse engineering notes for the TAS5754M bi-amp HybridFlow captures in
`BiAmp TR/`. Goal: determine whether the crossover and EQ can be reconfigured at
runtime from firmware instead of shipping a fixed `.bin` blob per tuning.

Source files (both 44.1 kHz, HF3 = "Bi-Amped 1.1"):

| File | `w 98` lines | register ops |
|---|---|---|
| `BiAmp TR/hf3-ppc2-44k1-L.cfg` | 390 | 289 |
| `BiAmp TR/hf3-ppc2-441-R.cfg` | 388 | 289 |

Both also contain writes to `0x9A`, `0x80`, `0x44` and `0x30` (other devices on
the TI EVM). Those are ignored — only address `0x98` is the DAC.

## 1. Anatomy of the capture

These are PPC2 *live I2C logs*, not flow exports (no `# Setting Page` comments).
Four phases:

1. **Page-0 setup** — `0x03=0x11` (mute), `0x2A=0x00`, `0x02=0x10` (standby),
   `0x3D`/`0x3E = 0x44`.
2. **miniDSP image download** — coefficient RAM bank A (pages `0x2C`–`0x34`) and
   bank B (pages `0x3E`–`0x46`, byte-identical), then instruction RAM
   (pages `0x98`–`0xA5`).
3. **Activation** — `P0-R43 (0x2B) = 0x1F` (run user program in RAM), bank swap
   via `page 0x2C reg 0x01`, exit shutdown (`0x02=0x00`, `0x03=0x00`,
   `0x2A=0x11`), `0x3D`/`0x3E = 0x4E`.
4. **Live PPC2 tuning log** — ~30 individual component writes into coefficient
   RAM, each written twice with a `page 0x2C reg 0x01 = 0x05` bank swap between.

Phase 4 is the reason these files are valuable. PPC2 emits exactly one component
per GUI change, so the capture *labels* the coefficient RAM addresses. A static
flow export cannot give you that.

The two files are **not** a matched stereo pair. They carry different EQ
tunings, different crossover section orders, and different input mixes — the L
file ends on `(L+R)/2`, the R file on right-only. They are two tuning
experiments against the same base flow.

## 2. Coefficient RAM addressing

Bank A occupies pages `0x2C`–`0x34`, 30 words per page at registers
`0x08, 0x0C, … 0x7C`. Bank B is the same layout at page + `0x12`.

```
word_index = (page - 0x2C) * 30 + (reg - 8) / 4
page       = 0x2C + word_index / 30
reg        = 8 + (word_index % 30) * 4
```

Bursts routinely run off the end of a page and continue at register `0x08` of
the next page, so a single biquad can be split across two I2C writes. Any
decoder must merge on that boundary.

Note the I2C register address needs bit 7 set (`reg | 0x80`) for multi-byte
auto-increment writes — see the driver's `tas57xx_write_hf()`.

## 3. Coefficient encoding

- 4 bytes per word, big-endian; **only the top 3 bytes are significant**.
- Value = `int24 / 2^23`, i.e. signed 1.23. `7f ff ff 00` = +1.0,
  `80 00 01 00` = −1.0, `40 00 00 00` = +0.5.
- A biquad is 5 consecutive words: **`b0, b1/2, b2, a1/2, a2`** with a1/a2
  **pre-negated** and the two `z^-1` terms **stored halved**:

$$y[n] = b_0x[n] + b_1x[n{-}1] + b_2x[n{-}2] + a_1y[n{-}1] + a_2y[n{-}2]$$

The halving matters — miss it and every filter decodes to a bogus high-Q
section near `fs/6`. It exists because `|a1| ≤ 2` for any stable biquad but
1.23 only reaches ±1, so the DSP's biquad instruction doubles those two taps
internally. `b0`, `b2` and `a2` are stored unscaled.

This is verified, not assumed. With the halving applied, the complementary
crossover pairs give a DC gain of exactly `1.000000` on the low branch and a
Nyquist gain of exactly `1.000000` on the high branch, and the recovered corner
frequencies land on exact round values (5000.0 Hz, 5500.0 Hz). Nothing else
reproduces that.

### Boost normalisation

For a peaking boost, `b0 = (1 + αA)/(1 + α/A) > 1`, which 1.23 cannot hold.
PPC2 therefore **divides the numerator by `b0`** and stores `b0 = 0x7FFFFF`,
realising the band as a broadband cut of `1/b0` with a flat top rather than a
boost. Level is made up elsewhere. When inverting, detect this case by
`|b1| ≠ |a1|` (an un-normalised peaking filter always has `b1 = -a1`) and
recover `1 + αA = -2\cos\omega_0 \cdot b_0/b_1`.

A slot holding a 0 dB peaking filter has numerator == denominator, i.e. it is
exactly unity regardless of the f0/Q baked into its coefficients. All eight EQ
slots are in that state in the shipped flow.

## 4. Write protocol

Coefficient RAM is double buffered. To change coefficients live:

1. Write the new words into the current bank.
2. `page 0x2C`, `reg 0x01 = 0x05` — swap.
3. Write the same words again (now into the other bank).

PPC2 does exactly this for every change in the capture. No re-download of the
flow is needed, and the DSP does not have to be stopped.

Master volume is **not** moved by the flow — it stays at `P0-R61`/`P0-R62`
(`0x3D`/`0x3E`).

## 5. Block map

Word indices are bank-A relative.

| Word | Page/reg | Block |
|---|---|---|
| 3, 8, 13, 18, 23 | `0x2C`/`0x14` … | Input conditioning (5 sections, non-RBJ) |
| 28 | `0x2C`/`0x78` | Crossover section, low branch |
| 33 | `0x2D`/`0x14` | Crossover section, high branch |
| 43 | `0x2D`/`0x3C` | Way A crossover section |
| 48, 53, 58, 63 | `0x2D`/`0x50`, `0x64`, `0x78`, `0x2E`/`0x14` | **Way A EQ, 4 biquads** |
| 72 | `0x2E`/`0x38` | Way B crossover section |
| 77, 82, 87, 92 | `0x2E`/`0x4C`, `0x60`, `0x74`, `0x2F`/`0x10` | **Way B EQ, 4 biquads** |
| 98, 103 | `0x2F`/`0x28`, `0x3C` | 1st-order sections (identical in L and R) |
| 108, 113 | `0x2F`/`0x50`, `0x64` | Unused (unity) |
| 118–142 | `0x2F`/`0x78` … | DRC / level detector (huge DC gain, not audio) |
| 196 | `0x32`/`0x48` | Output routing |
| 201 | `0x32`/`0x5C` | **Input channel mix (2 words: L gain, R gain)** |
| 206 | `0x32`/`0x70` | **Input channel mix (2 words: L gain, R gain)** |
| 212, 217, 222 | `0x33`/`0x10`, `0x24`, `0x38` | Post EQ (only 212 used) |

Channel mix values (8 bytes each at `0x32`/`0x5C` and `0x32`/`0x70`):

```
L only     7f ff ff 00  00 00 00 00
R only     00 00 00 00  7f ff ff 00
(L+R)/2    40 00 00 00  40 00 00 00
```

## 6. Recovered filter parameters

All EQ slots are ordinary RBJ peaking sections. `(norm)` marks a boost stored
with the numerator divided by `b0` (see section 3).

| Word | L file | R file |
|---|---|---|
| 48 | peak 1748 Hz Q 1.09 −10.50 dB | peak 1100 Hz Q 2.27 +8.00 dB (norm) |
| 53 | peak 3681 Hz Q 7.32 −4.20 dB | peak 1600 Hz Q 2.97 −6.00 dB |
| 58 | peak 1129 Hz Q 32.2 +3.10 dB (norm) | peak 615 Hz Q 3.77 +4.00 dB (norm) |
| 63 | peak 2164 Hz Q 1.58 +5.10 dB (norm) | peak 3100 Hz Q 0.73 +8.00 dB (norm) |
| 77 | peak 15482 Hz Q 1.84 −4.70 dB | peak 16971 Hz Q 0.68 −6.90 dB |
| 82 | peak 12600 Hz Q 1.93 −4.10 dB | peak 14066 Hz Q 1.08 −2.00 dB |
| 87 | peak 2000 Hz Q 0.98 0.00 dB | peak 18307 Hz Q 0.63 −1.20 dB |
| 92 | peak 2500 Hz Q 0.97 0.00 dB | peak 5700 Hz Q 2.62 +1.00 dB (norm) |

The gains land on exact 0.1 dB steps and the frequencies on round values, which
is what you would expect from values typed into the PPC2 GUI.

Words 98 and 103 are identical in both files: a 1st-order high-pass at 100.0 Hz
and a 1st-order low-pass at 200.0 Hz.

## 7. The crossover

The crossover sections are textbook bilinear designs — the earlier reading that
they were an exotic one-scalar family was an artifact of the missed `z^-1`
halving.

First order (words 43 and 72, L file), a complementary pair:

$$H_{low}(z) = \frac{\tfrac{1-a}{2}\left(1 + z^{-1}\right)}{1 - a z^{-1}}
\qquad
H_{high}(z) = \frac{\tfrac{1+a}{2}\left(1 - z^{-1}\right)}{1 - a z^{-1}}$$

$$a = \frac{1 - \tan(\pi f_c/f_s)}{1 + \tan(\pi f_c/f_s)}
\qquad
f_c = \frac{f_s}{\pi}\arctan\!\left(\frac{1-a}{1+a}\right)$$

`H_low` has DC gain exactly 1, `H_high` has Nyquist gain exactly 1, and they sum
to 1. Observed: `a` = 0.876976 as shipped → **918.8 Hz**, and `a` = 0.457664
after tuning → **5000.0 Hz**.

Second order (words 28/33, and 43/72 in the R file) is plain RBJ low-pass /
high-pass with `b ∝ (1, 2, 1)` and `(1, -2, 1)`:

| | shipped | tuned |
|---|---|---|
| words 28/33 | LP2/HP2 1837.5 Hz Q 0.5 | LP2/HP2 **5000.0 Hz** Q 0.5 |
| words 43/72 (R) | — | LP2 **5500.0 Hz** / HP2 5000.0 Hz Q 0.5 |

Q = 0.5 exactly, i.e. **2nd-order Linkwitz-Riley**. The shipped defaults
(918.8 / 1837.5 Hz) are `fs/48` and `fs/24` — PPC2's normalised placeholders.

So crossover frequency is a direct bilinear function of the coefficients, with
no calibration table needed.

## 8. Verdict

**Both EQ and crossover are reconfigurable at runtime.** Slot addresses,
coefficient format, biquad convention, RBJ math, the boost normalisation rule
and the double-buffer write protocol are all confirmed against exact round
values. Firmware can compute sections at runtime and write them without
re-downloading the flow.

Remaining caveat: HybridFlow coefficients are sample-rate specific, so the
design must use the same `fs` as the loaded flow
(`CONFIG_OUTPUT_SAMPLE_RATE_HZ`). The captures here are 44.1 kHz only.

See `hf1-eq-reverse-engineering.md` for the HybridFlow 1 map, which is the
better starting point for a generic web EQ.

## 9. Reproducing the analysis

The decoding steps, in order:

1. Keep only `w 98 …` lines. Track `reg 0x00` as the page select and `reg 0x7F`
   as the book select; everything else is a data write.
2. Split at `P0-R43 = 0x1F` — before it is the flow download, after it is the
   live tuning log.
3. Replay each half into a flat byte array for pages `0x2C`–`0x34` to get the
   "as shipped" and "as tuned" coefficient images.
4. Decode 4-byte words as `int24 / 2^23`, group into 5-word biquads at the
   indices in section 5.
5. Fit each biquad against the RBJ catalogue to recover f0 / Q / gain.

Biquad boundaries are *not* globally aligned — chains are packed sequentially
with scalars in between, so the start indices must come from the write
boundaries in the tuning log, not from a modulo rule.
