# HybridFlow 1 — PPC2 capture analysis

Reverse engineering notes for the three TAS5754M HybridFlow 1 captures. Goal:
find out whether HF1 exposes a slot layout stable enough to drive a **generic
runtime EQ** from the web UI, the way `components/dac_tas58xx/` does for the
TAS5825M.

The coefficient RAM addressing, the 1.23 encoding with halved `z^-1` taps, the
boost normalisation rule and the double-buffer write protocol are shared with
HF3 and are documented in `hf3-biamp-reverse-engineering.md` sections 2–4. This
file only covers what is specific to HF1.

| File | `w 98` lines | register ops |
|---|---|---|
| `DMA80/dma80-hf1-ppc-441.cfg` | 407 | 303 |
| `DMA80/dma80-hf1-ppc-44_1-new.cfg` | 407 | 303 |
| `Soundlink/soundlink-hf1.cfg` | 406 | 303 |

All three are 44.1 kHz PPC2 live logs, and all three replay the **same base
flow**: the coefficient image built from the download phase is byte-identical
across the three captures over words 0–270. They differ only in the tuning tail.

That is the single most useful fact here. Three independent tunings of one flow
means every word that changes between them is a user-facing parameter, and
every word that does not is fixed flow structure.

## 1. Tuning footprint

The 121 ops after `P0-R43 = 0x1F` write exactly the same set of slots in all
three captures:

```
83  88  93  98 103 108 113 118 123 128 133 138      12 biquads, stride 5
143(3 words) 146(5) 151(1) 152(25)                  DRC
177 182 189 194 199 204                             6 biquads
209(2) 251(2) 255(1) 0 1 2                          scalars
5(25)                                               DRC
```

## 2. The EQ chain — words 83…142

Twelve consecutive biquad slots. This is the user EQ, and its shipped defaults
prove it:

| Band | Word | Page/reg | Shipped default |
|---|---|---|---|
| 1 | 83 | `0x2E`/`0x5C` | 1st-order high-pass, 9.19 Hz |
| 2 | 88 | `0x2E`/`0x70` | peaking 14.95 Hz Q 1.02, **0 dB** |
| 3 | 93 | `0x2F`/`0x14` | peaking 29.51 Hz Q 1.00, **0 dB** |
| 4 | 98 | `0x2F`/`0x28` | peaking 58.84 Hz Q 1.00, **0 dB** |
| 5 | 103 | `0x2F`/`0x3C` | peaking 114.88 Hz Q 1.00, **0 dB** |
| 6 | 108 | `0x2F`/`0x50` | peaking 229.71 Hz Q 1.00, **0 dB** |
| 7 | 113 | `0x2F`/`0x64` | peaking 459.38 Hz Q 1.00, **0 dB** |
| 8 | 118 | `0x2F`/`0x78` | peaking 918.76 Hz Q 1.00, **0 dB** |
| 9 | 123 | `0x30`/`0x1C` | peaking 1837.50 Hz Q 0.98, **0 dB** |
| 10 | 128 | `0x30`/`0x30` | 1st-order high-pass, 4.59 Hz |
| 11 | 133 | `0x30`/`0x44` | unity |
| 12 | 138 | `0x30`/`0x58` | unity |

Bands 2–9 are octave spaced and sit at exact fractions of `fs`
(1837.5 = fs/24, 918.75 = fs/48, …), all at unity gain. That is PPC2's generic
"multi-band EQ, nothing dialled in yet" state. A 0 dB peaking section has
numerator == denominator, so those slots pass audio through untouched.

The chain is **not duplicated anywhere** — a search for a second copy of words
83…142 in the shipped image returns nothing. HF1 runs one EQ chain, not one per
channel.

## 3. What the three tunings put there

Every slot decodes cleanly as an RBJ peaking section. `(norm)` marks a boost
stored with the numerator divided by `b0`.

| Band | `dma80-hf1-ppc-441` | `dma80-hf1-ppc-44_1-new` | `soundlink-hf1` |
|---|---|---|---|
| 1 | 2619 Hz Q 0.69 −11.00 dB | 2032 Hz Q 5.23 −6.60 dB | 3626 Hz Q 1.94 −6.50 dB |
| 2 | 130.5 Hz Q 2.22 −6.20 dB | 112.5 Hz Q 3.33 −4.10 dB | 9649 Hz Q 1.85 −6.10 dB |
| 3 | 194.5 Hz Q 3.45 −5.60 dB | 758 Hz Q 16.7 −3.40 dB | 902 Hz Q 1.96 −5.70 dB |
| 4 | 344 Hz Q 9.66 −4.20 dB | 1338 Hz Q 13.9 −3.40 dB | 2498 Hz Q 5.22 −3.20 dB |
| 5 | 1301 Hz Q 1.40 −2.80 dB | 2536 Hz Q 5.55 −3.00 dB | 267 Hz Q 1.78 −2.10 dB |
| 6 | 7128 Hz Q 3.26 −2.00 dB | 1113 Hz Q 7.02 −1.40 dB | 1406 Hz Q 1.02 +1.00 dB (norm) |
| 7 | 690 Hz Q 0.94 +1.20 dB (norm) | 397 Hz Q 0.93 +1.30 dB (norm) | high shelf |
| 8 | 2993 Hz Q 0.79 +7.60 dB (norm) | 129.9 Hz Q 0.58 +3.59 dB (norm) | 294 Hz Q 0.57 +3.72 dB (norm) |
| 9 | high-pass 41.2 Hz Q 0.88 | 2380 Hz Q 2.94 +6.00 dB (norm) | 1320 Hz Q 7.82 +4.00 dB (norm) |
| 10 | 181 Hz Q 0.52 +5.19 dB (norm) | 6800 Hz Q 2.53 +6.00 dB (norm) | 1550 Hz Q 4.32 −1.50 dB |
| 11 | 75.6 Hz Q 0.67 +1.02 dB (norm) | 178 Hz Q 0.63 +2.07 dB (norm) | unity |
| 12 | unity | high-pass 41.2 Hz | unity |

