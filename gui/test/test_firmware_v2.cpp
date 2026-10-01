// The Device Manager against the CAN Triple 2.0 firmware line's update scheme.
//
// The Manager compiles the 1.x firmware tree (its image validator, its wire
// structs), and the 2.0 line changed what an update is: the image goes into a
// copy the unit keeps, starts on trial, and is confirmed or rolled back. So the
// items the Manager mirrors of the 2.0 line are held here against that line's
// own headers, through v2_wire_values.c, which can see them:
//   - CMD_FW_CONFIRM, the trial state, the three new results, the copies and
//     FwUpdateStatus2, byte for byte;
//   - the 2.0's image limit, bootloader version and oldest acceptable image,
//     as FirmwareImage applies them;
//   - the packed versions the status carries, decoded as the firmware packs.
// Then what the Manager does with them: the 52-byte status read, the confirm
// (tolerated by 1.x firmware), and which 2.0 files it takes.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "fake_device_link.h"
#include "../src/protocol/capacity.h"
#include "../src/protocol/device_link.h"
#include "../src/protocol/firmware_image.h"
#include "../src/protocol/firmware_update.h"
#include "../src/protocol/wire_structs.h"
#include "../src/scripting/script_simulator.h"

extern "C" {
extern const unsigned v2_cmd_fw_confirm;
extern const unsigned v2_cmd_fw_update_status;
extern const unsigned v2_err_invalid_cmd;
extern const unsigned v2_state[3];
extern const unsigned v2_result[14];
extern const unsigned v2_slot[4];
extern const unsigned v2_slot_count;
extern const unsigned v2_status_size;
extern const unsigned v2_status2_size;
extern const unsigned v2_status2_offsets[6];
extern const unsigned v2_app_max_size;
extern const unsigned v2_bootloader_version;
extern const unsigned v2_oldest_image_bootloader;
extern const unsigned v2_product;
unsigned v2_version_pack(unsigned major, unsigned minor, unsigned patch);
extern const unsigned char *const v2_capacity_report;
extern const unsigned v2_capacity_report_size;
extern const unsigned v2_capacity_format;
extern const unsigned v2_num_tables;
extern const unsigned v2_capacity_ext_size;
extern const unsigned v2_capacity_ext_offsets[9];
extern const unsigned v2_can_fast_data;
extern const unsigned v2_label_cmds[2];
extern const unsigned v2_label_kinds[3];
extern const unsigned v2_label_bytes;
extern const unsigned v2_capacity_label_at;
extern const unsigned v2_record_sizes[3];
extern const unsigned v2_message_offsets[9];
extern const unsigned v2_signal_offsets[10];
extern const unsigned v2_relay_offsets[5];
extern const unsigned v2_settings_cmds[2];
extern const unsigned v2_settings_format;
extern const unsigned v2_led_brightness_range[2];
extern const unsigned v2_settings_size;
extern const unsigned v2_settings_offsets[3];
extern const unsigned v2_crc8_features;
extern const unsigned v2_crc8_whole_id;
extern const unsigned v2_crc8_data_runs;
extern const unsigned v2_crc8_elem[4];
extern const unsigned v2_crc8_run[3];
extern const unsigned v2_crc8_idall[2];
extern const unsigned v2_crc8_first_of_0x7f;
extern const unsigned v2_crc8_max_elements;
extern const unsigned v2_max_crc8;
extern const unsigned v2_crc8_record_size;
extern const unsigned v2_script_budget;
extern const unsigned v2_script_op_count;
extern const unsigned char *const v2_script_costs;
extern const unsigned v2_retained_no_wear;
extern const unsigned v2_retained_max;
extern const unsigned v2_retained_interval_ms;
extern const unsigned v2_max_counters;
extern const unsigned v2_max_integrators;
}

namespace {

int fails = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            std::printf("  PASS  %s\n", what);                                   \
        } else {                                                                 \
            std::printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__);      \
            ++fails;                                                             \
        }                                                                        \
    } while (0)

// What a CAN Triple 2.0 answers to STATUS: the 1.x fields, then its copies.
QByteArray v2Status(quint8 state, quint8 lastResult, const ct::FwUpdateStatus2 &copies)
{
    ct::FwUpdateStatus s{};
    s.bootloader_version = 3;
    s.state = state;
    s.last_result = lastResult;
    s.running_major = 2;
    s.running_store_version = 20;
    s.staging_capacity = 224u * 1024u;
    QByteArray out(reinterpret_cast<const char *>(&s), int(sizeof(s)));
    out.append(reinterpret_cast<const char *>(&copies), int(sizeof(copies)));
    return out;
}

