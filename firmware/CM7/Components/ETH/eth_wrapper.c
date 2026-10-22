#include "eth_wrapper.h"
#include "data_wrapper.h"
#include "shared_memory.h"
#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "cmsis_os.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "main.h"

#define TCP_SERVER_PORT 7

extern struct netif gnetif;

/* Debugger watch variables (see eth_wrapper.h) */
volatile NetState_t g_net_state     = NET_STATE_INIT;
volatile int        g_dns_err       = 0;
volatile uint32_t   g_dns_tries     = 0;
volatile uint32_t   g_ntrip_bytes   = 0;
volatile uint32_t   g_gga_sent      = 0;
volatile bool       g_ntrip_enabled = true;

/* The active client connection. One client at a time. */
static struct netconn *g_active_conn = NULL;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes a null-terminated string to base64. out must be at least
 * ceil(strlen(in)/3)*4 + 1 bytes. */
static void base64_encode(const char *in, char *out)
{
    const uint8_t *p = (const uint8_t *)in;
    size_t n = strlen(in);
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v  = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)p[i + 2];
        out[j++] = B64[(v >> 18) & 63];
        out[j++] = B64[(v >> 12) & 63];
        out[j++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[j++] = (i + 2 < n) ? B64[(v     ) & 63] : '=';
    }
    out[j] = '\0';
}

/* Builds a GGA sentence from the live GNSS position using pure integer
 * arithmetic — newlib-nano does not include float support in snprintf by
 * default so %f silently produces nothing. */
static void build_gga(char *buf, size_t buflen)
{
    int32_t raw_lat = g_gnss_last_lat;
    int32_t raw_lon = g_gnss_last_lon;

    char ns = (raw_lat >= 0) ? 'N' : 'S';
    char ew = (raw_lon >= 0) ? 'E' : 'W';
    if (raw_lat < 0) raw_lat = -raw_lat;
    if (raw_lon < 0) raw_lon = -raw_lon;

    /* UBX units: 1e-7 degrees.  Convert to DDMM.MMMMM (NMEA format).
     *   degrees   = raw / 1e7
     *   min_1e7   = (raw % 1e7) * 60        <- minutes scaled by 1e7
     *   MM        = min_1e7 / 1e7            <- integer minutes
     *   .MMMMM    = (min_1e7 % 1e7) / 100   <- 5 decimal places of minutes */
    int32_t lat_d  = raw_lat / 10000000;
    int32_t lat_x  = (raw_lat % 10000000) * 60;
    int32_t lat_m  = lat_x / 10000000;
    int32_t lat_mf = (lat_x % 10000000) / 100;

    int32_t lon_d  = raw_lon / 10000000;
    int32_t lon_x  = (raw_lon % 10000000) * 60;
    int32_t lon_m  = lon_x / 10000000;
    int32_t lon_mf = (lon_x % 10000000) / 100;

    char lat_str[16], lon_str[16];
    snprintf(lat_str, sizeof(lat_str), "%02d%02d.%05d", (int)lat_d, (int)lat_m, (int)lat_mf);
    snprintf(lon_str, sizeof(lon_str), "%03d%02d.%05d", (int)lon_d, (int)lon_m, (int)lon_mf);

    char body[128];
    snprintf(body, sizeof(body),
             "GPGGA,120000.00,%s,%c,%s,%c,1,08,1.0,100.0,M,48.0,M,,",
             lat_str, ns, lon_str, ew);
    uint8_t chk = 0;
    for (const char *c = body; *c; c++) chk ^= (uint8_t)*c;
    snprintf(buf, buflen, "$%s*%02X\r\n", body, chk);
}

/* -----------------------------------------------------------------------
 * NTRIP client task
 * ----------------------------------------------------------------------- */
