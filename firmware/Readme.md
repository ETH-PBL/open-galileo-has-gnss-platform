# Ublox GNSS Node - Firmware

## Overview
This project is built on the **STM32H755ZIT3**, a high-performance dual-core microcontroller (Cortex-M7 + Cortex-M4). Should also work for the STM32H755ZIT6.

---

> **⚠ CubeMX Code Generation Warning**
> Pressing **Generate Code** in STM32CubeMX will overwrite `CM7/LWIP/Target/ethernetif.c` with a stock template, erasing all custom ETH diagnostic code, DMA descriptor addresses, and PHY logic. Before regenerating, commit your changes. After regenerating, revert this file via Git:
> ```
> git restore CM7/LWIP/Target/ethernetif.c
> ```
> or use the **Source Control** panel in VS Code to discard changes on that file only.

---

# System Architecture

## Cortex-M7: (Network & System Ops)
The M7 manages the communication to the outside world and the processing of data.

### FreeRTOS Tasks M7
*   `EthernetTask` (`Eth_Wrapper_Task`): Manages the LwIP stack (Netconn API). Runs a TCP server on **port 7** (data stream + commands) and spawns `NtripTask`.
*   `NtripTask` (`NTRIP_Client_Task`): DNS resolution, SPARTN correction download from PointPerfect, periodic GGA position updates. Automatically switches CM4 between SPARTN and Galileo HAS on connect/disconnect.
*   `UsbTask` (`USB_Wrapper_Task`): Serializes outbound CDC-serial data to the USB hardware.
*   `DataWrapperTask` (`Data_Wrapper_Task`): Drains CM4→CM7 shared-memory queues, formats CSV output, routes to USB + TCP, parses runtime commands.
*   `LEDTask` (`LED_Status_Task`): Drives the three status LEDs based on SPARTN activity, GNSS fix state, sensor liveness, and error flags.

### Pins and Interrupts
**1. Ethernet (LwIP + RMII)**
*   **PHY Chip:** Texas Instruments DP83826ERHBR.
*   **CubeMX Driver:** Configured using the `eth_custom_phy_interface` driver, located in CM7/Components/ETH.

**2. USB Virtual COM Port**
*   **Pins:** `PA11` (FS_DM), `PA12` (FS_DP)

**3. External QSPI NOR Flash**
*   **Chip:** Macronix MX25L51245GMI-08G (64 MB / 512 Mbit).
*   **Pins:** `PF6`, `PF7`, `PF8`, `PF9` (Data), `PF10` (CLK), `PG6` (CS).
*   **Hardware Reset Pin:** `PD15` (Configured as standard GPIO_Output, Default: High).

**4. External EEPROM (I2C1)**
*   **Pins:** `PB8` (SCL), `PB9` (SDA). 
*   **Purpose:** 8 KB storage (M24C64) for persistent settings like MAC address and static IPs.

---

## Cortex-M4: (Sensors & Radio)
The M4 core handles all strict real-time I/O, talking to the GNSS module, internal IMUs, and the MikroBUS expansion header.

### FreeRTOS Tasks M4
*   `GNSSTask` (`GNSS_TaskBody`): Polls the circular DMA buffer, feeds bytes to the UBX parser, forwards SPARTN corrections from the RTCM queue to the GNSS chip, and processes correction-source CMD commands from CM7.
*   `SENSTask` (`SENSTaskBody`): Blocks on the sensor event queue; on each interrupt event drains the hardware FIFO of the triggering sensor and pushes GPS-timestamped samples to CM7 via shared memory.
*   `MikroBusTask`: Manages the MikroBUS expansion header (SPI3, I2C4, UART5, ADC1, PWM). Not implemented.

### Pins and Interrupts
**1. GNSS Module**
*   **UART3:** `PD8` (TX), `PD9` (RX). Asynchronous mode.
*   **Control Pins (GPIO):**
    *   `PD7`: GNSS_RESET_N (Output Push Pull, High)
*   **Interrupts (EXTI):**
    *   `PC13`: GNSS_EXTINT
    *   `PE2`: GNSS_RTK_STAT
    *   `PE6`: GEOFENCE_STAT
*   **GNSS Timepulse** `PA3` TIM2 CH4

**2. On-Board Sensors (SENS_SPI)**
*   **SPI2:** `PA5` (SCK), `PB15` (MOSI), `PC2_C` (MISO).
*   **Configuration:** Full-Duplex Master, 10 MHz Baud Rate.
*   **Sensors:** 
    *   ISM330DLC (IMU — 6-DoF accel + gyro, 104 Hz, ±2 g / ±250 dps) — standard 4-wire SPI
    *   LSM303AGR (3-axis accelerometer + 3-axis magnetometer) — 3-wire SPI (MOSI/MISO shorted; MOSI floated during reads via GPIO reconfiguration)
