/*
 * SPDX-FileCopyrightText: 2026 Oliver Barwell
 *
 * SPDX-License-Identifier: MIT
 */
#include "enhanced.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// Ear overlay — appears to the right of the face while in listening mode.
// Layout (all relative to panel centre):
//   Container:  40×58 px, aligned at (+120, 0)
//   Outer pinna: 32×50 px oval, coral #D4708A
//   Inner concha: 18×32 px oval, muted #C4607A, offset +2 px right
//   Canal dot:   8×8 px circle, dark (secondaryColor)
//
// Pulse: lv_anim scales the container zoom 256 ↔ 310 (100 % ↔ 121 %)
// every 800 ms, repeating indefinitely while visible.

EnhancedEar::EnhancedEar(lv_obj_t* parent, lv_color_t outerColor,
                          lv_color_t innerColor, lv_color_t canalColor)
{
    _container = std::make_unique<Container>(parent);
    _container->setSize(40, 58);
    _container->align(LV_ALIGN_CENTER, 120, 0);
    _container->setBgOpa(0);
    _container->setBorderWidth(0);
    _container->setRadius(0);
    _container->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _container->setTransformPivot(20, 29);  // pivot at container centre
    _container->setHidden(true);

    _outer = std::make_unique<Container>(_container->get());
    _outer->setRadius(LV_RADIUS_CIRCLE);
    _outer->setSize(32, 50);
    _outer->align(LV_ALIGN_CENTER, 0, 0);
    _outer->setBgColor(outerColor);
    _outer->setBorderWidth(0);
    _outer->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _inner = std::make_unique<Container>(_container->get());
    _inner->setRadius(LV_RADIUS_CIRCLE);
    _inner->setSize(18, 32);
    _inner->align(LV_ALIGN_CENTER, 2, 0);
    _inner->setBgColor(innerColor);
    _inner->setBorderWidth(0);
    _inner->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _canal = std::make_unique<Container>(_container->get());
    _canal->setRadius(LV_RADIUS_CIRCLE);
    _canal->setSize(8, 8);
    _canal->align(LV_ALIGN_CENTER, 2, 0);
    _canal->setBgColor(canalColor);
    _canal->setBorderWidth(0);
    _canal->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
}

EnhancedEar::~EnhancedEar()
{
    if (_container) {
        lv_anim_del(_container->get(), nullptr);
    }
    _canal.reset();
    _inner.reset();
    _outer.reset();
    _container.reset();
}

void EnhancedEar::show(bool visible)
{
    _container->setHidden(!visible);
    if (visible) {
        startPulse();
    } else {
        stopPulse();
    }
}

void EnhancedEar::startPulse()
{
    lv_anim_init(&_anim);
    lv_anim_set_var(&_anim, _container->get());
    lv_anim_set_exec_cb(&_anim, [](void* obj, int32_t v) {
        lv_obj_set_style_transform_zoom(static_cast<lv_obj_t*>(obj), (lv_coord_t)v, 0);
    });
    lv_anim_set_values(&_anim, 256, 310);
    lv_anim_set_duration(&_anim, 800);
    lv_anim_set_playback_duration(&_anim, 800);
    lv_anim_set_repeat_count(&_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&_anim);
}

void EnhancedEar::stopPulse()
{
    if (_container) {
        lv_anim_del(_container->get(), nullptr);
        lv_obj_set_style_transform_zoom(_container->get(), 256, 0);
    }
}