// A .ctf for `product` asking for bootloader `minBootloader`, of `size` bytes:
// the header and the CRC the bootloader checks, over recognisable filler.
QString writeImage(const QTemporaryDir &dir, const QString &name, quint16 product,
                   quint16 minBootloader, quint32 size = 8192)
{
    QByteArray img(int(size), char(0));
    for (int i = 0; i < img.size(); ++i)
        img[i] = char((i * 7 + 11) & 0xFF);
    FwImageHeader hdr{};
    hdr.magic = FW_IMAGE_MAGIC;
    hdr.header_version = FW_IMAGE_HDR_VERSION;
    hdr.product_id = product;
    hdr.image_size = size;
    hdr.fw_version_major = 2;
    hdr.fw_version_minor = 0;
    hdr.fw_version_patch = 1;
    hdr.flash_store_version = 20;
    hdr.min_bootloader_version = minBootloader;
    std::memcpy(img.data() + FW_IMAGE_HEADER_OFFSET, &hdr, sizeof(hdr));
    quint32 crc = fw_crc32_update(FW_CRC32_INIT, img.constData(), FW_IMAGE_CRC_OFFSET);
    crc = fw_crc32_update(crc, img.constData() + FW_IMAGE_CRC_OFFSET + 4,
                          size - FW_IMAGE_CRC_OFFSET - 4);
    crc = fw_crc32_final(crc);
    std::memcpy(img.data() + FW_IMAGE_CRC_OFFSET, &crc, sizeof(crc));
    const QString path = QDir(dir.path()).filePath(name);
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(img);
    return path;
}

void testMirror()
{
    std::printf("the Manager's copies of the 2.0 update items match the 2.0 headers\n");
    CHECK(ct::CMD_FW_CONFIRM == v2_cmd_fw_confirm, "CMD_FW_CONFIRM");
    CHECK(ct::CMD_FW_UPDATE_STATUS == v2_cmd_fw_update_status, "CMD_FW_UPDATE_STATUS");
    CHECK(ct::ERR_INVALID_CMD == v2_err_invalid_cmd, "ERR_INVALID_CMD");

    CHECK(ct::FW_STATE_IDLE == v2_state[0] && ct::FW_STATE_PENDING == v2_state[1]
              && ct::FW_STATE_TRIAL == v2_state[2],
          "FW_STATE_IDLE, PENDING, TRIAL");
    const unsigned mine[14] = {
        ct::FW_RESULT_NONE,          ct::FW_RESULT_OK,           ct::FW_RESULT_BAD_MAGIC,
        ct::FW_RESULT_WRONG_PRODUCT, ct::FW_RESULT_BAD_SIZE,     ct::FW_RESULT_BAD_CRC,
        ct::FW_RESULT_BL_TOO_OLD,    ct::FW_RESULT_ERASE_FAILED, ct::FW_RESULT_PROGRAM_FAILED,
        ct::FW_RESULT_VERIFY_FAILED, ct::FW_RESULT_GAVE_UP,      ct::FW_RESULT_ROLLED_BACK,
        ct::FW_RESULT_RECOVERED,     ct::FW_RESULT_NO_IMAGE,
    };
    CHECK(std::memcmp(mine, v2_result, sizeof(mine)) == 0, "FW_RESULT_* 0..13");
    CHECK(ct::FW_SLOT_A == v2_slot[0] && ct::FW_SLOT_B == v2_slot[1]
              && ct::FW_SLOT_FACTORY == v2_slot[2] && ct::FW_SLOT_NONE == v2_slot[3]
              && unsigned(ct::FW_SLOT_COUNT) == v2_slot_count,
          "FW_SLOT_A, B, FACTORY, NONE, COUNT");

    CHECK(sizeof(ct::FwUpdateStatus) == v2_status_size, "sizeof(FwUpdateStatus)");
    CHECK(sizeof(ct::FwUpdateStatus2) == v2_status2_size, "sizeof(FwUpdateStatus2)");
    const unsigned offsets[6] = {
        unsigned(offsetof(ct::FwUpdateStatus2, running_slot)),
        unsigned(offsetof(ct::FwUpdateStatus2, previous_slot)),
        unsigned(offsetof(ct::FwUpdateStatus2, trial_boots)),
        unsigned(offsetof(ct::FwUpdateStatus2, slots_valid)),
        unsigned(offsetof(ct::FwUpdateStatus2, failed_version)),
        unsigned(offsetof(ct::FwUpdateStatus2, slot_version)),
    };
    CHECK(std::memcmp(offsets, v2_status2_offsets, sizeof(offsets)) == 0,
          "every FwUpdateStatus2 field at the firmware's offset");

    // Confirm is answered with an ACK, never with data: listing it as a read
    // would leave requestSync waiting for a payload that never comes.
    CHECK(!ct::DeviceLink::isReadResponse(ct::CMD_FW_CONFIRM), "FW_CONFIRM is ACK-only");
    CHECK(ct::DeviceLink::isReadResponse(ct::CMD_FW_UPDATE_STATUS), "STATUS is a read");

    CHECK(ct::FirmwareImage::appSlotSize(ct::BoardFamily::CanTriple2) == v2_app_max_size,
          "the 2.0's image limit");
    CHECK(ct::FirmwareImage::newestBootloader(ct::BoardFamily::CanTriple2)
              == v2_bootloader_version,
          "the 2.0's bootloader version");
    CHECK(ct::FirmwareImage::oldestImageBootloader(ct::BoardFamily::CanTriple2)
              == v2_oldest_image_bootloader,
          "the oldest 2.0 image the unit takes");
    CHECK(unsigned(FW_PRODUCT_CAN_TRIPLE_2) == v2_product, "the 2.0's product id");

    CHECK(ct::FirmwareImage::packedVersionText(v2_version_pack(2, 0, 1))
              == QStringLiteral("2.0.1"),
          "a packed version reads back as the firmware packed it");
    CHECK(ct::FirmwareImage::packedVersionText(v2_version_pack(12, 34, 56))
              == QStringLiteral("12.34.56"),
          "every field in its own place");
    CHECK(ct::FirmwareImage::packedVersionText(0).isEmpty(), "0 is no version");
}

