#ifndef SENS_WRAPPER_H
#define SENS_WRAPPER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Enum to identify which sensor triggered the interrupt */
typedef enum
{
    SENSOR_ISM330_INT = 0,
    SENSOR_LSM303_ACC_INT,
    SENSOR_LSM303_MAG_INT
} SensorType_t;

/* Structure to hold the interrupt event data */
typedef struct
{
    uint32_t timestamp_ticks;          // The exact time the IMU fired (in 10MHz ticks)
    uint32_t pps_local_baseline_ticks; // The TIM2 value of the LAST pulse
    uint64_t pps_global_baseline_ms;   // The GPS time of the LAST pulse
    uint8_t sensor_id;
} SensorEvent_t;

/* Sensor data structures */
typedef struct
{
    uint64_t timestamp;
    int16_t acceleration[3];
    int16_t angular_rate[3];
} ISM330_Data_t;

typedef struct
{
    uint64_t timestamp;
    int16_t acceleration[3];
} LSM303_Acc_Data_t;

typedef struct
{
    uint64_t timestamp;
    int16_t magnetic[3];
} LSM303_Mag_Data_t;

/* FreeRTOS Handles */
extern QueueHandle_t sensorEventQueue;
extern TaskHandle_t SENSTASKHANDLE;

void SensTask_Init(void);
void Sensors_Config(void);
void SENSTaskBody(void *argument);

#endif /* SENS_WRAPPER_H */