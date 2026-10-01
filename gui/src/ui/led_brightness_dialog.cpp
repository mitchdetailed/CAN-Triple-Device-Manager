#include "led_brightness_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include "../protocol/device_link.h"
#include "../protocol/device_session.h"
#include "theme.h"

namespace ct {

LedBrightnessDialog::LedBrightnessDialog(DeviceLink *link, const DeviceSettings &current,
                                         QWidget *parent)
    : QDialog(parent)
    , m_link(link)
    , m_original(current)
    , m_shown(current.led_brightness)
    , m_asked(current.led_brightness)
{
    setWindowTitle(tr("LED Brightness"));
    // Room for the slider to be set a percent at a time by hand: 95 steps over
    // about 380 pixels.
    setMinimumWidth(420);
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(
        tr("The brightness of every LED on the unit, as a percentage of the standard "
           "brightness. Bright and dim change together, so every light still reads the same "
           "way. The unit shows the setting as you move the slider, and keeps it when you "
           "press OK."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setRange(LED_BRIGHTNESS_MIN, LED_BRIGHTNESS_MAX);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(5);
    m_slider->setTickPosition(QSlider::TicksBelow);
    m_slider->setTickInterval(5);
    layout->addWidget(m_slider);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(colorRule(errorColor(palette())));
    m_status->hide();
    layout->addWidget(m_status);

    // The level the slider is at, bottom left beside OK and Cancel: where the
    // eye is when deciding.
    m_level = new QLabel(this);
    m_level->setObjectName(QStringLiteral("level"));
    QFont levelFont = m_level->font();
    levelFont.setBold(true);
    m_level->setFont(levelFont);
    m_level->setMinimumWidth(QFontMetrics(levelFont).horizontalAdvance(tr("%1 %").arg(100)));
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &LedBrightnessDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &LedBrightnessDialog::reject);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_level);
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    const int start =
        qBound(LED_BRIGHTNESS_MIN, int(current.led_brightness), LED_BRIGHTNESS_MAX);
    m_slider->setValue(start);
    m_level->setText(tr("%1 %").arg(start));
    connect(m_slider, &QSlider::valueChanged, this,
            [this](int value) { m_level->setText(tr("%1 %").arg(value)); });

    // Live, and paced: see kPreviewIntervalMs. It used to wait for the slider
    // to rest for 120 ms, so the lights did not move during a drag at all.
    // Precise: a coarse timer on Windows can fire a whole clock tick (15.6 ms)
    // early, and the pacing is the point of it.
    m_previewTimer.setSingleShot(true);
    m_previewTimer.setTimerType(Qt::PreciseTimer);
    m_previewTimer.setInterval(kPreviewIntervalMs);
    connect(m_slider, &QSlider::valueChanged, this, &LedBrightnessDialog::moved);
    connect(&m_previewTimer, &QTimer::timeout, this, &LedBrightnessDialog::intervalOver);
}

void LedBrightnessDialog::moved()
{
    // A preview went out less than an interval ago, or is out now: this value
    // waits for the timer, which the end of that preview starts. The link waits
    // for each reply in an event loop of its own, so the slider can move again
    // before a send returns; a second send begun inside the first would leave
    // this dialog wrong about what the unit is showing.
    if (m_sending || m_previewTimer.isActive())
        return;
    preview();
}

void LedBrightnessDialog::intervalOver()
{
    if (m_sending) {
        m_previewTimer.start(); // not inside a send: try again an interval on
        return;
    }
    if (percent() == m_asked)
        return; // nothing new since the last preview: the slider is at rest
    preview();
}

int LedBrightnessDialog::percent() const
{
    return m_slider->value();
}

void LedBrightnessDialog::setPercent(int percent)
{
    m_slider->setValue(percent);
}

QString LedBrightnessDialog::statusText() const
{
    // isHidden, not isVisible: the label's own state, whether or not the
    // dialog itself is on screen.
    return m_status->isHidden() ? QString() : m_status->text();
}

void LedBrightnessDialog::preview()
{
    // A unit that stopped answering holds each request for its retries (1.5 s):
    // following the slider then would freeze the window for as long as it
    // moved. The previews stop; OK and Cancel still ask.
    if (m_unanswered)
        return;
    if (percent() == m_shown && percent() == m_asked)
        return;
    m_asked = percent();
    m_sending = true;
    (void)send(m_asked, /*store=*/false);
    m_sending = false;
    m_previewTimer.start(); // the next no sooner than an interval from now
}

bool LedBrightnessDialog::send(int percent, bool store)
{
    DeviceSettings s = m_original; // every other setting as the unit had it
    s.led_brightness = quint8(percent);
    QString error;
    quint8 code = 0;
    if (device_session::writeDeviceSettings(m_link, s, store, &error, &code)) {
        m_shown = percent;
        m_status->clear();
        m_status->hide();
        return true;
    }
    if (code == 0)
        m_unanswered = true; // no reply at all, not a refusal: see preview()
    QString text;
    if (code == ERR_LOCKED) {
        text = tr("The unit wants its Send Configuration password before its settings change.");
    } else if (code == ERR_FLASH_WRITE) {
        m_shown = percent; // it shows the value; it only could not keep it
        text = tr("The unit shows this brightness but could not keep it: it goes back to the "
                  "one it kept when it restarts.");
    } else {
        text = tr("The unit did not take the setting: %1")
                   .arg(error.isEmpty() ? DeviceLink::errorCodeText(code) : error);
    }
    m_status->setText(text);
    m_status->show();
    return false;
}

void LedBrightnessDialog::accept()
{
    m_previewTimer.stop();
    if (send(percent(), /*store=*/true))
        QDialog::accept();
}

void LedBrightnessDialog::reject()
{
    m_previewTimer.stop();
    // Back to what the unit was showing when the dialog opened. A preview only,
    // like the ones it undoes: what the unit keeps was never touched. Asked
    // too, not only shown: Cancel can come while a preview is still out, and
    // the unit takes that preview after this reads m_shown.
    if (m_shown != m_original.led_brightness || m_asked != m_original.led_brightness)
        (void)send(m_original.led_brightness, /*store=*/false);
    QDialog::reject();
}

} // namespace ct