void testStatusRead()
{
    std::printf("the status read takes a 2.0's 52 bytes and a 1.x unit's 32\n");
    ct::FwUpdateStatus2 copies{};
    copies.running_slot = ct::FW_SLOT_B;
    copies.previous_slot = ct::FW_SLOT_A;
    copies.trial_boots = 1;
    copies.slots_valid = 0x7;
    copies.failed_version = v2_version_pack(2, 0, 2);
    copies.slot_version[0] = v2_version_pack(2, 0, 0);
    copies.slot_version[1] = v2_version_pack(2, 0, 1);
    copies.slot_version[2] = v2_version_pack(2, 0, 0);

    QByteArray reply = v2Status(ct::FW_STATE_TRIAL, ct::FW_RESULT_NONE, copies);
    ct::FakeDeviceLink link([&reply](quint8 cmd, const QByteArray &) {
        return cmd == ct::CMD_FW_UPDATE_STATUS ? ct::FakeDeviceLink::Reply::ack(reply)
                                               : ct::FakeDeviceLink::Reply::nack(
                                                     ct::ERR_INVALID_CMD);
    });
    ct::FirmwareUpdater updater(&link);

    ct::FwUpdateStatus st{};
    std::optional<ct::FwUpdateStatus2> got;
    QString error;
    CHECK(reply.size() == 52, "a 2.0 status is 52 bytes");
    CHECK(updater.readStatus(&st, &error, &got), "a 52-byte status is read");
    CHECK(st.state == ct::FW_STATE_TRIAL && st.bootloader_version == 3, "its 1.x fields");
    CHECK(got && got->running_slot == ct::FW_SLOT_B && got->trial_boots == 1
              && got->slot_version[1] == v2_version_pack(2, 0, 1)
              && got->failed_version == v2_version_pack(2, 0, 2),
          "and its copies");
    CHECK(updater.readStatus(&st, &error), "a caller that does not ask for copies still reads");

    reply = reply.left(int(sizeof(ct::FwUpdateStatus)));
    CHECK(updater.readStatus(&st, &error, &got), "a 32-byte status is read");
    CHECK(!got.has_value(), "and says the unit has no copies");

    got = copies;
    reply = reply.left(20);
    CHECK(!updater.readStatus(&st, &error, &got), "a short status is refused");
    CHECK(error.contains(QStringLiteral("Unexpected status reply")), "with the reason");
    CHECK(!got.has_value(), "and leaves no copies from an earlier read");
}

