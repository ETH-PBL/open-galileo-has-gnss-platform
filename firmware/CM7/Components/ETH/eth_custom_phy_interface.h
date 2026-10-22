/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    eth_custom_phy_interface.h
  * @brief   DP83826 PHY driver for STM32H7 HAL + LwIP (RMII, ENHANCED mode)
  *
  * Register definitions cross-checked against CycloneTCP dp83826_driver.h
  * (Oryx Embedded, GPL-2.0).
  ******************************************************************************
  */
#ifndef ETH_CUSTOM_PHY_INTERFACE_H
#define ETH_CUSTOM_PHY_INTERFACE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* PHY address                                                                 */
/* LED pins all strapped to 3.3V → ENHANCED mode address bits [2:0] = 1,1,1  */
/* → PHY address = 0x07. phy_discover() confirms at runtime.                 */
/* -------------------------------------------------------------------------- */
#define BOARD_ETH_PHY_ADDRESS   0x07U

/* -------------------------------------------------------------------------- */
/* Clause-22 registers (DP83826)                                              */
/* -------------------------------------------------------------------------- */
#define PHY_BCR     ((uint16_t)0x00)   /* Basic Control */
#define PHY_BSR     ((uint16_t)0x01)   /* Basic Status  */
#define PHY_ANAR    ((uint16_t)0x04)   /* AN Advertisement */
#define PHY_SR      ((uint16_t)0x10)   /* PHYSTS        */
#define PHY_PHYSCR  ((uint16_t)0x11)   /* PHY Special Ctrl */
#define PHY_MISR1   ((uint16_t)0x12)   /* Masked Int/Status */
#define PHY_FCSCR   ((uint16_t)0x14)   /* False Carrier SCR */
#define PHY_RCSR    ((uint16_t)0x17)   /* RMII Ctrl/Status  */
#define PHY_REGCR   ((uint16_t)0x0D)   /* MMD Access Ctrl   */
#define PHY_ADDAR   ((uint16_t)0x0E)   /* MMD Address/Data  */

/* BMCR (0x00) */
#define PHY_RESET                   ((uint16_t)0x8000)
#define PHY_LOOPBACK                ((uint16_t)0x4000)
#define PHY_AUTONEGOTIATION         ((uint16_t)0x1000)
#define PHY_RESTART_AUTONEGOTIATION ((uint16_t)0x0200)
#define PHY_POWERDOWN               ((uint16_t)0x0800)
#define PHY_ISOLATE                 ((uint16_t)0x0400)

/* BMSR (0x01) */
#define PHY_AUTONEGO_COMPLETE       ((uint16_t)0x0020)
#define PHY_LINKED_STATUS           ((uint16_t)0x0004)

/* PHYSTS (0x10) */
#define PHY_LINK_STATUS             ((uint16_t)0x0001)  /* 1 = link up   */
#define PHY_SPEED_STATUS            ((uint16_t)0x0002)  /* 1 = 10M       */
#define PHY_DUPLEX_STATUS           ((uint16_t)0x0004)  /* 1 = full      */

/* PHYSCR (0x11) */
#define PHY_PHYSCR_INT_OE           ((uint16_t)0x0001)
#define PHY_PHYSCR_INT_EN           ((uint16_t)0x0002)

/* MISR1 (0x12) */
#define PHY_MISR_LINK_INT_EN        ((uint16_t)0x0020)
#define PHY_LINK_INTERRUPT          ((uint16_t)0x2000)

/* RCSR (0x17) — DP83826 datasheet Table 8-41.
   Bit 5 (0x0020) IS defined: RMII Mode (0=MII, 1=RMII).
   This MUST be written to 1 because in ENHANCED mode Strap1 (LED1/CLKOUT
   pin 31) has an internal pull-up (default=1), which forces MII mode on
   POR per Figure 8-13. Software override via this bit is required to switch
   the PHY to RMII leader mode so pin 19 (RX_CLK/50MHz_RMII) outputs 50 MHz
   to STM32 PA1 (ETH_REF_CLK). Without this the STM32 RMII MAC has no clock.
   Bit 7 (RMII_CLK_SEL): latched from strap at POR — do NOT write. */
#define PHY_RCSR_RMII_MODE          ((uint16_t)0x0020)  /* bit 5: 1=RMII, 0=MII (strap default = MII) */
#define PHY_RCSR_ELAST_10BIT        ((uint16_t)0x0003)  /* bits [1:0] = 10-bit RX elasticity buffer */

/* -------------------------------------------------------------------------- */
/* MDIO IO functions (implemented in ethernetif.c)                            */
/* -------------------------------------------------------------------------- */
extern int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
extern int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);

/* -------------------------------------------------------------------------- */
/* Status codes                                                                */
/* -------------------------------------------------------------------------- */
#define ETH_PHY_STATUS_ERROR               ((int32_t)-1)
#define ETH_PHY_STATUS_LINK_DOWN           ((int32_t) 1)
#define ETH_PHY_STATUS_100MBITS_FULLDUPLEX ((int32_t) 2)
#define ETH_PHY_STATUS_100MBITS_HALFDUPLEX ((int32_t) 3)
#define ETH_PHY_STATUS_10MBITS_FULLDUPLEX  ((int32_t) 4)
#define ETH_PHY_STATUS_10MBITS_HALFDUPLEX  ((int32_t) 5)

/* -------------------------------------------------------------------------- */
/* Debug snapshot (watch in debugger)                                         */
/* -------------------------------------------------------------------------- */
typedef struct {
    volatile uint16_t id1, id2;
    volatile uint16_t bcr, bsr, physts;
    volatile uint16_t physcr, misr1, rcsr;
    volatile uint16_t sor1, sor2;
    volatile uint8_t  link_up, speed_is_10, duplex_full;
    volatile uint32_t init_status;
    volatile uint8_t  discovered_addr;
} phy_probe_t;

extern volatile phy_probe_t g_phy;

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */
int32_t eth_phy_init(void);
int32_t eth_phy_get_link_state(void);
uint8_t eth_phy_link_up(void);

#ifdef __cplusplus
}
#endif
#endif /* ETH_CUSTOM_PHY_INTERFACE_H */
/* USER CODE END Header */