void NTRIP_Client_Task(void *argument)
{
    struct netconn *conn;
    err_t err;
    ip_addr_t server_addr;

    /* Wait for the network stack to stabilise after link-up */
    osDelay(2000);

    /* Primary DNS: Google 8.8.8.8 (routed through ICS).
     * Fallback: Windows ICS proxy on the gateway. */
    ip_addr_t dns_primary, dns_fallback;
    ipaddr_aton("8.8.8.8", &dns_primary);
    ipaddr_aton("192.168.137.1", &dns_fallback);
    dns_setserver(0, &dns_primary);
    dns_setserver(1, &dns_fallback);

    /* --- Internet connectivity test --- */
    g_net_state = NET_STATE_DNS_TRYING;
    for (;;)
    {
        g_dns_tries++;
        err = netconn_gethostbyname("google.com", &server_addr);
        g_dns_err = err;
        if (err == ERR_OK)
        {
            g_net_state = NET_STATE_DNS_OK;
            break;
        }
        /* g_dns_err: -3=ERR_TIMEOUT (routing broken), -5=ERR_RTE (no gateway) */
        g_net_state = NET_STATE_DNS_FAIL;
        osDelay(5000);
        g_net_state = NET_STATE_DNS_TRYING;
    }

    /* --- Wait for a valid GNSS fix before connecting.
     * Use || so we wait until BOTH lat AND lon are non-zero — they are
     * written sequentially in data_wrapper and a race could leave one at 0. */
    g_net_state = NET_STATE_WAIT_FIX;
    while (g_gnss_last_lat == 0 || g_gnss_last_lon == 0)
        osDelay(1000);

    /* --- NTRIP client loop --- */
    char auth[64];
    char userpass[64];
    snprintf(userpass, sizeof(userpass), "%s:%s", NTRIP_USER, NTRIP_PASS);
    base64_encode(userpass, auth);

    char request[256];
    snprintf(request, sizeof(request),
             "GET /%s HTTP/1.0\r\n"
             "User-Agent: NTRIP STM32Client/1.0\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "Authorization: Basic %s\r\n"
             "\r\n",
             NTRIP_MOUNTPOINT, auth);

    char gga[96];

    for (;;)
    {
        /* Block here while NTRIP has been manually disabled — resume as soon as
         * SET_CORR_SPARTN re-enables it. */
        while (!g_ntrip_enabled)
            osDelay(500);

        g_net_state = NET_STATE_NTRIP_CONNECT;
        err = netconn_gethostbyname(NTRIP_HOST, &server_addr);
        g_dns_err = err;
        if (err != ERR_OK) { osDelay(2000); continue; }

        conn = netconn_new(NETCONN_TCP);
        if (conn == NULL) { osDelay(2000); continue; }

        err = netconn_connect(conn, &server_addr, NTRIP_PORT_NUM);
        if (err != ERR_OK)
        {
            g_dns_err = err;
            netconn_delete(conn);
            osDelay(2000);
            continue;
        }

        /* Send HTTP request */
        err = netconn_write(conn, request, strlen(request), NETCONN_COPY);
        if (err != ERR_OK)
        {
            netconn_close(conn); netconn_delete(conn);
            g_net_state = NET_STATE_NTRIP_LOST;
            osDelay(2000); continue;
        }
        g_net_state = NET_STATE_NTRIP_CONNECTED;

        /* Notify CM4 to switch the GNSS chip to SPARTN corrections. */
        {
            CmdPayload_t cmd = { .command_id = CMD_SET_CORRECTION_SPARTN,
                                 .parameters = {0, 0, 0, 0} };
            enqueue_cmd(queue_cmd_ptr, &cmd);
        }

        /* Build GGA fresh here so it always uses the latest fix, not a value
         * captured before the connection loop started. */
        build_gga(gga, sizeof(gga));

        /* Wait for ICY 200 OK, then send GGA so NEAR-SPARTN knows our position */
        osDelay(200);
        err = netconn_write(conn, gga, strlen(gga), NETCONN_COPY);
        if (err == ERR_OK) g_gga_sent++;

        netconn_set_recvtimeout(conn, 500);

        struct netbuf *inbuf;
        err_t recv_err;
        uint32_t last_gga_tick = osKernelGetTickCount();

        while ((recv_err = netconn_recv(conn, &inbuf)) == ERR_OK ||
               recv_err == ERR_TIMEOUT)
        {
            /* Periodic GGA resend to keep the stream alive */
            if (osKernelGetTickCount() - last_gga_tick >= NTRIP_GGA_INTERVAL_MS)
            {
                build_gga(gga, sizeof(gga));
                if (netconn_write(conn, gga, strlen(gga), NETCONN_COPY) == ERR_OK)
                    g_gga_sent++;
                last_gga_tick = osKernelGetTickCount();
            }

            if (recv_err == ERR_TIMEOUT)
            {
                /* If the physical link dropped or NTRIP was manually disabled,
                 * stop pretending the stream is alive. */
                if (!netif_is_link_up(&gnetif) || !g_ntrip_enabled)
                    break;
                g_net_state = NET_STATE_NTRIP_CONNECTED;
                continue;
            }

            if (inbuf == NULL) continue;

            /* Forward ALL bytes unchanged — the ZED-X20P's internal SPARTN
             * parser syncs to valid frames and ignores any leading HTTP header
             * text on its own. */
            uint8_t *data;
            uint16_t len;
            netbuf_first(inbuf);
            do {
                netbuf_data(inbuf, (void **)&data, &len);
                g_net_state = NET_STATE_NTRIP_RECEIVING;
                g_ntrip_bytes += len;
                Data_Wrapper_EnqueueRTCM(data, len);
            } while (netbuf_next(inbuf) >= 0);

            netbuf_delete(inbuf);
        }

        g_net_state = NET_STATE_NTRIP_LOST;

        /* Only auto-fallback to Galileo HAS on a natural disconnect (cable pull,
         * server close).  If NTRIP was manually disabled the caller already sent
         * the correction command it wanted, so don't override it here. */
        if (g_ntrip_enabled)
        {
            CmdPayload_t cmd = { .command_id = CMD_SET_CORRECTION_GAL_HAS,
                                 .parameters = {0, 0, 0, 0} };
            enqueue_cmd(queue_cmd_ptr, &cmd);
        }

        netconn_close(conn);
        netconn_delete(conn);
        osDelay(2000);
    }
}

