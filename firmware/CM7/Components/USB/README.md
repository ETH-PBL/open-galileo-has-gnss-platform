# USB Component

## Overview
This component provides a thin FreeRTOS-friendly wrapper around the STM32 USB CDC (Virtual COM Port) driver. It decouples the application from the low-level ISR callbacks using two FreeRTOS queues: one for outbound data that needs to be serialized to the USB hardware, and one for inbound data received from the host.

## Files

| File | Purpose |
|---|---|
| `usb_wrapper.h / .c` | Queue initialization, FreeRTOS TX task, thread-safe send/receive helpers, ISR callbacks |

---

## Architecture

### 1. Queues

| Queue | Depth | Item size | Direction |
|---|---|---|---|
| `usb_tx_queue` | 8 packets | 514 bytes (`USB_TxPacket_t`) | App → USB hardware |
| `usb_rx_queue` | 16 packets | 66 bytes (`USB_Packet_t`) | USB hardware → App |

The TX queue is allocated from a **static buffer** (`usb_tx_queue_storage`) to avoid stressing the FreeRTOS heap with large queue allocations at runtime.

### 2. TX Flow
`USB_Wrapper_Send` copies the caller's data into a `USB_TxPacket_t` and posts it to `usb_tx_queue` with the provided timeout. The caller does not block on USB hardware.

`USB_Wrapper_Task` dequeues one packet at a time, waits for the previous transmission to complete (`usb_tx_done` flag), then calls `CDC_Transmit_FS`. The `usb_tx_done` flag is set back to `true` by the `USB_Wrapper_OnTxCplt` ISR callback when the USB hardware finishes the transfer. If `CDC_Transmit_FS` returns an error (e.g., host disconnected), `usb_tx_done` is reset immediately to avoid deadlock.

### 3. RX Flow
The USB CDC driver calls `USB_Wrapper_OnRxData` from ISR context when data arrives from the host. The callback copies up to `USB_WRAPPER_MAX_PACKET_SIZE` (64) bytes into a `USB_Packet_t` and posts it to `usb_rx_queue` using `xQueueSendFromISR`.

The application polls for received data via `USB_Wrapper_Receive` (non-blocking with `timeout = 0`, or blocking with a tick timeout).

### 4. ISR Callbacks
These functions must be called from the matching points in `usbd_cdc_if.c`:

```c
// Called by CDC_Receive_FS when the host sends data
void USB_Wrapper_OnRxData(uint8_t *Buf, uint32_t *Len);

// Called by CDC_TransmitCplt_FS when a TX DMA transfer finishes
void USB_Wrapper_OnTxCplt(void);
```

---

## Public API

```c
void USB_Wrapper_Init(void);                                          // Call before starting the scheduler
void USB_Wrapper_Task(void *argument);                               // FreeRTOS task entry point

bool USB_Wrapper_Send(const uint8_t *data, uint16_t length,
                      uint32_t timeout);                             // Enqueue data for TX; returns false if queue full
bool USB_Wrapper_Receive(uint8_t *data, uint16_t *length,
                          uint32_t timeout);                         // Dequeue received data; returns false if none
```

---

## Usage Example

```c
// Initialization (call before vTaskStartScheduler)
USB_Wrapper_Init();

// Send a string (10 ms timeout; returns false if TX queue is full)
const char *msg = "Hello\r\n";
USB_Wrapper_Send((uint8_t *)msg, strlen(msg), pdMS_TO_TICKS(10));

// Poll for a command from the host (non-blocking)
uint8_t rx[64];
uint16_t rx_len;
if (USB_Wrapper_Receive(rx, &rx_len, 0)) {
    // process rx[0..rx_len-1]
}
```

---

## Dependencies
- FreeRTOS (queues, semaphores)
- STM32 USB Device library (`usbd_cdc_if.h`, `usbd_cdc.h`)
