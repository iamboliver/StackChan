/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include <hal/hal.h>
#include <cstdint>

namespace stackchan {

/**
 * @brief
 *
 */
class ImuEventModifier : public Modifier {
public:
    ImuEventModifier(uint32_t reactionDurationMs = 4000) : _reaction_duration_ms(reactionDurationMs)
    {
        _signal_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
            if (event == ImuMotionEvent::Shake) {
                _event_shake = true;
            } else if (event == ImuMotionEvent::PickUp) {
                _event_pickup = true;
            } else if (event == ImuMotionEvent::DoubleTap) {
                _event_double_tap = true;
            }
        });
    }

    ~ImuEventModifier()
    {
        GetHAL().onImuMotionEvent.disconnect(_signal_connection);
    }

    void _update(Modifiable& stackchan) override
    {
        uint32_t now = GetHAL().millis();

        // Double-tap: show listening ear for 8 seconds
        if (_event_double_tap) {
            _event_double_tap = false;
            stackchan.avatar().showListeningEar(true);
            _is_ear_shown = true;
            _ear_hide_at  = now + 8000;
        }
        if (_is_ear_shown && now >= _ear_hide_at) {
            stackchan.avatar().showListeningEar(false);
            _is_ear_shown = false;
        }

        // Pick-up: show curious expression briefly (shake takes priority if both fire at once)
        if (_event_pickup && !_is_reacting) {
            _event_pickup = false;
            handle_pickup(stackchan, now);
        }

        if (_is_pickup_reacting && now >= _pickup_restore_at) {
            restore_pickup(stackchan);
        }

        // 收到晃动事件
        if (_event_shake) {
            _event_shake = false;
            if (_is_pickup_reacting) {
                restore_pickup(stackchan);
            }
            handle_shake_start(stackchan, now);
        }

        // 如果处于晃动反应状态
        if (_is_reacting) {
            if (now >= _next_toggle_tick) {
                _next_toggle_tick = now + 600;

                _toggle_phase      = !_toggle_phase;
                int mouth_rotation = _toggle_phase ? -25 : 25;

                auto& avatar = stackchan.avatar();
                avatar.mouth().setRotation(mouth_rotation);
                avatar.mouth().setWeight(65);
            }

            //  Lock motion modify and move home
            auto& motion = stackchan.motion();
            if (!motion.isModifyLocked()) {
                motion.setModifyLock(true);
                motion.goHome(300);
            }

            // 检查是否结束反应
            if (now >= _restore_at) {
                restore_state(stackchan);
            }
        }
    }

private:
    void handle_shake_start(Modifiable& stackchan, uint32_t now)
    {
        if (!_is_reacting) {
            // 首次触发时，记录状态以便恢复
            _is_reacting = true;

            auto& avatar = stackchan.avatar();

            avatar.setModifyLock(true);
            avatar.leftEye().setVisible(false);
            avatar.rightEye().setVisible(false);

            stackchan.avatar().removeDecorator(_dizzy_decorator_id);
            stackchan.avatar().removeDecorator(_shy_decorator_id);
            _dizzy_decorator_id =
                stackchan.avatar().addDecorator(std::make_unique<avatar::DizzyDecorator>(lv_screen_active(), 0, 300));
            _shy_decorator_id =
                stackchan.avatar().addDecorator(std::make_unique<avatar::ShyDecorator>(lv_screen_active(), 0));
        }

        // 刷新恢复时间和切换时间
        _restore_at = now + _reaction_duration_ms;
        if (_next_toggle_tick <= now) {
            _next_toggle_tick = now;  // 立即触发第一次嘴巴动作
        }
    }

    void restore_state(Modifiable& stackchan)
    {
        if (!_is_reacting) {
            return;
        }

        auto& avatar = stackchan.avatar();
        avatar.setModifyLock(false);
        avatar.leftEye().setVisible(true);
        avatar.rightEye().setVisible(true);
        avatar.mouth().setWeight(0);
        avatar.mouth().setRotation(0);

        stackchan.avatar().removeDecorator(_dizzy_decorator_id);
        stackchan.avatar().removeDecorator(_shy_decorator_id);
        _dizzy_decorator_id = -1;
        _shy_decorator_id   = -1;

        auto& motion = stackchan.motion();
        motion.setModifyLock(false);

        _is_reacting = false;
    }

    void handle_pickup(Modifiable& stackchan, uint32_t now)
    {
        auto& avatar = stackchan.avatar();
        if (avatar.isModifyLocked()) return;

        avatar.setEmotion(avatar::Emotion::Doubt);
        stackchan.motion().goHome(200);

        _is_pickup_reacting = true;
        _pickup_restore_at  = now + 2000;
    }

    void restore_pickup(Modifiable& stackchan)
    {
        if (!_is_pickup_reacting) return;
        _is_pickup_reacting = false;

        if (!stackchan.avatar().isModifyLocked()) {
            stackchan.avatar().setEmotion(avatar::Emotion::Neutral);
        }
    }

    // 信号相关
    int _signal_connection;
    volatile bool _event_shake      = false;
    volatile bool _event_pickup     = false;
    volatile bool _event_double_tap = false;

    // Listening ear state
    bool     _is_ear_shown = false;
    uint32_t _ear_hide_at  = 0;

    // 状态控制
    bool _is_reacting          = false;
    bool _toggle_phase         = false;
    uint32_t _restore_at       = 0;
    uint32_t _next_toggle_tick = 0;
    uint32_t _reaction_duration_ms;

    int _dizzy_decorator_id = -1;
    int _shy_decorator_id   = -1;

    // Pick-up state
    bool _is_pickup_reacting    = false;
    uint32_t _pickup_restore_at = 0;
};

}  // namespace stackchan