*   **Interrupts (EXTI):**
    *   `PE3`: SENS_ISM_INT (ISM330DLC FIFO watermark)
    *   `PE4`: SENS_LSM_INT (LSM303AGR accelerometer FIFO watermark)
    *   `PE5`: SENS_LSM_INTM (LSM303AGR magnetometer threshold interrupt, used as DRDY proxy)

**3. MikroBUS Header**
*   **SPI3:** `PC10` (SCK), `PC11` (MISO), `PC12` (MOSI). 
*   **I2C4:** `PF14` (SCL), `PF15` (SDA).
*   **UART5:** `PC12` (TX), `PD2` (RX).
*   **ADC1:** `PA0` (INP16). Configured for 16-bit, single-ended reads.
*   **PWM:** `PA0` (TIM4)
*   **Control Pins (GPIO):**
    *   `PB0`: MB_RST (Reset pin for module, Output Push Pull, High)
*   **Interrupts (EXTI):**
    *   `PC8`: MB_INT (MikroBUS Hardware Interrupt)


### SPI Chip Selects (GPIO Outputs)
All SPI Chip Selects are handled manually in software. They are configured as **GPIO_Output** on the M4 core with a **Default High** state (active low).
*   `PB12`: SPI_CS_MB (MikroBUS)
*   `PD13`: SPI_CS_XL (Accelerometer)
*   `PG7`: SPI_CS_MAG (Magnetometer)
*   `PG8`: SPI_CS_ISM (IMU)

---

## GNSS Chip Configuration (ZED-X20P)

Configure once via **u-center 2** before deploying the firmware. Save all settings with `CFG-CFG` → save to BBR + Flash.

> **Board version note:**
> - **v1.0** — uses **UART2** of the ZED-X20P (connected to STM32 UART3, `PD8`/`PD9`). Apply the UART2 settings below.
> - **v2.0** — uses **UART1** of the ZED-X20P. Replace every `UART2` key with `UART1` in the tables below.

### UART Port (UART2 for v1.0 / UART1 for v2.0)
The active UART port on the ZED-X20P is physically connected to the STM32 UART3 (`PD8` TX → chip RX, `PD9` RX ← chip TX).

| CFG Key (v1.0) | CFG Key (v2.0) | Value | Notes |
|---|---|---|---|
| `CFG-UART2-BAUDRATE` | `CFG-UART1-BAUDRATE` | `115200` | Must match STM32 UART3 baud rate |
| `CFG-UART2INPROT-UBX` | `CFG-UART1INPROT-UBX` | `1` | Accept UBX commands from STM32 |
| `CFG-UART2INPROT-NMEA` | `CFG-UART1INPROT-NMEA` | `0` | Disable unused input protocol |
| `CFG-UART2INPROT-SPARTN` | `CFG-UART1INPROT-SPARTN` | `1` | Accept SPARTN corrections forwarded by STM32 via `HAL_UART_Transmit` |
| `CFG-UART2OUTPROT-UBX` | `CFG-UART1OUTPROT-UBX` | `1` | Send UBX messages to STM32 |
| `CFG-UART2OUTPROT-NMEA` | `CFG-UART1OUTPROT-NMEA` | `0` | Disable NMEA output to keep link clear |

### UBX Messages (enable on the active UART port)

| CFG Key (v1.0) | CFG Key (v2.0) | Value | Purpose in firmware |
|---|---|---|---|
| `CFG-MSGOUT-UBX_NAV_PVT_UART2` | `CFG-MSGOUT-UBX_NAV_PVT_UART1` | `1` | Parsed by `gnss_wrapper` — position, velocity, time forwarded to CM7 via shared memory |
| `CFG-MSGOUT-UBX_TIM_TP_UART2` | `CFG-MSGOUT-UBX_TIM_TP_UART1` | `1` | Timepulse timing — used by `sens_wrapper` to anchor sensor timestamps to GPS time |

All other `CFG-MSGOUT-*_UART2` (v1.0) or `CFG-MSGOUT-*_UART1` (v2.0) keys should be left at `0`.

### Navigation Rate

| CFG Key | Value | Notes |
|---|---|---|
| `CFG-RATE-MEAS` | `100` | Measurement period in ms → 10 Hz |
| `CFG-RATE-NAV` | `1` | One navigation solution per measurement |

