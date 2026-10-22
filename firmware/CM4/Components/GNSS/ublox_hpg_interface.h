#ifndef UBLOX_HPG_INTERFACE_H
#define UBLOX_HPG_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>

/* UBX Protocol Constants */
#define UBX_SYNC_CHAR_1 0xB5
#define UBX_SYNC_CHAR_2 0x62

#define UBX_CLASS_NAV 0x01
#define UBX_ID_NAV_PVT 0x07

#define UBX_NAV_PVT_LENGTH 92
/*
 * UBX-NAV-PVT Payload Structure, contains detailed position, velocity, and time information.
 * Must be packed so the compiler doesn't add padding bytes.
 */

typedef struct
{
  // Type         Name            Unit  Description (Scaling)
  unsigned long iTOW;       //  ms    GPS time of week of the navigation epoch. See the description of iTOW for
                            //        details.
  unsigned short year;      //  y     Year (UTC)
  unsigned char month;      //  month Month, range 1..12 (UTC)
  unsigned char day;        //  d     Day of month, range 1..31 (UTC)
  unsigned char hour;       //  h     Hour of day, range 0..23 (UTC)
  unsigned char min;        //  min   Minute of hour, range 0..59 (UTC)
  unsigned char sec;        //  s     Seconds of minute, range 0..60 (UTC)
  char valid;               //        Validity Flags (see graphic below)
  unsigned long tAcc;       //  ns    Time accuracy estimate (UTC)
  long nano;                //  ns    Fraction of second, range -1e9 .. 1e9 (UTC)
  unsigned char fixType;    //        GNSSfix Type, range 0..5
                            //        0x00 = No Fix
                            //        0x01 = Dead Reckoning only
                            //        0x02 = 2D-Fix
                            //        0x03 = 3D-Fix
                            //        0x04 = GNSS + dead reckoning combined
                            //        0x05 = Time only fix
                            //        0x06..0xff: reserved
  char flags;               //        Fix Status Flags (see graphic below)
  unsigned char reserved1;  //        Reserved
  unsigned char numSV;      //        Number of satellites used in Nav Solution
  long lon;                 //  deg   Longitude (1e-7)
  long lat;                 //  deg   Latitude (1e-7)
  long height;              //  mm    Height above Ellipsoid
  long hMSL;                //  mm    Height above mean sea level
  unsigned long hAcc;       //  mm    Horizontal Accuracy Estimate
  unsigned long vAcc;       //  mm    Vertical Accuracy Estimate
  long velN;                //  mm/s  NED north velocity
  long velE;                //  mm/s  NED east velocity
  long velD;                //  mm/s  NED down velocity
  long gSpeed;              //  mm/s  Ground Speed (2-D)
  long heading;             //  deg   Heading of motion 2-D (1e-5)
  unsigned long sAcc;       //  mm/s  Speed Accuracy Estimate
  unsigned long headingAcc; //  deg   Heading Accuracy Estimate (1e-5)
  unsigned short pDOP;      //        Position DOP (0.01)
  short reserved2;          //        Reserved
  unsigned long reserved3;  //        Reserved
} __attribute__((packed)) UBX_NAV_PVT_t;

/*
 * UBX-TIM-TP Payload Structure, contains time pulse information used to synchronize the GNSS_TIMEPULSE signal
 * Must be packed so the compiler doesn't add padding bytes.
 */

#define UBX_CLASS_TIM 0x0D
#define UBX_ID_TIM_TP 0x01

typedef struct
{
  uint32_t towMS;    // Time of Week for next/current time pulse (ms)
  uint32_t towSubMS; // Submillisecond part of Time of Week (2^-32 s)
  int32_t qErr;      // Quantization error of time pulse (ps)
  uint16_t week;     // GPS week number
  uint8_t flags;     // bit 0: timeBase (0=GNSS, 1=UTC)
                     // bit 1: utc (1=UTC available)
                     // bits 3..2: raim (0=not avail, 1=not active, 2=active)
                     // bit 4: qErrInvalid (0=qErr valid, 1=qErr invalid)
                     // bit 5: TpNotLocked (0=Next TP locked to GNSS, 1=Not locked)
  uint8_t refInfo;   // Time reference information
} __attribute__((packed)) UBX_TIM_TP_t;

/* CFG-NAVCOR key IDs (UBX-CFG-VALSET, L type) */
#define CFG_NAVCOR_ENABLE_HOST    0x100d0001UL
#define CFG_NAVCOR_ENABLE_GAL_HAS 0x100d0002UL

/* Function Prototypes */
void UBX_ProcessByte(uint8_t byte);
bool UBX_GetPvtData(UBX_NAV_PVT_t *outData);

/* CFG-VALSET send functions — CM4 only (GNSS UART lives on CM4) */
#ifdef CORE_CM4
#include "stm32h7xx_hal.h"

/**
 * @brief Send a UBX-CFG-VALSET to enable/disable HOST corrections (SPARTN/RTCM).
 *        Must be set true when feeding external corrections from a host device.
 *        When both HOST and GAL_HAS are true, HOST (SPARTN) takes priority.
 */
void UBX_EnableNavCorHost(UART_HandleTypeDef *huart, bool enable);

/**
 * @brief Send a UBX-CFG-VALSET to enable/disable Galileo HAS corrections.
 *        Only processed when HOST corrections are disabled.
 */
void UBX_EnableNavCorGalHas(UART_HandleTypeDef *huart, bool enable);
#endif /* CORE_CM4 */

#endif // UBLOX_HPG_INTERFACE_H