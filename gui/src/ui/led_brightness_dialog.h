// Online → LED Brightness…: every LED's brightness on a CAN Triple 2.0, from
// 100 % (what the LEDs have always shown) down to 5 %, bright and dim
// together, so every light still reads the same way.
//
// The unit follows the slider while it moves, as a preview it forgets at a
// restart, and keeps the value OK accepts. Cancel puts back what it was showing
// when the dialog opened. A refusal is said inside the dialog, which stays
// open, rather than in a box of its own.
#pragma once

#include <QDialog>
#include <QString>
#include <QTimer>

#include "../protocol/wire_structs.h"

class QLabel;
class QSlider;

namespace ct {

class DeviceLink;

class LedBrightnessDialog : public QDialog
{
    Q_OBJECT

public:
    // `current`: the settings the unit is running, read before opening.
    LedBrightnessDialog(DeviceLink *link, const DeviceSettings &current,
                        QWidget *parent = nullptr);

    int percent() const;
    // As the slider would set it; the unit is shown it at once, or within
    // kPreviewIntervalMs when it was shown another value less than that ago.
    void setPercent(int percent);
    // The last refusal, as shown; empty when the last exchange went through.
    QString statusText() const;

    // The shortest time between two previews. The first movement is shown at
    // once; while the slider keeps moving the unit is shown where it is at
    // most this often (25 a second: the lights follow a drag, and a fast drag
    // costs no more exchanges than a slow one); where it comes to rest is
    // always shown.
    static constexpr int kPreviewIntervalMs = 40;

public slots:
    void accept() override;
    void reject() override;

private:
    void moved();
    void intervalOver();
    void preview();
    bool send(int percent, bool store);

    DeviceLink *m_link;
    DeviceSettings m_original;
    int m_shown; // what the unit is showing now
    int m_asked; // the last value previewed, taken or not: a refusal is not repeated
    bool m_unanswered = false; // an exchange got no reply: no more previews
    bool m_sending = false;    // a preview is out, its reply not yet in
    QSlider *m_slider = nullptr;
    QLabel *m_level = nullptr; // the slider's level, beside OK
    QLabel *m_status = nullptr;
    QTimer m_previewTimer;
};

} // namespace ct