void testConfirm()
{
    std::printf("confirm sends FW_CONFIRM and tolerates firmware without it\n");
    {
        ct::FakeDeviceLink link([](quint8, const QByteArray &) {
            return ct::FakeDeviceLink::Reply::ack();
        });
        ct::FirmwareUpdater updater(&link);
        QString error;
        CHECK(updater.confirm(&error), "an ACK is a confirmation");
        CHECK(link.countOf(ct::CMD_FW_CONFIRM) == 1 && link.sent.first().second.isEmpty(),
              "one FW_CONFIRM, empty");
    }
    {
        ct::FakeDeviceLink link([](quint8, const QByteArray &) {
            return ct::FakeDeviceLink::Reply::nack(ct::ERR_INVALID_CMD);
        });
        ct::FirmwareUpdater updater(&link);
        QString error;
        CHECK(updater.confirm(&error), "ERR_INVALID_CMD (1.x firmware) is nothing to confirm");
    }
    {
        ct::FakeDeviceLink link([](quint8, const QByteArray &) {
            return ct::FakeDeviceLink::Reply::nack(ct::ERR_FLASH_WRITE);
        });
        ct::FirmwareUpdater updater(&link);
        QString error;
        CHECK(!updater.confirm(&error), "a refusal is not a confirmation");
        CHECK(!error.isEmpty(), "and says why");
    }
    {
        ct::FakeDeviceLink link([](quint8, const QByteArray &) {
            return ct::FakeDeviceLink::Reply::lost();
        });
        ct::FirmwareUpdater updater(&link);
        QString error;
        CHECK(!updater.confirm(&error), "no answer is not a confirmation");
    }
}

void testImages()
{
    std::printf("which 2.0 files the Manager takes\n");
    QTemporaryDir dir;
    QString err;

    auto img = ct::FirmwareImage::load(
        writeImage(dir, QStringLiteral("v2.ctf"), FW_PRODUCT_CAN_TRIPLE_2, 3), &err);
    CHECK(img && img->family() == ct::BoardFamily::CanTriple2, "a 2.0.x file loads");

    img = ct::FirmwareImage::load(writeImage(dir, QStringLiteral("big.ctf"),
                                             FW_PRODUCT_CAN_TRIPLE_2, 3, 200u * 1024u),
                                  &err);
    CHECK(img.has_value(), "a 200 KB 2.0 file loads (larger than a CAN Triple's slot)");

    err.clear();
    img = ct::FirmwareImage::load(writeImage(dir, QStringLiteral("huge.ctf"),
                                             FW_PRODUCT_CAN_TRIPLE_2, 3, 232u * 1024u),
                                  &err);
    CHECK(!img && err.contains(QStringLiteral("larger than that board's")),
          "one past the 2.0's limit is refused, naming the board");

    err.clear();
    img = ct::FirmwareImage::load(
        writeImage(dir, QStringLiteral("early.ctf"), FW_PRODUCT_CAN_TRIPLE_2, 2), &err);
    CHECK(!img && err.contains(QStringLiteral("early CAN Triple 2.0 firmware (2.0.1)")),
          "a file from before the 2.0 line is refused by name");

    err.clear();
    img = ct::FirmwareImage::load(
        writeImage(dir, QStringLiteral("future.ctf"), FW_PRODUCT_CAN_TRIPLE_2, 4), &err);
    CHECK(!img && err.contains(QStringLiteral("needs a newer bootloader")),
          "a file for a bootloader no 2.0 has is refused");

    err.clear();
    img = ct::FirmwareImage::load(
        writeImage(dir, QStringLiteral("one.ctf"), FW_PRODUCT_CAN_TRIPLE, 1), &err);
    CHECK(img && img->family() == ct::BoardFamily::CanTriple,
          "a CAN Triple file is held to nothing new");
    err.clear();
    img = ct::FirmwareImage::load(
        writeImage(dir, QStringLiteral("one-bl3.ctf"), FW_PRODUCT_CAN_TRIPLE, 3), &err);
    CHECK(!img && err.contains(QStringLiteral("needs a newer bootloader")),
          "and one asking for the 2.0's bootloader is refused");

    for (quint8 r : {ct::FW_RESULT_ROLLED_BACK, ct::FW_RESULT_RECOVERED, ct::FW_RESULT_NO_IMAGE})
        CHECK(!ct::FirmwareImage::resultText(r).startsWith(QStringLiteral("unknown")),
              "the 2.0's results have words");
}

