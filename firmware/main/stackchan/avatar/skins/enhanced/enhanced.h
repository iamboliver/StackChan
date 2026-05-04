/*
 * SPDX-FileCopyrightText: 2026 Oliver Barwell
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../../avatar/avatar.h"
#include "../../avatar/elements/feature.h"
#include "../default/default.h"
#include <lvgl.h>
#include <smooth_lvgl.hpp>
#include <memory>

namespace stackchan::avatar {

/**
 * @brief Enhanced avatar skin — layered anime-style eyes with iris/pupil/highlight,
 *        curved eyelid, and a coral-coloured arc mouth.
 */
class EnhancedAvatar : public Avatar {
public:
    lv_color_t primaryColor   = lv_color_white();
    lv_color_t secondaryColor = lv_color_hex(0x2C3E50);  // dark slate — pupil, eyelid, text
    lv_color_t bgColor        = lv_color_hex(0xD0E8FF);  // panel + mouth cover

    void init(lv_obj_t* parent, const lv_font_t* font = &lv_font_montserrat_16);
    void setBgColor(lv_color_t color) override;
    uitk::lvgl_cpp::Container* getPanel() const;

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _panel;
};

/**
 * @brief Enhanced eyes — white sclera, teal iris, black pupil, white highlight,
 *        and a circular eyelid that slides down to "open/close" with a curved edge.
 */
class EnhancedEyes : public Feature {
public:
    EnhancedEyes(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor, bool isLeftEye);
    ~EnhancedEyes();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;
    void setSize(int size) override;

private:
    bool _is_left_eye    = false;
    int _eyelid_offset_y = 0;

    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Container> _sclera;
    std::unique_ptr<uitk::lvgl_cpp::Container> _iris;
    std::unique_ptr<uitk::lvgl_cpp::Container> _pupil;
    std::unique_ptr<uitk::lvgl_cpp::Container> _highlight;
    std::unique_ptr<uitk::lvgl_cpp::Container> _eyelid;
};

/**
 * @brief Enhanced mouth — a coral-coloured circle with a sliding cover so only
 *        the bottom arc (smile) or top arc (frown) is visible. Cover height
 *        decreases with weight to animate mouth opening.
 */
class EnhancedMouth : public Feature {
public:
    EnhancedMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor);
    ~EnhancedMouth();

    void setPosition(const uitk::Vector2i& position) override;
    void setWeight(int weight) override;
    void setRotation(int rotation) override;
    void setEmotion(const Emotion& emotion) override;
    void setVisible(bool visible) override;
    void setCoverColor(lv_color_t color);

private:
    std::unique_ptr<uitk::lvgl_cpp::Container> _container;
    std::unique_ptr<uitk::lvgl_cpp::Container> _mouth_base;
    std::unique_ptr<uitk::lvgl_cpp::Container> _mouth_cover;

    bool _is_smile         = true;
    int _cover_base_height = 40;

    void _apply_cover();
};

}  // namespace stackchan::avatar
