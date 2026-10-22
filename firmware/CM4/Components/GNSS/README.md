# GNSS Component

## Overview
This component interfaces with the u-blox ZED-X20P GNSS module on the Cortex-M4 core. It manages UART communication with DMA, parses binary UBX protocol messages, handles precise hardware timepulse synchronization, forwards SPARTN/RTCM correction bytes to the chip, and switches the active correction source on command from CM7.

## Files

| File | Purpose |
|---|---|
| `gnss_wrapper.h / .c` | FreeRTOS task, circular DMA buffer processing, safe PVT getter, correction source switching |
| `ublox_hpg_interface.h / .c` | UBX byte-level parser state machine, message structures, UBX-CFG-VALSET builders |

---

## Architecture

### 1. UART + Circular DMA Reception
Incoming GNSS bytes are received into a static 512-byte circular buffer via DMA (`HAL_UART_Receive_DMA`). The DMA pointer is forced to the AHB SRAM address space (`| 0x20000000`) so the DMA controller can access it correctly on the CM4 local alias. `GNSS_TaskBody` polls `__HAL_DMA_GET_COUNTER` every 10 ms to determine how many new bytes arrived and handles buffer wrap-around transparently.

### 2. UBX Parser (`ublox_hpg_interface.c`)
A 9-state byte-level state machine processes each byte in sequence:
`SYNC1 → SYNC2 → CLASS → ID → LEN_1 → LEN_2 → PAYLOAD → CK_A → CK_B`

Two message types are decoded:

**`UBX-NAV-PVT` (0x01/0x07, 92 bytes)**
Position, velocity, and UTC time. Stored and exposed via `GNSS_GetLatestPVT()` under a FreeRTOS critical section. Also packed into a `GnssPayload_t` (including the active `CorrectionSource_t`) and pushed to the CM7 shared memory GNSS queue.

**`UBX-TIM-TP` (0x0D/0x01, 16 bytes)**
Time-pulse advance notice. When the UTC time base is valid and the TP is locked to GNSS, the message's `towMS` and `week` fields are converted to a Unix millisecond timestamp and stored in `pending_pps_global_ms`. This value is consumed by the EXTI callback when the physical TIMEPULSE pin fires (see Timepulse section).

### 3. Timepulse / 1PPS Synchronization
High-precision sensor timestamping uses a two-step commit:

1. **Pending pair** — `UBX-TIM-TP` arrives over UART ≈20 ms before the next pulse and fills `pending_pps_global_ms` + `pending_pps_valid`.
2. **Hardware commit** — The ZED-X20P `TIMEPULSE` pin fires on `PA3` (TIM2 CH4). The EXTI callback captures the current `TIM2` counter value into `active_pps_local_ticks` and promotes the pending GPS time into `active_pps_global_ms`. The `sens_wrapper` reads this pair to convert its local TIM2 ticks into absolute GPS microseconds.

### 4. SPARTN/RTCM Forwarding
Each iteration of `GNSS_TaskBody` drains one entry from the shared-memory RTCM queue (populated by `Data_Wrapper_EnqueueRTCM` on CM7 when the NTRIP client receives data) and forwards the bytes to the chip over UART via `HAL_UART_Transmit`.

### 5. Correction Source Control
The active correction source is stored as a module-level `CorrectionSource_t` (default: `CORRECTION_GAL_HAS`) and is embedded in every `GnssPayload_t` sent to CM7 so the data log reflects which source was active.

Three functions build and transmit a `UBX-CFG-VALSET` (RAM-only layer) to switch the chip:

| Function | `CFG-NAVCOR-ENABLE_HOST` | `CFG-NAVCOR-ENABLE_GAL_HAS` |
|---|---|---|
| `GNSS_SetCorrectionSpartn()` | 1 | 0 |
| `GNSS_SetCorrectionGalileoHAS()` | 0 | 1 |
| `GNSS_SetCorrectionNone()` | 0 | 0 |

Commands arrive via the shared-memory CMD queue (sent by CM7 `Data_Wrapper_ProcessCommand` in response to `SET_CORR_*` serial/TCP commands, and automatically by the NTRIP client on connect/disconnect).

---

## Public API

```c
// Call once, before starting the FreeRTOS scheduler
void GNSS_Init(UART_HandleTypeDef *huart);

// FreeRTOS task entry point
void GNSS_TaskBody(void *argument);

// Thread-safe PVT getter — returns true and clears the flag if a new message arrived
bool GNSS_GetLatestPVT(UBX_NAV_PVT_t *out_pvt);

// Correction source switching (also callable from user code; CM7 uses the CMD queue)
void GNSS_SetCorrectionSpartn(void);
void GNSS_SetCorrectionGalileoHAS(void);
void GNSS_SetCorrectionNone(void);
```

---

## Dependencies
- STM32 HAL (UART, DMA, GPIO)
- FreeRTOS (tasks, critical sections)
- `shared_memory` — GNSS queue, RTCM queue, CMD queue