### PointPerfect / SPARTN

| CFG Key | Value | Notes |
|---|---|---|
| `CFG-NAVCOR-ENABLE_HOST` | `0` | **Managed automatically by firmware** — set to `1` by `gnss_wrapper` when the NTRIP client connects, back to `0` when the connection drops. Leave at `0` as the u-center default. |
| `CFG-SPARTN-USE_SOURCE` | `1` | IP/NTRIP delivery mode — corrections arrive via UART2 from the STM32 |

### Galileo HAS — Free PPP Fallback

Galileo HAS (High Accuracy Service) is a free satellite-broadcast PPP augmentation on the E6-B signal. The ZED-X20P receives it directly via its own antenna, giving ~20–30 cm accuracy without any subscription or internet connection.

**Correction source switching is handled automatically by the firmware:**
- On startup, `gnss_wrapper` enables Galileo HAS (`CFG-NAVCOR-ENABLE_HOST=0`, `CFG-NAVCOR-ENABLE_GAL_HAS=1`).
- When the NTRIP client connects and SPARTN data starts flowing, it switches to HOST mode (`CFG-NAVCOR-ENABLE_HOST=1`, `CFG-NAVCOR-ENABLE_GAL_HAS=0`). Per u-blox documentation, HOST corrections take priority when both flags are enabled, so disabling GAL_HAS avoids ambiguity.
- When the NTRIP connection is lost (ethernet unplugged or server unreachable), the firmware switches back to Galileo HAS.

| CFG Key | Value | Notes |
|---|---|---|
| `CFG-SIGNAL-GAL_E6_ENA` | `1` | Enable Galileo E6 signal reception (HAS data is carried on E6-B sub-carrier) — set once in u-center |
| `CFG-NAVCOR-ENABLE_GAL_HAS` | `1` | **Managed automatically by firmware** — set as u-center default so the chip uses HAS on first boot before the firmware takes over |

> **Known limitation (HPG 2.10):** Galileo HAS and OSNMA (Open Service Navigation Message Authentication) cannot be enabled at the same time. If OSNMA is required, set `CFG-NAVCOR-ENABLE_GAL_HAS` = `0` and remove the `GNSS_SetCorrectionGalileoHAS()` startup call in `gnss_wrapper.c`.

### Timepulse Output

| CFG Key | Value | Notes |
|---|---|---|
| `CFG-TP-TP1-ENA` | `1` | Enable timepulse 1 output |
| `CFG-TP-PULSE_DEF` | `0` | Period mode (not frequency mode) |
| `CFG-TP-PERIOD_TP1` | `1000000` | Period in µs → 1 Hz |
| `CFG-TP-LEN_TP1` | `100000` | Pulse width in µs → 100 ms |
| `CFG-TP-TIMEGRID_TP1` | `1` | Lock to UTC |
| `CFG-TP-ALIGN_TO_TOW_TP1` | `1` | Align rising edge to top of second |
| `CFG-TP-POL_TP1` | `1` | Rising edge at top of second |

Connected to STM32 `PA3` (TIM2 CH4). The rising edge is captured by `sens_wrapper` to anchor IMU/magnetometer timestamps to GPS time.
---

## Status LEDs

All LEDs are **active-low** (GPIO LOW = ON, GPIO HIGH = OFF).  
Logic runs in `LED_Status_Task` on the M7 core (spawned from `Data_Wrapper_Task`).

| LED | Pin | State | Meaning |
|---|---|---|---|
| Green | `PG9` | Solid ON | SPARTN corrections flowing (received in last 5 s) |
| Green | `PG9` | 80 ms flash | New GNSS location received, no SPARTN data |
| Green | `PG9` | OFF | No GNSS or SPARTN activity |
| Yellow | `PE1` | ON | Sensor packets arriving but timestamp = 0 (PPS not yet locked) |
| Yellow | `PE1` | OFF | Sensors timestamped correctly, or no sensor data |
| Red | `PB14` | Blinking | No sensor data received in last 2 s |
| Red | `PB14` | Solid ON | Error — DNS failure / no internet access |
| Red | `PB14` | OFF | Sensors active, no error |

---

## Runtime Commands

Commands can be sent as plain ASCII strings (case-insensitive, trailing whitespace and CR/LF ignored) over two interfaces:

- **USB serial** — open any serial terminal at 250000 baud, type the command and press Enter.
- **TCP** — connect to the device on port 7 and send the command string (the same connection that streams data).

### Logging filter

