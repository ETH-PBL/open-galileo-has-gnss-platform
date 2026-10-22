#ifndef ETH_WRAPPER_H
#define ETH_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include "lwip/err.h"

/* =======================================================================
 * NTRIP configuration — only edit this section
 * ======================================================================= */

#define NTRIP_HOST        "ppntrip.services.u-blox.com"
#define NTRIP_PORT_NUM    2101
#define NTRIP_MOUNTPOINT  "NEAR-SPARTN"
#define NTRIP_USER        "TODO: add credentials"
#define NTRIP_PASS        "TODO: add credentials"

/* How often (ms) to re-send the GGA while streaming */
#define NTRIP_GGA_INTERVAL_MS  10000

/* =======================================================================
 * Debugger watch variables — add to the Expressions view in STM32CubeIDE
 * ======================================================================= */
typedef enum {
    NET_STATE_INIT            = 0,
    NET_STATE_WAIT_LINK       = 1,
    NET_STATE_DNS_TRYING      = 2,
    NET_STATE_DNS_OK          = 3,
    NET_STATE_DNS_FAIL        = 4,
    NET_STATE_WAIT_FIX        = 5,  /* waiting for GNSS fix before connecting */
    NET_STATE_NTRIP_CONNECT   = 6,
    NET_STATE_NTRIP_CONNECTED = 7,
    NET_STATE_NTRIP_RECEIVING = 8,
    NET_STATE_NTRIP_LOST      = 9,
} NetState_t;

extern volatile NetState_t g_net_state;   /* overall state machine             */
extern volatile int        g_dns_err;     /* last lwIP err_t from DNS/connect  */
extern volatile uint32_t   g_dns_tries;   /* DNS attempt counter               */
extern volatile uint32_t   g_ntrip_bytes; /* SPARTN bytes received (post-header) */
extern volatile uint32_t   g_gga_sent;    /* GGA sentences sent to server      */
extern volatile bool       g_ntrip_enabled; /* false = disconnect and do not reconnect */

/* FreeRTOS Task body for the Ethernet TCP server */
void Eth_Wrapper_Task(void *argument);

/* Function to send data over the active TCP connection */
err_t Eth_Wrapper_Send(const uint8_t *data, uint16_t len);

#endif /* ETH_WRAPPER_H */
