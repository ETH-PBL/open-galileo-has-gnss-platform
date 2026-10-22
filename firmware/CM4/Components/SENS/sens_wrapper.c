#include "sens_wrapper.h"
#include <stdio.h>
#include "ism330dlc_reg.h"
#include "lsm303agr_reg.h"
#include "shared_memory.h"
#include "main.h"
#include "spi.h"

QueueHandle_t sensorEventQueue;
TaskHandle_t SENSTASKHANDLE;

/* Sensor device contexts - to be initialized with your platform SPI read/write functions during sensor setup */
stmdev_ctx_t ism330_ctx;
stmdev_ctx_t lsm303_acc_ctx;
stmdev_ctx_t lsm303_mag_ctx;

/* Hardware SPI Binding Structures */
typedef struct
{
    GPIO_TypeDef *cs_port;
    uint16_t cs_pin;
} SPI_CS_t;

static SPI_CS_t ism_cs = {SPI_CS_ISM_GPIO_Port, SPI_CS_ISM_Pin};
static SPI_CS_t xl_cs = {SPI_CS_XL_GPIO_Port, SPI_CS_XL_Pin};
static SPI_CS_t mag_cs = {SPI_CS_MAG_GPIO_Port, SPI_CS_MAG_Pin};

/* Generic SPI Write for ST Sensor Drivers */
static int32_t platform_write(void *handle, uint8_t reg, const uint8_t *bufp, uint16_t len)
{
    SPI_CS_t *cs = (SPI_CS_t *)handle;
    uint8_t dummy_rx;

    /* If writing multiple bytes to LSM303AGR, enable the Address Auto-Increment bit */
    if (len > 1 && handle == &xl_cs)
    {
        reg |= 0x40;
    }

    HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_RESET);

    /* Use TransmitReceive to absorb incoming bytes and prevent SPI Overrun (OVR) */
    HAL_SPI_TransmitReceive(&hspi2, &reg, &dummy_rx, 1, 100);
    for (uint16_t i = 0; i < len; i++)
    {
        HAL_SPI_TransmitReceive(&hspi2, (uint8_t *)&bufp[i], &dummy_rx, 1, 100);
    }

    HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_SET);
    return 0;
}

/* Generic SPI Read for ST Sensor Drivers */
static int32_t platform_read(void *handle, uint8_t reg, uint8_t *bufp, uint16_t len)
{
    SPI_CS_t *cs = (SPI_CS_t *)handle;
    uint8_t dummy_rx;
    reg |= 0x80; /* Auto-set the read bit (MSB) for ST Sensors */

    /* If reading multiple bytes from LSM303AGR, enable the Address Auto-Increment bit */
    if (len > 1 && handle == &xl_cs)
    {
        reg |= 0x40;
    }

    HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_RESET);

    /* Clear Overrun flag just in case a previous transaction left it set */
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);

    /* Use TransmitReceive to absorb the dummy byte clocked in during the address transmission,
       leaving the RX FIFO clean for the actual data read */
    if (HAL_SPI_TransmitReceive(&hspi2, &reg, &dummy_rx, 1, 100) != HAL_OK)
    {
        HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_SET);
        return -1;
    }

    /* --- SPI MOSI HACK FOR 3-WIRE SENSORS --- */
    /* Float the MOSI pin (PB15) so the STM32 doesn't drive it during the read phase.
       This prevents hardware collisions with the LSM303 driving SDI in 3-wire mode. */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = GPIO_PIN_15;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    /* ---------------------------------------- */

    if (HAL_SPI_Receive(&hspi2, bufp, len, 100) != HAL_OK)
    {
        /* Restore MOSI */
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_SET);
        return -1;
    }

    /* --- RESTORE MOSI PIN --- */
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    /* ------------------------ */

    HAL_GPIO_WritePin(cs->cs_port, cs->cs_pin, GPIO_PIN_SET);
    return 0;
}

/**
 * @brief Reads IMU data from ISM330DLC hardware FIFO
 */
