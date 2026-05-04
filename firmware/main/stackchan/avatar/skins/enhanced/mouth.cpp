/*
 * SPDX-FileCopyrightText: 2026 Oliver Barwell
 *
 * SPDX-License-Identifier: MIT
 */
#include "enhanced.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// Container (clip boundary): 70×70
// Mouth base (coral circle):  70×70, centred
// Mouth cover (black rect):   70×cover_h, aligns to top (smile) or bottom (frown)
//   Sliding cover reveals only the bottom arc (smile) or top arc (frown) of the coral circle.
//   Container clips excess, producing a clean curved mouth shape.

static constexpr int kMouthContainerSize = 70;
static constexpr int kMouthBaseX         = 0;
static constexpr int kMouthBaseY         = 26;

EnhancedMouth::EnhancedMouth(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor)
{
    _container = std::make_unique<Container>(parent);
    _container->setRadius(0);
    _container->setAlign(LV_ALIGN_CENTER);
    _container->setBorderWidth(0);
    _container->setBgOpa(0);
    _container->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _container->setPadding(0, 0, 0, 0);
    _container->setSize(kMouthContainerSize, kMouthContainerSize);

    _mouth_base = std::make_unique<Container>(_container->get());
    _mouth_base->setRadius(LV_RADIUS_CIRCLE);
    _mouth_base->setSize(kMouthContainerSize, kMouthContainerSize);
    _mouth_base->align(LV_ALIGN_CENTER, 0, 0);
    _mouth_base->setBorderWidth(0);
    _mouth_base->setBgColor(lv_color_hex(0xD4708A));
    _mouth_base->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _mouth_cover = std::make_unique<Container>(_container->get());
    _mouth_cover->setRadius(0);
    _mouth_cover->setSize(kMouthContainerSize, _cover_base_height);
    _mouth_cover->setBorderWidth(0);
    _mouth_cover->setBgColor(secondaryColor);
    _mouth_cover->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    setPosition(_position);
    setWeight(0);
    setRotation(0);
    setEmotion(Emotion::Neutral);
}

EnhancedMouth::~EnhancedMouth()
{
    _mouth_cover.reset();
    _mouth_base.reset();
    _container.reset();
}

void EnhancedMouth::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    auto pos_x = kMouthBaseX + map_range(_position.x, -100, 100, -16, 16);
    auto pos_y = kMouthBaseY + map_range(_position.y, -100, 100, -16, 16);

    _container->setPos(pos_x, pos_y);
}

void EnhancedMouth::setWeight(int weight)
{
    Feature::setWeight(weight);
    _apply_cover();
}

void EnhancedMouth::setRotation(int rotation)
{
    Element::setRotation(rotation);
    _container->setTransformPivot(kMouthContainerSize / 2, kMouthContainerSize / 2);
    _container->setRotation(rotation);
}

void EnhancedMouth::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }

    switch (emotion) {
        case Emotion::Neutral: _is_smile = true;  _cover_base_height = 40; break;
        case Emotion::Happy:   _is_smile = true;  _cover_base_height = 30; break;
        case Emotion::Angry:   _is_smile = false; _cover_base_height = 42; break;
        case Emotion::Sad:     _is_smile = false; _cover_base_height = 35; break;
        case Emotion::Doubt:   _is_smile = true;  _cover_base_height = 48; break;
        case Emotion::Sleepy:  _is_smile = true;  _cover_base_height = 50; break;
        default: break;
    }
    _apply_cover();
}

void EnhancedMouth::setCoverColor(lv_color_t color)
{
    if (_mouth_cover) _mouth_cover->setBgColor(color);
}

void EnhancedMouth::setVisible(bool visible)
{
    Element::setVisible(visible);
    _container->setHidden(!visible);
}

void EnhancedMouth::_apply_cover()
{
    int cover_h = map_range(_weight, 0, 100, _cover_base_height, _cover_base_height - 18);
    if (cover_h < 0) cover_h = 0;
    if (cover_h > kMouthContainerSize) cover_h = kMouthContainerSize;

    _mouth_cover->setSize(kMouthContainerSize, cover_h);

    if (_is_smile) {
        _mouth_cover->align(LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        _mouth_cover->align(LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}
