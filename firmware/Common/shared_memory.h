#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

/* Include data structures from the wrappers */
#include "sens_wrapper.h"
#include "ublox_hpg_interface.h"

/* Base address for SRAM4 (D3 domain) */
#define SHARED_MEMORY_BASE 0x38000000UL

/* Offsets for the queues */
#define QUEUE_SENS_ADDR (SHARED_MEMORY_BASE + 0x0000) /* 0 KB offset (Size: ~16.5 KB) */
#define QUEUE_GNSS_ADDR (SHARED_MEMORY_BASE + 0x5000) /* 20 KB offset (Size: ~3 KB) */
#define QUEUE_CMD_ADDR (SHARED_MEMORY_BASE + 0x8000)  /* 32 KB offset */
#define QUEUE_RTCM_ADDR (SHARED_MEMORY_BASE + 0x9000) /* 36 KB offset */

/* Queue lengths MUST be a power of 2 for the bitwise AND wrap-around to work */
#define SENS_QUEUE_LEN 512
#define GNSS_QUEUE_LEN 32
#define CMD_QUEUE_LEN 16
#define RTCM_QUEUE_LEN 8

/* ---------------- payload definitions ----------------------------- */

/* 1. SENS Payload */
typedef enum
{
    SENS_TYPE_ISM330 = 0x01,
    SENS_TYPE_LSM303_ACC = 0x02,
    SENS_TYPE_LSM303_MAG = 0x03,
} SensType_t;

typedef struct
{
    SensType_t type;
    union
    {
        ISM330_Data_t ism330;
        LSM303_Acc_Data_t lsm303_acc;
        LSM303_Mag_Data_t lsm303_mag;
    } data;
} __attribute__((aligned(4))) SensPayload_t;

/* 2. GNSS Payload */
typedef enum
{
    CORRECTION_NONE    = 0,
    CORRECTION_SPARTN  = 1,
    CORRECTION_GAL_HAS = 2,
} CorrectionSource_t;

typedef struct
{
    UBX_NAV_PVT_t pvt_data;
    CorrectionSource_t correction_source;
} __attribute__((aligned(4))) GnssPayload_t;

/* 3. Command Payload */

/* Command IDs for correction source switching (CM7→CM4 via CMD queue) */
#define CMD_SET_CORRECTION_SPARTN   0x01U  /* enable HOST (SPARTN), disable GAL_HAS */
#define CMD_SET_CORRECTION_GAL_HAS  0x02U  /* disable HOST, enable GAL_HAS           */
#define CMD_SET_CORRECTION_NONE     0x03U  /* disable both HOST and GAL_HAS          */

typedef struct
{
    uint32_t command_id;
    uint32_t parameters[4];
} __attribute__((aligned(4))) CmdPayload_t;

/* 4. RTCM Payload */
typedef struct
{
    uint16_t length;
    uint8_t buffer[512];
} __attribute__((aligned(4))) RtcmPayload_t;

/* ---------------- queue control blocks ---------------------------- */

typedef struct
{
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile SensPayload_t buffer[SENS_QUEUE_LEN];
} __attribute__((aligned(32))) SensQueue_t;

typedef struct
{
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile GnssPayload_t buffer[GNSS_QUEUE_LEN];
} __attribute__((aligned(32))) GnssQueue_t;

typedef struct
{
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile CmdPayload_t buffer[CMD_QUEUE_LEN];
} __attribute__((aligned(32))) CmdQueue_t;

typedef struct
{
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile RtcmPayload_t buffer[RTCM_QUEUE_LEN];
} __attribute__((aligned(32))) RtcmQueue_t;

/* ---------------- external pointers ------------------------------ */
extern SensQueue_t *const queue_sens_ptr;
extern GnssQueue_t *const queue_gnss_ptr;
extern CmdQueue_t *const queue_cmd_ptr;
extern RtcmQueue_t *const queue_rtcm_ptr;

/* Initialization function */
void SharedMemory_Init(void);

/* ---------------- API -------------------------------------------- */
bool enqueue_sens(SensQueue_t *q, const SensPayload_t *src);
bool dequeue_sens(SensQueue_t *q, SensPayload_t *dst);

bool enqueue_gnss(GnssQueue_t *q, const GnssPayload_t *src);
bool dequeue_gnss(GnssQueue_t *q, GnssPayload_t *dst);

bool enqueue_cmd(CmdQueue_t *q, const CmdPayload_t *src);
bool dequeue_cmd(CmdQueue_t *q, CmdPayload_t *dst);

bool enqueue_rtcm(RtcmQueue_t *q, const RtcmPayload_t *src);
bool dequeue_rtcm(RtcmQueue_t *q, RtcmPayload_t *dst);

#endif /* SHARED_MEMORY_H */