# Python Scripts

Host-side tools for recording, visualising, and analysing data from the STM32H755 firmware.
All scripts read or write JSONL files (one JSON object per line) stored in `log_data/`.

---

## Data Flow

```
STM32 firmware
      │
      ├─── USB CDC (serial) ──► serial_logger.py  ──► log_data/*.jsonl
      │
      └─── Ethernet TCP :7  ──► tcp_logger.py     ──► log_data/*.jsonl
                           ──► tcp_listener.py    (print only, no file)

log_data/*.jsonl
      │
      ├──► plot_gnss.py                simple single-file map
      ├──► plot_gnss_advanced.py      multi-dataset interactive map
      ├──► plot_gnss_diff.py          distance comparison between two or three recordings
      ├──► plot_gnss_fixed_accuracy.py  accuracy vs a known ground-truth point
      └──► frequency_analyser.py      sensor rate & gap audit
```

---

## Scripts

### `serial_logger.py` — USB serial recorder

Reads the STM32 USB-CDC output, parses the CSV sensor stream, and writes each
record as a JSON object to a `.jsonl` log file.

At startup the script prompts interactively for:
1. **COM port** — lists all available ports and lets you pick one.
2. **Launch commands** — optional firmware commands sent once after connecting.

The log filename is generated automatically from the selected commands and the
current timestamp (e.g. `log_gnss_has_20260101_120000.jsonl`).

**Configuration** (edit at the top of the file):

| Variable | Default | Description |
|---|---|---|
| `BAUD_RATE` | `250000` | Serial baud rate |

**Launch commands:**

| Key | Command | Effect |
|---|---|---|
| `1` | `LOG_ALL` | Log all sensors (IMU + GNSS) — default |
| `2` | `LOG_GNSS` | Log GNSS PVT only |
| `3` | `SET_CORR_SPARTN` | Use SPARTN / PointPerfect corrections |
| `4` | `SET_CORR_GAL_HAS` | Use Galileo HAS free PPP corrections |
| `5` | `SET_CORR_NONE` | Disable all corrections |

**Usage:**
```
python serial_logger.py
# Stop with Ctrl+C
```

---

### `tcp_logger.py` — Ethernet TCP recorder

Same as `serial_logger.py` but connects over Ethernet TCP port 7 instead of USB.
Reconnects automatically if the connection drops.

At startup the script prompts for **launch commands** (same set as `serial_logger.py`).
The log filename is generated automatically (e.g. `log_tcp_gnss_has_20260101_120000.jsonl`).

**Configuration** (edit at the top of the file):

| Variable | Default | Description |
|---|---|---|
| `SERVER_IP` | `192.168.137.42` | STM32 IP address |
| `SERVER_PORT` | `7` | TCP port (fixed in firmware) |

**Usage:**
```
python tcp_logger.py
# Stop with Ctrl+C
```

---

### `tcp_listener.py` — Ethernet monitor (print only)

Connects to the STM32 TCP server and prints raw lines to the terminal.
Does **not** write any file — useful for quick live inspection.

**Configuration:**

| Variable | Default | Description |
|---|---|---|
| `SERVER_IP` | `192.168.137.42` | STM32 IP address |
| `SERVER_PORT` | `7` | TCP port |

**Usage:**
```
python tcp_listener.py
```

---

### `plot_gnss.py` — Simple GNSS map

Plots a single `.jsonl` file as coloured dots on a satellite basemap.
Colour encodes horizontal accuracy (green = best, red = worst).

**Configuration** (edit at the top of the file):

| Variable | Default |
|---|---|
| `INPUT_LOG` | `log_data/log_gnss_has_20260607_190908.jsonl` |
| `OUTPUT_HTML` | `log_data/rotation_has.html` |

**Usage:**
```
python plot_gnss.py
```

---

### `plot_gnss_advanced.py` — Multi-dataset interactive map

Loads every `.jsonl` file in `log_data/` (or explicit files from the command line)
and overlays all datasets on a single satellite map with a shared layer control.

**Features:**
- Marker shape encodes RTK fix quality:
  - ● Circle — RTK Fixed (`carrSoln = 2`)
  - ■ Square — RTK Float (`carrSoln = 1`)
  - ▲ Triangle — No RTK (`carrSoln = 0`)
- Each dataset gets its own colour (up to 8 datasets).
- **Headings layer** (off by default): one direction arrow per second showing
  vehicle heading. Toggle per dataset in the layer control.
- **Antenna offset panel** (top-left): three offset fields per dataset —
  *Hdg* (cm along recorded heading), *N* (cm north), *E* (cm east).
  Press Enter in any field or click *Apply all*; *Reset* returns all markers
  to their logged positions.
