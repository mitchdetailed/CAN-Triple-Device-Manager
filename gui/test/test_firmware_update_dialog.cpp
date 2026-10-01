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
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#include "fake_device_link.h"
#include "../src/protocol/wire_structs.h"
#include "../src/ui/firmware_update_dialog.h"
#include "fw_image.h"

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

// What a 1.0.14 unit with a bootloader reports: the 32 bytes, no copies.
QByteArray statusPayload(quint8 bootloader = 2)
{
    ct::FwUpdateStatus s{};
    s.bootloader_version = bootloader;
    s.running_major = 1;
    s.running_minor = 0;
    s.running_patch = 14;
    s.running_store_version = 20;
    s.staging_capacity = 0x3C000;
    return QByteArray(reinterpret_cast<const char *>(&s), int(sizeof(s)));
}

// A version as the 2.0 firmware packs one (test_firmware_v2 holds the packing
// against the firmware's own fw_version_pack).
quint32 pack(quint32 major, quint32 minor, quint32 patch)
{
    return (major << 16) | (minor << 8) | patch;
}

// A CAN Triple 2.0 running 2.0.1 from copy A, with 2.0.0 in B (where a
// rollback would go) and in the factory copy.
ct::FwUpdateStatus2 keptCopies()
{
    ct::FwUpdateStatus2 k{};
    k.running_slot = ct::FW_SLOT_A;
    k.previous_slot = ct::FW_SLOT_B;
    k.slots_valid = 0x7;
    k.slot_version[0] = pack(2, 0, 1);
    k.slot_version[1] = pack(2, 0, 0);
    k.slot_version[2] = pack(2, 0, 0);
    return k;
}

