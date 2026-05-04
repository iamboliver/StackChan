/*
 * SPDX-FileCopyrightText: 2026 Oliver Barwell
 *
 * SPDX-License-Identifier: MIT
 */
#include "enhanced.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

// Container (clip boundary): 48×48
// Sclera (white circle):     36×36, centred → y ∈ [6, 42]
// Iris (teal):               24×24, centred → y ∈ [12, 36]
// Pupil (black):             12×12, centred → y ∈ [18, 30]
// Highlight (white glint):    6×6,  centred at (+4, -4) offset
// Eyelid (black circle):     48×48, slides from y=-50 (open) to y=-6 (closed)
//   The large circular eyelid creates a curved upper eyelid edge when it
//   partially overlaps the sclera. The container clips it cleanly.

static constexpr int kContainerSize = 48;
static constexpr int kScleraSize    = 36;
static constexpr int kIrisSize      = 24;
static constexpr int kPupilSize     = 12;
static constexpr int kHighlightSize = 6;
static constexpr int kEyelidSize    = 48;

static constexpr int kEyeBaseX      = 70;
static constexpr int kEyeBaseY      = -16;
static constexpr int kEyePosOffset  = 16;

// Eyelid y: weight=0 → -6 (covers sclera), weight=100 → -50 (fully above, clipped)
static constexpr int kEyelidYClosed = -6;
static constexpr int kEyelidYOpen   = -50;

EnhancedEyes::EnhancedEyes(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor,
                           bool isLeftEye)
{
    _is_left_eye = isLeftEye;

    // Clipping container — children that overflow are hidden
    _container = std::make_unique<Container>(parent);
    _container->setRadius(0);
    _container->setAlign(LV_ALIGN_CENTER);
    _container->setBorderWidth(0);
    _container->setBgOpa(0);
    _container->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _container->setPadding(0, 0, 0, 0);
    _container->setTransformPivot(kContainerSize / 2, kContainerSize / 2);
    _container->setSize(kContainerSize, kContainerSize);

    // Sclera — white circle, rendered first (bottom of stack)
    _sclera = std::make_unique<Container>(_container->get());
    _sclera->setRadius(LV_RADIUS_CIRCLE);
    _sclera->setSize(kScleraSize, kScleraSize);
    _sclera->align(LV_ALIGN_CENTER, 0, 0);
    _sclera->setBorderWidth(0);
    _sclera->setBgColor(primaryColor);
    _sclera->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Iris — teal circle on top of sclera
    _iris = std::make_unique<Container>(_container->get());
    _iris->setRadius(LV_RADIUS_CIRCLE);
    _iris->setSize(kIrisSize, kIrisSize);
    _iris->align(LV_ALIGN_CENTER, 0, 0);
    _iris->setBorderWidth(0);
    _iris->setBgColor(lv_color_hex(0x4A90D9));
    _iris->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Pupil — black circle on top of iris
    _pupil = std::make_unique<Container>(_container->get());
    _pupil->setRadius(LV_RADIUS_CIRCLE);
    _pupil->setSize(kPupilSize, kPupilSize);
    _pupil->align(LV_ALIGN_CENTER, 0, 0);
    _pupil->setBorderWidth(0);
    _pupil->setBgColor(secondaryColor);
    _pupil->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Highlight — white glint, offset top-right of pupil
    _highlight = std::make_unique<Container>(_container->get());
    _highlight->setRadius(LV_RADIUS_CIRCLE);
    _highlight->setSize(kHighlightSize, kHighlightSize);
    _highlight->align(LV_ALIGN_CENTER, 4, -4);
    _highlight->setBorderWidth(0);
    _highlight->setBgColor(primaryColor);
    _highlight->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    // Eyelid — large black circle, topmost in z-order, slides to open/close eye
    _eyelid = std::make_unique<Container>(_container->get());
    _eyelid->setRadius(LV_RADIUS_CIRCLE);
    _eyelid->setSize(kEyelidSize, kEyelidSize);
    _eyelid->align(LV_ALIGN_CENTER, 0, 0);
    _eyelid->setBorderWidth(0);
    _eyelid->setBgColor(secondaryColor);
    _eyelid->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    setSize(0);
    setWeight(100);
    setPosition(_position);
    setRotation(0);
}

EnhancedEyes::~EnhancedEyes()
{
    _eyelid.reset();
    _highlight.reset();
    _pupil.reset();
    _iris.reset();
    _sclera.reset();
    _container.reset();
}

void EnhancedEyes::setPosition(const Vector2i& position)
{
    Element::setPosition(position);

    auto pos_x = (_is_left_eye ? -kEyeBaseX : kEyeBaseX)
                 + map_range(_position.x, -100, 100, -kEyePosOffset, kEyePosOffset);
    auto pos_y = kEyeBaseY + map_range(_position.y, -100, 100, -kEyePosOffset, kEyePosOffset);

    _container->setPos(pos_x, pos_y);
    _eyelid->setY(_eyelid_offset_y);
}

void EnhancedEyes::setWeight(int weight)
{
    Feature::setWeight(weight);

    _eyelid_offset_y = map_range(_weight, 0, 100, kEyelidYClosed, kEyelidYOpen);
    _eyelid->setY(_eyelid_offset_y);
}

void EnhancedEyes::setRotation(int rotation)
{
    Element::setRotation(rotation);
    _container->setRotation(rotation);
}

void EnhancedEyes::setEmotion(const Emotion& emotion)
{
    if (getIgnoreEmotion()) {
        return;
    }

    auto apply = [this](int weight, int rotation) {
        setWeight(weight);
        setRotation(_is_left_eye ? rotation : -rotation);
    };

    switch (emotion) {
        case Emotion::Neutral: apply(100,   0); break;
        case Emotion::Happy:   apply( 72, 1550); break;
        case Emotion::Angry:   apply( 65,  450); break;
        case Emotion::Sad:     apply( 65, -400); break;
        case Emotion::Doubt:   apply( 80,    0); break;
        case Emotion::Sleepy:  apply( 25,  -50); break;
        default: break;
    }
}

void EnhancedEyes::setVisible(bool visible)
{
    Element::setVisible(visible);
    _container->setHidden(!visible);
}

void EnhancedEyes::setSize(int size)
{
    Feature::setSize(size);

    // Scale sclera; iris/pupil/highlight scale proportionally
    static constexpr int kScleraMin = 24;
    static constexpr int kScleraMax = 44;

    int sclera_size = map_range(_size, -100, 100, kScleraMin, kScleraMax);
    int iris_size   = sclera_size * kIrisSize / kScleraSize;
    int pupil_size  = sclera_size * kPupilSize / kScleraSize;

    _sclera->setSize(sclera_size, sclera_size);
    _iris->setSize(iris_size, iris_size);
    _pupil->setSize(pupil_size, pupil_size);

    // Re-apply weight so eyelid tracks the new sclera bottom
    setWeight(getWeight());
}
