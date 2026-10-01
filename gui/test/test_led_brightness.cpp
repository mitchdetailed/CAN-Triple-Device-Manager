// Online → LED Brightness…, against a CAN Triple 2.0 in miniature (the unit's
// running and kept settings behind a FakeDeviceLink, answering as its
// serial_proto.c does). The dialog opens at the unit's value; shows the unit
// the first movement at once and follows a drag while it moves, no more often
// than kPreviewIntervalMs, ending where the slider rests; keeps the value on
// OK; puts the original back on Cancel and touches nothing when nothing moved;
// says a refusal inside itself, staying open, and does not repeat it; and stops
// previewing a unit that no longer answers. Plus the session helpers under it:
// a CAN Triple 1.x reads as "no settings", not as an error, and a reply of the
// wrong size or format is refused.

#include <QApplication>
#include <QElapsedTimer>
#include <QList>
#include <QPair>
#include <QAbstractSpinBox>
#include <QLabel>
#include <QPushButton>

#include <cstdio>
#include <cstring>
#include <functional>

#include "../src/protocol/device_session.h"
#include "../src/ui/led_brightness_dialog.h"
#include "fake_device_link.h"

using namespace ct;

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

DeviceSettings settingsAt(int percent)
{
    DeviceSettings s{};
    s.format = DEVICE_SETTINGS_FORMAT;
    s.led_brightness = quint8(percent);
    return s;
}

// One clock for the whole test: when each write reached the unit.
QElapsedTimer g_clock;

// The unit: what it shows, what it keeps, every write it took (and when), how
// many it was asked for, and a code to refuse writes with (0: take them).
// `keepRefused` refuses only the keeping, as a unit whose FRAM failed would:
// applied, not kept. `lost`: it does not answer at all.
struct Unit {
    DeviceSettings running = settingsAt(60);
    DeviceSettings kept = settingsAt(60);
    QList<QPair<int, bool>> writes; // (percent, store)
    QList<qint64> writeTimes;       // g_clock, ms
    int asked = 0;
    quint8 refuse = 0;
    bool keepRefused = false;
    bool lost = false;
    // Run once, after the unit has taken a write and before the host has its
    // reply: the moment a real DeviceLink spends in its own event loop, where
    // the next mouse move or a click on Cancel can land.
    std::function<void()> whileReplying;

    FakeDeviceLink::Reply answer(quint8 cmd, const QByteArray &payload)
    {
        if (cmd == CMD_READ_DEVICE_SETTINGS)
            return FakeDeviceLink::Reply::ack(
                QByteArray(reinterpret_cast<const char *>(&running), int(sizeof(running))));
        if (cmd != CMD_WRITE_DEVICE_SETTINGS)
            return FakeDeviceLink::Reply::nack(ERR_INVALID_CMD);
        ++asked;
        if (lost)
            return FakeDeviceLink::Reply::lost();
        if (refuse != 0)
            return FakeDeviceLink::Reply::nack(refuse);
        if (payload.size() != int(sizeof(DeviceSettings)) + 1)
            return FakeDeviceLink::Reply::nack(ERR_INVALID_LEN);
        DeviceSettings s{};
        std::memcpy(&s, payload.constData(), sizeof(s));
        const bool store = payload.at(int(sizeof(s))) != 0;
        if (s.format != DEVICE_SETTINGS_FORMAT || s.led_brightness < LED_BRIGHTNESS_MIN
            || s.led_brightness > LED_BRIGHTNESS_MAX)
            return FakeDeviceLink::Reply::nack(ERR_OUT_OF_BOUNDS);
        writes.append(qMakePair(int(s.led_brightness), store));
        writeTimes.append(g_clock.elapsed());
        running = s;
        if (store) {
            if (keepRefused)
                return FakeDeviceLink::Reply::nack(ERR_FLASH_WRITE);
            kept = s;
        }
        if (whileReplying) {
            const std::function<void()> f = whileReplying;
            whileReplying = nullptr;
            f();
        }
        return FakeDeviceLink::Reply::ack();
    }
};

void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

