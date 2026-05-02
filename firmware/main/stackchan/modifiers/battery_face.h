/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../avatar/decorators/decorators.h"
#include <smooth_ui_toolkit.hpp>
#include <hal/hal.h>
#include <cstdint>
#include <memory>

namespace stackchan {

class BatteryFaceModifier : public Modifier {
public:
    BatteryFaceModifier()
    {
        _next_check = GetHAL().millis() + 10000;
    }

    void _update(Modifiable& stackchan) override
    {
        if (!stackchan.hasAvatar()) return;

        uint32_t now = GetHAL().millis();
        auto& avatar = stackchan.avatar();

        if (_is_reacting && now >= _restore_at) {
            if (!avatar.isModifyLocked()) {
                avatar.setEmotion(avatar::Emotion::Neutral);
                _is_reacting = false;
            }
        }

        if (now < _next_check) return;
        _next_check = now + 30000;

        if (avatar.isModifyLocked() || _is_reacting) return;

        uint8_t level  = GetHAL().getBatteryLevel();
        bool charging  = GetHAL().isBatteryCharging();

        if (charging) {
            avatar.setEmotion(avatar::Emotion::Happy);
            avatar.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 3000, 500));
            _restore_at  = now + 3000;
            _is_reacting = true;
        } else if (level < 20) {
            avatar.setEmotion(avatar::Emotion::Angry);
            avatar.addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 4000, 700));
            _restore_at  = now + 4000;
            _is_reacting = true;
        } else if (level < 50) {
            avatar.setEmotion(avatar::Emotion::Sad);
            _restore_at  = now + 3000;
            _is_reacting = true;
        }
        // >= 50% and not charging: face stays neutral
    }

private:
    uint32_t _next_check = 0;
    uint32_t _restore_at = 0;
    bool _is_reacting    = false;
};

}  // namespace stackchan