- Tooltip on hover: timestamp (UTC), fix type, hAcc, vAcc, correction source,
  satellite count, heading.
- Output: `log_data/map_comparison2.html`

**Usage:**
```bash
# auto-load all .jsonl files in log_data/
python plot_gnss_advanced.py

# explicit files
python plot_gnss_advanced.py log_data/log_gnss_has.jsonl log_data/log_gnss_spartn.jsonl
```

**Key constants** (edit at the top of the file):

| Constant | Default | Description |
|---|---|---|
| `HEADING_STEP` | `10` | One arrow every N points (10 Hz → 1 arrow/s) |
| `ARROW_LENGTH_M` | `5` | Arrow shaft length in metres |
| `MARKER_RADIUS` | `5` | Marker radius in pixels |

---

### `plot_gnss_diff.py` — Two- or three-antenna distance comparison

Loads two or three simultaneously recorded `.jsonl` files, matches epochs by GPS
Time of Week (iTOW), and plots the position differences between the antennas over
time. Every pair of datasets is compared.

At startup the interactive picker lets you select 2 or 3 files, or run the
predefined **batch** of pairs defined in `PREDEFINED_BATCH_PAIRS` at the top of
the script. When not in batch mode the script prompts for a per-antenna heading
offset (in cm) to correct the physical separation between antennas.

**Output plots (2 × 2 grid, saved as PNG at 500 dpi):**

| Panel | Content |
|---|---|
| Top-left | Horizontal distance between each antenna pair (log scale) + mean/median + stats box |
| Top-right | N/E scatter plot (spatial bias per pair) |
| Bottom-left | ΔNorth and ΔEast over time (symlog) |
| Bottom-right | hAcc self-estimate from all chips over time |

Output file: `log_data/diff_<label_a>_vs_<label_b>[_vs_<label_c>].png`

**Usage:**
```bash
# interactive file picker (2 or 3 files, or 'batch')
python plot_gnss_diff.py

# explicit files
python plot_gnss_diff.py log_data/log_gnss_has.jsonl log_data/log_gnss_spartn.jsonl
```

Interactive prompt when selecting files manually:
```
Antenna offset correction  (positive = forward along heading, 0 = none)
  Offset A  [log_gnss_has]  cm: 10.8
  Offset B  [log_gnss_spartn]  cm: 0
```

---

### `plot_gnss_fixed_accuracy.py` — Accuracy vs fixed ground truth

Loads one or more `.jsonl` logs and measures how far each GNSS epoch is from a
hardcoded surveyed reference point.  The script detects the cold-start moment
(first `fixType > 0 → 0` transition), drops pre-cold-start data, and plots only
the post-reacquisition window.  `t = 0` on the time axis is the cold-start moment;
data begins at TTFF.

**Setup:** configure the ground-truth position at the top of the file.

| Mode | Variables to set | Description |
|---|---|---|
| LV95 (default) | `USE_LV95 = True`, `LV95_E`, `LV95_N`, `LV95_H_LN02` | Swiss LV95 / LN02 input — converted via the Swisstopo API + pyproj ETRS89 → ITRF2020 pipeline |
| WGS-84 | `USE_LV95 = False`, `GROUND_TRUTH_LAT`, `GROUND_TRUTH_LON`, `GROUND_TRUTH_HMSL` | Direct decimal degrees + MSL height |

**Output plots (2 × 2 grid, saved as PNG at 500 dpi):**

*Single file:*

| Panel | Content |
|---|---|
| Top-left | Horizontal error to ground truth + hAcc self-estimate (log scale) + stats box + fix-state transition lines |
| Top-right | N/E scatter coloured by fix type (RTK Fixed / Float / No RTK) with CEP50/CEP95 circles and mean-offset marker |
| Bottom-left | Height error vs ±vAcc self-estimate (symlog) |
| Bottom-right | ΔNorth and ΔEast offset over time (symlog) |

*Multiple files (overlay mode):*

| Panel | Content |
|---|---|
| Top-left | Horizontal error for each dataset (one colour per dataset) |
| Top-right | N/E scatter with CEP50 circles per dataset |
| Bottom-left | Height error per dataset |
| Bottom-right | hAcc self-estimate per dataset |

Output file: `log_data/accuracy_<label>.png` (single) or
`log_data/accuracy_<label_a>_vs_<label_b>[_vs_...].png` (multi).

**Usage:**
```bash
# interactive file picker (single, multiple, 'all', or 'batch')
python plot_gnss_fixed_accuracy.py

# explicit file(s)
python plot_gnss_fixed_accuracy.py log_data/log_gnss_has_20260101_120000.jsonl
python plot_gnss_fixed_accuracy.py log_data/log_gnss_has.jsonl log_data/log_gnss_spartn.jsonl
```

