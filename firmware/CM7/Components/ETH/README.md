# ETH Component

## Overview
The ETH component handles all Ethernet communication on the Cortex-M7. It runs two concurrent tasks: a TCP server that streams CSV data to one connected client and accepts runtime commands; and an NTRIP client that fetches SPARTN correction data from u-blox PointPerfect and forwards it to the GNSS chip via CM4. It also contains a custom PHY driver for the Texas Instruments DP83826.

## Files

| File | Purpose |
|---|---|
| `eth_wrapper.h / .c` | FreeRTOS tasks, NTRIP client, TCP server, GGA builder, state machine |
| `eth_custom_phy_interface.h / .c` | DP83826 PHY driver: init, RMII mode override, link-state query |

---

## Architecture

### 1. TCP Server (`Eth_Wrapper_Task`)
Binds a listening socket on **port 7** and accepts one client at a time. When a new connection arrives, the previous connection is forcibly closed. Incoming TCP data is passed directly to `Data_Wrapper_ProcessCommand` so the same commands work over both USB and Ethernet. Outgoing data is written by `Eth_Wrapper_Send`, which is called by `Data_Wrapper_Task` for every CSV line.

`Eth_Wrapper_Task` also spawns `NTRIP_Client_Task` once the Ethernet link is up.

### 2. NTRIP Client (`NTRIP_Client_Task`)
Runs a persistent state machine:

```
INIT → WAIT_LINK → DNS_TRYING → DNS_OK → WAIT_FIX → NTRIP_CONNECT → NTRIP_CONNECTED → NTRIP_RECEIVING
                                    ↓ fail
                                DNS_FAIL (red LED solid)
```

Steps:
1. **DNS test** — resolves `google.com` to verify internet connectivity. Primary DNS: `8.8.8.8`; fallback: `192.168.137.1` (Windows ICS gateway). Retries every 5 s on failure (`NET_STATE_DNS_FAIL` → red LED solid).
2. **Wait for fix** — blocks until both `g_gnss_last_lat` and `g_gnss_last_lon` are non-zero (both checked to avoid a race).
3. **Connect** — resolves `NTRIP_HOST`, opens a TCP connection to `NTRIP_PORT_NUM`, sends an HTTP/1.0 GET request with Base64-encoded `Authorization: Basic` credentials.
4. **On connect** — pushes `CMD_SET_CORRECTION_SPARTN` to the CM4 CMD queue; builds and sends a NMEA GGA sentence from the live GNSS position.
5. **Stream** — receives data in 500 ms timeout bursts. Every `NTRIP_GGA_INTERVAL_MS` (10 s) a fresh GGA is re-sent to keep the position-based stream alive. All received bytes (including the HTTP header) are forwarded unchanged to `Data_Wrapper_EnqueueRTCM` — the ZED-X20P's internal SPARTN parser syncs to valid frames and ignores the leading text.
6. **Disconnect** — on a natural disconnect (server close, cable unplug), automatically falls back to Galileo HAS (`CMD_SET_CORRECTION_GAL_HAS`). If NTRIP was manually disabled via `SET_CORR_GAL_HAS` or `SET_CORR_NONE`, the fallback command is skipped (the caller already sent the desired correction command). Reconnects after 2 s unless `g_ntrip_enabled == false`.

### 3. GGA Builder (`build_gga`)
Constructs a valid `$GPGGA` NMEA sentence from the live `g_gnss_last_lat` / `g_gnss_last_lon` globals using pure integer arithmetic (newlib-nano omits float printf support). The checksum is computed with a simple XOR over the sentence body.

### 4. DP83826 PHY Driver (`eth_custom_phy_interface.c`)
The board uses a **DP83826ERHBR** PHY (not the LAN8742 that CubeMX generates stubs for). The driver:

- **PHY address** — LED pins are all strapped to 3.3 V → ENHANCED mode → address 0x07. `eth_phy_init` runs a discovery scan to confirm.
- **RMII mode override** — In ENHANCED mode, `Strap1` defaults to MII mode at power-on. `eth_phy_init` writes `PHY_RCSR_RMII_MODE` (bit 5 of register 0x17) to switch the PHY to RMII leader mode so it outputs the 50 MHz clock on pin 19 → STM32 `PA1` (`ETH_REF_CLK`). Without this write the STM32 RMII MAC has no clock and link never comes up.
- **Elasticity buffer** — sets a 10-bit RX elasticity buffer for improved jitter tolerance.
- **Link state** — `eth_phy_get_link_state` reads PHYSTS (0x10) to return speed and duplex; `eth_phy_link_up` returns a simple boolean for polling by the LwIP driver.

A `phy_probe_t` snapshot struct (`g_phy`) exposes all relevant register values for live inspection in the STM32CubeIDE Expressions view.

---

## Configuration (edit `eth_wrapper.h`)

```c
#define NTRIP_HOST          "ppntrip.services.u-blox.com"
#define NTRIP_PORT_NUM      2101
#define NTRIP_MOUNTPOINT    "NEAR-SPARTN"
#define NTRIP_USER          "..."
#define NTRIP_PASS          "..."
#define NTRIP_GGA_INTERVAL_MS  10000   // how often to re-send GGA while streaming
```

---

## Observable State

```c
// Watch these in the debugger or log them over USB/TCP
volatile NetState_t g_net_state;     // overall NTRIP state machine state
volatile int        g_dns_err;       // last lwIP err_t from DNS (-3=TIMEOUT, -5=RTE)
volatile uint32_t   g_dns_tries;     // DNS attempt counter
volatile uint32_t   g_ntrip_bytes;   // cumulative SPARTN bytes received
volatile uint32_t   g_gga_sent;      // GGA sentences sent to server
volatile bool       g_ntrip_enabled; // false = disconnected and will not reconnect
```

---

## Dependencies
- LwIP Netconn API (`lwip/api.h`, `lwip/dns.h`)
- FreeRTOS / CMSIS-OS2
- `data_wrapper` — `Data_Wrapper_EnqueueRTCM`, `Data_Wrapper_ProcessCommand`, `g_gnss_last_lat/lon`
- `shared_memory` — CMD queue (correction source commands to CM4)
- STM32 HAL Ethernet + MDIO for PHY register access