void testCapacityReport()
{
    std::printf("the 2.0 firmware's capacity report states its retained values, and is read\n");
    CHECK(ct::CAPACITY_REPORT_FORMAT_EXT == v2_capacity_format, "format 2 on the 2.0 line");
    CHECK(sizeof(ct::CapacityExtension) == v2_capacity_ext_size, "sizeof(CapacityExtension)");
    const unsigned offsets[9] = {
        unsigned(offsetof(ct::CapacityExtension, size)),
        unsigned(offsetof(ct::CapacityExtension, retained_values)),
        unsigned(offsetof(ct::CapacityExtension, retained_interval_ms)),
        unsigned(offsetof(ct::CapacityExtension, retained_flags)),
        unsigned(offsetof(ct::CapacityExtension, script_budget)),
        unsigned(offsetof(ct::CapacityExtension, script_op_count)),
        unsigned(offsetof(ct::CapacityExtension, script_op_costs)),
        unsigned(offsetof(ct::CapacityExtension, crc8_features)),
        unsigned(offsetof(ct::CapacityExtension, can_features)),
    };
    CHECK(std::memcmp(offsets, v2_capacity_ext_offsets, sizeof(offsets)) == 0,
          "every CapacityExtension field at the firmware's offset");
    CHECK(ct::CAPACITY_RETAINED_NO_WEAR == v2_retained_no_wear, "CAPACITY_RETAINED_NO_WEAR");
    CHECK(unsigned(ct::MAX_COUNTERS) == v2_max_counters
              && unsigned(ct::MAX_INTEGRATORS) == v2_max_integrators,
          "the counter and integrator tables");
    CHECK(v2_retained_max == v2_max_counters + v2_max_integrators,
          "every counter and integrator is kept");

    const QByteArray bytes(reinterpret_cast<const char *>(v2_capacity_report),
                           int(v2_capacity_report_size));
    ct::DeviceCapacity cap;
    CHECK(ct::parseCapacityReport(bytes, &cap), "the unit's report parses");
    CHECK(cap.tables.size() == int(v2_num_tables), "with every table");
    CHECK(cap.retainedValues == int(v2_retained_max)
              && cap.retainedIntervalMs == int(v2_retained_interval_ms) && cap.retainedNoWear,
          "and the retained figures the firmware states");
    CHECK(cap.retainedLimit() == int(v2_retained_max) && !cap.retainedWearLimited(),
          "which the Manager's limits then use");
    CHECK(ct::retainedIntervalText(cap.retainedEveryMs()) == QStringLiteral("0.1 s"),
          "said as a user reads it");

    ct::DeviceCapacity again;
    CHECK(ct::parseCapacityReport(bytes + QByteArray(64, '\x5A'), &again)
              && again.retainedValues == int(v2_retained_max),
          "code after the block, as in a .ctf, is left alone");
    CHECK(!ct::parseCapacityReport(bytes.left(bytes.size() - 2), &again),
          "an extension cut short is refused");
    QByteArray longer = bytes;
    const int extAt = 4 + 4 * int(v2_num_tables);
    qToLittleEndian<quint16>(quint16(sizeof(ct::CapacityExtension) + 4), longer.data() + extAt);
    longer.append(QByteArray(4, '\x01'));
    CHECK(ct::parseCapacityReport(longer, &again) && again.retainedValues == int(v2_retained_max)
              && again.scriptCosts == cap.scriptCosts,
          "a longer extension from a later firmware is read for what this build knows");
    QByteArray first = bytes.left(extAt + 8);
    qToLittleEndian<quint16>(quint16(8), first.data() + extAt);
    CHECK(ct::parseCapacityReport(first, &again) && again.retainedValues == int(v2_retained_max)
              && again.scriptBudget == 0 && again.scriptCosts.isEmpty(),
          "the first 2.0 builds' 8-byte extension is read, and states no script model");
    QByteArray shortSize = bytes;
    qToLittleEndian<quint16>(quint16(6), shortSize.data() + extAt);
    CHECK(!ct::parseCapacityReport(shortSize, &again), "an extension shorter than 8 is refused");
    QByteArray formatOne = bytes.left(extAt);
    formatOne[0] = char(ct::CAPACITY_REPORT_FORMAT);
    CHECK(ct::parseCapacityReport(formatOne, &again) && again.retainedValues == 0
              && again.retainedLimit() == ct::kRetainedValuesBuiltIn
              && again.retainedWearLimited() && again.scriptCosts.isEmpty(),
          "a format-1 report (the CAN Triple) states nothing: 20, once a minute, in flash");
}