void testPreviewKeepAndCancel()
{
    std::printf("the unit is shown the first movement at once, OK keeps, Cancel puts it back\n");
    Unit unit;
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });

    LedBrightnessDialog dlg(&link, unit.running);
    CHECK(dlg.percent() == 60);
    CHECK(unit.writes.isEmpty()); // opening changes nothing

    // Three steps in quick succession: the first shown at once, the next two
    // inside the interval after it, so only where it came to rest follows.
    dlg.setPercent(30);
    CHECK(unit.writes == (QList<QPair<int, bool>>{{30, false}}));
    dlg.setPercent(29);
    dlg.setPercent(28);
    CHECK(unit.writes.size() == 1);
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);
    CHECK(unit.writes == (QList<QPair<int, bool>>{{30, false}, {28, false}}));
    CHECK(unit.running.led_brightness == 28 && unit.kept.led_brightness == 60);
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);
    CHECK(unit.writes.size() == 2); // at rest: nothing more

    dlg.accept();
    CHECK(dlg.result() == QDialog::Accepted);
    CHECK(unit.writes.size() == 3 && unit.writes.last() == qMakePair(28, true));
    CHECK(unit.kept.led_brightness == 28);

    // Cancel after a preview: the unit goes back to what it was showing.
    LedBrightnessDialog again(&link, unit.running);
    again.setPercent(90);
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);
    CHECK(unit.running.led_brightness == 90);
    again.reject();
    CHECK(again.result() == QDialog::Rejected);
    CHECK(unit.writes.last() == qMakePair(28, false));
    CHECK(unit.running.led_brightness == 28 && unit.kept.led_brightness == 28);

    // Cancel with nothing moved: nothing sent.
    const int before = int(unit.writes.size());
    LedBrightnessDialog idle(&link, unit.running);
    idle.reject();
    CHECK(unit.writes.size() == before);

    // The level beside OK: the unit's value when the dialog opens, then the
    // slider's as it moves. It took the place of a Standard (100 %) button,
    // which is gone, and of the number box beside the slider.
    LedBrightnessDialog shown(&link, unit.running);
    QLabel *level = shown.findChild<QLabel *>(QStringLiteral("level"));
    CHECK(level != nullptr);
    if (level) {
        CHECK(level->text() == QStringLiteral("28 %"));
        shown.setPercent(LED_BRIGHTNESS_MAX);
        CHECK(level->text() == QStringLiteral("100 %"));
        shown.setPercent(LED_BRIGHTNESS_MIN);
        CHECK(level->text() == QStringLiteral("5 %"));
    }
    for (QPushButton *b : shown.findChildren<QPushButton *>())
        CHECK(!b->text().startsWith(QStringLiteral("Standard")));
    CHECK(shown.findChildren<QAbstractSpinBox *>().isEmpty());
    shown.accept();
    CHECK(unit.kept.led_brightness == LED_BRIGHTNESS_MIN);
}

void testRefusals()
{
    std::printf("a refusal is said in the dialog, which stays open\n");
    Unit unit;
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });

    unit.refuse = ERR_LOCKED;
    LedBrightnessDialog dlg(&link, unit.running);
    dlg.setPercent(50);
    pump(LedBrightnessDialog::kPreviewIntervalMs * 10);
    CHECK(dlg.statusText().contains(QStringLiteral("Send Configuration password")));
    CHECK(unit.asked == 1); // said once, not asked again every interval
    dlg.accept();
    CHECK(dlg.result() != QDialog::Accepted); // still open
    CHECK(unit.kept.led_brightness == 60);
    unit.refuse = 0;
    dlg.accept();
    CHECK(dlg.result() == QDialog::Accepted && unit.kept.led_brightness == 50);
    CHECK(dlg.statusText().isEmpty());

    // Shown but not kept: said so, and the dialog stays open.
    unit.keepRefused = true;
    LedBrightnessDialog unkept(&link, unit.running);
    unkept.setPercent(20);
    unkept.accept();
    CHECK(unkept.result() != QDialog::Accepted);
    CHECK(unkept.statusText().contains(QStringLiteral("could not keep")));
    CHECK(unit.running.led_brightness == 20 && unit.kept.led_brightness == 50);
    unit.keepRefused = false;
    unkept.reject(); // back to what it showed when this dialog opened
    CHECK(unit.running.led_brightness == 50);
}

void testLiveDrag()
{
    std::printf("a drag: the unit follows it while it moves, no more often than the interval\n");
    Unit unit;
    unit.running = unit.kept = settingsAt(100);
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
    LedBrightnessDialog dlg(&link, unit.running);

    // 100 % down to 20 % a step every 5 ms, the way a pointer drags a slider:
    // 0.4 s of movement.
    const qint64 start = g_clock.elapsed();
    for (int p = 99; p >= 20; --p) {
        dlg.setPercent(p);
        pump(5);
    }
    const qint64 moved = g_clock.elapsed() - start;
    const int during = int(unit.writes.size());
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);

    const int interval = LedBrightnessDialog::kPreviewIntervalMs;
    CHECK(!unit.writes.isEmpty() && unit.writes.first() == qMakePair(99, false)); // at once
    CHECK(unit.writeTimes.first() - start < 5);
    // Following it: at least half the intervals the drag lasted, however
    // loaded the machine running this is, and never more than one an interval.
    CHECK(during >= int(moved / interval) / 2);
    CHECK(during <= int(moved / interval) + 2);
    bool spaced = true;
    for (int i = 1; i < unit.writeTimes.size(); ++i)
        spaced = spaced && unit.writeTimes[i] - unit.writeTimes[i - 1] >= interval - 5;
    CHECK(spaced);
    if (!spaced) {
        for (int i = 0; i < unit.writeTimes.size(); ++i)
            std::printf("    write %d: %d %% at %lld ms\n", i, unit.writes[i].first,
                        static_cast<long long>(unit.writeTimes[i] - start));
    }
    // Every one a preview, falling, and the last where the slider came to rest.
    bool falling = true;
    for (int i = 0; i < unit.writes.size(); ++i)
        falling = falling && !unit.writes[i].second
                  && (i == 0 || unit.writes[i].first < unit.writes[i - 1].first);
    CHECK(falling);
    CHECK(unit.writes.last() == qMakePair(20, false));
    CHECK(unit.running.led_brightness == 20 && unit.kept.led_brightness == 100);
    std::printf("  %d previews during a %lld ms drag, %d after it\n", during,
                static_cast<long long>(moved), int(unit.writes.size()) - during);

    // A single step (an arrow key) is shown at once too.
    const int before = int(unit.writes.size());
    dlg.setPercent(21);
    CHECK(int(unit.writes.size()) == before + 1 && unit.running.led_brightness == 21);
    dlg.accept();
    CHECK(dlg.result() == QDialog::Accepted && unit.kept.led_brightness == 21);
}

