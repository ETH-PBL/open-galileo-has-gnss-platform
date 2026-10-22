#ifndef USB_WRAPPER_H
#define USB_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum payload size for Full Speed USB CDC */
#define USB_WRAPPER_MAX_PACKET_SIZE 64

typedef struct {
    uint8_t data[USB_WRAPPER_MAX_PACKET_SIZE];
    uint16_t length;
} USB_Packet_t;

/* Lifecycle functions */
void USB_Wrapper_Init(void);
void USB_Wrapper_Task(void *argument);

/* Public functions to push/pull data */
bool USB_Wrapper_Send(const uint8_t *data, uint16_t length, uint32_t timeout);
bool USB_Wrapper_Receive(uint8_t *data, uint16_t *length, uint32_t timeout);

/* 
 * Callbacks to hook into the low-level CDC driver 
 * (usbd_cdc_if.c)
 */
void USB_Wrapper_OnRxData(uint8_t *Buf, uint32_t *Len);
void USB_Wrapper_OnTxCplt(void);

#endif /* USB_WRAPPER_H */