void testCrc8Features()
{
    std::printf("a 2.0 unit states its CRC8 elements and 100 rules, and a 1.x states none\n");
    CHECK(ct::CAPACITY_CRC8_WHOLE_ID == v2_crc8_whole_id
              && ct::CAPACITY_CRC8_DATA_RUNS == v2_crc8_data_runs,
          "CAPACITY_CRC8_*");
    CHECK(ct::CRC8_ELEM_ID == v2_crc8_elem[0] && ct::CRC8_ELEM_DATA == v2_crc8_elem[1]
              && ct::CRC8_ELEM_RAW == v2_crc8_elem[2] && ct::CRC8_ELEM_ID_ALL == v2_crc8_elem[3],
          "CRC8_ELEM_* as the firmware spells them");
    CHECK(ct::CRC8_ELEM_DATA_RUN == v2_crc8_run[0] && ct::CRC8_ELEM_RUN_MASK == v2_crc8_run[1]
              && ct::CRC8_RUN_LAST_MASK == v2_crc8_run[2]
              && (0x7Fu & ct::CRC8_RUN_FIRST_MASK) == v2_crc8_first_of_0x7f,
          "a run's spelling");
    CHECK(ct::CRC8_IDALL_BYTES_MASK == v2_crc8_idall[0]
              && ct::CRC8_IDALL_LSB_FIRST == v2_crc8_idall[1],
          "the whole identifier's spelling");
    CHECK(unsigned(ct::CRC8_MAX_ELEMENTS) == v2_crc8_max_elements
              && sizeof(ct::Crc8Config) == v2_crc8_record_size,
          "the rule record is the same on both sides");

    const QByteArray bytes(reinterpret_cast<const char *>(v2_capacity_report),
                           int(v2_capacity_report_size));
    ct::DeviceCapacity cap;
    CHECK(ct::parseCapacityReport(bytes, &cap), "the unit's report parses");
    CHECK(cap.crc8Features == int(v2_crc8_features) && cap.crc8WholeId() && cap.crc8DataRuns(),
          "with the CRC8 elements the firmware states");
    CHECK(cap.crc8MaxByte() == 63, "and byte positions to 63");
    CHECK(cap.capacityOf(ct::DeviceTable::Crc8) == int(v2_max_crc8) && v2_max_crc8 == 100,
          "100 CRC8 rules, where the 1.x holds 20");
    ct::DeviceCapacity back;
    CHECK(ct::DeviceCapacity::fromJson(cap.toJson(), &back) && back.sameCrc8(cap),
          "a document records the features");
    const ct::DeviceCapacity old = ct::DeviceCapacity::builtIn();
    CHECK(old.crc8Features == 0 && old.crc8MaxByte() == 7 && !cap.sameCrc8(old),
          "which a 1.x target does not share");

    // The builds before the field end the block at the script model (60).
    const int extAt = 4 + 4 * int(v2_num_tables);
    QByteArray sixty = bytes.left(extAt + 60);
    qToLittleEndian<quint16>(quint16(60), sixty.data() + extAt);
    ct::DeviceCapacity earlier;
    CHECK(ct::parseCapacityReport(sixty, &earlier) && earlier.crc8Features == 0
              && earlier.scriptCosts == cap.scriptCosts,
          "a 60-byte extension states the script model and no CRC8 elements");
}

void testDeviceSettings()
{
    std::printf("the unit's own settings: commands, format, brightness range, record\n");
    CHECK(ct::CMD_READ_DEVICE_SETTINGS == v2_settings_cmds[0]
              && ct::CMD_WRITE_DEVICE_SETTINGS == v2_settings_cmds[1],
          "CMD_READ_DEVICE_SETTINGS / CMD_WRITE_DEVICE_SETTINGS");
    CHECK(ct::DEVICE_SETTINGS_FORMAT == v2_settings_format, "DEVICE_SETTINGS_FORMAT");
    CHECK(unsigned(ct::LED_BRIGHTNESS_MIN) == v2_led_brightness_range[0]
              && unsigned(ct::LED_BRIGHTNESS_MAX) == v2_led_brightness_range[1],
          "5 % to 100 %");
    CHECK(unsigned(sizeof(ct::DeviceSettings)) == v2_settings_size
              && unsigned(offsetof(ct::DeviceSettings, format)) == v2_settings_offsets[0]
              && unsigned(offsetof(ct::DeviceSettings, led_brightness)) == v2_settings_offsets[1]
              && unsigned(offsetof(ct::DeviceSettings, reserved)) == v2_settings_offsets[2],
          "DeviceSettings, field by field");
}