Cuts are ordered by magnitude and land on exact 0.1 dB steps — these are
measurement-derived corrections (REW-style auto EQ) pasted into PPC2, not hand
tweaks. It confirms the slots take arbitrary f0/Q/gain, in any order, with no
constraint tying a slot to a frequency band.

## 4. The rest of the flow

Not EQ, but worth knowing so firmware does not stomp on it:

| Word | Block |
|---|---|
| 0, 1, 2 | constants +1.0, −1.0, +1.0 |
| 5…29 | DRC time constants (α / 1−α pairs summing to exactly 1.0) |
| 143…145, 151 | DRC thresholds |
| 146 | DRC detector filter |
| 152…176 | DRC time constants, second set |
| 177 | 2nd-order high-pass **150.0 Hz Q 0.707** (Butterworth rumble filter) |
| 182 | unity |
| 189 | 2nd-order low-pass **300.0 Hz Q 0.5** |
| 194 | 2nd-order high-pass **5000.0 Hz Q 0.5** |
| 199 | 2nd-order low-pass **5000.0 Hz Q 0.5** |
| 204 | 2nd-order high-pass **300.0 Hz Q 0.5** |
| 209, 210 | 1.0, 0.0 |
| 251, 252, 255 | output scalars (0.397164, 0.000154, 0.5) |

Words 189/194/199/204 form the band split of a **3-band DRC** at 300 Hz and
5 kHz, Linkwitz-Riley 2nd order, paired with the time constants at 5…29 and
152…176. They are not a loudspeaker crossover.

Every one of those corners is an exact round number, which is the strongest
confirmation that the coefficient decode is right.

## 5. Can we build a generic web EQ?

**Yes.** Everything needed is confirmed:

- 12 biquad slots at a fixed, known address, in the audio path, shipped at
  unity, in a flow that is identical across all three captures.
- Coefficient format and biquad convention verified against exact round values.
- Live write protocol verified (write bank, `page 0x2C reg 0x01 = 0x05`, write
  again). No flow re-download, no DSP stop, no audio interruption.
- The slots accept any f0/Q/gain, so we are free to pick our own band layout
  rather than inheriting PPC2's octave defaults.

### Shape of the work

The TAS5825M side is the template. The differences:

| | TAS5825M (existing) | TAS5754M (new) |
|---|---|---|
| Coefficient format | 5.27, 4 bytes | 1.23, 4 bytes, `z^-1` taps halved |
| Word order | `b0 b1 b2 −a1 −a2` | same, but `b1`/`a1` halved |
| Boost headroom | fits in 5.27 | must normalise by `b0` + make up gain |
| Addressing | book `0xAA` + page + sub-address | page only, `word → page/reg` formula |
| Auto-increment | plain register | needs `reg \| 0x80` |
| Commit | immediate | double-buffer swap, write twice |
| Bands | 15, table-driven, ±15 dB | 12 slots, computed at runtime |

What has to be written in `components/dac_tas57xx/`:

1. A coefficient-RAM write helper — there is currently **none**. The driver only
   replays a static blob through `tas57xx_write_hf()` and never touches
   coefficient RAM afterwards. It needs page select, `reg | 0x80`, splitting a
   biquad across a page boundary, and the bank swap.
2. RBJ peaking design in double precision plus a packer, including the `b0 > 1`
   normalisation and folding the makeup gain into the digital volume registers
   (`P0-R61`/`R62`).
3. A `tas57xx_eq_*` API mirroring `tas58xx_eq_set_band` / `set_all` / `flat`.
4. Re-apply after flow download. Unlike the TAS5825M, coefficient RAM here is
   rewritten every time the flow is downloaded, so the EQ must be re-applied
   from `tas57xx_on_i2s_started()` after `tas57xx_write_hf()` completes.
5. `web_server.c`: extend the existing `#ifdef CONFIG_DAC_TAS58XX` guard around
   `/api/eq` to cover `CONFIG_DAC_TAS57XX`. The JSON shape (`{"gains": [...],
   "bands": n}`) already carries a band count, and `data/www/eq.html` builds its
   sliders from it, so the UI needs little or no change if we keep the contract.
6. NVS: `settings_get_eq_gains` / `settings_set_eq_gains` already exist but are
   fixed at `SETTINGS_EQ_BANDS`; either reuse 15 and map onto 12 slots, or make
   the band count dynamic.

### The one real constraint

**Slot addresses are flow-specific.** Words 83…142 are HF1's layout; HF3 puts
its EQ at 48…67 and 77…96. A build that ships a different `.bin` gets a
different map, and there is nothing in the blob that identifies which flow it
is.

Options, in order of preference:

- Pin the EQ feature to a known flow per board via Kconfig, and store the slot
  base + count in the board definition. Least code, matches how flows are
  already chosen per target.
- Add a small sidecar file next to `/spiffs/hf/tas57xx_fw.bin` naming the slot
  base and count, generated by `hybridflow_convert_cfg.py`.
- Probe at runtime by looking for a run of ≥ 8 consecutive unity or
  numerator == denominator biquads in the downloaded image. Works on both HF1
  and HF3 but is fragile once a flow ships with pre-tuned EQ.

Also note the flow is sample-rate specific and these captures are 44.1 kHz, so
the RBJ design must use the loaded flow's `fs`, not an assumed 48 kHz — the
TAS5825M code's `BQ_SAMPLE_RATE_HZ` constant cannot be copied over as-is.
