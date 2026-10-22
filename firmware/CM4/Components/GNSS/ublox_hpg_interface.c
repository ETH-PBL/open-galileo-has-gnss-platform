#include "ublox_hpg_interface.h"
#include <stddef.h> // For NULL
#include "gnss_wrapper.h"

// State machine states
typedef enum
{
    UBX_STATE_SYNC1,
    UBX_STATE_SYNC2,
    UBX_STATE_CLASS,
    UBX_STATE_ID,
    UBX_STATE_LEN_1,
    UBX_STATE_LEN_2,
    UBX_STATE_PAYLOAD,
    UBX_STATE_CK_A,
    UBX_STATE_CK_B
} UBX_State;

// State machine variables
static UBX_State currentState = UBX_STATE_SYNC1;
static uint8_t msgClass = 0;
static uint8_t msgID = 0;
static uint16_t msgLength = 0;
static uint16_t payloadIndex = 0;
static uint8_t ckA = 0;
static uint8_t ckB = 0;

// Payload buffer
// Sized to handle UBX-NAV-PVT (92 bytes)
#define PAYLOAD_BUFFER_SIZE 128
static uint8_t payloadBuffer[PAYLOAD_BUFFER_SIZE];

// Variable to store the completed and validated PVT message
static UBX_NAV_PVT_t pvtData;
static bool pvtDataReady = false;

// Helper to calculate the running 8-Bit Fletcher Checksum
static void updateChecksum(uint8_t byte)
{
    ckA = ckA + byte;
    ckB = ckB + ckA;
}

// Helper to process a fully received and validated message
static void processValidMessage(void)
{
    // Check if it's our target NAV-PVT message
    if (msgClass == UBX_CLASS_NAV && msgID == UBX_ID_NAV_PVT)
    {
        if (msgLength == UBX_NAV_PVT_LENGTH)
        {

            // Directly cast the raw byte buffer to our neatly aligned packed struct
            UBX_NAV_PVT_t *receivedPvt = (UBX_NAV_PVT_t *)payloadBuffer;

            // Save it for the main application to retrieve
            pvtData = *receivedPvt;
            pvtDataReady = true;
        }
    }
    // Check if it's our target TIM-TP message
    else if (msgClass == UBX_CLASS_TIM && msgID == UBX_ID_TIM_TP)
    {
        if (msgLength == sizeof(UBX_TIM_TP_t))
        {
            UBX_TIM_TP_t *tim_tp = (UBX_TIM_TP_t *)payloadBuffer;

            // Check if the time base is UTC (bit 0) and the TP is locked to GNSS (bit 5 == 0)
            bool isUtc = (tim_tp->flags & 0x01);
            bool isLocked = ((tim_tp->flags >> 5) & 0x01) == 0;

            if (isUtc && isLocked)
            {
                // Store this time. It will be applied when the physical pin goes HIGH
                // Unix epoch from GPS epoch (Jan 6 1980) = 315964800 seconds
                uint64_t unix_ms = 315964800000ULL + ((uint64_t)tim_tp->week * 604800000ULL) + tim_tp->towMS;
                pending_pps_global_ms = unix_ms;
                pending_pps_valid = true;
            }
        }
    }
}

