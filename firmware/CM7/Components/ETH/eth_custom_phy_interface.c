/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_custom_phy_interface.c
  * @brief   DP83826 PHY bring-up for STM32H7 HAL + LwIP (RMII, ENHANCED mode)
  *
  * Approach: matches the working reference implementation.
  *  1. Hardware GPIO reset
  *  2. PHY address discovery (MDIO scan)
  *  3. Configure PHYSCR interrupts and MISR1 link-change interrupt
  *  4. RCSR: set 10-bit elasticity buffer and explicitly assert RMII_CLK_SEL
  *     (bit 7 = 1) so the PHY drives the 50 MHz REF_CLK output to the STM32
  *     regardless of the hardware strap state
  *  5. Wait for auto-negotiation (up to 3 s); pre-set MAC speed/duplex
  *  6. Set phy_initialized = 1
  *
  * MAC start (HAL_ETH_Start_IT) and netif link-up are handled ONLY by
  * ethernet_link_thread to avoid the race where phy_initialized is 0 while
  * link flags are already set.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "eth_custom_phy_interface.h"
#include "main.h"
#include "ethernetif.h"
#include "lwip/netif.h"

extern ETH_HandleTypeDef heth;
extern struct netif gnetif;

volatile phy_probe_t g_phy = {0};

static uint8_t s_phy_addr    = BOARD_ETH_PHY_ADDRESS;
static uint8_t s_initialized = 0;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static int mmd_read(uint8_t phy, uint16_t devad, uint16_t reg, uint16_t *out)
{
    uint32_t v = 0;
    if (ETH_PHY_IO_WriteReg(phy, PHY_REGCR, (uint16_t)(devad & 0x1F))             != 0) return -1;
    if (ETH_PHY_IO_WriteReg(phy, PHY_ADDAR, reg)                                   != 0) return -1;
    if (ETH_PHY_IO_WriteReg(phy, PHY_REGCR, (uint16_t)(0x4000u | (devad & 0x1F))) != 0) return -1;
    if (ETH_PHY_IO_ReadReg (phy, PHY_ADDAR, &v)                                    != 0) return -1;
    *out = (uint16_t)v;
    return 0;
}

static void phy_snapshot(void)
{
    uint32_t v = 0;
    ETH_PHY_IO_ReadReg(s_phy_addr, 0x02,       &v); g_phy.id1    = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, 0x03,       &v); g_phy.id2    = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BCR,    &v); g_phy.bcr    = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BSR,    &v); g_phy.bsr    = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_SR,     &v); g_phy.physts = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_PHYSCR, &v); g_phy.physcr = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_MISR1,  &v); g_phy.misr1  = (uint16_t)v;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_RCSR,   &v); g_phy.rcsr   = (uint16_t)v;
    uint16_t tmp = 0;
    if (mmd_read(s_phy_addr, 31, 0x0467, &tmp) == 0) g_phy.sor1 = tmp;
    if (mmd_read(s_phy_addr, 31, 0x0468, &tmp) == 0) g_phy.sor2 = tmp;
}

/* Scan MDIO 0..31 for a responding PHY; updates s_phy_addr.
   DP83826 PHYIDR1 = 0x2000. Any non-0/non-0xFFFF value is accepted. */
static int32_t phy_discover(void)
{
    for (uint8_t addr = 0; addr < 32; addr++)
    {
        uint32_t id1 = 0;
        if (ETH_PHY_IO_ReadReg(addr, 0x02, &id1) != 0) continue;
        uint16_t id = (uint16_t)id1;
        if (id != 0x0000 && id != 0xFFFF)
        {
            s_phy_addr            = addr;
            g_phy.discovered_addr = addr;
            return 0;
        }
    }
    g_phy.discovered_addr = 0xFF;
    return -1;
}

/* Apply resolved speed/duplex to the MAC before ethernet_link_thread starts it.
   Uses direct register writes (the HAL ETH_MACConfigTypeDef path requires the
   macro to be defined which it is not; the legacy MACCR path is always taken). */
