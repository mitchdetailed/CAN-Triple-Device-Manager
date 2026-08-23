// Application-wide guard: the mouse wheel must never change a numeric field.
//
// Qt's default is that a spin box under the cursor consumes wheel events and
// steps its value — so scrolling through a dialog silently edits whatever
// happens to be under the pointer, and the user has no reason to look. That is
// a data-corruption path in a configuration tool (a factor, a CAN ID, a table
// site), so the wheel is refused on EVERY QAbstractSpinBox: QSpinBox,
// QDoubleSpinBox, TrimmedDoubleSpinBox, and the grid cell editors alike.
//
// The event is not simply swallowed. It is handed to the nearest enclosing
// scroll area so the surrounding view still scrolls normally — otherwise the
// wheel would go dead over parts of a tall dialog, which is its own annoyance.
// Values are still editable by typing, by the up/down buttons, and by the
// arrow keys once the field has focus.
#pragma once

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>

namespace ct {

class WheelGuard : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Wheel)
            return QObject::eventFilter(watched, event);
        auto *spin = qobject_cast<QAbstractSpinBox *>(watched);
        if (!spin)
            return QObject::eventFilter(watched, event);

        // Re-target the scroll at the enclosing scroll area, if any. The
        // viewport is not a spin box, so this filter passes it straight
        // through — no recursion.
        for (QWidget *p = spin->parentWidget(); p; p = p->parentWidget()) {
            if (auto *area = qobject_cast<QAbstractScrollArea *>(p)) {
                QCoreApplication::sendEvent(area->viewport(), event);
                break;
            }
        }
        return true; // never reaches the spin box
    }
};

} // namespace ct