// What a CAN Triple 2.0 on the 2.0 line reports: the 32 bytes, then its copies.
QByteArray v2StatusPayload(quint8 state, quint8 lastResult, const ct::FwUpdateStatus2 &copies)
{
    ct::FwUpdateStatus s{};
    s.bootloader_version = 3;
    s.state = state;
    s.last_result = lastResult;
    s.running_major = 2;
    s.running_minor = 0;
    s.running_patch = 1;
    s.running_store_version = 20;
    s.staging_capacity = 224u * 1024u;
    QByteArray out(reinterpret_cast<const char *>(&s), int(sizeof(s)));
    out.append(reinterpret_cast<const char *>(&copies), int(sizeof(copies)));
    return out;
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

// What a unit answers to CMD_GET_HARDWARE: the given family on its own
// board's silicon, running 1.0.15.
QByteArray hardwarePayload(quint8 family, quint8 format = ct::HARDWARE_REPORT_FORMAT)
{
    ct::HardwareReport r{};
    r.format = format;
    r.family = family;
    r.package = family == ct::HW_FAMILY_CAN_TRIPLE_2 ? ct::HW_PACKAGE_LQFP100
                                                      : ct::HW_PACKAGE_LQFP48;
    r.dev_id = ct::HW_DEV_ID_STM32G47X;
    r.fw_major = 1;
    r.fw_minor = 0;
    r.fw_patch = 15;
    r.bootloader_version = 2;
    if (family == ct::HW_FAMILY_CAN_TRIPLE_2) {
        r.board_rev_major = 2;
        r.board_rev_source = ct::HW_REV_SOURCE_DEFAULT;
    }
    return QByteArray(reinterpret_cast<const char *>(&r), int(sizeof(r)));
}

// A unit that answers the hardware report and the status read, and nothing
// else, with no password in the way.
struct ReportingUnit {
    QByteArray hardware;
    QByteArray status = statusPayload();

    ct::FakeDeviceLink::Reply answer(quint8 cmd, const QByteArray &)
    {
        if (cmd == ct::CMD_GET_HARDWARE)
            return ct::FakeDeviceLink::Reply::ack(hardware);
        if (cmd == ct::CMD_FW_UPDATE_STATUS)
            return ct::FakeDeviceLink::Reply::ack(status);
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

// A valid .ctf for `product` in `dir`: the header, and the CRC the bootloader
// checks, over recognisable filler. It asks for the bootloader its board's
// line stamps (a 2.0 file from before the 2.0 line asked for less, and is
// refused) unless `minBootloader` says otherwise.
QString writeImage(const QTemporaryDir &dir, const QString &name, quint16 product,
                   quint16 minBootloader = 0)
{
    QByteArray img(8192, char(0));
    for (int i = 0; i < img.size(); ++i)
        img[i] = char((i * 7 + 11) & 0xFF);
    FwImageHeader hdr{};
    hdr.magic = FW_IMAGE_MAGIC;
    hdr.header_version = FW_IMAGE_HDR_VERSION;
    hdr.product_id = product;
    hdr.image_size = quint32(img.size());
    hdr.fw_version_major = 1;
    hdr.fw_version_minor = 0;
    hdr.fw_version_patch = 15;
    hdr.flash_store_version = 20; // informational only; flash_store.h would bring protocol.h's
                                  // CMD_* macros, which collide with ct::CMD_*
    hdr.min_bootloader_version =
        minBootloader ? minBootloader : (product == FW_PRODUCT_CAN_TRIPLE_2 ? 3 : 1);
    std::memcpy(img.data() + FW_IMAGE_HEADER_OFFSET, &hdr, sizeof(hdr));
    quint32 crc = fw_crc32_update(FW_CRC32_INIT, img.constData(), FW_IMAGE_CRC_OFFSET);
    crc = fw_crc32_update(crc, img.constData() + FW_IMAGE_CRC_OFFSET + 4,
                          quint32(img.size()) - FW_IMAGE_CRC_OFFSET - 4);
    crc = fw_crc32_final(crc);
    std::memcpy(img.data() + FW_IMAGE_CRC_OFFSET, &crc, sizeof(crc));
    const QString path = QDir(dir.path()).filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(img);
    return path;
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

    std::printf("a CAN Triple says so, and nothing about its board stands in the way\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(link.countOf(ct::CMD_GET_HARDWARE) == 1, "the board was asked for once");
        CHECK(windowSays(window, QStringLiteral("Board: <b>CAN Triple</b>")),
              "the window names the board");
        CHECK(windowSays(window, kShown), "the running firmware is shown");
        CHECK(!windowSays(window, QStringLiteral("does not update that board")),
              "no refusal on account of the board");
        window.close();
    }

    std::printf("a CAN Triple 2.0 is updated with its own firmware, and only that\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2),
                           v2StatusPayload(ct::FW_STATE_IDLE, ct::FW_RESULT_OK, keptCopies())};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("Board: <b>CAN Triple 2.0 rev 2.00</b>")),
              "the window names the board and its revision");
        CHECK(!windowSays(window, QStringLiteral("does not update that board")),
              "the board itself is no reason to refuse");
        QTemporaryDir dir;
        QString err;
        CHECK(window.selectImage(writeImage(dir, QStringLiteral("one.ctf"),
                                            FW_PRODUCT_CAN_TRIPLE),
                                 &err),
              "a CAN Triple (1.x) file loads");
        CHECK(windowSays(window, QStringLiteral("This image is firmware for the CAN Triple, "
                                                "and this unit is a CAN Triple 2.0")),
              "and is refused for this unit, naming both boards");
        CHECK(window.selectImage(writeImage(dir, QStringLiteral("two.ctf"),
                                            FW_PRODUCT_CAN_TRIPLE_2),
                                 &err),
              "a CAN Triple 2.0 file loads");
        CHECK(windowSays(window, QStringLiteral("For: <b>CAN Triple 2.0</b>")),
              "the window says which board the file is for");
        CHECK(!windowSays(window, QStringLiteral("and this unit is a")),
              "and nothing stands between that file and this unit");
        CHECK(!windowSays(window, QStringLiteral("needs bootloader version")),
              "not even its bootloader");
        window.close();
    }

    std::printf("a CAN Triple 2.0 shows the firmware it keeps\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2),
                           v2StatusPayload(ct::FW_STATE_IDLE, ct::FW_RESULT_OK, keptCopies())};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("Firmware kept on the unit: A 2.0.1 (running), "
                                                "B 2.0.0 (previous), factory 2.0.0")),
              "each copy, which one runs and which one a rollback goes to");
        CHECK(!windowSays(window, QStringLiteral("on trial")), "nothing on trial");
        CHECK(!windowSays(window, QStringLiteral("Last install attempt failed")),
              "and nothing failed");
        window.close();
    }

    std::printf("a rollback is reported as one, naming the firmware it gave up on\n");
    {
        ct::FwUpdateStatus2 copies = keptCopies();
        copies.failed_version = pack(2, 0, 2);
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2),
                           v2StatusPayload(ct::FW_STATE_IDLE, ct::FW_RESULT_ROLLED_BACK, copies)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("Firmware 2.0.2 did not start properly")),
              "the version that did not start");
        CHECK(!windowSays(window, QStringLiteral("Last install attempt failed")),
              "not worded as a failed install");
        window.close();
    }

    std::printf("a repaired image, and an image on trial, say so\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2),
                           v2StatusPayload(ct::FW_STATE_IDLE, ct::FW_RESULT_RECOVERED,
                                           keptCopies())};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("found damaged at start-up")),
              "a repair is reported");
        window.close();
    }
    {
        ct::FwUpdateStatus2 copies = keptCopies();
        copies.trial_boots = 1;
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2),
                           v2StatusPayload(ct::FW_STATE_TRIAL, ct::FW_RESULT_OK, copies)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("new and on trial")), "a trial is reported");
        CHECK(link.countOf(ct::CMD_FW_CONFIRM) == 0,
              "and opening the window does not end it: only an update it ran does");
        window.close();
    }

    std::printf("a CAN Triple (1.x) has no copies to show\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, kShown), "its status is shown");
        CHECK(!windowSays(window, QStringLiteral("Firmware kept on the unit")), "without copies");
        window.close();
    }

    std::printf("a 2.0 still on the provisional bootloader cannot take a 2.0-line file\n");
    {
        // Its firmware answers the 32 bytes, bootloader 2: board 1 today.
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE_2), statusPayload(2)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        QTemporaryDir dir;
        QString err;
        CHECK(window.selectImage(writeImage(dir, QStringLiteral("two.ctf"),
                                            FW_PRODUCT_CAN_TRIPLE_2),
                                 &err),
              "the file loads");
        CHECK(windowSays(window, QStringLiteral("needs bootloader version 3 but the device has "
                                                "version 2")),
              "and the unit's bootloader is named as the obstacle");
        err.clear();
        CHECK(!window.selectImage(writeImage(dir, QStringLiteral("early.ctf"),
                                             FW_PRODUCT_CAN_TRIPLE_2, 2),
                                  &err),
              "a file from before the 2.0 line does not load at all");
        CHECK(err.contains(QStringLiteral("early CAN Triple 2.0 firmware")),
              "and says what it is");
        window.close();
    }

    std::printf("a CAN Triple 2.0 file is refused for a CAN Triple (1.x)\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        QTemporaryDir dir;
        QString err;
        CHECK(window.selectImage(writeImage(dir, QStringLiteral("two.ctf"),
                                            FW_PRODUCT_CAN_TRIPLE_2),
                                 &err),
              "the file loads");
        CHECK(windowSays(window, QStringLiteral("This image is firmware for the CAN Triple 2.0, "
                                                "and this unit is a CAN Triple.")),
              "and is refused for this unit, naming both boards");
        window.close();
    }

    std::printf("a board this Manager does not know is refused by name\n");
    {
        ReportingUnit unit{hardwarePayload(9)};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, QStringLiteral("This unit is a board family 9")),
              "the refusal names the board by its number");
        CHECK(windowSays(window, QStringLiteral("update the Device Manager")),
              "and says what to do about it");
        window.close();
    }

    std::printf("a board report this Manager cannot read stops the update\n");
    {
        ReportingUnit unit{hardwarePayload(ct::HW_FAMILY_CAN_TRIPLE,
                                           quint8(ct::HARDWARE_REPORT_FORMAT + 1))};
        ct::FakeDeviceLink link([&unit](quint8 cmd, const QByteArray &p) {
            return unit.answer(cmd, p);
        });
        ct::FirmwareUpdateDialog window(&link);
        window.show();
        QCoreApplication::processEvents();
        CHECK(windowSays(window, kShown), "the status itself was read");
        CHECK(windowSays(window, QStringLiteral("Which board this unit is could not be read")),
              "the window says the board is unknown, and why");
        window.close();
    }

    if (fails == 0)
        std::printf("test_firmware_update_dialog: all checks passed\n");
    else
        std::printf("test_firmware_update_dialog: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
