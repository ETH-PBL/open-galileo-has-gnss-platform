/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "adc.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "sens_wrapper.h"
#include "gnss_wrapper.h"
#include "shared_memory.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* DUAL_CORE_BOOT_SYNC_SEQUENCE: Define for dual core boot synchronization    */
/*                             demonstration code based on hardware semaphore */
/* This define is present in both CM7/CM4 projects                            */
/* To comment when developping/debugging on a single core                     */
#define DUAL_CORE_BOOT_SYNC_SEQUENCE

#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif
#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
volatile uint32_t error_counter = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
#if defined(DUAL_CORE_BOOT_SYNC_SEQUENCE)
  /*HW semaphore Clock enable*/
  __HAL_RCC_HSEM_CLK_ENABLE();
  /* Activate HSEM notification for Cortex-M4*/
  HAL_HSEM_ActivateNotification(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_0));
  /*
  Domain D2 goes to STOP mode (Cortex-M4 in deep-sleep) waiting for Cortex-M7 to
  perform system initialization (system clock config, external memory configuration.. )
  */
  HAL_PWREx_ClearPendingEvent();
  /* Keep CM4 debug interface active during STOP mode to avoid losing J-Link connection */
  HAL_DBGMCU_EnableDBGStopMode();
  HAL_PWREx_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFE, PWR_D2_DOMAIN);
  /* Clear HSEM flag */
  __HAL_HSEM_CLEAR_FLAG(__HAL_HSEM_SEMID_TO_MASK(HSEM_ID_0));

#endif /* DUAL_CORE_BOOT_SYNC_SEQUENCE */

  SharedMemory_Init();
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  MX_I2C4_Init();
  MX_SPI2_Init();
  MX_SPI3_Init();
  MX_UART5_Init();
  MX_USART3_UART_Init();
  MX_ADC1_Init();
  /* USER CODE BEGIN 2 */

  /* Start TIM2 Input Capture to enable the 10MHz counter and 1PPS sync */
  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_4);

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/* USER CODE BEGIN 4 */

/**
 * @brief Retargets the C library printf function to the ITM debug port.
 *        This allows printf output to show up in the STM32CubeIDE SWV ITM Data Console.
 */
int __io_putchar(int ch)
{
  /* Send character to the ITM (Instrumentation Trace Macrocell) */
  ITM_SendChar(ch);
  return ch;
}

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
  /* GNSS 1PPS timepulse captured by TIM2 CH4 */
  if (htim->Instance == TIM2 && htim->Channel == HAL_TIM_ACTIVE_CHANNEL_4)
  {
    uint32_t capture = HAL_TIM_ReadCapturedValue(htim, TIM_CHANNEL_4);

    if (pending_pps_valid)
    {
      active_pps_local_ticks = capture; /* Capture the exact 10MHz tick */
      active_pps_global_ms = pending_pps_global_ms;
      active_pps_valid = true;
      pending_pps_valid = false;
    }
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  /* External reference to TIM2 for the GNSS timepulse counter */
  extern TIM_HandleTypeDef htim2;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;

  /* Prevent crashing if sensor interrupts fire before the task initializes the queue */
  if (sensorEventQueue == NULL)
  {
    return;
  }

  uint32_t current_time_ticks = __HAL_TIM_GET_COUNTER(&htim2);

  SensorEvent_t event;
  /* Grab precise timing from the GNSS timepulse timer (TIM2) */
  event.timestamp_ticks = current_time_ticks;
  event.pps_local_baseline_ticks = active_pps_local_ticks;
  event.pps_global_baseline_ms = active_pps_global_ms;

  /* Identify the sensor based on the interrupt pin macros defined in main.h */
  if (GPIO_Pin == SENS_ISM_INT_Pin)
  {
    static uint32_t last_ism_ticks = 0;
    /* Debounce: 10MHz timer = 10,000 ticks per ms. 
       At 104 Hz ODR, valid interrupts arrive every ~9.6ms.
       A 0.2ms (20,00 ticks) debounce safely ignores fast EXTI bounces. */
    if ((uint32_t)(current_time_ticks - last_ism_ticks) < 2000)
    {
      return; /* Ignore this bounce */
    }
    last_ism_ticks = current_time_ticks;

    event.sensor_id = SENSOR_ISM330_INT;
    if (xQueueSendFromISR(sensorEventQueue, &event, &xHigherPriorityTaskWoken) != pdPASS)
    {
      /* Queue is full! Handle dropped ISM330 sample here*/
      error_counter++;
    }
  }
  else if (GPIO_Pin == SENS_LSM_INT_Pin)
  {
    static uint32_t last_lsm_acc_ticks = 0;
    /* Debounce: 10MHz timer = 10,000 ticks per ms. 
       At 100 Hz ODR, valid interrupts arrive every 10ms.
       A 0.2ms (20,000 ticks) debounce safely ignores fast EXTI bounces. */
    if ((uint32_t)(current_time_ticks - last_lsm_acc_ticks) < 2000)
    {
      return; /* Ignore this bounce */
    }
    last_lsm_acc_ticks = current_time_ticks;

    event.sensor_id = SENSOR_LSM303_ACC_INT;
    if (xQueueSendFromISR(sensorEventQueue, &event, &xHigherPriorityTaskWoken) != pdPASS)
    {
      /* Queue is full! Handle dropped LSM303 Acc sample here */
      error_counter++;
    }
  }
  else if (GPIO_Pin == SENS_LSM_INTM_Pin)
  {
    static uint32_t last_lsm_mag_ticks = 0;
    /* Debounce: 10MHz timer = 10,000 ticks per ms. 
       At 50 Hz ODR, valid interrupts arrive every 20ms.
       A 0.2ms (20,000 ticks) debounce safely ignores fast EXTI bounces. */
    if ((uint32_t)(current_time_ticks - last_lsm_mag_ticks) < 2000)
    {
      return; /* Ignore this bounce */
    }
    last_lsm_mag_ticks = current_time_ticks;
    
    event.sensor_id = SENSOR_LSM303_MAG_INT;
    if (xQueueSendFromISR(sensorEventQueue, &event, &xHigherPriorityTaskWoken) != pdPASS)
    {
      /* Queue is full! Handle dropped LSM303 Mag sample here */
      error_counter++;
    }
  }

  /* Yield if a higher priority task was woken by the queue send */
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
