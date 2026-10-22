#include "usb_wrapper.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "usbd_cdc_if.h"
#include "usbd_cdc.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

static QueueHandle_t usb_rx_queue = NULL;
static QueueHandle_t usb_tx_queue = NULL;

/* Define the capacity of your RX/TX queues here */
#define USB_RX_QUEUE_LENGTH 16
#define USB_TX_QUEUE_LENGTH 8

static volatile bool usb_tx_done = true;

#define USB_TX_BUFFER_SIZE 512
static uint8_t usb_tx_buffer[USB_TX_BUFFER_SIZE];

/* Structure to hold a packet in the TX queue */
typedef struct
{
    uint8_t data[USB_TX_BUFFER_SIZE];
    uint16_t length;
} USB_TxPacket_t;

/* Statically allocate memory for the TX queue to avoid stressing the FreeRTOS heap */
static StaticQueue_t usb_tx_queue_cb;
static uint8_t usb_tx_queue_storage[USB_TX_QUEUE_LENGTH * sizeof(USB_TxPacket_t)];

void USB_Wrapper_Init(void)
{
    usb_rx_queue = xQueueCreate(USB_RX_QUEUE_LENGTH, sizeof(USB_Packet_t));
    usb_tx_queue = xQueueCreateStatic(USB_TX_QUEUE_LENGTH,
                                      sizeof(USB_TxPacket_t),
                                      usb_tx_queue_storage,
                                      &usb_tx_queue_cb);
}

void USB_Wrapper_Task(void *argument)
{
    USB_TxPacket_t tx_packet;

    for (;;)
    {
        /* Block indefinitely until there is data to send */
        if (xQueueReceive(usb_tx_queue, &tx_packet, portMAX_DELAY) == pdTRUE)
        {
            /* Wait for the previous USB transmission to complete */
            while (!usb_tx_done)
            {
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            memcpy(usb_tx_buffer, tx_packet.data, tx_packet.length);
            usb_tx_done = false;

            if (CDC_Transmit_FS(usb_tx_buffer, tx_packet.length) != USBD_OK)
            {
                /* If transmission failed to start, reset the flag so we don't deadlock */
                usb_tx_done = true;
            }
        }
    }
}

bool USB_Wrapper_Send(const uint8_t *data, uint16_t length, uint32_t timeout)
{
    if (data == NULL || length == 0)
        return false;

    if (usb_tx_queue == NULL)
        return false;

    USB_TxPacket_t packet;
    packet.length = (length > USB_TX_BUFFER_SIZE) ? USB_TX_BUFFER_SIZE : length;
    memcpy(packet.data, data, packet.length);

    /* Post to the TX queue and return true if successfully enqueued */
    if (xQueueSend(usb_tx_queue, &packet, timeout) == pdTRUE)
    {
        return true;
    }

    return false;
}

bool USB_Wrapper_Receive(uint8_t *data, uint16_t *length, uint32_t timeout)
{
    USB_Packet_t packet;

    if (usb_rx_queue == NULL || data == NULL || length == NULL)
        return false;

    if (xQueueReceive(usb_rx_queue, &packet, timeout) == pdTRUE)
    {
        memcpy(data, packet.data, packet.length);
        *length = packet.length;
        return true;
    }

    *length = 0;
    return false;
}

/* Invoked inside the ISR context by CDC_Receive_FS */
void USB_Wrapper_OnRxData(uint8_t *Buf, uint32_t *Len)
{
    /* We directly post the incoming bulk as a packet block (usually matching 64 Bytes limit anyway) */
    USB_Packet_t packet;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (usb_rx_queue == NULL || Buf == NULL || Len == NULL || *Len == 0)
        return;

    /* In FS, the maximum size from the IP callback is typically 64 */
    uint32_t chunk_size = *Len;
    if (chunk_size > USB_WRAPPER_MAX_PACKET_SIZE)
    {
        chunk_size = USB_WRAPPER_MAX_PACKET_SIZE;
    }

    memcpy(packet.data, Buf, chunk_size);
    packet.length = chunk_size;

    xQueueSendFromISR(usb_rx_queue, &packet, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* Invoked inside the ISR context by CDC_TransmitCplt_FS */
void USB_Wrapper_OnTxCplt(void)
{
    usb_tx_done = true;
}