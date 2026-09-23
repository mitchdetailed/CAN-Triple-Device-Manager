// The Update Firmware window on a unit that protects Get.
//
// CMD_FW_UPDATE_STATUS has answered only a host that proved Get ever since the
// bootloader arrived, and a device reset forgets every proof. So on a unit with
// a Get password the read that confirms an install was refused after a
// perfectly good update: the window reported the update as failed and skipped
// the backup-restore offer. The same refusal met the window's FIRST read unless
// the session happened to have proven Get already, which left the update
// blocked before it began. MainWindow now collects the Get key before opening
// the window and hands it a re-prove callback; refreshDeviceStatus() calls it
// once when the read fails, then reads again.
//
// This drives the real window against a fake link that refuses the status read
// until the callback has run, and shows it, since the first read runs in
// showEvent. The read after an install is the same refreshDeviceStatus(), so
// the three cases below cover it too:
//   - a callback that proves: the status is shown and was read twice;
//   - no callback, the old behaviour: refused, read once;
//   - a callback that fails: no second read, the refusal is shown.
#include <QApplication>
#include <QLabel>

#include <cstdio>

#include "fake_device_link.h"
#include "../src/protocol/wire_structs.h"
#include "../src/ui/firmware_update_dialog.h"

namespace {

int fails = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            std::printf("  PASS  %s\n", what);                                   \
        } else {                                                                 \
            std::printf("  FAIL  %s\n", what);                                   \
            ++fails;                                                             \
        }                                                                        \
    } while (0)

// What a 1.0.14 unit with a bootloader reports.
QByteArray statusPayload()
{
    ct::FwUpdateStatus s{};
    s.bootloader_version = 2;
    s.running_major = 1;
    s.running_minor = 0;
    s.running_patch = 14;
    s.running_store_version = 20;
    s.staging_capacity = 0x3C000;
    return QByteArray(reinterpret_cast<const char *>(&s), int(sizeof(s)));
}

// A unit whose Get password is set and not yet proven this session: the status
// read is refused with ERR_LOCKED until `proven` is set, which is the device's
// answer after the reset that ends every update.
struct GetProtectedUnit {
    bool proven = false;

    ct::FakeDeviceLink::Reply answer(quint8 cmd, const QByteArray &)
    {
        if (cmd == ct::CMD_FW_UPDATE_STATUS) {
            return proven ? ct::FakeDeviceLink::Reply::ack(statusPayload())
                          : ct::FakeDeviceLink::Reply::nack(ct::ERR_LOCKED);
        }
        // The capacity report is optional; a unit that cannot give one leaves
        // the window's version check standing alone.
        return ct::FakeDeviceLink::Reply::nack(ct::ERR_INVALID_CMD);
    }
};

bool windowSays(const QWidget &w, const QString &text)
{
    for (const QLabel *label : w.findChildren<QLabel *>()) {
        if (label->text().contains(text))
            return true;
    }
    return false;
}

const QString kShown = QStringLiteral("Running firmware");
const QString kRefused = QStringLiteral("password protected");

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("the status read re-proves Get once and reads again\n");
    {
        GetProtectedUnit unit;
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        int reproves = 0;
        ct::FirmwareUpdateDialog window(&link, nullptr, {}, [&]() {
            ++reproves;
            unit.proven = true;
            return true;
        });
        window.show();
        QCoreApplication::processEvents();
        CHECK(reproves == 1, "the re-prove callback ran exactly once");
        CHECK(link.countOf(ct::CMD_FW_UPDATE_STATUS) == 2,
              "the status was read, refused, then read again");
        CHECK(windowSays(window, kShown), "the window shows the running firmware");
        CHECK(!windowSays(window, kRefused), "no refusal is left on screen");
        window.close();
    }

    std::printf("without a callback the refusal stands, as it did before\n");
    {
        GetProtectedUnit unit;
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(link.countOf(ct::CMD_FW_UPDATE_STATUS) == 1, "the status was read once");
        CHECK(windowSays(window, kRefused), "the window says the unit is password protected");
        CHECK(!windowSays(window, kShown), "no firmware version is claimed");
        window.close();
    }

    std::printf("a re-prove that fails is not followed by a second read\n");
    {
        GetProtectedUnit unit;
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        int reproves = 0;
        ct::FirmwareUpdateDialog window(&link, nullptr, {}, [&]() {
            ++reproves;
            return false; // wrong key, or the link went away
        });
        window.show();
        QCoreApplication::processEvents();
        CHECK(reproves == 1, "the re-prove callback ran once");
        CHECK(link.countOf(ct::CMD_FW_UPDATE_STATUS) == 1, "no second read after a failed proof");
        CHECK(windowSays(window, kRefused), "the refusal is shown");
        window.close();
    }

    if (fails == 0)
        std::printf("test_firmware_update_dialog: all checks passed\n");
    else
        std::printf("test_firmware_update_dialog: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
