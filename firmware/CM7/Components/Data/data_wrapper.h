#ifndef DATA_WRAPPER_H
#define DATA_WRAPPER_H

#include <stdint.h>

/* Live GNSS position updated from PVT whenever a fix is available.
 * Units: degrees × 1e-7 (same as UBX NAV-PVT lat/lon fields).
 * Both are 0 when no fix has been received yet.
 * Read by eth_wrapper to build the NTRIP GGA sentence. */
extern volatile int32_t g_gnss_last_lat;  /* + = North */
extern volatile int32_t g_gnss_last_lon;  /* + = East  */

/* Event counters — incremented by Data_Wrapper_Task, read by LED_Status_Task.
 * Compare snapshots over time to detect activity / stalls. */
extern volatile uint32_t g_gnss_pkt_count;   /* new GNSS location received    */
extern volatile uint32_t g_sens_pkt_count;   /* sensor packet received         */
extern volatile uint32_t g_sens_no_ts_count; /* sensor packet with timestamp=0 */

/* Forward SPARTN/RTCM correction bytes received from the NTRIP client
 * to CM4 via the shared RTCM queue so the GNSS chip can use them. */
void Data_Wrapper_EnqueueRTCM(const uint8_t *data, uint16_t len);

/* Parse and apply a logging filter command received over USB or TCP.
 * Supported commands (case-insensitive, trailing whitespace ignored):
 *   LOG_ALL   – print all outputs (sensors + GNSS)
 *   LOG_GNSS  – print GNSS only; sensor lines are silently dropped */
void Data_Wrapper_ProcessCommand(const char *cmd, uint16_t len);

/* FreeRTOS Task body */
void Data_Wrapper_Task(void *argument);

#endif /* DATA_WRAPPER_H */