These commands control which data types are formatted and forwarded over USB and Ethernet. The CM4→CM7 shared-memory queues are always drained regardless; only the output is filtered.

| Command | Effect |
|---|---|
| `LOG_ALL` | Enable all outputs — ISM330, LSM303 ACC/MAG, and GNSS PVT lines (default) |
| `LOG_GNSS` | GNSS PVT lines only — sensor lines are silently dropped |

### Correction source

These commands override the active GNSS correction source. The automatic switching logic (Galileo HAS on startup → SPARTN when NTRIP connects → Galileo HAS when NTRIP drops) remains in place but can be overridden at any time.

| Command | Effect |
|---|---|
| `SET_CORR_SPARTN` | Switch CM4 to SPARTN (HOST) corrections. Re-enables NTRIP reconnection if it was previously disabled. If NTRIP is already connected the switch is immediate; if not, an error is returned and the device will switch automatically once the NTRIP connection is re-established. |
| `SET_CORR_GAL_HAS` | Switch CM4 to Galileo HAS corrections and **immediately disconnect** from the NTRIP server to stop data charges. The NTRIP client will not reconnect until `SET_CORR_SPARTN` is sent. |
| `SET_CORR_NONE` | Disable all active corrections and **immediately disconnect** from the NTRIP server. The NTRIP client will not reconnect until `SET_CORR_SPARTN` is sent. |

> **Note on NTRIP charges:** `SET_CORR_GAL_HAS` and `SET_CORR_NONE` both close the TCP connection to the PointPerfect NTRIP server within one 500 ms timeout cycle, stopping any data usage. `SET_CORR_SPARTN` is required to re-enable the connection.

---

## Data Output Format

Data is streamed as ASCII CSV lines over both USB serial and the TCP connection on **port 7**. Each line starts with a type tag:

| Tag | Fields |
|---|---|
| `ISM330` | `timestamp_us, ax, ay, az, gx, gy, gz` |
| `LSM_ACC` | `timestamp_us, ax, ay, az` |
| `LSM_MAG` | `timestamp_us, mx, my, mz` |
| `GNSS_PVT` | `epoch_us, year, month, day, hour, min, sec, nano, iTOW, valid, tAcc, fixType, flags, numSV, lon, lat, height, hMSL, hAcc, vAcc, velN, velE, velD, gSpeed, heading, sAcc, headingAcc, pDOP, correction_source` |

Sensor timestamps are in **microseconds** (GPS-synchronized via the GNSS 1PPS signal). A timestamp of `0` means no GPS fix was available when the sample was captured. GNSS `epoch_us` is Unix time in microseconds. `correction_source` is `SPARTN`, `GAL_HAS`, or `NONE`.

---

## Component Documentation

Detailed documentation for each firmware module lives in the component README files:

| Component | Core | README |
|---|---|---|
| GNSS wrapper + UBX parser | CM4 | [CM4/Components/GNSS/README.md](CM4/Components/GNSS/README.md) |
| Sensor wrapper (ISM330DLC, LSM303AGR) | CM4 | [CM4/Components/SENS/README.md](CM4/Components/SENS/README.md) |
| Data wrapper + LED task + command parser | CM7 | [CM7/Components/Data/README.md](CM7/Components/Data/README.md) |
| Ethernet wrapper + NTRIP client + DP83826 PHY | CM7 | [CM7/Components/ETH/README.md](CM7/Components/ETH/README.md) |
| USB CDC wrapper | CM7 | [CM7/Components/USB/README.md](CM7/Components/USB/README.md) |

---

## Authors

**Ciril Clerc** (Bachelor Student)  
cclerc@student.ethz.ch

## Credits

**Leon Nachtrab** (Master Student)  
lnachtrab@ethz.ch

**Henrique Duarte Hachmeister Caraça** (Master Student)  
hduarte@student.ethz.ch

**Marvin Steinkellner** (Bachelor Student)  
msteinkel@ethz.ch

**Dr. Philipp Mayer** (Supervisor)  
mayerph@iis.ee.ethz.ch

**Julian Moosmann** (Supervisor)  
julian.moosmann@pbl.ee.ethz.ch

---

## AI Usage Declaration

The following generative AI tools were used during the development of this firmware codebase:

- **Claude Code** (Anthropic) — used for code generation assistance, debugging, exploring implementation strategies, and writing documentation.
- **Google Gemini** (Google) — used for code generation assistance, debugging, exploring implementation strategies, and writing documentation.

All AI-generated output was critically reviewed, tested, and modified by the author to ensure correctness, security, and fitness for purpose. The author takes full responsibility for the final code and its implementation.