// Public function to be called for each incoming byte from the UART
void UBX_ProcessByte(uint8_t byte)
{
    switch (currentState)
    {
    case UBX_STATE_SYNC1: // Wait for 0xB5
        if (byte == UBX_SYNC_CHAR_1)
        {
            currentState = UBX_STATE_SYNC2;
        }
        break;

    case UBX_STATE_SYNC2: // Wait for 0x62
        if (byte == UBX_SYNC_CHAR_2)
        {
            currentState = UBX_STATE_CLASS;
            // Start of a valid frame: Reset checksum for the new message
            ckA = 0;
            ckB = 0;
        }
        else
        {
            currentState = UBX_STATE_SYNC1; // Reset on failure
        }
        break;

    case UBX_STATE_CLASS:
        msgClass = byte;
        updateChecksum(byte);
        currentState = UBX_STATE_ID;
        break;

    case UBX_STATE_ID:
        msgID = byte;
        updateChecksum(byte);
        currentState = UBX_STATE_LEN_1;
        break;

    case UBX_STATE_LEN_1:
        msgLength = byte;
        updateChecksum(byte);
        currentState = UBX_STATE_LEN_2;
        break;

    case UBX_STATE_LEN_2:
        msgLength |= (byte << 8); // Assemble little-endian 16-bit length
        updateChecksum(byte);

        // Boundary safety check to prevent buffer overflow
        if (msgLength > PAYLOAD_BUFFER_SIZE)
        {
            currentState = UBX_STATE_SYNC1; // Throw away the frame
        }
        else if (msgLength == 0)
        {
            currentState = UBX_STATE_CK_A; // Edge case: empty payload
        }
        else
        {
            payloadIndex = 0;
            currentState = UBX_STATE_PAYLOAD;
        }
        break;

    case UBX_STATE_PAYLOAD:
        payloadBuffer[payloadIndex++] = byte;
        updateChecksum(byte);

        if (payloadIndex >= msgLength)
        {
            currentState = UBX_STATE_CK_A;
        }
        break;

    case UBX_STATE_CK_A:
        if (byte == ckA)
        {
            currentState = UBX_STATE_CK_B;
        }
        else
        {
            currentState = UBX_STATE_SYNC1; // Checksum A failure
        }
        break;

    case UBX_STATE_CK_B:
        if (byte == ckB)
        {
            // Checksum validated! Process the frame
            processValidMessage();
        }
        currentState = UBX_STATE_SYNC1; // Reset to look for next frame
        break;

    default:
        currentState = UBX_STATE_SYNC1;
        break;
    }
}

// Public function to pull the data down from your main loop/application
bool UBX_GetPvtData(UBX_NAV_PVT_t *outData)
{
    if (pvtDataReady && outData != NULL)
    {
        *outData = pvtData;
        pvtDataReady = false;
        return true;
    }
    return false;
}

/* Builds a UBX-CFG-VALSET message for a single L-type (1-byte bool) key.
 * Writes 17 bytes into buf and returns 17. */
static uint8_t build_cfgvalset_l(uint8_t *buf, uint32_t key_id, bool value)
{
    buf[0]  = 0xB5; buf[1]  = 0x62; // sync chars
    buf[2]  = 0x06; buf[3]  = 0x8A; // class CFG, id VALSET
    buf[4]  = 0x09; buf[5]  = 0x00; // payload length = 9 (little-endian)
    buf[6]  = 0x00;                  // version
    buf[7]  = 0x01;                  // layers: RAM only
    buf[8]  = 0x00; buf[9]  = 0x00; // reserved
    buf[10] = (uint8_t)(key_id      ); // key ID, little-endian
    buf[11] = (uint8_t)(key_id >>  8);
    buf[12] = (uint8_t)(key_id >> 16);
    buf[13] = (uint8_t)(key_id >> 24);
    buf[14] = value ? 0x01 : 0x00;  // value

    // 8-bit Fletcher checksum over class..payload (bytes 2..14)
    uint8_t ck_a = 0, ck_b = 0;
    for (uint8_t i = 2; i <= 14; i++) { ck_a += buf[i]; ck_b += ck_a; }
    buf[15] = ck_a;
    buf[16] = ck_b;
    return 17;
}

void UBX_EnableNavCorHost(UART_HandleTypeDef *huart, bool enable)
{
    uint8_t msg[17];
    uint8_t len = build_cfgvalset_l(msg, CFG_NAVCOR_ENABLE_HOST, enable);
    HAL_UART_Transmit(huart, msg, len, 100);
}

void UBX_EnableNavCorGalHas(UART_HandleTypeDef *huart, bool enable)
{
    uint8_t msg[17];
    uint8_t len = build_cfgvalset_l(msg, CFG_NAVCOR_ENABLE_GAL_HAS, enable);
    HAL_UART_Transmit(huart, msg, len, 100);
}