void testWhileAPreviewIsOut()
{
    std::printf("a move or Cancel while a preview waits for its reply\n");
    Unit unit;
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });

    // The slider moves again before the first preview's reply is in: nothing
    // is sent inside that send, and the new value follows when the interval is
    // over.
    LedBrightnessDialog dlg(&link, unit.running);
    unit.whileReplying = [&dlg]() { dlg.setPercent(25); };
    dlg.setPercent(30);
    CHECK(unit.writes == (QList<QPair<int, bool>>{{30, false}}));
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);
    CHECK(unit.writes == (QList<QPair<int, bool>>{{30, false}, {25, false}}));
    CHECK(unit.running.led_brightness == 25);
    dlg.accept();
    CHECK(unit.kept.led_brightness == 25);

    // Cancel before a preview's reply is in: the unit goes back all the same,
    // though the dialog had not yet heard that it took the preview.
    LedBrightnessDialog again(&link, unit.running);
    unit.whileReplying = [&again]() { again.reject(); };
    again.setPercent(70);
    CHECK(again.result() == QDialog::Rejected);
    CHECK(unit.writes.last() == qMakePair(25, false));
    CHECK(unit.running.led_brightness == 25 && unit.kept.led_brightness == 25);
}

void testUnanswered()
{
    std::printf("a unit that stops answering: the previews stop, OK still asks\n");
    Unit unit;
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
    LedBrightnessDialog dlg(&link, unit.running);
    unit.lost = true;
    dlg.setPercent(50);
    CHECK(unit.asked == 1);
    CHECK(!dlg.statusText().isEmpty());
    for (int p = 49; p >= 40; --p) {
        dlg.setPercent(p);
        pump(10);
    }
    pump(LedBrightnessDialog::kPreviewIntervalMs * 3);
    CHECK(unit.asked == 1); // not one more
    unit.lost = false;
    dlg.accept();
    CHECK(unit.asked == 2);
    CHECK(dlg.result() == QDialog::Accepted && unit.kept.led_brightness == 40);
}

void testSession()
{
    std::printf("the session helpers: a 1.x has no settings; a bad reply is refused\n");
    Unit unit;
    unit.running = settingsAt(37);
    FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
    device_session::DeviceSettingsState state;
    QString error;
    CHECK(device_session::readDeviceSettings(&link, &state, &error));
    CHECK(state.supported && state.settings.led_brightness == 37);

    FakeDeviceLink old([](quint8, const QByteArray &) {
        return FakeDeviceLink::Reply::nack(ERR_INVALID_CMD);
    });
    CHECK(device_session::readDeviceSettings(&old, &state, &error));
    CHECK(!state.supported && error.isEmpty());

    DeviceSettings out{};
    const DeviceSettings good = settingsAt(40);
    const QByteArray bytes(reinterpret_cast<const char *>(&good), int(sizeof(good)));
    CHECK(device_session::parseDeviceSettings(bytes, &out) && out.led_brightness == 40);
    CHECK(!device_session::parseDeviceSettings(bytes.left(7), &out));
    CHECK(!device_session::parseDeviceSettings(bytes + QByteArray(1, '\0'), &out));
    DeviceSettings future = good;
    future.format = 2;
    CHECK(!device_session::parseDeviceSettings(
        QByteArray(reinterpret_cast<const char *>(&future), int(sizeof(future))), &out));

    quint8 code = 0xFF;
    CHECK(device_session::writeDeviceSettings(&link, settingsAt(45), true, &error, &code));
    CHECK(code == ERR_OK && unit.kept.led_brightness == 45);
    CHECK(!device_session::writeDeviceSettings(&link, settingsAt(4), true, &error, &code));
    CHECK(code == ERR_OUT_OF_BOUNDS);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_clock.start();

    testPreviewKeepAndCancel();
    testRefusals();
    testLiveDrag();
    testWhileAPreviewIsOut();
    testUnanswered();
    testSession();

    if (fails == 0)
        std::printf("test_led_brightness: all checks passed\n");
    else
        std::printf("test_led_brightness: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
