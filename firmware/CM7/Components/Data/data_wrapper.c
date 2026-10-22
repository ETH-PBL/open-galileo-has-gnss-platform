#include "data_wrapper.h"
#include "shared_memory.h"
#include "usb_wrapper.h"
#include "FreeRTOS.h"
#include "eth_wrapper.h"
#include "task.h"
#include "cmsis_os.h"
#include <stdio.h>
#include <string.h>
#include "usbd_cdc_if.h"
#include "lwip/api.h"
#include "lwip/netif.h"
#include "main.h"

/* Log filter flags — toggled by Data_Wrapper_ProcessCommand().
 * true = include that sensor type in USB/ETH output; false = drop it. */
static volatile bool g_log_sens_enabled = true;
static volatile bool g_log_gnss_enabled = true;

extern struct netif gnetif;

#define CSV_BUFFER_SIZE 512

/* Counter to track how many packets were dropped due to a full USB queue */
static volatile uint32_t usb_dropped_packets = 0;

/* Live position for NTRIP GGA — updated whenever CM4 sends a PVT with a fix */
volatile int32_t g_gnss_last_lat = 0;  /* degrees × 1e-7, + = North */
volatile int32_t g_gnss_last_lon = 0;  /* degrees × 1e-7, + = East  */

/* Event counters for the LED status task */
volatile uint32_t g_gnss_pkt_count   = 0;
volatile uint32_t g_sens_pkt_count   = 0;
volatile uint32_t g_sens_no_ts_count = 0;

/* Chunk SPARTN data into RtcmPayload_t entries and push them to the shared
 * queue so CM4/gnss_wrapper can forward them to the GNSS chip. */
void Data_Wrapper_EnqueueRTCM(const uint8_t *data, uint16_t len)
{
    uint16_t offset = 0;
    while (offset < len)
    {
        RtcmPayload_t payload;
        uint16_t chunk = len - offset;
        if (chunk > (uint16_t)sizeof(payload.buffer))
            chunk = (uint16_t)sizeof(payload.buffer);
        payload.length = chunk;
        memcpy((void *)payload.buffer, data + offset, chunk);
        enqueue_rtcm(queue_rtcm_ptr, &payload);
        offset += chunk;
    }
}

/* Helper function to send data over both USB and Ethernet (TCP) */
static void Send_Data(uint8_t *data, uint16_t len)
{
    /* 1. Send over USB */
    if (!USB_Wrapper_Send(data, len, pdMS_TO_TICKS(10)))
    {
        usb_dropped_packets++;
    }

    /* 2. Send over Ethernet (TCP) if a client is connected */
    Eth_Wrapper_Send(data, len);
}

static const char *correction_source_str(CorrectionSource_t src)
{
    switch (src)
    {
    case CORRECTION_SPARTN:  return "SPARTN";
    case CORRECTION_GAL_HAS: return "GAL_HAS";
    default:                 return "NONE";
    }
}

/* Helper function to safely print 64-bit timestamps without requiring %llu support from newlib-nano */
static void format_timestamp(uint64_t ts, char *buf, size_t size)
{
    uint32_t msb = (uint32_t)(ts / 1000000000ULL);
    uint32_t lsb = (uint32_t)(ts % 1000000000ULL);
    if (msb == 0)
    {
        snprintf(buf, size, "%lu", lsb);
    }
    else
    {
        snprintf(buf, size, "%lu%09lu", msb, lsb);
    }
}

/* Helper function to convert UTC calendar date to Unix epoch seconds */
static int64_t convert_to_unix_sec(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec)
{
    int64_t y = year;
    month = (month >= 1 && month <= 12) ? month : 1;
    if (month <= 2)
    {
        y--;
    }
    /* Leap years since 1970 */
    int64_t leaps = (y / 4) - (y / 100) + (y / 400) - (1970 / 4) + (1970 / 100) - (1970 / 400);
    static const int64_t days_in_month[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};

    int64_t days = (year - 1970) * 365LL + leaps + days_in_month[month - 1] + (day - 1);
    return days * 86400LL + hour * 3600LL + min * 60LL + sec;
}

/* Case-insensitive comparison of buf against a known uppercase keyword. */
static bool cmd_match(const char *buf, const char *keyword)
{
    while (*keyword)
    {
        char c = *buf++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != *keyword++) return false;
    }
    return (*buf == '\0');
}

