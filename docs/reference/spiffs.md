# SPIFFS filesystem

The firmware keeps its web pages and DAC configuration files on a **SPIFFS partition** in
flash, so you can update the web UI and DSP programs without recompiling.

## Partition layout

A `storage` partition is added to the partition table:

| Board | Partition size | Address |
| --- | --- | --- |
| SqueezeAMP (8 MB or more) | 316 KB | 0x5B1000 |
| SqueezeAMP 4M | 192 KB | 0x3D1000 |

It is mounted at `/spiffs` on boot.

## Data directory

`data/` in the project root holds the files that get flashed to the partition:

```text
data/
├── www/               # Web interface pages
│   ├── index.html     # Setup and control panel
│   ├── logs.html      # Live log viewer
│   ├── bq.html        # Parametric biquad chains (TAS5825M boards)
│   └── speedtest.html # Network throughput test
├── hf/                # DSP programs loaded at boot
│   ├── tas57xx_fw.bin # Hybrid flow (SqueezeAMP)
│   └── tas58xx_fw.bin # PPC3 dump (TAS5825M/TAS5805M boards)
└── bg/                # ST7789 background image
    └── background.bin
```

## Flashing the image

!!! warning "PlatformIO does not do this for you"

    `pio run -t upload` writes the firmware only. Without a separate `-t uploadfs`, the
    device boots but the captive portal and web UI are missing, which shows up as
    "file not found" during setup. This is the single most common setup problem.

```bash
# PlatformIO — firmware first, then the filesystem
pio run -e <env> -t upload
pio run -e <env> -t uploadfs

# ESP-IDF — firmware, partition table and SPIFFS in one step
idf.py -p /dev/ttyUSB0 flash
```

**Upgrading from a build without the storage partition** must be done over serial. The
partition table itself changes, and OTA cannot rewrite it.

Once the partition exists you can update individual files over WiFi with the API below, or
re-flash the whole image over serial.

## File management API

Three HTTP endpoints manage SPIFFS files over WiFi without reflashing.

**Upload a file:**

```bash
curl -X POST "http://<device-ip>/api/fs/upload?path=/spiffs/hf/my_flow.bin" \
     --data-binary @my_flow.bin
```

**Delete a file:**

```bash
curl -X POST "http://<device-ip>/api/fs/delete?path=/spiffs/hf/old_flow.bin"
```

**List a directory:**

```bash
curl "http://<device-ip>/api/fs/list?dir=/spiffs/hf"
```

Paths are restricted to `/spiffs/` and directory traversal via `..` is rejected. The
maximum upload size is 64 KB.

## What lives on SPIFFS

| Path | Used by |
| --- | --- |
| `/spiffs/www/` | Web server — setup portal, logs, equaliser |
| `/spiffs/hf/tas57xx_fw.bin` | [SqueezeAMP hybrid flow DSP](../boards/squeezeamp.md#hybrid-flow-dsp) |
| `/spiffs/hf/tas58xx_fw.bin` | [Full PPC3 tuning](../boards/esparagus-audio-brick.md#full-ppc3-tuning) |
| `/spiffs/bg/background.bin` | [ST7789 background image](../features/tft-display.md#background-image) |
