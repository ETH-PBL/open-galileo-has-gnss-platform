# Data Component

## Overview
The Data component is the central hub on the Cortex-M7. It drains the CM4→CM7 shared-memory queues (sensor and GNSS data), formats the readings as CSV lines, and broadcasts them over both USB (virtual COM port) and the TCP server. It also parses runtime commands arriving from either interface and routes them to the appropriate subsystem.

## Files

| File | Purpose |
|---|---|
| `data_wrapper.h / .c` | FreeRTOS task, CSV formatting, output routing, command parser, RTCM enqueue helper, LED status task |

---

## Architecture

### 1. Main Task (`Data_Wrapper_Task`)
The task runs in a tight loop on the M7 core:

1. **Command polling** — checks the USB RX queue (non-blocking) and calls `Data_Wrapper_ProcessCommand` on any received bytes.
2. **Sensor queue drain** — calls `dequeue_sens` in a loop, updates LED counters, and appends a CSV line to an internal 512-byte buffer for each packet when sensor logging is enabled.
3. **GNSS queue drain** — calls `dequeue_gnss`, updates the live lat/lon globals used by the NTRIP GGA builder, and appends a GNSS_PVT CSV line when GNSS logging is enabled.
4. **Flush** — when the buffer has data, calls `Send_Data` which transmits over USB and TCP simultaneously, then yields 1 ms to let LwIP process the sent packets.

The task also spawns `LED_Status_Task` at startup.

### 2. CSV Output Format

**Sensor lines:**
```
ISM330,<timestamp_us>,<ax>,<ay>,<az>,<gx>,<gy>,<gz>
LSM_ACC,<timestamp_us>,<ax>,<ay>,<az>
LSM_MAG,<timestamp_us>,<mx>,<my>,<mz>
```
Timestamps are in microseconds (GPS-synchronized). A timestamp of `0` means no GPS fix was available when the sample was taken.

**GNSS line:**
```
GNSS_PVT,<epoch_us>,<year>,<month>,<day>,<hour>,<min>,<sec>,<nano>,<iTOW>,<valid>,<tAcc>,
         <fixType>,<flags>,<numSV>,<lon>,<lat>,<height>,<hMSL>,<hAcc>,<vAcc>,
         <velN>,<velE>,<velD>,<gSpeed>,<heading>,<sAcc>,<headingAcc>,<pDOP>,<correction_source>
```
`epoch_us` is Unix time in microseconds derived from the PVT UTC fields. `correction_source` is `SPARTN`, `GAL_HAS`, or `NONE`.

### 3. Command Parser (`Data_Wrapper_ProcessCommand`)
Accepts plain ASCII strings (case-insensitive, trailing CR/LF/space stripped). Called from both the USB RX path and the TCP server receive path in `eth_wrapper`.

| Command | Action |
|---|---|
| `LOG_ALL` | Enable CSV output for sensors + GNSS (default) |
| `LOG_GNSS` | Disable sensor output; GNSS lines still emitted |
| `SET_CORR_SPARTN` | Sets `g_ntrip_enabled = true`; if NTRIP is already connected, pushes `CMD_SET_CORRECTION_SPARTN` to the CMD queue for CM4 immediately; otherwise returns an error string and waits for the connection to come up |
| `SET_CORR_GAL_HAS` | Sets `g_ntrip_enabled = false` (disconnects NTRIP); pushes `CMD_SET_CORRECTION_GAL_HAS` to CM4 |
| `SET_CORR_NONE` | Sets `g_ntrip_enabled = false` (disconnects NTRIP); pushes `CMD_SET_CORRECTION_NONE` to CM4 |

### 4. RTCM Enqueue Helper (`Data_Wrapper_EnqueueRTCM`)
Called by the NTRIP client (in `eth_wrapper`) whenever SPARTN correction bytes arrive. Splits the data into `RtcmPayload_t` chunks (≤512 bytes each) and pushes them to the shared-memory RTCM queue so CM4's `gnss_wrapper` can forward them to the GNSS chip.

### 5. LED Status Task (`LED_Status_Task`)
A low-priority task spawned by `Data_Wrapper_Task`. It polls four counters every 250 ms and drives the three active-low status LEDs:

| LED | Pin | Condition |
|---|---|---|
| Green solid | `PG9` | SPARTN bytes received in the last 5 s (`g_ntrip_bytes` rising) |
| Green 80 ms flash | `PG9` | New GNSS location received, no SPARTN |
| Green off | `PG9` | No GNSS or SPARTN activity |
| Yellow on | `PE1` | Sensor packets arriving but timestamp = 0 (no PPS lock) |
| Yellow off | `PE1` | Sensors correctly timestamped, or no sensor data |
| Red blink ~2 Hz | `PB14` | No sensor data received in last 2 s |
| Red solid | `PB14` | DNS failure / no internet (`NET_STATE_DNS_FAIL`) |
| Red off | `PB14` | Sensors active, no error |

---

## Global Variables

```c
volatile int32_t  g_gnss_last_lat;    // Latest fix latitude  (degrees × 1e-7, + = North)
volatile int32_t  g_gnss_last_lon;    // Latest fix longitude (degrees × 1e-7, + = East)
volatile uint32_t g_gnss_pkt_count;   // Total GNSS packets received
volatile uint32_t g_sens_pkt_count;   // Total sensor packets received
volatile uint32_t g_sens_no_ts_count; // Sensor packets with timestamp == 0
```

---

## Dependencies
- FreeRTOS / CMSIS-OS2
- `shared_memory` — sensor queue, GNSS queue, RTCM queue, CMD queue
- `usb_wrapper` — USB TX/RX queues
- `eth_wrapper` — `Eth_Wrapper_Send`, `g_net_state`, `g_ntrip_bytes`, `g_ntrip_enabled`
- STM32 HAL (GPIO for LEDs)