void Data_Wrapper_ProcessCommand(const char *cmd, uint16_t len)
{
    char buf[32];
    if (len >= sizeof(buf)) len = (uint16_t)(sizeof(buf) - 1);
    memcpy(buf, cmd, len);
    buf[len] = '\0';

    /* Trim trailing whitespace / CR / LF */
    int end = (int)len - 1;
    while (end >= 0 && (buf[end] == '\r' || buf[end] == '\n' || buf[end] == ' '))
        buf[end--] = '\0';

    if (cmd_match(buf, "LOG_ALL"))
    {
        g_log_sens_enabled = true;
        g_log_gnss_enabled = true;
    }
    else if (cmd_match(buf, "LOG_GNSS"))
    {
        g_log_sens_enabled = false;
        g_log_gnss_enabled = true;
    }
    else if (cmd_match(buf, "SET_CORR_SPARTN"))
    {
        /* Re-enable NTRIP reconnection (in case it was manually disabled). */
        g_ntrip_enabled = true;
        if (g_net_state == NET_STATE_NTRIP_CONNECTED ||
            g_net_state == NET_STATE_NTRIP_RECEIVING)
        {
            /* Already connected — switch CM4 immediately. */
            CmdPayload_t cmd_pkt = { .command_id = CMD_SET_CORRECTION_SPARTN,
                                     .parameters = {0, 0, 0, 0} };
            enqueue_cmd(queue_cmd_ptr, &cmd_pkt);
        }
        else
        {
            /* Not currently connected; NTRIP task will auto-send the SPARTN
             * command to CM4 once it reconnects. */
            const char *msg = "ERR,SPARTN unavailable: NTRIP not connected\r\n";
            Send_Data((uint8_t *)msg, (uint16_t)strlen(msg));
        }
    }
    else if (cmd_match(buf, "SET_CORR_GAL_HAS"))
    {
        /* Disable NTRIP to stop charges — breaks the live connection. */
        g_ntrip_enabled = false;
        CmdPayload_t cmd_pkt = { .command_id = CMD_SET_CORRECTION_GAL_HAS,
                                 .parameters = {0, 0, 0, 0} };
        enqueue_cmd(queue_cmd_ptr, &cmd_pkt);
    }
    else if (cmd_match(buf, "SET_CORR_NONE"))
    {
        /* Disable NTRIP to stop charges — breaks the live connection. */
        g_ntrip_enabled = false;
        CmdPayload_t cmd_pkt = { .command_id = CMD_SET_CORRECTION_NONE,
                                 .parameters = {0, 0, 0, 0} };
        enqueue_cmd(queue_cmd_ptr, &cmd_pkt);
    }
}

/* -----------------------------------------------------------------------
 * LED status task
 *
 * Green SOLID  — SPARTN correction data flowing (g_ntrip_bytes rising)
 * Green FLASH  — New GNSS location received, no SPARTN
 * Yellow ON    — Sensor data arriving but timestamp == 0 (no PPS sync yet)
 * Red BLINK    — No sensor data received in last 2 s
 * Red SOLID    — Error (DNS failure / no internet)
 *
 * Active-low LEDs: GPIO_PIN_RESET = ON, GPIO_PIN_SET = OFF
 * ----------------------------------------------------------------------- */
