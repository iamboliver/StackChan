/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/bmi270/bmi270.h"
#include "utils/motion_detector/motion_detector.h"
#include <mooncake_log.h>
#include <esp_timer.h>
#include <cmath>
#include <memory>

static const std::string_view _tag = "HAL-IMU";

static std::unique_ptr<BMI270> _bmi270;

static void _imu_task(void* param)
{
    auto motion_detector = std::make_unique<MotionDetector>();
    motion_detector->setShakeThreshold(16.0f);

    while (1) {
        if (_bmi270 && _bmi270->update()) {
            auto& data = _bmi270->getData();
            // mclog::debug("IMU Accel: {:.2f}\t{:.2f}\t{:.2f}", data.accel_x, data.accel_y, data.accel_z);

            motion_detector->update(data.accel_x, data.accel_y, data.accel_z);

            if (motion_detector->isShakeDetected()) {
                mclog::tagInfo(_tag, "Shake Detected!");
                GetHAL().onImuMotionEvent.emit(ImuMotionEvent::Shake);
            }
            if (motion_detector->isPickUpDetected()) {
                mclog::tagInfo(_tag, "Pick Up Detected!");
                GetHAL().onImuMotionEvent.emit(ImuMotionEvent::PickUp);
            }

            // Software double-tap: two diff-magnitude spikes < 500 ms apart
            static constexpr float    kTapThreshold      = 12.0f;
            static constexpr uint32_t kDoubleTapWindowMs = 500;
            static float   _dt_prev_ax = 0, _dt_prev_ay = 0, _dt_prev_az = 0;
            static bool    _dt_armed   = false;
            static int64_t _dt_time_us = 0;

            float dax = data.accel_x - _dt_prev_ax;
            float day = data.accel_y - _dt_prev_ay;
            float daz = data.accel_z - _dt_prev_az;
            _dt_prev_ax = data.accel_x;
            _dt_prev_ay = data.accel_y;
            _dt_prev_az = data.accel_z;
            float diff_mag = sqrtf(dax * dax + day * day + daz * daz);

            if (diff_mag > kTapThreshold) {
                int64_t now_us = esp_timer_get_time();
                if (_dt_armed && (now_us - _dt_time_us) < (int64_t)kDoubleTapWindowMs * 1000) {
                    mclog::tagInfo(_tag, "Double Tap Detected!");
                    GetHAL().onImuMotionEvent.emit(ImuMotionEvent::DoubleTap);
                    _dt_armed = false;
                } else {
                    _dt_armed   = true;
                    _dt_time_us = now_us;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void Hal::imu_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    _bmi270 = std::make_unique<BMI270>(i2c_bus, 0x69);
    if (!_bmi270->begin()) {
        _bmi270.reset();
        mclog::tagError(_tag, "BMI270 init failed");
        return;
    }
    mclog::tagInfo(_tag, "BMI270 init ok");

    xTaskCreateWithCaps(_imu_task, "imu", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM);
}