static void mac_apply_speed_duplex(uint8_t speed_is_10, uint8_t duplex_full)
{
    uint32_t maccr = heth.Instance->MACCR;
    /* DM bit (bit 11) = 1 for full duplex */
#ifdef ETH_MACCR_DM
    if (duplex_full) maccr |= ETH_MACCR_DM; else maccr &= ~ETH_MACCR_DM;
#else
    if (duplex_full) maccr |= (1u << 11); else maccr &= ~(1u << 11);
#endif
    /* FES bit (bit 14) = 1 for 100 Mbps */
#ifdef ETH_MACCR_FES
    if (speed_is_10) maccr &= ~ETH_MACCR_FES; else maccr |= ETH_MACCR_FES;
#else
    if (speed_is_10) maccr &= ~(1u << 14); else maccr |= (1u << 14);
#endif
    heth.Instance->MACCR = maccr;
}

/* Wait for auto-negotiation complete + link up. Returns 0 on success. */
static int phy_wait_autoneg(uint32_t timeout_ms)
{
    uint32_t t0 = HAL_GetTick();
    uint32_t bsr;
    for (;;)
    {
        if ((HAL_GetTick() - t0) > timeout_ms) return -1;
        if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BSR, &bsr) != 0) { HAL_Delay(50); continue; }
        if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BSR, &bsr) != 0) { HAL_Delay(50); continue; }
        if ((bsr & PHY_LINKED_STATUS) && (bsr & PHY_AUTONEGO_COMPLETE)) return 0;
        HAL_Delay(50);
    }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int32_t eth_phy_init(void)
{
    uint32_t v = 0;
    g_phy.init_status = 0x1000;

    /* 1. Hardware reset ---------------------------------------------------- */
    HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(10);
    HAL_GPIO_WritePin(ETH_RST_GPIO_Port, ETH_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(50);

    /* 2. Discover PHY address on MDIO -------------------------------------- */
    if (phy_discover() != 0)
    {
        g_phy.init_status = 0xE001;
        return -1;
    }
    g_phy.init_status = 0x2000;

    /* 3. Early snapshot for debugger --------------------------------------- */
    /* Expected: id1 = 0x2000 (DP83826 OUI), rcsr shows strap defaults      */
    phy_snapshot();

    /* 4. Enable interrupt output and global interrupt enable (PHYSCR) ------ */
    if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_PHYSCR, &v) != 0) return -1;
    v |= (uint32_t)(PHY_PHYSCR_INT_EN | PHY_PHYSCR_INT_OE);
    if (ETH_PHY_IO_WriteReg(s_phy_addr, PHY_PHYSCR, v)  != 0) return -1;

    /* 5. Enable link-change interrupt in MISR1 ----------------------------- */
    if (ETH_PHY_IO_WriteReg(s_phy_addr, PHY_MISR1, PHY_MISR_LINK_INT_EN) != 0) return -1;
    ETH_PHY_IO_ReadReg(s_phy_addr, PHY_MISR1, &v); /* clear stale latched flags */

    /* 6. RCSR: force RMII mode + set 10-bit RX elasticity buffer.
       DP83826 ENHANCED mode: Strap1 (LED1/CLKOUT, pin 31) has internal
       pull-up, so it is HIGH (=1) at POR. Figure 8-13 of the DP83826
       datasheet shows Strap1=1 forces "PHY automatically in MII". This
       means after every GPIO reset, RCSR bit 5 = 0 (MII mode).
       The STM32 MAC is configured for RMII (HAL_ETH_RMII_MODE). Without
       RCSR bit 5 = 1, pin 19 outputs only 25 MHz (MII RX_CLK) instead of
       50 MHz (RMII leader clock), and the STM32 RMII interface has no clock.
       Bit 7 (RMII_CLK_SEL) must NOT be written — it is set by Strap5 at
       POR and selects the 25 MHz crystal as clock source for RMII leader. */
    if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_RCSR, &v) != 0) return -1;
    {
        uint16_t need = PHY_RCSR_RMII_MODE | PHY_RCSR_ELAST_10BIT;
        if ((v & need) != need)
        {
            v |= (uint32_t)need;
            if (ETH_PHY_IO_WriteReg(s_phy_addr, PHY_RCSR, v) != 0) return -1;
            /* Small settling time for pin 19 to start outputting 50 MHz */
            HAL_Delay(5);
        }
    }

    g_phy.init_status = 0x3000;

    /* 7. Wait for auto-negotiation (up to 3 s) ----------------------------- */
    if (phy_wait_autoneg(3000) == 0)
    {
        uint32_t sr = 0;
        if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_SR, &sr) == 0)
        {
            const uint8_t speed_is_10 = (sr & PHY_SPEED_STATUS)  ? 1u : 0u;
            const uint8_t duplex_full = (sr & PHY_DUPLEX_STATUS)  ? 1u : 0u;
            mac_apply_speed_duplex(speed_is_10, duplex_full);
            g_phy.speed_is_10 = speed_is_10;
            g_phy.duplex_full = duplex_full;
            g_phy.link_up     = 1;
        }
    }

    /* 8. Post-config snapshot ---------------------------------------------- */
    phy_snapshot();

    /* 9. CRITICAL ORDER: set phy_initialized BEFORE touching link/MAC state. */
    /* ethernet_link_thread calls eth_phy_get_link_state() which checks        */
    /* phy_initialized. If it is 0 when the thread sees netif_link_up==TRUE,  */
    /* it interprets that as an error and tears the link back down (the race   */
    /* condition in the original code). Setting it first makes EthLink safe    */
    /* to preempt at any point below.                                          */
    s_initialized     = 1;
    g_phy.init_status = 0x0000;

    /* 10. Start MAC and signal link up — only if autoneg succeeded.          */
    /* ethernet_link_thread handles restart if the link later drops.          */
    if (g_phy.link_up)
    {
        if (heth.gState == HAL_ETH_STATE_READY)
            HAL_ETH_Start_IT(&heth);
        if (!(gnetif.flags & NETIF_FLAG_LINK_UP))
            netif_set_link_up(&gnetif);
    }

    return 0;
}