void testLabelStore()
{
    std::printf("a 2.0 unit keeps names apart: its records are the 1.x ones without labels\n");
    CHECK(ct::CMD_WRITE_LABELS == v2_label_cmds[0] && ct::CMD_READ_LABELS == v2_label_cmds[1],
          "CMD_WRITE_LABELS / CMD_READ_LABELS");
    CHECK(ct::LABEL_KIND_MESSAGE == v2_label_kinds[0] && ct::LABEL_KIND_SIGNAL == v2_label_kinds[1]
              && ct::LABEL_KIND_RELAY == v2_label_kinds[2],
          "LABEL_KIND_*");
    CHECK(unsigned(ct::LABEL_STORE_BYTES) == v2_label_bytes && v2_label_bytes == 32,
          "32-byte names");
    CHECK(unsigned(offsetof(ct::CapacityExtension, label_bytes)) == v2_capacity_label_at,
          "where the capacity report says so");
    CHECK(unsigned(ct::V2_MESSAGE_BYTES) == v2_record_sizes[0]
              && unsigned(ct::V2_SIGNAL_BYTES) == v2_record_sizes[1]
              && unsigned(ct::V2_RELAY_BYTES) == v2_record_sizes[2],
          "the 2.0's message, signal and relay records are the Manager's slices");
    // Field by field: each 2.0 record is one run of the 1.x struct, starting at
    // V2_*_AT, so every field sits where the 1.x one does, less that start.
    const unsigned msg[9] = {
        unsigned(offsetof(ct::CanMessageConfig, can_id)),
        unsigned(offsetof(ct::CanMessageConfig, flags)),
        unsigned(offsetof(ct::CanMessageConfig, src_bus)),
        unsigned(offsetof(ct::CanMessageConfig, route_bus_mask)),
        unsigned(offsetof(ct::CanMessageConfig, dlc)),
        unsigned(offsetof(ct::CanMessageConfig, period_ms)),
        unsigned(offsetof(ct::CanMessageConfig, tx_trigger_cond)),
        unsigned(offsetof(ct::CanMessageConfig, tx_trigger_flags)),
        unsigned(offsetof(ct::CanMessageConfig, password_slot)),
    };
    bool msgOk = true;
    for (int i = 0; i < 9; ++i)
        msgOk &= msg[i] - unsigned(ct::V2_MESSAGE_AT) == v2_message_offsets[i];
    CHECK(msgOk, "every message field where the 2.0 has it");
    const unsigned sig[10] = {
        unsigned(offsetof(ct::CanSignalConfig, factor)),
        unsigned(offsetof(ct::CanSignalConfig, offset)),
        unsigned(offsetof(ct::CanSignalConfig, min_val)),
        unsigned(offsetof(ct::CanSignalConfig, max_val)),
        unsigned(offsetof(ct::CanSignalConfig, default_value)),
        unsigned(offsetof(ct::CanSignalConfig, mux_id)),
        unsigned(offsetof(ct::CanSignalConfig, mux_mask)),
        unsigned(offsetof(ct::CanSignalConfig, msg_and_flags)),
        unsigned(offsetof(ct::CanSignalConfig, tx_source)),
        unsigned(offsetof(ct::CanSignalConfig, bits)),
    };
    bool sigOk = true;
    for (int i = 0; i < 10; ++i)
        sigOk &= sig[i] - unsigned(ct::V2_SIGNAL_AT) == v2_signal_offsets[i];
    CHECK(sigOk, "every signal field where the 2.0 has it");
    const unsigned rel[5] = {
        unsigned(offsetof(ct::RelayConfig, address)),
        unsigned(offsetof(ct::RelayConfig, bitmask)),
        unsigned(offsetof(ct::RelayConfig, flags)),
        unsigned(offsetof(ct::RelayConfig, src_bus)),
        unsigned(offsetof(ct::RelayConfig, forward_bus_mask)),
    };
    bool relOk = true;
    for (int i = 0; i < 5; ++i)
        relOk &= rel[i] - unsigned(ct::V2_RELAY_AT) == v2_relay_offsets[i];
    CHECK(relOk, "every relay field where the 2.0 has it");

    const QByteArray bytes(reinterpret_cast<const char *>(v2_capacity_report),
                           int(v2_capacity_report_size));
    ct::DeviceCapacity cap;
    CHECK(ct::parseCapacityReport(bytes, &cap) && cap.labelsApart() && cap.labelBytes == 32
              && cap.channelNameBytes() == 32 && cap.messageNameBytes() == 32,
          "the unit's report: names apart, 32 bytes of each kept");
    CHECK(cap.itemSizeOf(ct::DeviceTable::Messages) == ct::V2_MESSAGE_BYTES
              && cap.itemSizeOf(ct::DeviceTable::Signals) == ct::V2_SIGNAL_BYTES
              && cap.itemSizeOf(ct::DeviceTable::Relays) == ct::V2_RELAY_BYTES,
          "and its message, signal and relay record sizes are the 2.0's");
    ct::DeviceCapacity back;
    CHECK(ct::DeviceCapacity::fromJson(cap.toJson(), &back) && back.sameLabels(cap)
              && back.labelsApart(),
          "a document records where its target keeps names");
    const ct::DeviceCapacity old = ct::DeviceCapacity::builtIn();
    CHECK(!old.labelsApart() && old.channelNameBytes() == 31 && old.messageNameBytes() == 17,
          "a 1.x target keeps them in the records: 31 and 17 bytes");
    // The CAN FD build's report ended at 64 bytes, with names in the records.
    const int extAt = 4 + 4 * int(v2_num_tables);
    QByteArray sixtyFour = bytes.left(extAt + 64);
    qToLittleEndian<quint16>(quint16(64), sixtyFour.data() + extAt);
    ct::DeviceCapacity canFd;
    CHECK(ct::parseCapacityReport(sixtyFour, &canFd) && !canFd.labelsApart() && canFd.fastData(),
          "a 64-byte extension states fast data phases and names in the records");
}