**Terminal output (example, single file):**
```
[log_gnss_has_20260606_164309]
  42 pre-fix records dropped, 2941 post-fix records used
  TTFF: 38.2 s
  Time to Float: 48.5 s
  Time to Fixed: 67.1 s
  Horizontal CEP50=3.1cm, CEP95=8.4cm, StdDev=3.9cm
  Mean offset: N=+1.2cm, E=-0.8cm, H=+2.1cm
  Fixes: Fixed=2941 Float=87 NoRTK=10
```

---

### `frequency_analyser.py` — Sensor rate & gap audit

Reads a `.jsonl` log file and prints a per-sensor frequency and data-quality
report. Also validates that the GNSS `epoch_us` timestamp matches the UTC
calendar fields (`year`, `month`, …, `nano`) embedded in each `GNSS_PVT` record.

**Configuration** (edit at the top of the file):

| Variable | Default |
|---|---|
| `INPUT_LOG` | `log_data/log_m1_has.jsonl` |

**Usage:**
```
python frequency_analyser.py
```

**Example output:**
```
[GNSS_PVT]
  Total Measurements : 3038
  Avg Frequency      : 9.98 Hz
  Avg Interval       : 100.17 ms
  Median Interval    : 100.00 ms
  Data Quality       : EXCELLENT - No missing measurements detected.
  Time Validation    : PASSED - 3038/3038 calculated epoch_us match UTC fields.
```

---

## Log File Format (`log_data/*.jsonl`)

Each line is a self-contained JSON object. The `sensor` field identifies the type.

### `GNSS_PVT`
```json
{
  "sensor": "GNSS_PVT",
  "epoch_us": 1735000000000000,
  "year": 2025, "month": 6, "day": 12,
  "hour": 14, "min": 23, "sec": 45, "nano": 123000000,
  "iTOW": 432225000,
  "valid": 55, "tAcc": 15,
  "fixType": 3, "flags": 131, "numSV": 18,
  "lon": 74523410, "lat": 470123456,
  "height": 456789, "hMSL": 400000,
  "hAcc": 42, "vAcc": 61,
  "velN": 100, "velE": -50, "velD": 5,
  "gSpeed": 112, "heading": 3456700,
  "sAcc": 50, "headingAcc": 9000000, "pDOP": 110,
  "correction_source": "SPARTN"
}
```

Key field notes:
- `lat`, `lon`: integer × 10⁻⁷ degrees (divide by `1e7` for decimal degrees)
- `hAcc`, `vAcc`: mm (divide by `1000` for metres)
- `height`: WGS-84 ellipsoidal height in mm
- `hMSL`: height above MSL (EGM96) in mm
- `heading`: integer × 10⁻⁵ degrees (divide by `1e5` for decimal degrees, clockwise from North)
- `headingAcc`: integer × 10⁻⁵ degrees
- `flags` bits 7–6: `carrSoln` — `0` = no RTK, `1` = float, `2` = fixed
- `epoch_us`: Unix timestamp in microseconds (STM32 system clock, anchored to GNSS 1PPS)

### `ISM330` (accelerometer + gyroscope)
```json
{"sensor": "ISM330", "ts": 1735000000000000, "acc_x": 120, "acc_y": -30, "acc_z": 9810, "gyr_x": 5, "gyr_y": -2, "gyr_z": 0}
```

### `LSM_ACC` / `LSM_MAG` (accelerometer / magnetometer)
```json
{"sensor": "LSM_ACC", "ts": 1735000000000000, "acc_x": 100, "acc_y": 20, "acc_z": 9800}
{"sensor": "LSM_MAG", "ts": 1735000000000000, "mag_x": 250, "mag_y": -100, "mag_z": 400}
```

`ts` is the same microsecond epoch as `epoch_us` in GNSS records.

---

## Dependencies

```
pip install pyserial folium branca matplotlib pyproj requests
```

| Package | Used by |
|---|---|
| `pyserial` | `serial_logger.py` |
| `folium`, `branca` | `plot_gnss.py`, `plot_gnss_advanced.py` |
| `matplotlib` | `plot_gnss_diff.py`, `plot_gnss_fixed_accuracy.py` |
| `requests` | `plot_gnss_fixed_accuracy.py` — calls the Swisstopo REFRAME API for LV95 → WGS-84 conversion |
| `pyproj` | `plot_gnss_fixed_accuracy.py` — ETRS89 → ITRF2020 time-dependent Helmert transformation |
| stdlib only | `tcp_logger.py`, `tcp_listener.py`, `frequency_analyser.py` |