static void LED_Status_Task(void *argument)
{
    uint32_t prev_ntrip  = 0;
    uint32_t prev_gnss   = 0;
    uint32_t prev_sens   = 0;
    uint32_t prev_no_ts  = 0;
    uint32_t spartn_tick = 0;
    uint32_t sens_tick   = 0;
    bool     red_blink   = false;

    /* Give the rest of the system a moment to start up */
    osDelay(1000);

    for (;;)
    {
        uint32_t now = osKernelGetTickCount();

        /* Snapshot counters */
        uint32_t cur_ntrip = g_ntrip_bytes;
        uint32_t cur_gnss  = g_gnss_pkt_count;
        uint32_t cur_sens  = g_sens_pkt_count;
        uint32_t cur_no_ts = g_sens_no_ts_count;

        bool spartn_new = (cur_ntrip != prev_ntrip);
        bool gnss_new   = (cur_gnss  != prev_gnss);
        bool sens_new   = (cur_sens  != prev_sens);
        bool no_ts_new  = (cur_no_ts != prev_no_ts);

        if (spartn_new) spartn_tick = now;
        if (sens_new)   sens_tick   = now;

        prev_ntrip = cur_ntrip;
        prev_gnss  = cur_gnss;
        prev_sens  = cur_sens;
        prev_no_ts = cur_no_ts;

        bool spartn_active = (now - spartn_tick < 5000);
        bool sens_active   = (now - sens_tick   < 2000);

        /* ---- GREEN LED ---- */
        if (spartn_active)
        {
            /* SPARTN data flowing → solid ON */
            HAL_GPIO_WritePin(MCU_LED_Green_GPIO_Port, MCU_LED_Green_Pin, GPIO_PIN_RESET);
        }
        else if (gnss_new)
        {
            /* New GNSS location, no SPARTN → brief 80 ms flash */
            HAL_GPIO_WritePin(MCU_LED_Green_GPIO_Port, MCU_LED_Green_Pin, GPIO_PIN_RESET);
            osDelay(80);
            HAL_GPIO_WritePin(MCU_LED_Green_GPIO_Port, MCU_LED_Green_Pin, GPIO_PIN_SET);
        }
        else
        {
            HAL_GPIO_WritePin(MCU_LED_Green_GPIO_Port, MCU_LED_Green_Pin, GPIO_PIN_SET);
        }

        /* ---- YELLOW LED ---- */
        /* ON when sensor packets arriving without a valid PPS timestamp */
        HAL_GPIO_WritePin(MCU_LED_Yellow_GPIO_Port, MCU_LED_Yellow_Pin,
                          no_ts_new ? GPIO_PIN_RESET : GPIO_PIN_SET);

        /* ---- RED LED ---- */
        bool error = (g_net_state == NET_STATE_DNS_FAIL);

        if (error)
        {
            /* Error → solid ON */
            HAL_GPIO_WritePin(MCU_LED_Red_GPIO_Port, MCU_LED_Red_Pin, GPIO_PIN_RESET);
        }
        else if (!sens_active)
        {
            /* No sensor data → blink at ~2 Hz */
            red_blink = !red_blink;
            HAL_GPIO_WritePin(MCU_LED_Red_GPIO_Port, MCU_LED_Red_Pin,
                              red_blink ? GPIO_PIN_RESET : GPIO_PIN_SET);
        }
        else
        {
            /* All good → off */
            HAL_GPIO_WritePin(MCU_LED_Red_GPIO_Port, MCU_LED_Red_Pin, GPIO_PIN_SET);
        }

        osDelay(250);
    }
}