void Read_ISM330_Data(uint64_t timestamp)
{
    uint16_t num_words = 0;
    ISM330_Data_t sensor_data;

    /* Get the number of unread 16-bit words in the FIFO */
    if (ism330dlc_fifo_data_level_get(&ism330_ctx, &num_words) != 0)
    {
        printf("Error reading ISM330 FIFO level\r\n");
        return;
    }

    /* If both Accel and Gyro have the same ODR, the pattern is 6 words long (3 Gyro, 3 Accel) */
    uint16_t num_patterns = num_words / 6;

    /* Force a read to clear a stuck interrupt if level reads < 6 but the ISR fired */
    if (num_patterns == 0)
    {
        num_patterns = 1;
    }

    /* Drain all pending samples from the FIFO */
    for (int i = 0; i < num_patterns; i++)
    {
        uint8_t buffer[12];

        /* Read 12 bytes (6 words) at a time */
        if (ism330dlc_fifo_raw_data_get(&ism330_ctx, buffer, 12) != 0)
        {
            break;
        }

        /* Parse the FIFO pattern: By default, Gyroscope data is written to the FIFO first, followed by Accelerometer */
        sensor_data.angular_rate[0] = (int16_t)((buffer[1] << 8) | buffer[0]);
        sensor_data.angular_rate[1] = (int16_t)((buffer[3] << 8) | buffer[2]);
        sensor_data.angular_rate[2] = (int16_t)((buffer[5] << 8) | buffer[4]);

        sensor_data.acceleration[0] = (int16_t)((buffer[7] << 8) | buffer[6]);
        sensor_data.acceleration[1] = (int16_t)((buffer[9] << 8) | buffer[8]);
        sensor_data.acceleration[2] = (int16_t)((buffer[11] << 8) | buffer[10]);

        sensor_data.timestamp = timestamp;

        SensPayload_t payload;
        payload.type = SENS_TYPE_ISM330;
        payload.data.ism330 = sensor_data;
        enqueue_sens(queue_sens_ptr, &payload);
    }
}

/**
 * @brief Reads Accelerometer data from LSM303AGR hardware FIFO
 */
void Read_LSM303_Acc_Data(uint64_t timestamp)
{
    uint8_t unread_samples = 0;
    LSM303_Acc_Data_t acc_data;

    /* Check how many unread samples are in the FIFO */
    if (lsm303agr_xl_fifo_data_level_get(&lsm303_acc_ctx, &unread_samples) != 0)
    {
        printf("Error reading LSM303 FIFO level\r\n");
        return;
    }

    /* Force a read to clear a stuck interrupt if level reads 0 but the ISR fired */
    if (unread_samples == 0)
    {
        unread_samples = 1;
    }

    /* Read out all buffered samples. For LSM303AGR, reading the standard
       OUT_X_L_A register automatically pops the oldest value from the FIFO */
    for (int i = 0; i < unread_samples; i++)
    {
        if (lsm303agr_acceleration_raw_get(&lsm303_acc_ctx, acc_data.acceleration) != 0)
        {
            break;
        }

        /* Add 10,000 microseconds (10ms) for each subsequent sample in the FIFO burst (100Hz ODR) */
        acc_data.timestamp = timestamp + (i * 10000ULL);

        SensPayload_t payload;
        payload.type = SENS_TYPE_LSM303_ACC;
        payload.data.lsm303_acc = acc_data;
        enqueue_sens(queue_sens_ptr, &payload);
    }
}

/**
 * @brief Reads Magnetometer data from LSM303AGR
 */
void Read_LSM303_Mag_Data(uint64_t timestamp)
{
    LSM303_Mag_Data_t mag_data;

    if (lsm303agr_magnetic_raw_get(&lsm303_mag_ctx, mag_data.magnetic) != 0)
    {
        printf("Error reading LSM303 magnetometer data\r\n");
        return;
    }

    /* The sensor requires reading the INT_SOURCE_REG_M register to clear the
       hardware interrupt pin when using the threshold generator hack */
    uint8_t dummy;
    platform_read(&mag_cs, LSM303AGR_INT_SOURCE_REG_M, &dummy, 1);

    mag_data.timestamp = timestamp;

    SensPayload_t payload;
    payload.type = SENS_TYPE_LSM303_MAG;
    payload.data.lsm303_mag = mag_data;
    enqueue_sens(queue_sens_ptr, &payload);
}

uint64_t Translate_To_Global_Time(SensorEvent_t *event);

/**
 * @brief Initializes the Sensor Queue and Task
 */