void testCanFeatures()
{
    std::printf("a 2.0 unit states fast CAN FD data phases, and a 1.x or an earlier 2.0 none\n");
    CHECK(ct::CAPACITY_CAN_FAST_DATA == v2_can_fast_data, "CAPACITY_CAN_FAST_DATA");
    const QByteArray bytes(reinterpret_cast<const char *>(v2_capacity_report),
                           int(v2_capacity_report_size));
    ct::DeviceCapacity cap;
    CHECK(ct::parseCapacityReport(bytes, &cap) && cap.fastData(),
          "the unit's report states 4, 5 and 8 Mbit/s data phases");
    ct::DeviceCapacity back;
    CHECK(ct::DeviceCapacity::fromJson(cap.toJson(), &back) && back.sameCan(cap) && back.fastData(),
          "a document records them");
    CHECK(!ct::DeviceCapacity::builtIn().fastData() && !cap.sameCan(ct::DeviceCapacity::builtIn()),
          "which a 1.x target does not share");
    // The CRC8 builds sent the same 64 bytes with the field a reserved 0.
    const int extAt = 4 + 4 * int(v2_num_tables);
    QByteArray reserved = bytes;
    qToLittleEndian<quint16>(quint16(0), reserved.data() + extAt + 62);
    ct::DeviceCapacity crc8Build;
    CHECK(ct::parseCapacityReport(reserved, &crc8Build) && !crc8Build.fastData()
              && crc8Build.crc8Features == cap.crc8Features,
          "an earlier 2.0 build, whose word was reserved, states no fast data phases");
}

void testScriptCostModel()
{
    std::printf("a 2.0 unit's script cost model is read from its report and charged\n");
    CHECK(unsigned(ct::CAPACITY_SCRIPT_OPS) == v2_script_op_count, "CAPACITY_SCRIPT_OPS");
    const QByteArray bytes(reinterpret_cast<const char *>(v2_capacity_report),
                           int(v2_capacity_report_size));
    ct::DeviceCapacity cap;
    CHECK(ct::parseCapacityReport(bytes, &cap), "the unit's report parses");
    const QByteArray table(reinterpret_cast<const char *>(v2_script_costs),
                           int(v2_script_op_count));
    CHECK(cap.scriptBudget == int(v2_script_budget), "the budget the firmware states");
    CHECK(cap.scriptCosts == table, "every opcode's cost, as script_vm.h has it");

    ct::DeviceCapacity back;
    CHECK(ct::DeviceCapacity::fromJson(cap.toJson(), &back) && back.sameScriptModel(cap),
          "and a document records it");
    CHECK(!cap.sameScriptModel(ct::DeviceCapacity::builtIn()), "which a 1.x target does not share");

    // The CAN Triple 1.x's model, which is in force outside any scope.
    const quint32 mod1x = ct::scriptOpCost(0x0F), budget1x = ct::scriptTickBudget();
    CHECK(mod1x != v2_script_costs[0x0F], "the two lines charge MOD differently");
    {
        const ct::ScriptCostScope scope(cap);
        bool same = true;
        for (unsigned op = 0; op < 256; ++op) {
            const unsigned want = op < v2_script_op_count ? v2_script_costs[op] : 1u;
            same = same && ct::scriptOpCost(quint8(op)) == want;
        }
        CHECK(same, "in force, every opcode costs what the unit charges, 1 past the table");
        CHECK(ct::scriptTickBudget() == v2_script_budget, "and a tick stops at its budget");
        {
            const ct::ScriptCostScope inner(ct::DeviceCapacity::builtIn());
            CHECK(ct::scriptOpCost(0x0F) == mod1x && ct::scriptTickBudget() == budget1x,
                  "a 1.x target inside it charges the 1.x model");
        }
        CHECK(ct::scriptOpCost(0x0F) == v2_script_costs[0x0F]
                  && ct::scriptTickBudget() == v2_script_budget,
              "and the 2.0's comes back when that ends");
    }
    CHECK(ct::scriptOpCost(0x0F) == mod1x && ct::scriptTickBudget() == budget1x,
          "out of scope, the 1.x model again");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testMirror();
    testStatusRead();
    testConfirm();
    testImages();
    testCapacityReport();
    testScriptCostModel();
    testCrc8Features();
    testCanFeatures();
    testLabelStore();
    testDeviceSettings();
    if (fails == 0)
        std::printf("test_firmware_v2: all checks passed\n");
    else
        std::printf("test_firmware_v2: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
