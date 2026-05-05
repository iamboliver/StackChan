/*
 * SPDX-FileCopyrightText: 2026 Oliver Barwell
 *
 * SPDX-License-Identifier: MIT
 */
#include "enhanced.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

void EnhancedAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _panel = std::make_unique<Container>(parent);
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(320, 240);
    _panel->setRadius(0);
    _panel->setBorderWidth(0);
    _panel->setBgColor(bgColor);
    _panel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _key_elements.leftEye  = std::make_unique<EnhancedEyes>(_panel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<EnhancedEyes>(_panel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<EnhancedMouth>(_panel->get(), primaryColor, bgColor);
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_panel->get(), primaryColor, secondaryColor, font);

    _ear = std::make_unique<EnhancedEar>(_panel->get(),
        lv_color_hex(0xD4708A),  // outer — matches mouth
        lv_color_hex(0xC4607A),  // inner concha
        secondaryColor);          // canal — dark slate
}

void EnhancedAvatar::setBgColor(lv_color_t color)
{
    bgColor = color;
    if (_panel) _panel->setBgColor(color);
    auto* mouth = dynamic_cast<EnhancedMouth*>(_key_elements.mouth.get());
    if (mouth) mouth->setCoverColor(color);
}

void EnhancedAvatar::showListeningEar(bool show)
{
    if (_ear) _ear->show(show);
}

Container* EnhancedAvatar::getPanel() const
{
    if (_panel) {
        return _panel.get();
    }
    return NULL;
}