void SensTask_Init(void)
{
    /* Create a queue capable of holding 200 sensor events */
    sensorEventQueue = xQueueCreate(200, sizeof(SensorEvent_t));

    /* Bind hardware interfaces to the ST drivers */
    ism330_ctx.write_reg = platform_write;
    ism330_ctx.read_reg = platform_read;
    ism330_ctx.handle = (void *)&ism_cs;

    lsm303_acc_ctx.write_reg = platform_write;
    lsm303_acc_ctx.read_reg = platform_read;
    lsm303_acc_ctx.handle = (void *)&xl_cs;

    lsm303_mag_ctx.write_reg = platform_write;
    lsm303_mag_ctx.read_reg = platform_read;
    lsm303_mag_ctx.handle = (void *)&mag_cs;

    /* Apply settings and awake the sensors */
    Sensors_Config();

    /* Drain FIFOs and clear any pending interrupts that might have fired
       during configuration so we can detect clean RISING edges */
    Read_ISM330_Data(0);
    Read_LSM303_Acc_Data(0);
    Read_LSM303_Mag_Data(0);
}

/**
 * @brief Configures the power modes, sample rates, and hardware FIFOs
 *        on the ISM330DLC and LSM303AGR.
 *        Call this function from your main sensor setup routine AFTER
 *        initializing the stmdev_ctx_t structures.
 */
