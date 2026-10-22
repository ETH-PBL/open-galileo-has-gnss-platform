/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define GNSS_RTK_STAT_Pin GPIO_PIN_2
#define GNSS_RTK_STAT_GPIO_Port GPIOE
#define SENS_ISM_INT_Pin GPIO_PIN_3
#define SENS_ISM_INT_GPIO_Port GPIOE
#define SENS_LSM_INT_Pin GPIO_PIN_4
#define SENS_LSM_INT_GPIO_Port GPIOE
#define SENS_LSM_INTM_Pin GPIO_PIN_5
#define SENS_LSM_INTM_GPIO_Port GPIOE
#define GEOFENCE_STAT_Pin GPIO_PIN_6
#define GEOFENCE_STAT_GPIO_Port GPIOE
#define GNSS_EXTINT_Pin GPIO_PIN_13
#define GNSS_EXTINT_GPIO_Port GPIOC
#define MB_ADC_Pin GPIO_PIN_0
#define MB_ADC_GPIO_Port GPIOA
#define GNSS_TIMEPULSE_Pin GPIO_PIN_3
#define GNSS_TIMEPULSE_GPIO_Port GPIOA
#define MB_RST_Pin GPIO_PIN_0
#define MB_RST_GPIO_Port GPIOB
#define MB_I2C_SCL_Pin GPIO_PIN_14
#define MB_I2C_SCL_GPIO_Port GPIOF
#define MB_I2C_SDA_Pin GPIO_PIN_15
#define MB_I2C_SDA_GPIO_Port GPIOF
#define SPI_CS_MB_Pin GPIO_PIN_12
#define SPI_CS_MB_GPIO_Port GPIOB
#define SPI_CS_XL_Pin GPIO_PIN_13
#define SPI_CS_XL_GPIO_Port GPIOD
#define MB_PWM_Pin GPIO_PIN_14
#define MB_PWM_GPIO_Port GPIOD
#define SPI_CS_MAG_Pin GPIO_PIN_7
#define SPI_CS_MAG_GPIO_Port GPIOG
#define SPI_CS_ISM_Pin GPIO_PIN_8
#define SPI_CS_ISM_GPIO_Port GPIOG
#define MB_INT_Pin GPIO_PIN_8
#define MB_INT_GPIO_Port GPIOC
#define GNSS_RESET_N_Pin GPIO_PIN_7
#define GNSS_RESET_N_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