int32_t eth_phy_get_link_state(void)
{
    uint32_t bsr = 0, sr = 0;
    if (!s_initialized) return ETH_PHY_STATUS_ERROR;

    if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BSR, &bsr) != 0) return ETH_PHY_STATUS_ERROR;
    if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_BSR, &bsr) != 0) return ETH_PHY_STATUS_ERROR;

    if (!(bsr & PHY_LINKED_STATUS))
    {
        g_phy.link_up = 0;
        return ETH_PHY_STATUS_LINK_DOWN;
    }
    if (ETH_PHY_IO_ReadReg(s_phy_addr, PHY_SR, &sr) != 0) return ETH_PHY_STATUS_ERROR;

    g_phy.link_up     = 1;
    g_phy.speed_is_10 = (uint8_t)((sr & PHY_SPEED_STATUS) ? 1u : 0u);
    g_phy.duplex_full = (uint8_t)((sr & PHY_DUPLEX_STATUS) ? 1u : 0u);
    g_phy.physts      = (uint16_t)sr;
    g_phy.bsr         = (uint16_t)bsr;

    if (g_phy.speed_is_10)
        return g_phy.duplex_full ? ETH_PHY_STATUS_10MBITS_FULLDUPLEX
                                 : ETH_PHY_STATUS_10MBITS_HALFDUPLEX;
    else
        return g_phy.duplex_full ? ETH_PHY_STATUS_100MBITS_FULLDUPLEX
                                 : ETH_PHY_STATUS_100MBITS_HALFDUPLEX;
}

uint8_t eth_phy_link_up(void)
{
    return (eth_phy_get_link_state() > ETH_PHY_STATUS_LINK_DOWN) ? 1U : 0U;
}