void Data_Wrapper_Task(void *argument)
{
    /* Start the LED status task */
    static const osThreadAttr_t led_attr = {
        .name       = "LEDTask",
        .stack_size = 512,
        .priority   = osPriorityLow,
    };
    osThreadNew(LED_Status_Task, NULL, &led_attr);
    SensPayload_t sens_payload;
    GnssPayload_t gnss_payload;
    char csv_buffer[CSV_BUFFER_SIZE];
    int16_t len = 0;
    // TickType_t last_test_time = xTaskGetTickCount();

    for (;;)
    {
        bool data_processed = false;

        /* Poll for incoming commands via USB (non-blocking) */
        {
            uint8_t rx_buf[64];
            uint16_t rx_len = 0;
            if (USB_Wrapper_Receive(rx_buf, &rx_len, 0))
                Data_Wrapper_ProcessCommand((const char *)rx_buf, rx_len);
        }

        /* 1. Check and process Sensor Queue */
        while (dequeue_sens(queue_sens_ptr, &sens_payload))
        {
            data_processed = true;
            char ts_str[24];
            int written = 0;

            /* Update LED counters */
            g_sens_pkt_count++;
            uint64_t ts = 0;
            switch (sens_payload.type) {
                case SENS_TYPE_ISM330:     ts = sens_payload.data.ism330.timestamp;    break;
                case SENS_TYPE_LSM303_ACC: ts = sens_payload.data.lsm303_acc.timestamp; break;
                case SENS_TYPE_LSM303_MAG: ts = sens_payload.data.lsm303_mag.timestamp; break;
            }
            if (ts == 0) g_sens_no_ts_count++;

            if (!g_log_sens_enabled) continue;

            if ((CSV_BUFFER_SIZE - len) < 128)
            {
                Send_Data((uint8_t *)csv_buffer, len);
                len = 0;
            }

            switch (sens_payload.type)
            {
            case SENS_TYPE_ISM330:
                format_timestamp(sens_payload.data.ism330.timestamp, ts_str, sizeof(ts_str));
                written = snprintf(csv_buffer + len, CSV_BUFFER_SIZE - len, "ISM330,%s,%d,%d,%d,%d,%d,%d\r\n",
                                   ts_str,
                                   sens_payload.data.ism330.acceleration[0],
                                   sens_payload.data.ism330.acceleration[1],
                                   sens_payload.data.ism330.acceleration[2],
                                   sens_payload.data.ism330.angular_rate[0],
                                   sens_payload.data.ism330.angular_rate[1],
                                   sens_payload.data.ism330.angular_rate[2]);
                break;
            case SENS_TYPE_LSM303_ACC:
                format_timestamp(sens_payload.data.lsm303_acc.timestamp, ts_str, sizeof(ts_str));
                written = snprintf(csv_buffer + len, CSV_BUFFER_SIZE - len, "LSM_ACC,%s,%d,%d,%d\r\n",
                                   ts_str,
                                   sens_payload.data.lsm303_acc.acceleration[0],
                                   sens_payload.data.lsm303_acc.acceleration[1],
                                   sens_payload.data.lsm303_acc.acceleration[2]);
                break;
            case SENS_TYPE_LSM303_MAG:
                format_timestamp(sens_payload.data.lsm303_mag.timestamp, ts_str, sizeof(ts_str));
                written = snprintf(csv_buffer + len, CSV_BUFFER_SIZE - len, "LSM_MAG,%s,%d,%d,%d\r\n",
                                   ts_str,
                                   sens_payload.data.lsm303_mag.magnetic[0],
                                   sens_payload.data.lsm303_mag.magnetic[1],
                                   sens_payload.data.lsm303_mag.magnetic[2]);
                break;
            }

            if (written > 0 && written < (CSV_BUFFER_SIZE - len))
            {
                len += written;
            }
        }

        /* 2. Check and process GNSS Queue */
        while (dequeue_gnss(queue_gnss_ptr, &gnss_payload))
        {
            data_processed = true;
            char epoch_us_str[24];
            UBX_NAV_PVT_t *pvt = &gnss_payload.pvt_data;

            /* Keep position globals fresh for the NTRIP GGA sentence */
            if (pvt->fixType >= 2)
            {
                g_gnss_last_lat = pvt->lat;
                g_gnss_last_lon = pvt->lon;
            }
            g_gnss_pkt_count++;

            if (!g_log_gnss_enabled) continue;

            if ((CSV_BUFFER_SIZE - len) < 256)
            {
                Send_Data((uint8_t *)csv_buffer, len);
                len = 0;
            }

            int64_t unix_sec = convert_to_unix_sec(pvt->year, pvt->month, pvt->day, pvt->hour, pvt->min, pvt->sec);
            int32_t nano_us = (pvt->nano >= 0) ? ((pvt->nano + 500) / 1000) : ((pvt->nano - 500) / 1000);
            int64_t epoch_us = (unix_sec * 1000000LL) + nano_us;
            format_timestamp((uint64_t)epoch_us, epoch_us_str, sizeof(epoch_us_str));

            int written = snprintf(csv_buffer + len, CSV_BUFFER_SIZE - len,
                                   "GNSS_PVT,%s,%u,%u,%u,%u,%u,%u,%ld,%lu,%d,%lu,%u,%d,%u,%ld,%ld,%ld,%ld,%lu,%lu,%ld,%ld,%ld,%ld,%ld,%lu,%lu,%u,%s\r\n",
                                   epoch_us_str, pvt->year, pvt->month, pvt->day, pvt->hour, pvt->min, pvt->sec, pvt->nano, pvt->iTOW, pvt->valid, pvt->tAcc,
                                   pvt->fixType, pvt->flags, pvt->numSV,
                                   pvt->lon, pvt->lat, pvt->height, pvt->hMSL,
                                   pvt->hAcc, pvt->vAcc, pvt->velN, pvt->velE, pvt->velD,
                                   pvt->gSpeed, pvt->heading, pvt->sAcc, pvt->headingAcc, pvt->pDOP,
                                   correction_source_str(gnss_payload.correction_source));

            if (written > 0 && written < (CSV_BUFFER_SIZE - len))
            {
                len += written;
            }
        }

        /* 3. Send a periodic test message every 1000 ms */
        // if ((xTaskGetTickCount() - last_test_time) >= pdMS_TO_TICKS(1000))
        // {
        //     last_test_time = xTaskGetTickCount();
        //
        //     if ((CSV_BUFFER_SIZE - len) < 64)
        //     {
        //         Send_Data((uint8_t *)csv_buffer, len);
        //         len = 0;
        //     }
        //
        //     int written = snprintf(csv_buffer + len, CSV_BUFFER_SIZE - len, "TEST,This is a message from the wrapper!\r\n");
        //     if (written > 0 && written < (CSV_BUFFER_SIZE - len))
        //     {
        //         len += written;
        //     }
        //     data_processed = true;
        // }

        if (len > 0)
        {
            Send_Data((uint8_t *)csv_buffer, len);
            len = 0;

            /* Yield to LwIP to process the sent packets, preventing queue exhaustion */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        else if (!data_processed)
        {
            /* Yield if no data was processed at all */
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}