#ifndef GNSS_WRAPPER_H
#define GNSS_WRAPPER_H

#include "stm32h7xx_hal.h"
#include "ublox_hpg_interface.h"
#include <stdbool.h>

// Holds the time for the upcoming pulse (from UART)
extern volatile uint64_t pending_pps_global_ms;
extern volatile bool pending_pps_valid;

// The active "Sync Pair" (Committed when the GNSS_TIMEPULSE pin fires)
extern volatile uint32_t active_pps_local_ticks;
extern volatile uint64_t active_pps_global_ms;
extern volatile bool active_pps_valid;

/* Expose the wrapper functions */

/**
 * @brief Initializes the GNSS wrapper and starts the Circular DMA reception.
 */
void GNSS_Init(UART_HandleTypeDef *huart);

/**
 * @brief The FreeRTOS Task body to handle DMA polling and parsing.
 */
void GNSS_TaskBody(void *argument);

/**
 * @brief Fetch the latest valid PVT message parsed by the wrapper.
 */
bool GNSS_GetLatestPVT(UBX_NAV_PVT_t *out_pvt);

/**
 * @brief Switch to SPARTN/RTCM host corrections (HOST=1, GAL_HAS=0).
 *        HOST corrections have priority when both HOST and GAL_HAS are enabled.
 *        Call when the NTRIP client connects.
 */
void GNSS_SetCorrectionSpartn(void);

/**
 * @brief Switch to Galileo HAS corrections (HOST=0, GAL_HAS=1).
 *        Active whenever no host corrections are available.
 *        Selected by default on startup.
 */
void GNSS_SetCorrectionGalileoHAS(void);

/**
 * @brief Disable all external corrections (HOST=0, GAL_HAS=0).
 */
void GNSS_SetCorrectionNone(void);

#endif // GNSS_WRAPPER_H