/* -----------------------------------------------------------------------
 * TCP server task
 * ----------------------------------------------------------------------- */
err_t Eth_Wrapper_Send(const uint8_t *data, uint16_t len)
{
    if (g_active_conn != NULL)
    {
        err_t err = netconn_write(g_active_conn, data, len, NETCONN_COPY);
        if (err != ERR_OK) { g_active_conn = NULL; return err; }
        return ERR_OK;
    }
    return ERR_CONN;
}

void Eth_Wrapper_Task(void *argument)
{
    struct netconn *conn, *newconn;
    err_t err, accept_err;

    g_net_state = NET_STATE_WAIT_LINK;
    while (!netif_is_link_up(&gnetif))
        osDelay(500);

    osDelay(100);

    const osThreadAttr_t ntripTask_attributes = {
        .name       = "NtripTask",
        .stack_size = 1024 * 4,
        .priority   = (osPriority_t)osPriorityNormal,
    };
    osThreadNew(NTRIP_Client_Task, NULL, &ntripTask_attributes);

    conn = netconn_new(NETCONN_TCP);
    if (conn == NULL) { vTaskDelete(NULL); return; }

    err = netconn_bind(conn, NULL, TCP_SERVER_PORT);
    if (err != ERR_OK) { netconn_delete(conn); vTaskDelete(NULL); return; }

    netconn_listen(conn);

    for (;;)
    {
        accept_err = netconn_accept(conn, &newconn);
        if (accept_err != ERR_OK) continue;

        if (g_active_conn != NULL)
        {
            netconn_close(g_active_conn);
            netconn_delete(g_active_conn);
        }
        g_active_conn = newconn;

        struct netbuf *inbuf;
        while (netconn_recv(newconn, &inbuf) == ERR_OK)
        {
            if (inbuf != NULL)
            {
                uint8_t *rx_data;
                uint16_t rx_len;
                netbuf_data(inbuf, (void **)&rx_data, &rx_len);
                Data_Wrapper_ProcessCommand((const char *)rx_data, rx_len);
                netbuf_delete(inbuf);
            }
        }

        netconn_close(newconn);
        netconn_delete(newconn);
        g_active_conn = NULL;
    }
}