void Sensors_Config(void)
{
    /* Force sensors into Power-Down mode so they stop generating interrupts during setup */
    uint8_t ism_pd = 0x00;
    platform_write(&ism_cs, ISM330DLC_CTRL1_XL, &ism_pd, 1);
    platform_write(&ism_cs, ISM330DLC_CTRL2_G, &ism_pd, 1);

    uint8_t lsm_acc_pd = 0x00;
    platform_write(&xl_cs, LSM303AGR_CTRL_REG1_A, &lsm_acc_pd, 1);

    uint8_t lsm_mag_pd = 0x03; /* MD = 11 (Power Down) */
    platform_write(&mag_cs, LSM303AGR_CFG_REG_A_M, &lsm_mag_pd, 1);

    /* Give sensors time to power down cleanly */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Reboot memory content to guarantee a clean state across soft-resets.
       Using RAW writes instead of ST drivers to avoid Read-Modify-Write corruption
       if the SPI bus or sensors are in an unexpected state during soft-reset. */
    uint8_t ism_reboot = 0x81; /* BOOT = 1, SW_RESET = 1 */
    platform_write(&ism_cs, ISM330DLC_CTRL3_C, &ism_reboot, 1);

    uint8_t lsm_acc_reboot = 0x80; /* BOOT = 1 */
    platform_write(&xl_cs, LSM303AGR_CTRL_REG5_A, &lsm_acc_reboot, 1);

    uint8_t lsm_mag_reboot = 0x63; /* REBOOT = 1, SOFT_RST = 1, MD = 11 (Power Down) */
    platform_write(&mag_cs, LSM303AGR_CFG_REG_A_M, &lsm_mag_reboot, 1);

    /* Give sensors maximum time to restart their internal SRAMs (LIS2MDL takes up to 50ms) */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ----- 3-WIRE SPI CONFIGURATION ----- */
    /* Because MOSI and MISO are physically tied with a resistor (SPI hack), we MUST force
       the LSM303AGR sensors into 3-wire mode BEFORE doing any read-modify-write operations.
       (The ISM330DLC is wired standard 4-wire, so it is left in its default SIM=0 mode). */

    /* LSM303AGR Accel: CTRL_REG4_A (0x23). Default is 0x00. We add SIM=1 (0x01) */
    uint8_t lsm_ctrl4_a = 0x01;
    lsm303agr_write_reg(&lsm303_acc_ctx, LSM303AGR_CTRL_REG4_A, &lsm_ctrl4_a, 1);

    /* Disable I2C interfaces to lock sensors into SPI mode exclusively */
    ism330dlc_i2c_interface_set(&ism330_ctx, ISM330DLC_I2C_DISABLE);

    /* -------------------------------------------------------------
     * ISM330DLC (IMU) Configuration (High Precision, 104 Hz)
     * ------------------------------------------------------------- */
    /* Enable Block Data Update (BDU) to prevent MSB/LSB corruption */
    ism330dlc_block_data_update_set(&ism330_ctx, 1);

    /* Set Data Ready and FIFO interrupts to PULSED mode to guarantee rising edges for EXTI */
    ism330dlc_data_ready_mode_set(&ism330_ctx, ISM330DLC_DRDY_PULSED);

    /* Set High Performance Mode for Accel and Gyro */
    ism330dlc_xl_power_mode_set(&ism330_ctx, ISM330DLC_XL_HIGH_PERFORMANCE);
    ism330dlc_gy_power_mode_set(&ism330_ctx, ISM330DLC_GY_HIGH_PERFORMANCE);

    /* Set Full Scale for highest precision (±2g Accel, ±250dps Gyro) */
    ism330dlc_xl_full_scale_set(&ism330_ctx, ISM330DLC_2g);
    ism330dlc_gy_full_scale_set(&ism330_ctx, ISM330DLC_250dps);

    /* Batch Accelerometer and Gyroscope to the FIFO (No Decimation) */
    ism330dlc_fifo_xl_batch_set(&ism330_ctx, ISM330DLC_FIFO_XL_NO_DEC);
    ism330dlc_fifo_gy_batch_set(&ism330_ctx, ISM330DLC_FIFO_GY_NO_DEC);

    /* Set FIFO data rate (Must match configured sensor ODR, e.g., 104Hz) */
    ism330dlc_fifo_data_rate_set(&ism330_ctx, ISM330DLC_FIFO_104Hz);

    /* Force FIFO into Bypass Mode to completely flush it */
    ism330dlc_fifo_mode_set(&ism330_ctx, ISM330DLC_BYPASS_MODE);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Restore to Stream mode (Continuous mode: overwrites oldest if full) */
    ism330dlc_fifo_mode_set(&ism330_ctx, ISM330DLC_STREAM_MODE);

    /* Set FIFO watermark to 6 words (1 complete IMU sample: 3 Gyro + 3 Accel) */
    ism330dlc_fifo_watermark_set(&ism330_ctx, 6);

    /* Route FIFO Threshold interrupt to INT1 pad */
    ism330dlc_int1_route_t ism_int1_route = {0};
    ism_int1_route.int1_fth = 1;
    ism330dlc_pin_int1_route_set(&ism330_ctx, ism_int1_route);

    /* -------------------------------------------------------------
     * LSM303AGR (Accelerometer) Configuration (High Res 12-bit, 100 Hz)
     * ------------------------------------------------------------- */
    /* Explicitly enable X, Y, Z axes (in case soft reset left them disabled) */
    uint8_t reg1a = 0;
    lsm303agr_read_reg(&lsm303_acc_ctx, LSM303AGR_CTRL_REG1_A, &reg1a, 1);
    reg1a |= 0x07;
    lsm303agr_write_reg(&lsm303_acc_ctx, LSM303AGR_CTRL_REG1_A, &reg1a, 1);

    /* Set High Resolution 12-bit Mode */
    lsm303agr_xl_operating_mode_set(&lsm303_acc_ctx, LSM303AGR_HR_12bit);

    /* Set Full Scale to ±2g for highest precision */
    lsm303agr_xl_full_scale_set(&lsm303_acc_ctx, LSM303AGR_2g);

    /* Enable Block Data Update (BDU) and FIFO */
    lsm303agr_xl_block_data_update_set(&lsm303_acc_ctx, 1);
    lsm303agr_xl_fifo_set(&lsm303_acc_ctx, PROPERTY_ENABLE);

    /* Force FIFO into Bypass Mode to completely flush any stuck interrupts */
    lsm303agr_xl_fifo_mode_set(&lsm303_acc_ctx, LSM303AGR_BYPASS_MODE);
    vTaskDelay(pdMS_TO_TICKS(10));
    lsm303agr_xl_fifo_mode_set(&lsm303_acc_ctx, LSM303AGR_DYNAMIC_STREAM_MODE);

    /* Set FIFO watermark to 1 sample and route to INT1 */
    lsm303agr_xl_fifo_watermark_set(&lsm303_acc_ctx, 1);
    lsm303agr_ctrl_reg3_a_t xl_ctrl3_a = {0};
    xl_ctrl3_a.i1_wtm = 1;
    lsm303agr_xl_pin_int1_config_set(&lsm303_acc_ctx, &xl_ctrl3_a);

    /* -------------------------------------------------------------
     * LSM303AGR (Magnetometer) Configuration (High Res, Continuous, 50 Hz)
     * ------------------------------------------------------------- */
    /* Disable I2C interfaces to lock sensors into SPI mode exclusively */
    lsm303agr_mag_i2c_interface_set(&lsm303_mag_ctx, LSM303AGR_I2C_DISABLE);

    lsm303agr_mag_power_mode_set(&lsm303_mag_ctx, LSM303AGR_HIGH_RESOLUTION);

    /* Enable Block Data Update (BDU) and Temperature Compensation */
    lsm303agr_mag_block_data_update_set(&lsm303_mag_ctx, 1);
    lsm303agr_mag_offset_temp_comp_set(&lsm303_mag_ctx, 1);

    /* Configure threshold generator hack to create DRDY interrupts */
    lsm303agr_mag_int_gen_threshold_set(&lsm303_mag_ctx, 1);

    lsm303agr_int_crtl_reg_m_t mag_int_ctrl = {0};
    lsm303agr_mag_int_gen_conf_get(&lsm303_mag_ctx, &mag_int_ctrl);
    mag_int_ctrl.xien = 1;
    mag_int_ctrl.yien = 1;
    mag_int_ctrl.zien = 1;
    mag_int_ctrl.iea = 1; /* Interrupt active high */
    mag_int_ctrl.ien = 1; /* Enable interrupt generation */
    mag_int_ctrl.iel = 1; /* Latched interrupt */
    lsm303agr_mag_int_gen_conf_set(&lsm303_mag_ctx, &mag_int_ctrl);

    /* Route interrupt generator to INT_DRDY_M pin */
    lsm303agr_mag_int_on_pin_set(&lsm303_mag_ctx, 1);

    /* ---> FINAL STEP: POWER UP (SET ODRs) <--- */
    ism330dlc_xl_data_rate_set(&ism330_ctx, ISM330DLC_XL_ODR_104Hz);
    ism330dlc_gy_data_rate_set(&ism330_ctx, ISM330DLC_GY_ODR_104Hz);
    lsm303agr_xl_data_rate_set(&lsm303_acc_ctx, LSM303AGR_XL_ODR_100Hz);
    lsm303agr_mag_data_rate_set(&lsm303_mag_ctx, LSM303AGR_MG_ODR_50Hz);
    lsm303agr_mag_operating_mode_set(&lsm303_mag_ctx, LSM303AGR_CONTINUOUS_MODE);
}

