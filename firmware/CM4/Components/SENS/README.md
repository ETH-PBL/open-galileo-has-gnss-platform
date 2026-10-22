# SENS (Sensors) Component

## Overview
This component manages the two onboard inertial sensors on the Cortex-M4 core. It uses hardware interrupts and hardware FIFOs to efficiently capture data, then converts each reading's local STM32 timer tick to an absolute GPS microsecond timestamp using the GNSS 1PPS sync pair. Formatted packets are pushed to the CM7 shared memory sensor queue.

## Files

| File | Purpose |
|---|---|
| `sens_wrapper.h / .c` | FreeRTOS task, SPI platform layer, sensor configuration, FIFO drain helpers, timestamp translation |
| `ism330dlc_reg.h / .c` | STMicroelectronics C driver for the ISM330DLC IMU (6-DoF: accel + gyro) |
| `lsm303agr_reg.h / .c` | STMicroelectronics C driver for the LSM303AGR eCompass (accel + magnetometer) |

---

## Supported Sensors

| Sensor | Type | Interface | ODR | Full Scale |
|---|---|---|---|---|
| ISM330DLC | 6-DoF IMU (accel + gyro) | SPI2 (4-wire) | 104 Hz | ±2 g / ±250 dps |
| LSM303AGR (XL) | 3-axis accelerometer | SPI2 (3-wire) | 100 Hz | ±2 g, 12-bit HR mode |
| LSM303AGR (Mag) | 3-axis magnetometer | SPI2 (3-wire) | 50 Hz | High resolution |

---

## Architecture

### 1. SPI Platform Layer (`platform_write` / `platform_read`)
A shared SPI2 bus drives all three devices with manual software chip selects (`SPI_CS_ISM`, `SPI_CS_XL`, `SPI_CS_MAG`). The ST sensor drivers are bound to these helpers via `stmdev_ctx_t`.

**3-wire SPI workaround for LSM303AGR:** Both LSM303AGR devices (accelerometer and magnetometer) are wired with MOSI and MISO physically shorted. During register reads, `PB15` (MOSI) is temporarily reconfigured as a high-impedance input to prevent the STM32 from driving the line while the sensor clocks its data out. The pin is restored to `AF_PP` immediately after the receive completes.

The ISM330DLC is wired in standard 4-wire mode and is unaffected.

### 2. Sensor Initialization (`SensTask_Init` / `Sensors_Config`)
`SensTask_Init` creates the FreeRTOS event queue (200 entries), wires the platform I/O callbacks into the ST driver contexts, calls `Sensors_Config`, then does an initial FIFO drain to clear any stale interrupts accumulated during configuration.

`Sensors_Config` follows a strict sequence to prevent stale interrupt storms:
1. Power-down all sensors
2. Hard-reboot internal memory (BOOT + SW_RESET) and wait 100 ms
3. Put LSM303AGR into 3-wire SPI mode before any read-modify-write operations
4. Configure each sensor (BDU, power mode, ODR, FIFO, interrupts)
5. Power up by setting ODRs last

### 3. Interrupt-Driven Data Collection
All three sensors are configured to assert an EXTI line when their FIFO watermark is reached (ISM330DLC) or when new data is ready (LSM303AGR via a threshold-generator interrupt hack for the magnetometer):

| Interrupt Pin | Sensor |
|---|---|
| `PE3` (`SENS_ISM_INT`) | ISM330DLC FIFO threshold (6 words = 1 complete IMU sample) |
| `PE4` (`SENS_LSM_INT`) | LSM303AGR accelerometer FIFO watermark |
| `PE5` (`SENS_LSM_INTM`) | LSM303AGR magnetometer threshold interrupt (used as DRDY proxy) |

When an EXTI fires, the ISR captures the current `TIM2` counter tick and the latest GNSS sync pair (`active_pps_local_ticks`, `active_pps_global_ms`), packages them into a `SensorEvent_t`, and posts it to `sensorEventQueue`.

### 4. FreeRTOS Task (`SENSTaskBody`)
The task blocks on `sensorEventQueue`. When an event arrives, it calls `Translate_To_Global_Time` to convert the tick delta to GPS microseconds, then calls the appropriate FIFO drain helper.

**FIFO drain helpers:**
- `Read_ISM330_Data` — reads 12-byte blocks (3 gyro + 3 accel words per pattern) until the FIFO is empty; each sample is pushed as a `SENS_TYPE_ISM330` payload.
- `Read_LSM303_Acc_Data` — pops all buffered samples from the accelerometer FIFO; timestamps for samples beyond the first are offset by 10 000 µs (10 ms inter-sample period at 100 Hz).
- `Read_LSM303_Mag_Data` — reads one magnetometer sample and clears the interrupt by reading `INT_SOURCE_REG_M`.

Each reading is packaged as a `SensPayload_t` and pushed to the CM7 shared memory sensor queue via `enqueue_sens`.

### 5. Timestamp Translation
```
global_time_us = (active_pps_global_ms × 1000) + (delta_ticks / 10)
```
TIM2 runs at 10 MHz (150 MHz core / prescaler 15), so 10 ticks = 1 µs. If no GPS fix has been received yet (`active_pps_global_ms == 0`), the timestamp is returned as 0 and the CM7 LED task will light the yellow LED.

---

## Data Structures

```c
// Event posted from ISR to the SENSTask queue
typedef struct {
    uint32_t timestamp_ticks;          // TIM2 value at interrupt time (10 MHz)
    uint32_t pps_local_baseline_ticks; // TIM2 value of the last GNSS 1PPS edge
    uint64_t pps_global_baseline_ms;   // GPS time of the last 1PPS edge (Unix ms)
    uint8_t  sensor_id;                // SENSOR_ISM330_INT / LSM303_ACC / LSM303_MAG
} SensorEvent_t;

// Payload pushed to shared memory
typedef struct { uint64_t timestamp; int16_t acceleration[3]; int16_t angular_rate[3]; } ISM330_Data_t;
typedef struct { uint64_t timestamp; int16_t acceleration[3]; }                          LSM303_Acc_Data_t;
typedef struct { uint64_t timestamp; int16_t magnetic[3]; }                               LSM303_Mag_Data_t;
```

---

## Dependencies
- STM32 HAL (SPI, GPIO, EXTI, TIM)
- FreeRTOS (tasks, queues)
- `shared_memory` — sensor queue
