#pragma once

/**
 * @brief Value controls that ignore the mouse wheel.
 *
 * The develop panel is roughly 3000px tall, so reaching a section means
 * scrolling past dozens of sliders and spin boxes. Qt's default behaviour lets
 * the wheel adjust whichever one the cursor happens to cross, which silently
 * rewrites an adjustment the user never touched.
 *
 * These subclasses ignore wheel events instead of consuming them. Ignoring is
 * what matters: Qt then forwards the event to the enclosing QScrollArea, so the
 * panel scrolls underneath the cursor, which is what the user intended.
 *
 * @note Only sliders and spin boxes are covered. QComboBox keeps its stock
 *       behaviour on purpose until there is a decision about wheel-select.
 */

#include <QDoubleSpinBox>
#include <QSlider>
#include <QSpinBox>
#include <QWheelEvent>

class NoWheelSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelSpinBox final : public QSpinBox {
public:
    using QSpinBox::QSpinBox;

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};

class NoWheelDoubleSpinBox final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};
