#include "eyes/imu_service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

namespace eyes {
namespace {

constexpr char kTag[] = "lilguy_imu";
constexpr int kCalibrationSamples = 180;
constexpr float kAccelStdDevLimit = 0.22F;
constexpr float kGyroStdDevLimit = 0.08F;
constexpr float kGyroMeanMagnitudeLimit = 0.25F;
constexpr float kMinGravityMagnitude = 8.0F;
constexpr float kMaxGravityMagnitude = 11.5F;

enum SampleAxis : std::size_t {
    accel_x,
    accel_y,
    accel_z,
    gyro_x,
    gyro_y,
    gyro_z,
    axis_count,
};

}  // namespace

bool ImuService::start(QueueHandle_t motion_queue, QueueHandle_t calibration_queue,
                       Calibration calibration, bool auto_calibrate)
{
    if (motion_queue == nullptr || calibration_queue == nullptr) {
        return false;
    }
    motion_queue_ = motion_queue;
    calibration_queue_ = calibration_queue;
    calibration_ = calibration;
    calibration_requested_.store(auto_calibrate && !calibration.valid);
    const BaseType_t result = xTaskCreatePinnedToCore(task_entry, "lilguy_imu", 5120, this, 4, nullptr, 1);
    return result == pdPASS;
}

void ImuService::task_entry(void *argument)
{
    static_cast<ImuService *>(argument)->run();
}

void ImuService::run()
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    qmi8658_dev_t device{};
    esp_err_t result = qmi8658_init(&device, bus, QMI8658_ADDRESS_HIGH);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "QMI8658 init failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    const auto configure = [&](esp_err_t config_result, const char *operation) {
        if (config_result == ESP_OK) {
            return true;
        }
        ESP_LOGE(kTag, "%s failed: %s", operation, esp_err_to_name(config_result));
        return false;
    };
    if (!configure(qmi8658_set_accel_range(&device, QMI8658_ACCEL_RANGE_8G), "accel range") ||
        !configure(qmi8658_set_accel_odr(&device, QMI8658_ACCEL_ODR_500HZ), "accel ODR") ||
        !configure(qmi8658_set_gyro_range(&device, QMI8658_GYRO_RANGE_512DPS), "gyro range") ||
        !configure(qmi8658_set_gyro_odr(&device, QMI8658_GYRO_ODR_500HZ), "gyro ODR") ||
        // CTRL5 bits 0 and 1 enable the accelerometer and gyroscope low-pass filters.
        !configure(qmi8658_write_register(&device, QMI8658_CTRL5, 0x03), "low-pass filters")) {
        vTaskDelete(nullptr);
        return;
    }
    // These library setters only select output conversion and intentionally return void.
    qmi8658_set_accel_unit_mps2(&device, true);
    qmi8658_set_gyro_unit_rads(&device, true);

    bool calibrating = false;
    int calibration_count = 0;
    std::array<float, axis_count> sums{};
    std::array<float, axis_count> sums_squared{};

    while (true) {
        if (calibration_requested_.exchange(false)) {
            calibrating = true;
            calibration_count = 0;
            sums.fill(0.0F);
            sums_squared.fill(0.0F);
            ESP_LOGI(kTag, "Hold the device still: IMU neutral calibration started");
        }

        bool ready = false;
        result = qmi8658_is_data_ready(&device, &ready);
        if (result != ESP_OK || !ready) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        qmi8658_data_t data{};
        result = qmi8658_read_sensor_data(&device, &data);
        if (result != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (calibrating) {
            const std::array<float, axis_count> values{
                data.accelX, data.accelY, data.accelZ, data.gyroX, data.gyroY, data.gyroZ,
            };
            for (std::size_t axis = 0; axis < values.size(); ++axis) {
                sums[axis] += values[axis];
                sums_squared[axis] += values[axis] * values[axis];
            }
            ++calibration_count;

            if (calibration_count >= kCalibrationSamples) {
                std::array<float, axis_count> means{};
                std::array<float, axis_count> deviations{};
                const float divisor = static_cast<float>(calibration_count);
                for (std::size_t axis = 0; axis < means.size(); ++axis) {
                    means[axis] = sums[axis] / divisor;
                    const float variance =
                        std::max(0.0F, sums_squared[axis] / divisor - means[axis] * means[axis]);
                    deviations[axis] = std::sqrt(variance);
                }
                const float gravity_magnitude =
                    std::sqrt(means[accel_x] * means[accel_x] +
                              means[accel_y] * means[accel_y] +
                              means[accel_z] * means[accel_z]);
                const float gyro_magnitude =
                    std::sqrt(means[gyro_x] * means[gyro_x] + means[gyro_y] * means[gyro_y] +
                              means[gyro_z] * means[gyro_z]);
                const bool stable_accel = deviations[accel_x] <= kAccelStdDevLimit &&
                                          deviations[accel_y] <= kAccelStdDevLimit &&
                                          deviations[accel_z] <= kAccelStdDevLimit;
                const bool stable_gyro = deviations[gyro_x] <= kGyroStdDevLimit &&
                                         deviations[gyro_y] <= kGyroStdDevLimit &&
                                         deviations[gyro_z] <= kGyroStdDevLimit &&
                                         gyro_magnitude <= kGyroMeanMagnitudeLimit;
                const bool plausible_gravity = gravity_magnitude >= kMinGravityMagnitude &&
                                               gravity_magnitude <= kMaxGravityMagnitude;
                if (stable_accel && stable_gyro && plausible_gravity) {
                    calibration_ = {
                        means[accel_x],
                        means[accel_y],
                        means[accel_z],
                        means[gyro_x],
                        means[gyro_y],
                        means[gyro_z],
                        true,
                    };
                    calibrating = false;
                    xQueueOverwrite(calibration_queue_, &calibration_);
                    ESP_LOGI(kTag,
                             "Calibration complete: neutral=(%.3f %.3f %.3f), gyro=(%.3f %.3f %.3f)",
                             calibration_.neutral_accel_x, calibration_.neutral_accel_y,
                             calibration_.neutral_accel_z, calibration_.gyro_bias_x,
                             calibration_.gyro_bias_y, calibration_.gyro_bias_z);
                } else {
                    calibration_count = 0;
                    sums.fill(0.0F);
                    sums_squared.fill(0.0F);
                    ESP_LOGW(kTag,
                             "Calibration restarted: stillness/gravity check failed (g=%.2f, gyro=%.2f)",
                             gravity_magnitude, gyro_magnitude);
                }
            }
        }

        if (calibrating || !calibration_.valid) {
            vTaskDelay(pdMS_TO_TICKS(8));
            continue;
        }

        const Calibration neutral = calibration_;
        const float raw_accel_magnitude =
            std::sqrt(data.accelX * data.accelX + data.accelY * data.accelY +
                      data.accelZ * data.accelZ);
        const ImuPacket packet{
            {
                data.accelX - neutral.neutral_accel_x,
                data.accelY - neutral.neutral_accel_y,
                data.accelZ - neutral.neutral_accel_z,
                raw_accel_magnitude,
                data.gyroX - neutral.gyro_bias_x,
                data.gyroY - neutral.gyro_bias_y,
                data.gyroZ - neutral.gyro_bias_z,
                static_cast<std::uint32_t>(esp_timer_get_time() / 1000),
            },
        };
        xQueueOverwrite(motion_queue_, &packet);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

}  // namespace eyes