/**
 * @brief FreeRTOS Task to handle sensor interrupts and data fetching
 */
void SENSTaskBody(void *argument)
{
    SensorEvent_t receivedEvent;

    for (;;)
    {
        /* Block indefinitely until an interrupt event is placed in the queue */
        if (xQueueReceive(sensorEventQueue, &receivedEvent, portMAX_DELAY) == pdPASS)
        {
            uint64_t global_time = Translate_To_Global_Time(&receivedEvent);
            /* Process the specific sensor that caused the interrupt */
            switch (receivedEvent.sensor_id)
            {
            case SENSOR_ISM330_INT:
                Read_ISM330_Data(global_time);
                break;

            case SENSOR_LSM303_ACC_INT:

                Read_LSM303_Acc_Data(global_time);
                break;

            case SENSOR_LSM303_MAG_INT:
                Read_LSM303_Mag_Data(global_time);
                break;
            }
        }
    }
}

/**
 * @brief Translates a local timestamp to a global timestamp synchronized with the GNSS timepulse
 * @param event Pointer to the sensor event structure
 * @return Global timestamp in milliseconds
 */
uint64_t Translate_To_Global_Time(SensorEvent_t *event)
{
    // If the baseline was 0, we didn't have a GPS lock yet when this fired
    if (event->pps_global_baseline_ms == 0)
    {
        return 0;
    }

    // Math handles 32-bit wrap-around safely via unsigned integer subtraction.
    uint32_t delta_ticks = event->timestamp_ticks - event->pps_local_baseline_ticks;

    // 150MHz / 15 (prescaler 14+1) = 10MHz -> 10 ticks = 1 us
    uint32_t delta_us = delta_ticks / 10;

    // Return UTC time in microseconds
    return (event->pps_global_baseline_ms * 1000ULL) + delta_us;
}