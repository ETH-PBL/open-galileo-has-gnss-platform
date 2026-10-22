#include "shared_memory.h"
#include "stm32h7xx_hal.h"
#include <string.h>

/* ---------------- queue instances in shared RAM ------------------ */
#define QPTR(addr, type) ((type *)(addr))

SensQueue_t *const queue_sens_ptr = QPTR(QUEUE_SENS_ADDR, SensQueue_t);
GnssQueue_t *const queue_gnss_ptr = QPTR(QUEUE_GNSS_ADDR, GnssQueue_t);
CmdQueue_t *const queue_cmd_ptr = QPTR(QUEUE_CMD_ADDR, CmdQueue_t);
RtcmQueue_t *const queue_rtcm_ptr = QPTR(QUEUE_RTCM_ADDR, RtcmQueue_t);

/* --------- tiny cache helpers (CM7 only, no-ops on CM4) ---------- */
static inline void clean_cache(void *addr, size_t len)
{
    /* SRAM4 is configured via MPU as Non-Cacheable. Hardware cache operations
       on non-cacheable memory generate an Imprecise Bus Fault. */
    (void)addr;
    (void)len;
}

static inline void invalidate_cache(void *addr, size_t len)
{
    /* SRAM4 is configured via MPU as Non-Cacheable. Hardware cache operations
       on non-cacheable memory generate an Imprecise Bus Fault. */
    (void)addr;
    (void)len;
}

void SharedMemory_Init(void)
{
#ifdef CORE_CM7
    /* In dual-core STM32H7s, the CM4 entering STOP mode (to wait for CM7 boot)
       causes the D3 domain to automatically enter DSTOP, gating the SRAM4 clock.
       We must explicitly keep the D3 domain in RUN mode to allow memory access. */
    HAL_PWREx_ConfigD3Domain(PWR_D3_DOMAIN_RUN);

    /* Enable the SRAM4 clock (D3 domain) directly via the RCC register.
       This avoids missing/renamed HAL macro errors across firmware versions. */
    RCC->AHB4ENR |= (1UL << 29); /* Bit 29: SRAM4EN */
    (void)RCC->AHB4ENR;          /* Dummy read to guarantee clock activation */

    /* CM7 typically boots first. It clears the shared memory region
       before releasing the CM4 core to ensure a known 0-state. */
    memset((void *)QUEUE_SENS_ADDR, 0, sizeof(SensQueue_t));
    memset((void *)QUEUE_GNSS_ADDR, 0, sizeof(GnssQueue_t));
    memset((void *)QUEUE_CMD_ADDR, 0, sizeof(CmdQueue_t));
    memset((void *)QUEUE_RTCM_ADDR, 0, sizeof(RtcmQueue_t));

    /* Ensure the zeroes are committed to memory before the CM4 starts */
    clean_cache((void *)QUEUE_SENS_ADDR, sizeof(SensQueue_t));
    clean_cache((void *)QUEUE_GNSS_ADDR, sizeof(GnssQueue_t));
    clean_cache((void *)QUEUE_CMD_ADDR, sizeof(CmdQueue_t));
    clean_cache((void *)QUEUE_RTCM_ADDR, sizeof(RtcmQueue_t));
#endif
}

/* ---------------- SENS enqueue / dequeue ---------------------- */
bool enqueue_sens(SensQueue_t *q, const SensPayload_t *src)
{
    volatile SensQueue_t *vq = (volatile SensQueue_t *)q;
    uint16_t next_head = (vq->head + 1) & (SENS_QUEUE_LEN - 1);
    if (next_head == vq->tail)
    {
        return false; /* queue full, drop new */
    }
    vq->buffer[vq->head] = *src;
    __DMB(); /* Ensure payload is written before head is updated */
    vq->head = next_head;
    return true;
}

bool dequeue_sens(SensQueue_t *q, SensPayload_t *dst)
{
    volatile SensQueue_t *vq = (volatile SensQueue_t *)q;
    if (vq->head == vq->tail)
        return false; /* empty */

    __DMB(); /* Ensure head is read before payload */
    *dst = vq->buffer[vq->tail];
    __DMB(); /* Ensure payload is read before updating tail */
    vq->tail = (vq->tail + 1) & (SENS_QUEUE_LEN - 1);
    return true;
}

/* ---------------- GNSS enqueue / dequeue ---------------------- */
bool enqueue_gnss(GnssQueue_t *q, const GnssPayload_t *src)
{
    volatile GnssQueue_t *vq = (volatile GnssQueue_t *)q;
    uint16_t next_head = (vq->head + 1) & (GNSS_QUEUE_LEN - 1);
    if (next_head == vq->tail)
    {
        return false;
    }
    vq->buffer[vq->head] = *src;
    __DMB(); /* Ensure payload is written before head is updated */
    vq->head = next_head;
    return true;
}

bool dequeue_gnss(GnssQueue_t *q, GnssPayload_t *dst)
{
    volatile GnssQueue_t *vq = (volatile GnssQueue_t *)q;
    if (vq->head == vq->tail)
        return false;

    __DMB(); /* Ensure head is read before payload */
    *dst = vq->buffer[vq->tail];
    __DMB(); /* Ensure payload is read before updating tail */
    vq->tail = (vq->tail + 1) & (GNSS_QUEUE_LEN - 1);
    return true;
}

/* ---------------- CMD enqueue / dequeue ---------------------- */
bool enqueue_cmd(CmdQueue_t *q, const CmdPayload_t *src)
{
    volatile CmdQueue_t *vq = (volatile CmdQueue_t *)q;
    uint16_t next_head = (vq->head + 1) & (CMD_QUEUE_LEN - 1);
    if (next_head == vq->tail)
    {
        return false;
    }
    vq->buffer[vq->head] = *src;
    __DMB(); /* Ensure payload is written before head is updated */
    vq->head = next_head;
    return true;
}

bool dequeue_cmd(CmdQueue_t *q, CmdPayload_t *dst)
{
    volatile CmdQueue_t *vq = (volatile CmdQueue_t *)q;
    if (vq->head == vq->tail)
        return false;

    __DMB(); /* Ensure head is read before payload */
    *dst = vq->buffer[vq->tail];
    __DMB(); /* Ensure payload is read before updating tail */
    vq->tail = (vq->tail + 1) & (CMD_QUEUE_LEN - 1);
    return true;
}

/* ---------------- RTCM enqueue / dequeue ---------------------- */
bool enqueue_rtcm(RtcmQueue_t *q, const RtcmPayload_t *src)
{
    volatile RtcmQueue_t *vq = (volatile RtcmQueue_t *)q;
    uint16_t next_head = (vq->head + 1) & (RTCM_QUEUE_LEN - 1);
    if (next_head == vq->tail)
    {
        return false;
    }
    vq->buffer[vq->head] = *src;
    __DMB(); /* Ensure payload is written before head is updated */
    vq->head = next_head;
    return true;
}

bool dequeue_rtcm(RtcmQueue_t *q, RtcmPayload_t *dst)
{
    volatile RtcmQueue_t *vq = (volatile RtcmQueue_t *)q;
    if (vq->head == vq->tail)
        return false;

    __DMB(); /* Ensure head is read before payload */
    *dst = vq->buffer[vq->tail];
    __DMB(); /* Ensure payload is read before updating tail */
    vq->tail = (vq->tail + 1) & (RTCM_QUEUE_LEN - 1);
    return true;
}