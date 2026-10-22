#include "gnss_wrapper.h"
#include "FreeRTOS.h"
#include "task.h"
#include "shared_memory.h"

/* A 512-byte circular buffer is sufficient for typical GNSS PVT messages (which are ~100 bytes) */
#define GNSS_RX_BUFFER_SIZE 512

static uint8_t gnss_rx_buffer[GNSS_RX_BUFFER_SIZE];
static uint16_t old_pos = 0;
static UART_HandleTypeDef *gnss_huart;

/* Variables to securely share the parsed PVT data across tasks */
static UBX_NAV_PVT_t latest_pvt;
static bool new_pvt_available = false;

/* Current active correction source — embedded in every GnssPayload_t sent to CM7 */
static CorrectionSource_t current_correction_source = CORRECTION_GAL_HAS;

// Holds the time for the upcoming pulse (from UART)
volatile uint64_t pending_pps_global_ms = 0;
volatile bool pending_pps_valid = false;

// The active "Sync Pair" (Committed when the GNSS_TIMEPULSE pin fires)
volatile uint32_t active_pps_local_ticks = 0;
volatile uint64_t active_pps_global_ms = 0;
volatile bool active_pps_valid = false;

/**
 * @brief Initializes the GNSS module and starts DMA reception.
 * @param huart: Pointer to the UART handle.
 */
void GNSS_Init(UART_HandleTypeDef *huart)
{
    gnss_huart = huart;
    old_pos = 0;

    // Force DMA to use the AHB SRAM address space (0x30000000) instead of the CM4 local alias (0x10000000)
    uint8_t *dma_rx_ptr = (uint8_t *)((uint32_t)gnss_rx_buffer | 0x20000000);
    HAL_UART_Receive_DMA(gnss_huart, dma_rx_ptr, GNSS_RX_BUFFER_SIZE);
}

void GNSS_SetCorrectionSpartn(void)
{
    UBX_EnableNavCorHost(gnss_huart, true);
    UBX_EnableNavCorGalHas(gnss_huart, false);
    current_correction_source = CORRECTION_SPARTN;
}

void GNSS_SetCorrectionGalileoHAS(void)
{
    UBX_EnableNavCorHost(gnss_huart, false);
    UBX_EnableNavCorGalHas(gnss_huart, true);
    current_correction_source = CORRECTION_GAL_HAS;
}

void GNSS_SetCorrectionNone(void)
{
    UBX_EnableNavCorHost(gnss_huart, false);
    UBX_EnableNavCorGalHas(gnss_huart, false);
    current_correction_source = CORRECTION_NONE;
}

/**
 * @brief FreeRTOS Task to get bytes from the DMA buffer, feed them to the UBX parser, and update the latest PVT data when a new valid message is parsed.
 */
void GNSS_TaskBody(void *argument)
{
    uint16_t curr_pos;

    /* Default correction source: Galileo HAS.
     * Switched to SPARTN via CMD queue when the NTRIP client connects. */
    GNSS_SetCorrectionGalileoHAS();

    for (;;)
    {
        if (gnss_huart != NULL)
        {
            // __HAL_DMA_GET_COUNTER returns the remaining data units in the current DMA transfer.
            // Subtracting it from the total buffer size gives us the current write index of the DMA.
            curr_pos = GNSS_RX_BUFFER_SIZE - __HAL_DMA_GET_COUNTER(gnss_huart->hdmarx);

            // If DMA has moved forward, we have new bytes to process
            if (curr_pos != old_pos)
            {
                if (curr_pos > old_pos)
                {
                    // Linear processing
                    for (uint16_t i = old_pos; i < curr_pos; i++)
                    {
                        UBX_ProcessByte(gnss_rx_buffer[i]);
                    }
                }
                else
                {
                    // Rollover processing: Process to the end of buffer, then from start to curr_pos
                    for (uint16_t i = old_pos; i < GNSS_RX_BUFFER_SIZE; i++)
                    {
                        UBX_ProcessByte(gnss_rx_buffer[i]);
                    }
                    for (uint16_t i = 0; i < curr_pos; i++)
                    {
                        UBX_ProcessByte(gnss_rx_buffer[i]);
                    }
                }
                old_pos = curr_pos;
            }

            // Check if the parser completed a full, valid message
            UBX_NAV_PVT_t temp_pvt;
            if (UBX_GetPvtData(&temp_pvt))
            {
                // Enter critical section to ensure atomicity while writing the struct
                taskENTER_CRITICAL();
                latest_pvt = temp_pvt;
                new_pvt_available = true;
                taskEXIT_CRITICAL();

                /* Enqueue the new PVT data to the shared memory for CM7 */
                GnssPayload_t payload;
                payload.pvt_data = temp_pvt;
                payload.correction_source = current_correction_source;
                enqueue_gnss(queue_gnss_ptr, &payload);
            }

            /* Forward one SPARTN/RTCM correction chunk per iteration to the
             * GNSS chip.  One entry per 10 ms cycle is enough: the queue
             * holds 8 × 512-byte entries and SPARTN updates arrive at most
             * every ~1 s, so the queue drains well ahead of the next burst. */
            RtcmPayload_t rtcm;
            if (dequeue_rtcm(queue_rtcm_ptr, &rtcm))
            {
                HAL_UART_Transmit(gnss_huart,
                                  (uint8_t *)rtcm.buffer,
                                  rtcm.length,
                                  100);
            }

            /* Process correction source commands sent from CM7 via CMD queue. */
            CmdPayload_t cmd;
            while (dequeue_cmd(queue_cmd_ptr, &cmd))
            {
                switch (cmd.command_id)
                {
                case CMD_SET_CORRECTION_SPARTN:
                    GNSS_SetCorrectionSpartn();
                    break;
                case CMD_SET_CORRECTION_GAL_HAS:
                    GNSS_SetCorrectionGalileoHAS();
                    break;
                case CMD_SET_CORRECTION_NONE:
                    GNSS_SetCorrectionNone();
                    break;
                default:
                    break;
                }
            }
        }

        // Yield task for 10ms. At 115200 baud, ~115 bytes arrive in 10ms.
        // The 512-byte ring buffer provides plenty of safety margin.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool GNSS_GetLatestPVT(UBX_NAV_PVT_t *out_pvt)
{
    bool available = false;
    taskENTER_CRITICAL();
    if (new_pvt_available && out_pvt != NULL)
    {
        *out_pvt = latest_pvt;
        new_pvt_available = false;
        available = true;
    }
    taskEXIT_CRITICAL();
    return available;
}