// Console self-test: protocol framing, COBS, CRC, extraction mapping, document
// JSON round-trips, the access passwords and the .ct3s secure container.
// Exits 0 on success, 1 on first failure.
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cmath>
#include <QElapsedTimer>

#include <cstdio>
#include <cstring>

#include <QDateTime>
#include <QMessageAuthenticationCode>
#include <QPasswordDigestor>

#include <QSet>

#include "../src/model/access_keys.h"
#include "../src/model/channel_catalog.h"
#include "../src/model/config_lock.h"
#include "../src/model/config_report.h"
#include "../src/model/configuration.h"
#include "../src/model/dbc_import.h"
#include "../src/model/device_mapper.h"
#include "../src/model/config_file.h"
#include "../src/model/secure_file.h"
#include "../src/model/validation.h"
#include "../src/protocol/asc_log.h"
#include "../src/protocol/cobs.h"
#include "../src/protocol/config_transfer.h"
#include "../src/protocol/crc16.h"
#include "../src/protocol/device_link.h"
#include "../src/protocol/framer.h"

// The bytecode format a retained script image is written in, so this file can
// build one the DEVICE's own verifier accepts rather than a blob that only
// proves base64 survives a round trip.
extern "C" {
#include "script_vm.h"
}

static int failures = 0;

// The schema this build writes — kConfigSchemaVersion in configuration.cpp,
// which is file-static there. Every CommsSection::fromJson call below that is
// re-reading something this build just WROTE passes it: the pre-14 legacy
// protection keys migrate only when the file actually predates 14, so handing
// such a call a stale number would ratchet a Read Only section into Hidden.
static constexpr int kCurrentSchemaVersion = 20;
// A .ct3 fileVersion this build has never heard of, for checking that a file
// from a NEWER release is refused rather than half-read. DERIVED, not typed:
// it was the literal 14 until 2.3.0, the 13 -> 14 schema bump caught it up, and
// the test then asserted that the CURRENT version is rejected — precisely the
// failure the previous comment predicted here and did not prevent.
static constexpr int kFutureSchemaProbe = kCurrentSchemaVersion + 1;


#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            ++failures;                                                                          \
        }                                                                                        \
    } while (0)

// The body of a format-2 .ct3, for the checks that assert on what the writer
// PUT IN THE FILE rather than on what a reload produces. Reading the file as
// JSON is what these used to do and it no longer works: the body is sealed, and
// going through the container is the only honest way to look at it. A round
// trip through Configuration would prove something weaker — it cannot tell a
// key that was written from one the loader defaulted.
static QJsonObject configBodyOf(const QString &path)
{
    QByteArray plain;
    if (!ct::readBinaryConfigFile(path, &plain, nullptr, nullptr))
        return QJsonObject();
    return QJsonDocument::fromJson(plain).object();
}

// Give a condition the Reset that makes it behave like the level these tests
// were written against: the exact inverse of its Set, which is also what the
// migration writes into every configuration loaded from an older file.
static void giveInverseReset(ct::ConditionRow &c)
{
    CHECK(ct::invertConditionExpr(c.setTerms, c.setJoiners, &c.resetTerms, &c.resetJoiners));
}

using namespace ct;

static void testCrc()
{
    // CRC16-CCITT-FALSE reference vector
    const char *digits = "123456789";
    CHECK(crc16(reinterpret_cast<const uint8_t *>(digits), 9) == 0x29B1);
}

static void testCobs()
{
    const QList<QByteArray> cases = {
        QByteArray(),
        QByteArray("\x00", 1),
        QByteArray("\x00\x00", 2),
        QByteArray("\x11\x22\x00\x33", 4),
        QByteArray("\x11\x22\x33\x44", 4),
        QByteArray(253, 'x'),
        QByteArray(254, 'x'),
        QByteArray(255, 'x'),
        QByteArray(300, 'x') + QByteArray(1, '\0') + QByteArray(300, 'y'),
    };
    for (const QByteArray &input : cases) {
        const QByteArray encoded = cobsEncode(input);
        CHECK(!encoded.contains('\0'));
        const QByteArray decoded = cobsDecode(encoded);
        CHECK(decoded == input);
    }
}

static void testFramer()
{
    const QByteArray payload("\x01\x02\x03\x00\xFF", 5);
    const QByteArray frame = buildFrame(CMD_WRITE_MSG_CFG, payload);
    CHECK(frame.startsWith('\0'));
    CHECK(frame.endsWith('\0'));

    FrameSplitter splitter;
    // Split the frame across two feeds, with printf noise in front.
    QList<Packet> packets = splitter.feed("Bus1 RX id=0x640 dlc=8\r\n" + frame.left(3));
    CHECK(packets.isEmpty());
    packets = splitter.feed(frame.mid(3));
    CHECK(packets.size() == 1);
    if (packets.size() == 1) {
        CHECK(packets[0].cmd == CMD_WRITE_MSG_CFG);
        CHECK(packets[0].payload == payload);
    }

    // Two frames back-to-back plus garbage chunk between them.
    packets = splitter.feed(frame + QByteArray("garbage") + QByteArray(1, '\0') + frame);
    CHECK(packets.size() == 2);

    // A multi-kilobyte run of delimiter-free garbage — a wrong baud rate, or a
    // hostile device — must be handled cheaply and drop nothing valid. This is
    // the O(n^2) resync trap: the splitter used to try every suffix of the
    // chunk from offset 1, each an O(n) decode, so 8 KB of noise was ~30M
    // byte-operations that froze the UI thread. The window fix bounds it. Timed
    // so a regression to O(n^2) blows the budget rather than merely slowing.
    {
        QByteArray flood(8000, '\x5A'); // no 0x00 anywhere, so one giant chunk
        QElapsedTimer t;
        t.start();
        packets = splitter.feed(flood + QByteArray(1, '\0'));
        const qint64 ms = t.elapsed();
        CHECK(packets.isEmpty());
        CHECK(ms < 200); // O(n^2) here is seconds; the window keeps it well under
        // And a real frame arriving right after the flood is still recovered —
        // the garbage must not have wedged or mis-synced the splitter.
        packets = splitter.feed(frame);
        CHECK(packets.size() == 1);
        // A valid frame carrying noise glued to its front INSIDE an
        // over-window chunk: the frame is a suffix, so the bounded search must
        // still find it.
        QByteArray noisyBig = QByteArray(6000, '\x5A') + frame.mid(1);
        packets = splitter.feed(noisyBig);
        CHECK(packets.size() == 1);
        if (packets.size() == 1)
            CHECK(packets[0].payload == payload);
    }

    // Corrupted CRC must be dropped silently.
    QByteArray bad = frame;
    bad[bad.size() - 3] = char(bad[bad.size() - 3] ^ 0x01);
    packets = splitter.feed(bad);
    CHECK(packets.isEmpty());

    // Firmware frames carry only a TRAILING delimiter, so printf noise glues
    // onto the front of the encoded bytes — the splitter must resync.
    const QByteArray firmwareStyle = frame.mid(1); // strip the leading 0x00
    packets = splitter.feed(QByteArray("Bus2 RX id=0x1A0 dlc=8\r\n") + firmwareStyle);
    CHECK(packets.size() == 1);
    if (packets.size() == 1)
        CHECK(packets[0].payload == payload);

    // Wire limit. This used to read "under the firmware's 128-byte hazard" —
    // the v1 RX-DMA fault that set MAX_TX_PAYLOAD at 112 in the first place.
    // That fault has been fixed since circular DMA landed (FIRMWARE-NOTES #5),
    // and the cap is now 496 payload bytes inside a 512-byte wire frame,
    // matched by a 1 KB rxBuffer on the device. What the limit protects is no
    // longer a hardware bug but the receive buffer's real size, which makes it
    // no less worth asserting: a frame past it is not rejected, it is lapped.
    const QByteArray maxPayload(MAX_TX_PAYLOAD, '\xAA');
    // A worst-case frame is 4 header + 496 payload + 2 CRC = 502 raw bytes;
    // COBS adds one code byte per block (at most ceil(502/254) = 2) and the two
    // delimiters bring it to 506, six under the 512-byte cap. Both delimiters
    // count toward MAX_TX_WIRE_BYTES, which is why there is no slack term here.
    CHECK(buildFrame(CMD_WRITE_SIG_CFG, maxPayload).size() <= 506);
    CHECK(buildFrame(CMD_WRITE_SIG_CFG, maxPayload).size() <= MAX_TX_WIRE_BYTES);
    CHECK(4 + MAX_TX_PAYLOAD + 2 /* raw */ + 2 /* COBS */ + 2 /* delimiters */
          <= MAX_TX_WIRE_BYTES);
    // Nothing this host sends can exceed that, because every write chunk is
    // sized against MAX_TX_PAYLOAD. (test_firmware_link checks all of them, and
    // checks that each is maximal as well as legal; this is the one that keeps
    // the framer's own limit honest against the record that grew.)
    CHECK(4 + WRITE_CHUNK_SIGNALS * int(sizeof(CanSignalConfig)) <= MAX_TX_PAYLOAD);
}

static void testExtraction()
{
    // Intel (Word Swap): 16-bit unsigned at start bit 16
    CommsChannelRow row;
    row.startBit = 16;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    ExtractionFields f;
    QString reason;
    CHECK(computeExtraction(row, SectionAlignment::WordSwap, 8, &f, &reason));
    CHECK(f.byteOrder == 0);
    CHECK(f.startBit == 16);
    CHECK(f.bitLength == 16);
    CHECK(f.valueType == SIGNAL_TYPE_UINT16);

    // Signed narrows to the smallest int that fits
    row.startBit = 0;
    row.bitLength = 8;
    row.dbcType = int(DbcType::Signed);
    CHECK(computeExtraction(row, SectionAlignment::WordSwap, 8, &f, &reason));
    CHECK(f.valueType == SIGNAL_TYPE_INT8);
    CHECK(f.bitLength == 8);

    // IEEE754 forces a 32-bit float regardless of the requested bit length
    row.startBit = 0;
    row.bitLength = 16; // ignored
    row.dbcType = int(DbcType::IEEE754);
    CHECK(computeExtraction(row, SectionAlignment::WordSwap, 8, &f, &reason));
    CHECK(f.bitLength == 32);
    CHECK(f.valueType == SIGNAL_TYPE_FLOAT);

    // Motorola (Normal): the start bit is the signal's LSB. A 16-bit field
    // over bytes 2..3 (value = data[2]<<8 | data[3]) has its LSB at byte 3
    // bit 0 = start bit 24; the walk ascends data[3] then steps to byte 2.
    row.startBit = 24;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    CHECK(computeExtraction(row, SectionAlignment::Normal, 8, &f, &reason));
    CHECK(f.byteOrder == 1);
    CHECK(f.startBit == 24);
    CHECK(f.bitLength == 16);

    // The classic top-of-frame Motorola field (value = data[0]<<8 | data[1]):
    // LSB at byte 1 bit 0 = start bit 8, walking bytes 1 then 0.
    row.startBit = 8;
    row.bitLength = 16;
    CHECK(computeExtraction(row, SectionAlignment::Normal, 8, &f, &reason));
    CHECK(f.startBit == 8);

    // Intel field past DLC rejected
    row.startBit = 56;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    CHECK(!computeExtraction(row, SectionAlignment::WordSwap, 8, &f, &reason));

    // Motorola field that underflows past byte 0 rejected: from byte 0 bit 7
    // one more bit would step to byte -1.
    row.startBit = 7;
    row.bitLength = 16;
    CHECK(!computeExtraction(row, SectionAlignment::Normal, 8, &f, &reason));

    // Start byte outside the frame rejected outright.
    row.startBit = 80;
    row.bitLength = 8;
    CHECK(!computeExtraction(row, SectionAlignment::Normal, 8, &f, &reason));

    // Negative start bit rejected
    row.startBit = -1;
    row.bitLength = 8;
    CHECK(!computeExtraction(row, SectionAlignment::WordSwap, 8, &f, &reason));
}

static void testMapper()
{
    Configuration config;
    config.bus[0].enabled = true; // buses default Off; messages on an Off bus
                                  // upload deactivated
    CommsSection section;
    section.name = QStringLiteral("Test RX");
    section.device = SectionDevice::ReceiveMessage;
    section.alignment = SectionAlignment::WordSwap;
    section.baseAddress = 0x640;
    section.messageLengthBytes = 8;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Engine RPM");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    section.rows.append(row);
    config.bus[0].sections.append(section);

    MathRow math;
    math.op = MATH_OP_MUL;
    math.aIsChannel = true;
    math.aChannel = QStringLiteral("Engine RPM");
    math.bIsChannel = false;
    math.bConst = 2.0;
    math.destChannel = QStringLiteral("GP Raw 1");
    config.mathRows.append(math);

    // Forward reference: row 0 may read the destination of row 1.
    MathRow forward;
    forward.op = MATH_OP_ADD;
    forward.aIsChannel = true;
    forward.aChannel = QStringLiteral("GP Raw 2");
    forward.bIsChannel = false;
    forward.bConst = 1.0;
    forward.destChannel = QStringLiteral("GP Raw 3");
    config.mathRows.prepend(forward);
    MathRow producer;
    producer.op = MATH_OP_ADD;
    producer.aIsChannel = true;
    producer.aChannel = QStringLiteral("Engine RPM");
    producer.bIsChannel = false;
    producer.bConst = 0.0;
    producer.destChannel = QStringLiteral("GP Raw 2");
    config.mathRows.append(producer);

    const MappingResult mapped = mapToDevice(config);
    CHECK(mapped.ok());
    CHECK(mapped.tables.messages.size() == 1);
    if (!mapped.tables.messages.isEmpty()) {
        CHECK(mapped.tables.messages[0].can_id == 0x640);
        CHECK(mapped.tables.messages[0].src_bus == 1);
        CHECK(mapped.tables.messages[0].flags & MSGFLAG_ACTIVE);
    }
    // RPM + three virtual math destinations (GP Raw 1/2/3), and then the device
    // channels, which every Send carries whether or not the document reads one.
    // They are allocated LAST, so the document's own indices checked below do
    // not move — which is the property that makes the change invisible here
    // beyond this count.
    const int kDocSignals = 4;
    CHECK(mapped.tables.signalConfigs.size() == kDocSignals + DEVCH_COUNT);
    if (mapped.tables.signalConfigs.size() >= kDocSignals) {
        const CanSignalConfig &sig = mapped.tables.signalConfigs[0];
        CHECK(sigMsgIdx(sig) == 0);
        CHECK(sigBitLength(sig) == 16);
        CHECK(std::strcmp(sig.label, "Engine RPM") == 0);
        CHECK(sigMsgIdx(mapped.tables.signalConfigs[1]) == SIG_MSG_NONE);
    }
    CHECK(mapped.tables.math.size() == 3);
    if (mapped.tables.math.size() == 3) {
        // Row 0 forward-references row 2's destination — must resolve.
        CHECK(mapped.tables.math[0].input_a_type == 1);
        CHECK(mapped.tables.math[0].input_a_idx
              == mapped.tables.math[2].dest_signal_idx);
    }

    // Validation runs clean on this config (info entries only)
    const auto issues = validateConfiguration(config);
    for (const auto &issue : issues)
        CHECK(issue.severity != ValidationIssue::Error);

    // Reverse mapping produces a section with one row again
    Configuration roundTrip;
    mapFromDevice(mapped.tables, roundTrip, nullptr);
    CHECK(roundTrip.bus[0].sections.size() == 1);
    if (!roundTrip.bus[0].sections.isEmpty())
        CHECK(roundTrip.bus[0].sections[0].rows.size() == 1);
}

// Advanced Math: the C operand across every representation it crosses — the
// document (.ct3 JSON), the 24-byte wire record (where C rides in four raw
// bytes) and back through Get. Both C modes (constant and channel) must
// survive each hop byte-for-byte, legacy pad bytes must normalise instead of
// decoding as garbage, and a schema <= 9 row (no c* keys) must default.
static void testAdvancedMath()
{
    Configuration config;
    config.bus[0].enabled = true;

    CommsSection section;
    section.name = QStringLiteral("Adv RX");
    section.device = SectionDevice::ReceiveMessage;
    section.alignment = SectionAlignment::WordSwap;
    section.baseAddress = 0x300;
    section.messageLengthBytes = 8;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Heading Raw");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    section.rows.append(row);
    CommsChannelRow trim = row;
    trim.channelName = QStringLiteral("Trim");
    trim.startBit = 16;
    section.rows.append(trim);
    config.bus[0].sections.append(section);

    // C as a CONSTANT: wrap a running heading into [0, 360).
    MathRow wrap;
    wrap.op = MATH_OP_WRAP;
    wrap.aIsChannel = true;
    wrap.aChannel = QStringLiteral("Heading Raw");
    wrap.bIsChannel = false;
    wrap.bConst = 0.0;
    wrap.cIsChannel = false;
    wrap.cConst = 360.0;
    wrap.destChannel = QStringLiteral("Heading");
    config.mathRows.append(wrap);

    // C as a CHANNEL: the offset rides in a live channel.
    MathRow muladd;
    muladd.op = MATH_OP_MULADD;
    muladd.aIsChannel = true;
    muladd.aChannel = QStringLiteral("Heading Raw");
    muladd.bIsChannel = false;
    muladd.bConst = 0.1;
    muladd.cIsChannel = true;
    muladd.cChannel = QStringLiteral("Trim");
    muladd.destChannel = QStringLiteral("Corrected");
    config.mathRows.append(muladd);

    // A binary op rides along to prove arity-2 rows never carry C.
    MathRow plain;
    plain.op = MATH_OP_ADD;
    plain.aIsChannel = true;
    plain.aChannel = QStringLiteral("Heading Raw");
    plain.bIsChannel = false;
    plain.bConst = 1.0;
    plain.destChannel = QStringLiteral("Plus One");
    config.mathRows.append(plain);

    const MappingResult mapped = mapToDevice(config);
    CHECK(mapped.ok());
    CHECK(mapped.tables.math.size() == 3);
    if (mapped.tables.math.size() == 3) {
        const MathConfig &w = mapped.tables.math[0];
        CHECK(w.op == MATH_OP_WRAP);
        CHECK(w.input_c_type == 0);
        CHECK(mathInputCConst(w) == 360.0f);
        CHECK(w.reserved0 == 0);

        const MathConfig &ma = mapped.tables.math[1];
        CHECK(ma.op == MATH_OP_MULADD);
        CHECK(ma.input_c_type == 1);
        CHECK(int(mathInputCIdx(ma))
              == mapped.channelToSignal.value(QStringLiteral("trim"), -1));
        CHECK(ma.input_c_val[2] == 0 && ma.input_c_val[3] == 0);

        // Arity 2: C stays at the wire defaults, all zero.
        const MathConfig &pl = mapped.tables.math[2];
        CHECK(pl.op == MATH_OP_ADD);
        CHECK(pl.input_c_type == 0);
        CHECK(pl.input_c_val[0] == 0 && pl.input_c_val[1] == 0 && pl.input_c_val[2] == 0
              && pl.input_c_val[3] == 0);
        CHECK(pl.reserved0 == 0);
    }

    // Get: the rows come back with both C modes intact...
    Configuration back;
    mapFromDevice(mapped.tables, back, nullptr);
    CHECK(back.mathRows.size() == 3);
    if (back.mathRows.size() == 3) {
        CHECK(back.mathRows[0].op == MATH_OP_WRAP);
        CHECK(!back.mathRows[0].cIsChannel);
        CHECK(back.mathRows[0].cConst == 360.0);
        CHECK(back.mathRows[1].cIsChannel);
        CHECK(back.mathRows[1].cChannel == QStringLiteral("Trim"));
        CHECK(!back.mathRows[2].cIsChannel);
        CHECK(back.mathRows[2].cConst == 0.0);
        CHECK(back.mathRows[2].cChannel.isEmpty());
    }

    // ...and re-mapping the round-tripped document reproduces the SAME wire
    // records byte-for-byte — map/unmap is lossless where the device looks.
    const MappingResult again = mapToDevice(back);
    CHECK(again.ok());
    CHECK(again.tables.math.size() == mapped.tables.math.size());
    for (int i = 0; i < qMin(again.tables.math.size(), mapped.tables.math.size()); ++i)
        CHECK(std::memcmp(&again.tables.math[i], &mapped.tables.math[i], sizeof(MathConfig))
              == 0);

    // Legacy pad: a slot written before the 24-byte format leaves 0xFF where
    // C now lives. On an arity-2 op a Get must normalise that to "unused
    // const 0" — never decode it (0xFFFF is not a channel, and four 0xFF
    // bytes are a NaN, not a constant the user wrote).
    {
        DeviceTables padded = mapped.tables;
        MathConfig &pl = padded.math[2];
        pl.input_c_type = 0xFF;
        pl.input_c_val[0] = pl.input_c_val[1] = 0xFF;
        pl.input_c_val[2] = pl.input_c_val[3] = 0xFF;
        pl.reserved0 = 0xFF;
        Configuration fromPad;
        mapFromDevice(padded, fromPad, nullptr);
        CHECK(fromPad.mathRows.size() == 3);
        if (fromPad.mathRows.size() == 3) {
            CHECK(!fromPad.mathRows[2].cIsChannel);
            CHECK(fromPad.mathRows[2].cConst == 0.0);
            CHECK(fromPad.mathRows[2].cChannel.isEmpty());
        }
    }

    // .ct3 save/load: both C modes survive the JSON round-trip, and the
    // reloaded document still maps to the original wire bytes.
    {
        QTemporaryFile file;
        CHECK(file.open());
        const QString path = file.fileName();
        file.close();
        QString error;
        CHECK(config.saveToFile(path, &error));
        Configuration loaded;
        CHECK(loaded.loadFromFile(path, &error));
        CHECK(loaded.mathRows.size() == 3);
        if (loaded.mathRows.size() == 3) {
            CHECK(!loaded.mathRows[0].cIsChannel);
            CHECK(loaded.mathRows[0].cConst == 360.0);
            CHECK(loaded.mathRows[1].cIsChannel);
            CHECK(loaded.mathRows[1].cChannel == QStringLiteral("Trim"));
        }
        const MappingResult remapped = mapToDevice(loaded);
        CHECK(remapped.ok());
        CHECK(remapped.tables.math.size() == mapped.tables.math.size());
        for (int i = 0; i < qMin(remapped.tables.math.size(), mapped.tables.math.size()); ++i)
            CHECK(std::memcmp(&remapped.tables.math[i], &mapped.tables.math[i],
                              sizeof(MathConfig))
                  == 0);
    }

    // A schema <= 9 row carries no c* keys at all: they must default to an
    // unused const-0 operand (this is what "older files keep loading" means).
    {
        QJsonObject legacy;
        legacy["op"] = int(MATH_OP_ADD);
        legacy["aIsChannel"] = true;
        legacy["aChannel"] = QStringLiteral("Heading Raw");
        legacy["bIsChannel"] = false;
        legacy["bConst"] = 3.0;
        legacy["dest"] = QStringLiteral("Plus Three");
        legacy["active"] = true;
        const MathRow lr = MathRow::fromJson(legacy);
        CHECK(!lr.cIsChannel);
        CHECK(lr.cChannel.isEmpty());
        CHECK(lr.cConst == 0.0);
    }
}

// Locking a configuration to one unit. The rules that matter are the refusals,
// so those are what is pinned: the wrong device, and — the one a careless
// implementation gets backwards — a device that cannot say who it is.
static void testDeviceLock()
{
    Configuration config;
    config.clear();

    // Unlocked goes anywhere, including to a unit that reports no ID at all.
    CHECK(!config.isLockedToDevice());
    CHECK(config.mayBeSentTo(QStringLiteral("510043000350315154373520")));
    CHECK(config.mayBeSentTo(QString()));

    config.setDeviceLock(QStringLiteral("510043000350315154373520"), deriveAccessKey("bench"));
    CHECK(config.isLockedToDevice());
    CHECK(config.mayBeSentTo(QStringLiteral("510043000350315154373520")));
    CHECK(!config.mayBeSentTo(QStringLiteral("510043000350315154373521"))); // one digit out

    // The UID is hex: its case and any separators someone pastes in are
    // presentation, not identity, so they must not decide whether a send works.
    CHECK(config.mayBeSentTo(QStringLiteral("510043000350315154373520").toLower()));
    CHECK(config.mayBeSentTo(QStringLiteral("5100 4300 0350 3151 5437 3520")));

    // "I cannot tell you who I am" must read as a refusal, never as a match.
    // Firmware too old for the identity command reports nothing, and treating
    // that as a pass would make the lock evaporate on exactly the units least
    // likely to be the intended one.
    CHECK(!config.mayBeSentTo(QString()));

    // Round-trips through a saved file, key and all — a lock that did not
    // survive Save/Open would be no lock at all.
    {
        QTemporaryFile file;
        file.setFileTemplate(QDir::tempPath() + QStringLiteral("/ct_lock_XXXXXX.ct3"));
        CHECK(file.open());
        const QString path = file.fileName();
        file.close();
        QString error;
        CHECK(config.saveToFile(path, &error));

        Configuration reloaded;
        CHECK(reloaded.loadFromFile(path, &error));
        CHECK(reloaded.lockedDeviceUid() == QStringLiteral("510043000350315154373520"));
        CHECK(reloaded.deviceLockKey() == deriveAccessKey(QStringLiteral("bench")));
        CHECK(!reloaded.mayBeSentTo(QStringLiteral("510043000350315154373521")));
        CHECK(!reloaded.mayBeSentTo(QString()));
    }

    // Clearing the lock clears the key with it — a configuration that goes
    // anywhere must not still be carrying the key that used to guard it.
    config.setDeviceLock(QString(), deriveAccessKey(QStringLiteral("bench")));
    CHECK(!config.isLockedToDevice());
    CHECK(config.deviceLockKey() == kNoAccessKey);
    CHECK(config.mayBeSentTo(QStringLiteral("anything")));
    {
        QTemporaryFile file;
        file.setFileTemplate(QDir::tempPath() + QStringLiteral("/ct_unlock_XXXXXX.ct3"));
        CHECK(file.open());
        const QString path = file.fileName();
        file.close();
        QString error;
        CHECK(config.saveToFile(path, &error));
        Configuration reloaded;
        CHECK(reloaded.loadFromFile(path, &error));
        CHECK(!reloaded.isLockedToDevice());
    }
}

static void testConfigJson()
{
    Configuration config;
    CommsSection section;
    section.name = QStringLiteral("Json RX");
    section.baseAddress = 0x7DF;
    section.compound = true;
    CompoundIdentifier ident;
    ident.byteOffset = 1;
    ident.id = 0x41;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Engine RPM");
    ident.rows.append(row);
    section.identifiers.append(ident);
    config.bus[1].sections.append(section);

    // Per-bus settings incl. the v9 termination flag round-trip through JSON.
    config.bus[1].enabled = true;
    config.bus[1].rateKbps = 500;
    config.bus[1].dataRateKbps = 2000;
    config.bus[1].termination = true;
    config.bus[0].termination = false;

    Channel user;
    user.name = QStringLiteral("My Channel");
    user.baseResolution = 0.25;
    config.catalog().addOrUpdateUserChannel(user);

    config.setConfigTitle(QStringLiteral("Race Setup 3"));

    QTemporaryFile file;
    CHECK(file.open());
    const QString path = file.fileName();
    file.close();
    QString error;
    CHECK(config.saveToFile(path, &error));

    Configuration loaded;
    CHECK(loaded.loadFromFile(path, &error));
    CHECK(loaded.bus[1].sections.size() == 1);
    if (!loaded.bus[1].sections.isEmpty()) {
        const CommsSection &s = loaded.bus[1].sections[0];
        CHECK(s.compound);
        CHECK(s.identifiers.size() == 1);
        CHECK(s.baseAddress == 0x7DF);
        if (!s.identifiers.isEmpty())
            CHECK(s.identifiers[0].rows.size() == 1);
    }
    CHECK(loaded.catalog().findByName(QStringLiteral("My Channel")).baseResolution == 0.25);
    CHECK(loaded.configTitle() == QStringLiteral("Race Setup 3"));

    // ---- v13 file compatibility: a pre-v13 document stores its 1-axis tables
    // under "tables2x8" with at most 8 sites. It must still load (into the wider
    // row), and re-saving must emit only the new "tables2x16" key. ----
    {
        QTemporaryFile legacyFile;
        CHECK(legacyFile.open());
        const QString legacyPath = legacyFile.fileName();
        legacyFile.close();

        QJsonObject legacyTable;
        legacyTable["output"] = QStringLiteral("LegacyOut");
        legacyTable["dataType"] = QStringLiteral("u8");
        legacyTable["decimals"] = 0;
        legacyTable["xChannel"] = QStringLiteral("LegacyAxis");
        legacyTable["xInterp"] = true;
        QJsonArray lSites, lOuts;
        for (int k = 0; k < 8; ++k) { // the old maximum width
            lSites.append(k * 10);
            lOuts.append(k * 5);
        }
        legacyTable["xSites"] = lSites;
        legacyTable["outputs"] = lOuts;
        legacyTable["active"] = true;
        QJsonObject legacyRoot;
        legacyRoot["fileType"] = QStringLiteral("CANTripleConfig");
        legacyRoot["fileVersion"] = 2;
        legacyRoot["tables2x8"] = QJsonArray{legacyTable};
        QFile lf(legacyPath);
        CHECK(lf.open(QIODevice::WriteOnly));
        lf.write(QJsonDocument(legacyRoot).toJson());
        lf.close();

        Configuration legacy;
        CHECK(legacy.loadFromFile(legacyPath, &error));
        CHECK(legacy.table2x16Rows.size() == 1);
        if (legacy.table2x16Rows.size() == 1) {
            const Table2x16Row &lt = legacy.table2x16Rows[0];
            CHECK(lt.outputChannel == QStringLiteral("LegacyOut"));
            CHECK(lt.xSites.size() == 8); // 8 sites, not padded to 16
            CHECK(lt.outputs.size() == 8);
            CHECK(qFuzzyCompare(lt.xSites.last(), 70.0));
        }

        // Re-save: the new key carries the data and the legacy key is gone.
        QTemporaryFile resavedFile;
        CHECK(resavedFile.open());
        const QString resavedPath = resavedFile.fileName();
        resavedFile.close();
        CHECK(legacy.saveToFile(resavedPath, &error));
        const QJsonObject resaved = configBodyOf(resavedPath);
        CHECK(resaved.contains(QStringLiteral("tables2x16")));
        CHECK(!resaved.contains(QStringLiteral("tables2x8")));
        CHECK(resaved["tables2x16"].toArray().size() == 1);

        // An intentionally EMPTY new-format array must not fall back to a
        // legacy key that happens to still be present.
        QJsonObject bothRoot = legacyRoot;
        bothRoot["tables2x16"] = QJsonArray{};
        QTemporaryFile bothFile;
        CHECK(bothFile.open());
        const QString bothPath = bothFile.fileName();
        bothFile.close();
        QFile bf(bothPath);
        CHECK(bf.open(QIODevice::WriteOnly));
        bf.write(QJsonDocument(bothRoot).toJson());
        bf.close();
        Configuration both;
        CHECK(both.loadFromFile(bothPath, &error));
        CHECK(both.table2x16Rows.isEmpty());
    }

    // ---- schema 12: the two-axis table is an 8x8, and a schema-11 file's 4x4
    // tables MUST LOAD rather than be refused.
    //
    // This is the migration that decides whether the capacity expansion costs
    // anyone their saved work. The two document forms are field for field
    // identical — a 4x4 row always carried variable-length site lists and an
    // `outputs` grid strided by xSites.size(), never a fixed 4 — so a saved 4x4
    // already IS an 8x8 whose sites stop early. What has to be proved is that
    // the loader believes that: the counts survive, the cells survive in order,
    // and the table lands in the TOP-LEFT of the wider grid rather than being
    // stretched, re-strided or dropped. ----
    {
        QJsonObject legacy4x4;
        legacy4x4["output"] = QStringLiteral("Ignition");
        legacy4x4["dataType"] = QStringLiteral("float");
        legacy4x4["decimals"] = 1;
        legacy4x4["xChannel"] = QStringLiteral("LoadAxis");
        legacy4x4["yChannel"] = QStringLiteral("RpmAxis");
        legacy4x4["xInterp"] = true;
        legacy4x4["yInterp"] = false;
        // A PARTIAL 4x4 — 4 X sites by 3 Y sites — because a full one cannot
        // tell a correct migration from one that assumed a 4-wide stride.
        legacy4x4["xSites"] = QJsonArray{10, 20, 30, 40};
        legacy4x4["ySites"] = QJsonArray{1000, 2000, 3000};
        // Row-major over the X width of 4: cell(x, y) = outputs[y*4 + x].
        QJsonArray legacyOuts;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x)
                legacyOuts.append(x + 10 * y);
        legacy4x4["outputs"] = legacyOuts;
        legacy4x4["active"] = true;

        QJsonObject root11;
        root11["fileType"] = QStringLiteral("CANTripleConfig");
        root11["fileVersion"] = 11; // the last schema that spoke 4x4
        root11["tables4x4"] = QJsonArray{legacy4x4};

        QTemporaryFile f11;
        CHECK(f11.open());
        const QString p11 = f11.fileName();
        f11.close();
        QFile w11(p11);
        CHECK(w11.open(QIODevice::WriteOnly));
        w11.write(QJsonDocument(root11).toJson());
        w11.close();

        Configuration migrated;
        CHECK(migrated.loadFromFile(p11, &error));
        CHECK(migrated.table8x8Rows.size() == 1);
        if (migrated.table8x8Rows.size() == 1) {
            const Table8x8Row &t = migrated.table8x8Rows[0];
            CHECK(t.outputChannel == QStringLiteral("Ignition"));
            CHECK(t.xChannel == QStringLiteral("LoadAxis"));
            CHECK(t.yChannel == QStringLiteral("RpmAxis"));
            CHECK(t.xInterp && !t.yInterp);
            CHECK(t.decimalPlaces == 1);
            // Counts preserved: 4 and 3, NOT widened to 8 and not clipped.
            CHECK(t.xSites.size() == 4);
            CHECK(t.ySites.size() == 3);
            CHECK(t.outputs.size() == 12);
            CHECK(qFuzzyCompare(t.xSites.value(3), 40.0));
            CHECK(qFuzzyCompare(t.ySites.value(2), 3000.0));
            // Cells in order, still strided by the row's own X width.
            CHECK(t.outputs.value(0) == 0.0);                       // cell(0,0)
            CHECK(qFuzzyCompare(t.outputs.value(2 * 4 + 3), 23.0)); // cell(3,2)
        }

        // ...and it lands in the TOP-LEFT of the wire's fixed 8-wide grid: the
        // 4x3 corner holds the cells, the counts say 4 and 3, and everything
        // outside is zero. This is where the two strides actually differ — the
        // document packs at 4, the device reads at 8 — so a straight copy of
        // the outputs array would shear the grid diagonally.
        {
            ConstantRow lx, ly;
            lx.name = QStringLiteral("LoadAxis");
            lx.dataType = QStringLiteral("u16");
            ly.name = QStringLiteral("RpmAxis");
            ly.dataType = QStringLiteral("u16");
            migrated.constantRows.append(lx);
            migrated.constantRows.append(ly);
            const MappingResult mmr = mapToDevice(migrated);
            CHECK(mmr.ok());
            CHECK(mmr.tables.tables8x8Def.size() == 1);
            CHECK(mmr.tables.tables8x8Row.size() == TABLE_8X8_SITES);
            if (mmr.tables.tables8x8Def.size() == 1
                && mmr.tables.tables8x8Row.size() == TABLE_8X8_SITES) {
                CHECK(mmr.tables.tables8x8Def[0].x_count == 4);
                CHECK(mmr.tables.tables8x8Def[0].y_count == 3);
                CHECK(mmr.tables.tables8x8Def[0].flags & TABLEFLAG_X_INTERP);
                CHECK(!(mmr.tables.tables8x8Def[0].flags & TABLEFLAG_Y_INTERP));
                CHECK(qFuzzyCompare(mmr.tables.tables8x8Def[0].x_sites[3], 40.0f));
                CHECK(mmr.tables.tables8x8Def[0].x_sites[4] == 0.0f); // unused
                CHECK(mmr.tables.tables8x8Row[0].v[0] == 0.0f);       // cell(0,0)
                CHECK(qFuzzyCompare(mmr.tables.tables8x8Row[2].v[3], 23.0f)); // cell(3,2)
                CHECK(mmr.tables.tables8x8Row[0].v[4] == 0.0f); // past the used width
                CHECK(mmr.tables.tables8x8Row[3].v[0] == 0.0f); // past the used height
            }
        }

        // Re-saving writes the CURRENT schema under the new key, and the old key
        // is gone — a file cannot carry both, so nothing downstream has to
        // decide which one wins. The version itself is not the subject here, so
        // it is compared against the constant rather than a literal; typing the
        // number in was what turned the 13 -> 14 bump into a test failure in a
        // function about lookup tables.
        QTemporaryFile f12;
        CHECK(f12.open());
        const QString p12 = f12.fileName();
        f12.close();
        CHECK(migrated.saveToFile(p12, &error));
        const QJsonObject saved = configBodyOf(p12);
        CHECK(saved["fileVersion"].toInt() == kCurrentSchemaVersion);
        CHECK(saved.contains(QStringLiteral("tables8x8")));
        CHECK(!saved.contains(QStringLiteral("tables4x4")));
        CHECK(saved["tables8x8"].toArray().size() == 1);

        // Full schema-12 round trip: everything above survives save and reload,
        // including the widths only an 8x8 can hold.
        Configuration wide;
        Table8x8Row full;
        full.outputChannel = QStringLiteral("Wide");
        full.xChannel = QStringLiteral("LoadAxis");
        full.yChannel = QStringLiteral("RpmAxis");
        for (int k = 0; k < TABLE_8X8_SITES; ++k) {
            full.xSites.append(k * 10);
            full.ySites.append(k * 100);
        }
        for (int y = 0; y < TABLE_8X8_SITES; ++y)
            for (int x = 0; x < TABLE_8X8_SITES; ++x)
                full.outputs.append(x + 10 * y);
        wide.table8x8Rows.append(full);

        QTemporaryFile fw;
        CHECK(fw.open());
        const QString pw = fw.fileName();
        fw.close();
        CHECK(wide.saveToFile(pw, &error));
        Configuration wideBack;
        CHECK(wideBack.loadFromFile(pw, &error));
        CHECK(wideBack.table8x8Rows.size() == 1);
        if (wideBack.table8x8Rows.size() == 1) {
            const Table8x8Row &t = wideBack.table8x8Rows[0];
            CHECK(t.xSites.size() == TABLE_8X8_SITES);
            CHECK(t.ySites.size() == TABLE_8X8_SITES);
            CHECK(t.outputs.size() == 64);
            CHECK(qFuzzyCompare(t.xSites.value(7), 70.0));
            CHECK(qFuzzyCompare(t.ySites.value(7), 700.0));
            CHECK(qFuzzyCompare(t.outputs.value(63), 77.0)); // cell(7,7)
        }

        // A file from a build that does not exist yet is still refused — the
        // schema bump has to keep meaning something.
        QJsonObject future = root11;
        // One past the current schema, so this keeps testing the guard rather
        // than a version that has since become real.
        future["fileVersion"] = kFutureSchemaProbe;
        QTemporaryFile ff;
        CHECK(ff.open());
        const QString pf = ff.fileName();
        ff.close();
        QFile wf(pf);
        CHECK(wf.open(QIODevice::WriteOnly));
        wf.write(QJsonDocument(future).toJson());
        wf.close();
        Configuration refused;
        CHECK(!refused.loadFromFile(pf, &error));
    }
    CHECK(loaded.effectiveTitle() == QStringLiteral("Race Setup 3"));
    CHECK(loaded.bus[1].termination == true);
    CHECK(loaded.bus[1].rateKbps == 500 && loaded.bus[1].dataRateKbps == 2000);
    CHECK(loaded.bus[0].termination == false);

    // Unit taxonomy: defaults (which often differ from the first listed unit)
    // and a couple of unit lists match the spec.
    CHECK(ChannelCatalog::defaultUnitForQuantity(QStringLiteral("Pressure and Stress"))
          == QStringLiteral("kPa"));
    CHECK(ChannelCatalog::defaultUnitForQuantity(QStringLiteral("Speed")) == QStringLiteral("km/h"));
    CHECK(ChannelCatalog::defaultUnitForQuantity(QStringLiteral("Weight & Force"))
          == QStringLiteral("kg"));
    CHECK(ChannelCatalog::defaultUnitForQuantity(QStringLiteral("Fuel Economy"))
          == QStringLiteral("l/mi")); // no explicit default -> first listed
    CHECK(ChannelCatalog::unitsForQuantity(QStringLiteral("Temperature"))
          == (QStringList{"C", "F", "K"}));
    CHECK(ChannelCatalog::quantities().contains(QStringLiteral("Rotational Acceleration")));

    // Any-order search. Existing word-prefix habits keep working (a term that
    // prefixes a word is also a substring of the name)...
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Engine Oil Temp"), QStringLiteral("temp eng oil")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Engine Oil Temp"), QStringLiteral("e o t")));
    CHECK(!ChannelCatalog::matchesSearch(QStringLiteral("Engine Oil Temp"), QStringLiteral("water")));
    // ...and a term may now match mid-word, which is the only way to find a
    // run-together name like CruiseSetSpeed by one of its inner words.
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("set")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("SET")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("speed")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("cruise speed")));
    CHECK(!ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("brake")));
    // Every term still has to appear — the terms are ANDed, not ORed.
    CHECK(!ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("set brake")));
    // Regex when the text carries metacharacters.
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("^Cruise")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("Speed$")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("set|limit")));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Wheel Speed FL"), QStringLiteral("F[LR]$")));
    CHECK(!ChannelCatalog::matchesSearch(QStringLiteral("CruiseSetSpeed"), QStringLiteral("^Speed")));
    // A half-typed pattern must not throw away the list: an invalid regex falls
    // back to plain substring matching so the box stays usable while typing.
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Brake(Front)"), QStringLiteral("Brake(")));
    // Empty / whitespace-only search matches everything.
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Anything"), QString()));
    CHECK(ChannelCatalog::matchesSearch(QStringLiteral("Anything"), QStringLiteral("   ")));

    // v10 compound transmit: the cadence mode round-trips through JSON and maps
    // to the MSGFLAG_TX_SEQUENTIAL wire flag + a gated transmit signal.
    {
        Configuration txCfg;
        txCfg.bus[0].enabled = true;
        Channel gen;
        gen.name = QStringLiteral("Src");
        gen.userDefined = true;
        txCfg.catalog().addOrUpdateUserChannel(gen);
        MathRow m; // generates Src so the transmit source exists
        m.op = MATH_OP_ADD;
        m.aIsChannel = false;
        m.aConst = 3;
        m.bIsChannel = false;
        m.bConst = 0;
        m.destChannel = QStringLiteral("Src");
        txCfg.mathRows.append(m);
        CommsSection tx;
        tx.name = QStringLiteral("Compound Tx");
        tx.device = SectionDevice::TransmitMessage;
        tx.alignment = SectionAlignment::WordSwap;
        tx.baseAddress = 0x321;
        tx.messageLengthBytes = 8;
        tx.transmitRateHz = 50;
        tx.compound = true;
        tx.compoundTxMode = CompoundTxMode::Sequential;
        CompoundIdentifier ci;
        ci.byteOffset = 0;
        ci.id = 3;
        ci.idMask = 0xFF;
        ci.configured = true;
        CommsChannelRow cr;
        cr.channelName = QStringLiteral("Src");
        cr.startBit = 8;
        cr.bitLength = 8;
        cr.defaultValue = 5.0; // must survive Send→Get too
        ci.rows.append(cr);
        tx.identifiers.append(ci);
        txCfg.bus[0].sections.append(tx);

        QTemporaryFile f2;
        CHECK(f2.open());
        const QString p2 = f2.fileName();
        f2.close();
        CHECK(txCfg.saveToFile(p2, &error));
        Configuration l2;
        CHECK(l2.loadFromFile(p2, &error));
        CHECK(l2.bus[0].sections.size() == 1);
        if (!l2.bus[0].sections.isEmpty()) {
            CHECK(l2.bus[0].sections[0].compound);
            CHECK(l2.bus[0].sections[0].compoundTxMode == CompoundTxMode::Sequential);
        }

        const MappingResult tmr = mapToDevice(txCfg);
        CHECK(tmr.ok());
        CHECK(tmr.tables.messages.size() == 1);
        if (!tmr.tables.messages.isEmpty())
            CHECK(tmr.tables.messages[0].flags & MSGFLAG_TX_SEQUENTIAL);
        bool sawGatedTx = false;
        for (const CanSignalConfig &s : tmr.tables.signalConfigs)
            if (sigMsgIdx(s) == 0 && s.mux_mask == 0xFFu && s.mux_id == 3u)
                sawGatedTx = true;
        CHECK(sawGatedTx);

        // Send→Get fidelity: a transmit row's defaultValue survives the mapper
        // round-trip (it is emitted onto the wire signal and read back).
        Configuration back;
        mapFromDevice(tmr.tables, back);
        bool sawDefault = false;
        for (int bi = 0; bi < 3; ++bi)
            for (const CommsSection &sec : back.bus[bi].sections)
                if (sec.baseAddress == 0x321)
                    for (const CompoundIdentifier &ident : sec.identifiers)
                        for (const CommsChannelRow &row : ident.rows)
                            if (qAbs(row.defaultValue - 5.0) < 1e-6)
                                sawDefault = true;
        CHECK(sawDefault);

        // A compound transmit with always-present rows but no populated
        // identifiers warns (the pre-created empty identifier slots don't count).
        Configuration warnCfg;
        warnCfg.bus[0].enabled = true;
        Channel g2;
        g2.name = QStringLiteral("Always Src");
        g2.userDefined = true;
        warnCfg.catalog().addOrUpdateUserChannel(g2);
        MathRow m2;
        m2.op = MATH_OP_ADD;
        m2.destChannel = QStringLiteral("Always Src");
        warnCfg.mathRows.append(m2);
        CommsSection tx2;
        tx2.name = QStringLiteral("Empty Compound Tx");
        tx2.device = SectionDevice::TransmitMessage;
        tx2.baseAddress = 0x322;
        tx2.messageLengthBytes = 8;
        tx2.transmitRateHz = 50;
        tx2.compound = true;
        CommsChannelRow always;
        always.channelName = QStringLiteral("Always Src");
        always.startBit = 0;
        always.bitLength = 8;
        tx2.rows.append(always);
        for (int i = 0; i < 16; ++i)
            tx2.identifiers.append(CompoundIdentifier{}); // pre-created empty slots
        warnCfg.bus[0].sections.append(tx2);
        bool warned = false;
        for (const ValidationIssue &issue : validateConfiguration(warnCfg))
            if (issue.severity == ValidationIssue::Warning
                && issue.message.contains(QStringLiteral("no identifier channels")))
                warned = true;
        CHECK(warned);
    }
}

static MonitorStreamPayload makeFrame(quint32 ms, quint8 bus, quint8 dir, quint32 id,
                                      quint8 flags, std::initializer_list<uint8_t> data)
{
    MonitorStreamPayload f{};
    f.timestamp_ms = ms;
    f.bus_idx = bus;
    f.direction = dir;
    f.can_id = id;
    f.flags = flags;
    f.data_len = quint8(data.size());
    int i = 0;
    for (uint8_t b : data)
        f.data[i++] = b;
    return f;
}

static void testAscLog()
{
    // Header matches the reference layout, in the C locale.
    const QDateTime when(QDate(2025, 7, 8), QTime(7, 39, 10));
    const QString header = ascHeader(when);
    CHECK(header == QStringLiteral(
        "date 20250708 07:39:10\n"
        "base hex  timestamps absolute\n"
        "Begin Triggerblock Tue Jul 08 07:39:10 AM 2025\n"
        "     0.000000 Start of measurement\n"
        "// Generated by CAN Triple Device Manager\n"
        "// Timestamp  Bus     CANId Rx/Tx d Length Data\n"));

    // Standard 8-byte Rx frame — id zero-padded to %03X ("0F3"), reference
    // column alignment otherwise (with our millisecond timestamp).
    const auto rx = makeFrame(54, 2, 0, 0xF3, 0x00,
                              {0x7E, 0x0D, 0x00, 0x00, 0xF0, 0xC4, 0x00, 0xF0});
    CHECK(ascFrameLine(rx, 0) == QStringLiteral(
        "     0.054000   2       0F3    Rx d      8 7E 0D 00 00 F0 C4 00 F0"));

    // 3-digit id, 5-byte payload — verifies right-justified id/length columns.
    const auto rx3 = makeFrame(58481, 2, 0, 0x1A1, 0x00, {0x14, 0xC4, 0x00, 0x00, 0x8A});
    CHECK(ascFrameLine(rx3, 0) == QStringLiteral(
        "    58.481000   2       1A1    Rx d      5 14 C4 00 00 8A"));

    // Extended id as %08X with an 'x' suffix; Tx direction; relative timestamp.
    const auto tx = makeFrame(1500, 3, 1, 0x18DAF110, 0x01, {0x01, 0x02});
    CHECK(ascFrameLine(tx, 500) == QStringLiteral(
        "     1.000000   3 18DAF110x    Tx d      2 01 02"));

    // Zero-length frame still formats (no data bytes, length 0).
    const auto empty = makeFrame(0, 1, 0, 0x100, 0x00, {});
    CHECK(ascFrameLine(empty, 0) == QStringLiteral(
        "     0.000000   1       100    Rx d      0 "));

    // CAN FD frame -> Vector "CANFD" line. Verify field values by tokenizing
    // (the format has wide padded columns; whitespace split is robust).
    ct::MonitorStreamPayload fd{};
    fd.timestamp_ms = 2000;
    fd.bus_idx = 2;
    fd.direction = 0; // Rx
    fd.can_id = 0x1FF; // standard
    fd.flags = ct::MONFLAG_FD | ct::MONFLAG_BRS; // FD, BRS on, ESI off
    fd.data_len = 16;
    for (int i = 0; i < 16; ++i)
        fd.data[i] = quint8(i + 1);
    const QStringList tokFd = ascFrameLine(fd, 0).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    CHECK(tokFd.size() == 9 + 16 + 8); // header(9) + 16 data + trailer(8) = 33
    if (tokFd.size() == 33) {
        CHECK(tokFd[0] == QStringLiteral("2.000000"));
        CHECK(tokFd[1] == QStringLiteral("CANFD"));
        CHECK(tokFd[2] == QStringLiteral("2"));    // channel
        CHECK(tokFd[3] == QStringLiteral("Rx"));   // direction
        CHECK(tokFd[4] == QStringLiteral("1FF"));  // id (standard, %03X)
        CHECK(tokFd[5] == QStringLiteral("1"));    // BRS
        CHECK(tokFd[6] == QStringLiteral("0"));    // ESI
        CHECK(tokFd[7] == QStringLiteral("a"));    // DLC nibble for 16 bytes (=10)
        CHECK(tokFd[8] == QStringLiteral("16"));   // data length
        CHECK(tokFd[9] == QStringLiteral("01") && tokFd[24] == QStringLiteral("10")); // 1st/last
        CHECK(tokFd[27] == QStringLiteral("3000")); // flags: FD(bit12) | BRS(bit13)
        CHECK(tokFd[32] == QStringLiteral("0"));   // last bit-timing field
    }

    // Extended FD, ESI set, 64 bytes, Tx -> id gets %08X + 'x', DLC 'f', ESI 1.
    ct::MonitorStreamPayload fdx{};
    fdx.timestamp_ms = 3000;
    fdx.bus_idx = 3;
    fdx.direction = 1; // Tx
    fdx.can_id = 0x18DAF110;
    fdx.flags = ct::MONFLAG_FD | ct::MONFLAG_BRS | ct::MONFLAG_ESI | ct::MONFLAG_EXTENDED;
    fdx.data_len = 64;
    for (int i = 0; i < 64; ++i)
        fdx.data[i] = quint8(i);
    const QStringList tokFdx = ascFrameLine(fdx, 0).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    CHECK(tokFdx.size() == 9 + 64 + 8); // = 81
    if (tokFdx.size() == 81) {
        CHECK(tokFdx[3] == QStringLiteral("Tx"));
        CHECK(tokFdx[4] == QStringLiteral("18DAF110x")); // extended %08X + 'x'
        CHECK(tokFdx[5] == QStringLiteral("1"));         // BRS
        CHECK(tokFdx[6] == QStringLiteral("1"));         // ESI
        CHECK(tokFdx[7] == QStringLiteral("f"));         // DLC nibble for 64 bytes (=15)
        CHECK(tokFdx[8] == QStringLiteral("64"));        // data length
        CHECK(tokFdx[9 + 64 + 2] == QStringLiteral("7000")); // flags: FD | BRS | ESI
    }
}

static void testConstants()
{
    Configuration config;
    ConstantRow k;
    k.name = QStringLiteral("Boost Target");
    k.dataType = QStringLiteral("float");
    k.decimalPlaces = 1;
    k.value = 12.5;
    config.constantRows.append(k);

    // Constants are generated channels and drive a dedicated wire table.
    CHECK(config.generatedChannelNames().contains(QStringLiteral("Boost Target")));
    const MappingResult mapped = mapToDevice(config);
    CHECK(mapped.ok());
    CHECK(mapped.tables.constants.size() == 1);
    if (mapped.tables.constants.size() == 1) {
        const ConstantConfig &cc = mapped.tables.constants[0];
        CHECK(cc.is_active == 1);
        CHECK(qAbs(cc.value - 12.5f) < 1e-6f);
        CHECK(cc.dest_signal_idx < mapped.tables.signalConfigs.size());
        // Its value slot describes the data type so Get can reconstruct it.
        const CanSignalConfig &sig = mapped.tables.signalConfigs[cc.dest_signal_idx];
        CHECK(sigValueType(sig) == SIGNAL_TYPE_FLOAT);
        CHECK(std::strcmp(sig.label, "Boost Target") == 0);
    }

    // Get reconstructs the constant row from the tables.
    Configuration roundTrip;
    mapFromDevice(mapped.tables, roundTrip, nullptr);
    CHECK(roundTrip.constantRows.size() == 1);
    if (roundTrip.constantRows.size() == 1) {
        const ConstantRow &r = roundTrip.constantRows[0];
        CHECK(r.name == QStringLiteral("Boost Target"));
        CHECK(r.dataType == QStringLiteral("float"));
        CHECK(qAbs(r.value - 12.5) < 1e-6);
    }

    // JSON persistence round-trip.
    QTemporaryFile file;
    CHECK(file.open());
    const QString path = file.fileName();
    file.close();
    QString error;
    CHECK(config.saveToFile(path, &error));
    Configuration loaded;
    CHECK(loaded.loadFromFile(path, &error));
    CHECK(loaded.constantRows.size() == 1);
    if (loaded.constantRows.size() == 1) {
        CHECK(loaded.constantRows[0].name == QStringLiteral("Boost Target"));
        CHECK(loaded.constantRows[0].decimalPlaces == 1);
        CHECK(qAbs(loaded.constantRows[0].value - 12.5) < 1e-6);
    }

    // A constant whose name collides with a communications signal is a mapping
    // error — it must not silently overwrite that signal's slot/extraction.
    {
        Configuration bad;
        bad.bus[0].enabled = true;
        CommsSection rx;
        rx.device = SectionDevice::ReceiveMessage;
        rx.alignment = SectionAlignment::WordSwap;
        rx.baseAddress = 0x321;
        rx.messageLengthBytes = 8;
        CommsChannelRow row;
        row.channelName = QStringLiteral("Shared");
        row.startBit = 0;
        row.bitLength = 16;
        rx.rows.append(row);
        bad.bus[0].sections.append(rx);
        ConstantRow clash;
        clash.name = QStringLiteral("Shared");
        clash.dataType = QStringLiteral("u16");
        clash.value = 5;
        bad.constantRows.append(clash);
        CHECK(!mapToDevice(bad).ok());
    }
}

// v16 integrators: document -> wire -> document, plus the guard rails that stop
// a row the device could not run from ever being sent.
static void testIntegrators()
{
    Configuration config;
    config.bus[0].enabled = true;

    // A receive row supplies the channel being accumulated and the two boolean
    // triggers — an integrator input must be something the config generates.
    CommsSection rx;
    rx.name = QStringLiteral("Receive 0x400");
    rx.device = SectionDevice::ReceiveMessage;
    rx.alignment = SectionAlignment::WordSwap;
    rx.baseAddress = 0x400;
    rx.messageLengthBytes = 8;
    for (const auto &spec : {std::make_pair(QStringLiteral("Fuel Flow"), 0),
                             std::make_pair(QStringLiteral("Engine Run"), 16),
                             std::make_pair(QStringLiteral("Trip Clear"), 32)}) {
        Channel c;
        c.name = spec.first;
        c.userDefined = true;
        config.catalog().addOrUpdateUserChannel(c);
        CommsChannelRow row;
        row.channelName = spec.first;
        row.startBit = spec.second;
        row.bitLength = 16;
        rx.rows.append(row);
    }
    config.bus[0].sections.append(rx);

    IntegratorRow g;
    g.outputChannel = QStringLiteral("Fuel Used");
    g.inputChannel = QStringLiteral("Fuel Flow");
    g.enableChannel = QStringLiteral("Engine Run");
    g.resetChannel = QStringLiteral("Trip Clear");
    g.rateHz = 20;
    g.resetValue = 0;
    g.minValue = 0;
    g.maxValue = 500;
    config.integratorRows.append(g);

    // A second integrator on a fixed value — a configurable ramp, no channel.
    IntegratorRow ramp;
    ramp.outputChannel = QStringLiteral("Ramp");
    ramp.inputIsChannel = false;
    ramp.inputValue = 0.5;
    ramp.rateHz = 4;
    config.integratorRows.append(ramp);

    // A third as a DECREMENTOR: counts down from a peak and is retained across
    // power cycles. Start and reset are deliberately different numbers so a
    // mapper that conflated the two fields would fail here.
    IntegratorRow dec;
    dec.outputChannel = QStringLiteral("Fuel Left");
    dec.inputChannel = QStringLiteral("Fuel Flow");
    dec.resetChannel = QStringLiteral("Trip Clear");
    dec.countDown = true;
    dec.startValue = 60;
    dec.resetValue = 55;
    dec.minValue = 0;
    dec.maxValue = 60;
    dec.rateHz = 20;
    dec.preserveValue = true;
    config.integratorRows.append(dec);

    CHECK(config.generatedChannelNames().contains(QStringLiteral("Fuel Used")));
    const MappingResult mapped = mapToDevice(config);
    CHECK(mapped.ok());
    CHECK(mapped.tables.integrators.size() == 3);
    if (mapped.tables.integrators.size() == 3) {
        const IntegratorConfig &ic = mapped.tables.integrators[0];
        CHECK(ic.flags & INTEGFLAG_ACTIVE);
        CHECK(!(ic.flags & INTEGFLAG_CONST_INPUT));
        CHECK(ic.rate_hz == 20);
        CHECK(qAbs(ic.max_value - 500.0f) < 1e-6f);
        CHECK(ic.input_signal_idx == mapped.channelToSignal.value(QStringLiteral("fuel flow")));
        CHECK(ic.enable_signal_idx == mapped.channelToSignal.value(QStringLiteral("engine run")));
        CHECK(ic.reset_signal_idx == mapped.channelToSignal.value(QStringLiteral("trip clear")));
        CHECK(ic.dest_signal_idx == mapped.channelToSignal.value(QStringLiteral("fuel used")));

        const IntegratorConfig &rc = mapped.tables.integrators[1];
        CHECK(rc.flags & INTEGFLAG_CONST_INPUT);
        CHECK(qAbs(rc.input_const - 0.5f) < 1e-6f);
        CHECK(rc.rate_hz == 4);
        // Unused inputs must be the "none" sentinel, not slot 0 — slot 0 is a
        // real channel, and a stray 0 would gate the integrator on it.
        CHECK(rc.enable_signal_idx == SIG_MSG_NONE);
        CHECK(rc.reset_signal_idx == SIG_MSG_NONE);
        CHECK(!(rc.flags & INTEGFLAG_COUNT_DOWN));
        CHECK(!(rc.flags & INTEGFLAG_PRESERVE));

        // The decrementor: direction, preserve, and two DISTINCT value fields.
        const IntegratorConfig &dc = mapped.tables.integrators[2];
        CHECK(dc.flags & INTEGFLAG_COUNT_DOWN);
        CHECK(dc.flags & INTEGFLAG_PRESERVE);
        CHECK(qAbs(dc.start_value - 60.0f) < 1e-6f);
        CHECK(qAbs(dc.reset_value - 55.0f) < 1e-6f);
    }

    // Get reconstructs every row, including which input mode each used.
    Configuration roundTrip;
    mapFromDevice(mapped.tables, roundTrip, nullptr);
    CHECK(roundTrip.integratorRows.size() == 3);
    if (roundTrip.integratorRows.size() == 3) {
        const IntegratorRow &back = roundTrip.integratorRows[2];
        CHECK(back.countDown);
        CHECK(back.preserveValue);
        CHECK(qAbs(back.startValue - 60.0) < 1e-4);
        CHECK(qAbs(back.resetValue - 55.0) < 1e-4);
        CHECK(!roundTrip.integratorRows[0].countDown);
        CHECK(!roundTrip.integratorRows[0].preserveValue);
    }
    if (roundTrip.integratorRows.size() == 3) {
        const IntegratorRow &r = roundTrip.integratorRows[0];
        CHECK(r.outputChannel == QStringLiteral("Fuel Used"));
        CHECK(r.inputIsChannel);
        CHECK(r.inputChannel == QStringLiteral("Fuel Flow"));
        CHECK(r.enableChannel == QStringLiteral("Engine Run"));
        CHECK(r.resetChannel == QStringLiteral("Trip Clear"));
        CHECK(r.rateHz == 20);
        CHECK(qAbs(r.maxValue - 500.0) < 1e-4);

        const IntegratorRow &r2 = roundTrip.integratorRows[1];
        CHECK(!r2.inputIsChannel);
        CHECK(qAbs(r2.inputValue - 0.5) < 1e-6);
        CHECK(r2.enableChannel.isEmpty());
        CHECK(r2.resetChannel.isEmpty());
    }

    // JSON persistence round-trip.
    {
        QTemporaryFile file;
        CHECK(file.open());
        const QString path = file.fileName();
        file.close();
        QString error;
        CHECK(config.saveToFile(path, &error));
        Configuration loaded;
        CHECK(loaded.loadFromFile(path, &error));
        CHECK(loaded.integratorRows.size() == 3);
        if (loaded.integratorRows.size() == 3) {
            CHECK(loaded.integratorRows[0].inputChannel == QStringLiteral("Fuel Flow"));
            CHECK(loaded.integratorRows[0].rateHz == 20);
            CHECK(loaded.integratorRows[1].inputIsChannel == false);
            CHECK(qAbs(loaded.integratorRows[1].inputValue - 0.5) < 1e-6);
            CHECK(loaded.integratorRows[2].countDown);
            CHECK(loaded.integratorRows[2].preserveValue);
            CHECK(qAbs(loaded.integratorRows[2].startValue - 60.0) < 1e-6);
            CHECK(qAbs(loaded.integratorRows[2].resetValue - 55.0) < 1e-6);
        }
    }

    // A renamed channel has to follow every one of the four references.
    {
        Configuration renamed;
        config.copyContentTo(renamed);
        CHECK(renamed.renameChannelReferences(QStringLiteral("Fuel Flow"),
                                              QStringLiteral("Flow")) > 0);
        CHECK(renamed.integratorRows[0].inputChannel == QStringLiteral("Flow"));
    }

    // An input channel nothing writes YET still maps: reading a channel is
    // always expressible, so the input resolves to a virtual value slot holding
    // the default. It must NOT collapse to the constant-input path, which would
    // accumulate input_const forever and read back as a different row.
    {
        Configuration open;
        IntegratorRow orphan;
        orphan.outputChannel = QStringLiteral("Total");
        orphan.inputChannel = QStringLiteral("Nothing Writes This");
        open.integratorRows.append(orphan);
        const MappingResult m = mapToDevice(open);
        CHECK(m.ok());
        CHECK(m.tables.integrators.size() == 1);
        if (m.tables.integrators.size() == 1) {
            const IntegratorConfig &ic = m.tables.integrators[0];
            CHECK(!(ic.flags & INTEGFLAG_CONST_INPUT)); // still a channel input
            CHECK(ic.input_signal_idx != SIG_MSG_NONE);
            CHECK(m.signalToChannel.value(ic.input_signal_idx)
                  == QStringLiteral("Nothing Writes This"));
        }
    }

    // A BLANK channel input is still an error: there is nothing to read, and it
    // must not be mistaken for the constant-input mode.
    {
        Configuration bad;
        IntegratorRow blank;
        blank.outputChannel = QStringLiteral("Total");
        blank.inputIsChannel = true; // no inputChannel
        bad.integratorRows.append(blank);
        const MappingResult m = mapToDevice(bad);
        CHECK(!m.ok());
        CHECK(m.tables.integrators.isEmpty());
    }

    // More rows than the device has slots is an error, not a silent truncation.
    {
        Configuration full;
        for (int i = 0; i < MAX_INTEGRATORS + 1; ++i) {
            IntegratorRow r;
            r.outputChannel = QStringLiteral("Total %1").arg(i);
            r.inputIsChannel = false;
            r.inputValue = 1;
            full.integratorRows.append(r);
        }
        const MappingResult m = mapToDevice(full);
        CHECK(!m.ok());
        CHECK(m.tables.integrators.size() == MAX_INTEGRATORS);
    }

    // Feeding an integrator its own output doubles it every step instead of
    // accumulating; validation must call that out rather than let it diverge.
    // A Warning, not an Error: one row reading and writing its own slot is the
    // user's call, and errors block Send.
    {
        Configuration loop;
        IntegratorRow self;
        self.outputChannel = QStringLiteral("Total");
        self.inputChannel = QStringLiteral("Total");
        loop.integratorRows.append(self);
        bool flagged = false;
        for (const ValidationIssue &v : validateConfiguration(loop)) {
            if (v.severity == ValidationIssue::Warning
                && v.message.contains(QStringLiteral("same channel")))
                flagged = true;
            CHECK(v.severity != ValidationIssue::Error);
        }
        CHECK(flagged);
    }

    // v17: counters and integrators share ONE 20-entry preserve ring, so the
    // budget has to be counted across both. 18 counters + 8 integrators is
    // under the limit for each table alone but over it together — the case a
    // per-table check would wave through and the device would silently trim.
    {
        const auto preservedCount = [](int counters, int integrators) {
            Configuration cfg;
            for (int i = 0; i < counters; ++i) {
                CounterRow c;
                c.outputChannel = QStringLiteral("C%1").arg(i);
                c.preserveValue = true;
                cfg.counterRows.append(c);
            }
            for (int i = 0; i < integrators; ++i) {
                IntegratorRow g;
                g.outputChannel = QStringLiteral("I%1").arg(i);
                g.inputIsChannel = false;
                g.inputValue = 1;
                g.preserveValue = true;
                cfg.integratorRows.append(g);
            }
            int errors = 0;
            for (const ValidationIssue &v : validateConfiguration(cfg))
                if (v.severity == ValidationIssue::Error
                    && v.message.contains(QStringLiteral("Preserve value enabled")))
                    ++errors;
            return errors;
        };
        CHECK(preservedCount(18, 8) == 1); // 26 total — over the shared ring
        CHECK(preservedCount(18, 2) == 0); // 20 total — exactly the limit
        CHECK(preservedCount(0, 8) == 0);  // integrators alone always fit
    }

    // A preserved integrator with no reset input can never be cleared at all —
    // a power cycle no longer helps, which is worth saying differently.
    {
        Configuration cfg;
        IntegratorRow g;
        g.outputChannel = QStringLiteral("Odometer");
        g.inputIsChannel = false;
        g.inputValue = 1;
        g.preserveValue = true;
        cfg.integratorRows.append(g);
        bool flagged = false;
        for (const ValidationIssue &v : validateConfiguration(cfg))
            if (v.message.contains(QStringLiteral("nothing can ever clear")))
                flagged = true;
        CHECK(flagged);
    }

    // A count-down integrator starting at or below its floor is finished before
    // it begins — the "forgot to set the peak" mistake.
    {
        Configuration cfg;
        IntegratorRow g;
        g.outputChannel = QStringLiteral("Fuel Left");
        g.inputIsChannel = false;
        g.inputValue = 1;
        g.countDown = true;
        g.startValue = 0; // never set
        g.minValue = 0;
        g.maxValue = 60;
        cfg.integratorRows.append(g);
        bool flagged = false;
        for (const ValidationIssue &v : validateConfiguration(cfg))
            if (v.message.contains(QStringLiteral("already at or below")))
                flagged = true;
        CHECK(flagged);
    }
}

static void testDbcImport()
{
    const QString dbc = QStringLiteral(R"DBC(VERSION "unit-test"

BO_ 1600 EngineData: 8 ECU
 SG_ EngineRPM : 0|16@1+ (1,0) [0|20000] "rpm" Dash
 SG_ EngineTemp : 23|16@0- (0.1,-40) [-40|215] "degC" Dash
 SG_ BoostF : 32|32@1+ (1,0) [0|500] "kPa" Dash

BO_ 2147483939 ExtMsg: 8 ECU
 SG_ Foo : 8|8@1+ (1,0) [0|255] "" Vector__XXX

BO_ 512 MuxMsg: 8 ECU
 SG_ Selector M : 0|8@1+ (1,0) [0|255] "" Vector__XXX
 SG_ ValA m0 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX
 SG_ ValB m1 : 8|16@1+ (1,0) [0|65535] "" Vector__XXX

SIG_VALTYPE_ 1600 BoostF : 1;
)DBC");

    QStringList warnings;
    const DbcFile file = parseDbc(dbc, &warnings);
    CHECK(file.version == QStringLiteral("unit-test"));
    CHECK(file.messages.size() == 3);

    // --- Messages come back in arbitration-id order, lowest first ---
    // The file lists them 1600, 2147483939, 512; the import sorts them. Note
    // which one lands FIRST: ExtMsg's raw id is 0x80000123, whose top bit is
    // only the extended-frame flag, so its actual arbitration id is 0x123 —
    // the lowest of the three. Sorting on the raw id instead would file it
    // last, which is exactly the mistake this pins.
    CHECK(file.messages[0].name == QStringLiteral("ExtMsg"));      // 0x123
    CHECK(file.messages[1].name == QStringLiteral("MuxMsg"));      // 0x200
    CHECK(file.messages[2].name == QStringLiteral("EngineData"));  // 0x640
    for (int i = 0; i + 1 < file.messages.size(); ++i)
        CHECK(file.messages[i].canId <= file.messages[i + 1].canId);

    // Everything below looks messages up BY NAME, so these assertions test the
    // parsing rather than the ordering and do not have to move if the sort ever
    // changes again.
    const auto byName = [&file](const char *name) -> const DbcMessage & {
        for (const DbcMessage &m : file.messages)
            if (m.name == QLatin1String(name))
                return m;
        static const DbcMessage none;
        return none;
    };

    // --- Message + signal parsing ---
    const DbcMessage &eng = byName("EngineData");
    CHECK(eng.name == QStringLiteral("EngineData"));
    CHECK(eng.canId == 0x640u);
    CHECK(!eng.extended);
    CHECK(eng.dlc == 8);
    CHECK(eng.signalList.size() == 3);
    const DbcSignal &rpm = eng.signalList[0];
    CHECK(rpm.name == QStringLiteral("EngineRPM"));
    CHECK(rpm.startBit == 0 && rpm.bitLength == 16);
    CHECK(!rpm.bigEndian && !rpm.isSigned);
    CHECK(qAbs(rpm.factor - 1.0) < 1e-9);
    CHECK(rpm.valueType == 0); // no SIG_VALTYPE_ marker — plain integer
    const DbcSignal &temp = eng.signalList[1];
    CHECK(temp.bigEndian && temp.isSigned);
    CHECK(temp.startBit == 23 && temp.bitLength == 16);
    CHECK(qAbs(temp.factor - 0.1) < 1e-9 && qAbs(temp.offset + 40.0) < 1e-9);

    // --- SIG_VALTYPE_ float marker -> IEEE754 comms row + float channel ---
    const DbcSignal &boost = eng.signalList[2];
    CHECK(boost.valueType == 1);
    {
        const CommsChannelRow r = rowFromDbcSignal(boost, QStringLiteral("Boost"));
        CHECK(r.dbcType == int(DbcType::IEEE754));
        CHECK(r.bitLength == 32);
        const Channel c = channelFromDbcSignal(boost, QStringLiteral("Boost"));
        CHECK(c.dataType == QStringLiteral("float"));
    }

    const DbcMessage &ext = byName("ExtMsg");
    CHECK(ext.extended);
    CHECK(ext.canId == 0x123u); // 2147483939 = 0x80000123, bit31 = extended flag

    // --- Motorola MSB -> LSB start-bit conversion (the crux) ---
    // Intel is identity.
    CHECK(dbcStartBitToLsb(16, 16, false) == 16);
    // Motorola 16-bit whose MSB is byte 0 bit 7 (DBC start 7) has its LSB at
    // byte 1 bit 0 = app start 8 (value = data[0]<<8 | data[1]).
    CHECK(dbcStartBitToLsb(7, 16, true) == 8);
    // MSB at byte 2 bit 7 (DBC start 23) -> LSB at byte 3 bit 0 = 24.
    CHECK(dbcStartBitToLsb(23, 16, true) == 24);
    // MSB at byte 1 bit 7 (DBC start 15) -> LSB at byte 2 bit 0 = 16.
    CHECK(dbcStartBitToLsb(15, 16, true) == 16);

    // Converted rows must satisfy computeExtraction with the right byte order.
    {
        const CommsChannelRow r = rowFromDbcSignal(temp, QStringLiteral("Engine Temp"));
        CHECK(r.startBit == 24); // 23 (MSB) -> 24 (LSB)
        CHECK(r.dbcType == int(DbcType::Signed));
        ExtractionFields f;
        QString reason;
        CHECK(computeExtraction(r, alignmentForDbcSignal(temp), 8, &f, &reason));
        CHECK(f.byteOrder == 1 && f.startBit == 24 && f.valueType == SIGNAL_TYPE_INT16);
    }
    {
        const CommsChannelRow r = rowFromDbcSignal(rpm, QStringLiteral("Engine RPM"));
        CHECK(r.startBit == 0);
        ExtractionFields f;
        QString reason;
        CHECK(computeExtraction(r, alignmentForDbcSignal(rpm), 8, &f, &reason));
        CHECK(f.byteOrder == 0 && f.startBit == 0);
    }

    // --- Channel derivation ---
    const Channel tch = channelFromDbcSignal(temp, QStringLiteral("Engine Temp"));
    CHECK(tch.quantity == QStringLiteral("Temperature")); // "degC"
    CHECK(tch.dataType == QStringLiteral("s16"));
    CHECK(tch.decimalPlaces == 1); // factor 0.1
    CHECK(quantityForUnit(QStringLiteral("rpm")) == QStringLiteral("Rotational Speed"));
    CHECK(quantityForUnit(QStringLiteral("kPa")) == QStringLiteral("Pressure and Stress"));
    CHECK(quantityForUnit(QString()) == QStringLiteral("Unitless"));

    // --- v14 multi-term conditions: JSON, back-compat, and the shared fold ---
    {
        // The bracketing helper is what the report and the editor both render,
        // so it has to spell out the LEFT-TO-RIGHT grouping.
        CHECK(joinConditionTerms({QStringLiteral("A")}, {}, QStringLiteral("AND"),
                                 QStringLiteral("OR"))
              == QStringLiteral("A"));
        CHECK(joinConditionTerms({QStringLiteral("A"), QStringLiteral("B")},
                                 {int(COND_JOIN_AND)}, QStringLiteral("AND"),
                                 QStringLiteral("OR"))
              == QStringLiteral("A AND B"));
        CHECK(joinConditionTerms({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")},
                                 {int(COND_JOIN_AND), int(COND_JOIN_OR)}, QStringLiteral("AND"),
                                 QStringLiteral("OR"))
              == QStringLiteral("(A AND B) OR C"));
        // The case where left-to-right and C precedence differ must render with
        // the brackets that show which one this app means.
        CHECK(joinConditionTerms({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")},
                                 {int(COND_JOIN_OR), int(COND_JOIN_AND)}, QStringLiteral("AND"),
                                 QStringLiteral("OR"))
              == QStringLiteral("(A OR B) AND C"));

        // A pre-modes file holds one expression and no mode, and MIGRATES to a
        // Set/Reset whose Reset is the logical inverse of its Set. That pairing
        // is what makes the migrated condition behave exactly like the level it
        // used to be — it sets whenever the expression holds and resets whenever
        // it does not — and testMigratedResetIsTheExactInverse proves the
        // inversion itself over every operator and joiner combination.
        QJsonObject legacy;
        legacy["aChannel"] = QStringLiteral("Engine RPM");
        legacy["op"] = int(COND_OP_GT);
        legacy["bIsChannel"] = false;
        legacy["bConst"] = 5000.0;
        legacy["outputChannel"] = QStringLiteral("RPM High");
        legacy["active"] = true;
        const ConditionRow fromOld = ConditionRow::fromJson(legacy);
        CHECK(fromOld.mode == ConditionMode::SetReset);
        CHECK(fromOld.setTerms.size() == 1);
        CHECK(fromOld.setJoiners.isEmpty());
        CHECK(fromOld.setTerms[0].aChannel == QStringLiteral("Engine RPM"));
        CHECK(fromOld.setTerms[0].op == int(COND_OP_GT));
        CHECK(qFuzzyCompare(fromOld.setTerms[0].bConst, 5000.0));
        // > inverts to <=, on the same operands.
        CHECK(fromOld.resetTerms.size() == 1);
        CHECK(fromOld.resetTerms[0].op == int(COND_OP_LTE));
        CHECK(fromOld.resetTerms[0].aChannel == QStringLiteral("Engine RPM"));
        CHECK(qFuzzyCompare(fromOld.resetTerms[0].bConst, 5000.0));
        CHECK(fromOld.outputChannel == QStringLiteral("RPM High"));

        // A modes row round-trips through JSON with both expressions intact.
        ConditionRow multi;
        ConditionTermRow t1, t2, t3;
        t1.aChannel = QStringLiteral("RPM");
        t1.op = int(COND_OP_LT);
        t1.bConst = 1500;
        t2.aChannel = QStringLiteral("RPM");
        t2.op = int(COND_OP_GT);
        t2.bConst = 100;
        t3.aChannel = QStringLiteral("TPS");
        t3.op = int(COND_OP_LT);
        t3.bConst = 1;
        multi.setTerms = {t1, t2, t3};
        multi.setJoiners = {int(COND_JOIN_AND), int(COND_JOIN_OR)};
        multi.resetTerms = {t3};
        multi.resetJoiners = {};
        multi.outputChannel = QStringLiteral("InBand");
        const ConditionRow back = ConditionRow::fromJson(multi.toJson());
        CHECK(back.mode == ConditionMode::SetReset);
        CHECK(back.setTerms.size() == 3);
        CHECK(back.setJoiners == multi.setJoiners);
        CHECK(back.setTerms[2].aChannel == QStringLiteral("TPS"));
        CHECK(back.resetTerms.size() == 1);
        CHECK(back.resetTerms[0].aChannel == QStringLiteral("TPS"));
        CHECK(back.inputChannels().contains(QStringLiteral("TPS")));

        // A Momentary carries its latch frequency, clamped to what the device
        // can spend against its 10 ms pass.
        ConditionRow mom;
        mom.mode = ConditionMode::Momentary;
        mom.latchHz = 20;
        mom.setTerms = {t1};
        mom.outputChannel = QStringLiteral("Pulse");
        const ConditionRow momBack = ConditionRow::fromJson(mom.toJson());
        CHECK(momBack.mode == ConditionMode::Momentary);
        CHECK(momBack.latchHz == 20);
        QJsonObject silly = mom.toJson();
        silly["latchHz"] = 5000;
        CHECK(ConditionRow::fromJson(silly).latchHz == int(COND_LATCH_MAX_HZ));

        // The joiner-per-gap invariant is repaired on load, so a hand-edited or
        // truncated file can't produce an expression the mapper can't emit.
        QJsonObject ragged = multi.toJson();
        QJsonObject raggedSet = ragged["set"].toObject();
        raggedSet["joiners"] = QJsonArray{}; // gaps with no joiners
        ragged["set"] = raggedSet;
        const ConditionRow fixed = ConditionRow::fromJson(ragged);
        CHECK(fixed.setTerms.size() == 3);
        CHECK(fixed.setJoiners.size() == 2);
        CHECK(fixed.setJoiners[0] == int(COND_JOIN_AND)); // defaults to AND

        // Renaming a channel reaches every comparison of BOTH expressions, not
        // just the first, and not just the Set.
        Configuration rn;
        rn.conditionRows.append(multi);
        rn.renameChannelReferences(QStringLiteral("TPS"), QStringLiteral("Throttle"));
        CHECK(rn.conditionRows[0].setTerms[2].aChannel == QStringLiteral("Throttle"));
        CHECK(rn.conditionRows[0].resetTerms[0].aChannel == QStringLiteral("Throttle"));
        rn.renameChannelReferences(QStringLiteral("RPM"), QStringLiteral("EngineSpeed"));
        CHECK(rn.conditionRows[0].setTerms[0].aChannel == QStringLiteral("EngineSpeed"));
        CHECK(rn.conditionRows[0].setTerms[1].aChannel == QStringLiteral("EngineSpeed"));
    }

    // --- Storage type is sized from the PHYSICAL range, not the bit width ---
    // Picking by raw width silently truncates a scaled channel: the firmware
    // clamps every reading to the channel's min/max, so a too-small type caps
    // the signal far below what it can actually carry.
    {
        // The real-world case: 16-bit raw at 0.036 km/h per bit spans
        // 0..2359.26 and needs 3 dp. u16 at 3 dp reaches only 65.535, so the
        // type must widen to u32 (which reaches 4294967.295).
        DbcSignal speed;
        speed.name = QStringLiteral("WheelSpeed");
        speed.startBit = 0;
        speed.bitLength = 16;
        speed.isSigned = false;
        speed.factor = 0.036;
        speed.offset = 0.0;
        speed.unit = QStringLiteral("km/h");
        double lo = 0, hi = 0;
        dbcPhysicalRange(speed, &lo, &hi);
        CHECK(qAbs(lo) < 1e-9);
        CHECK(qAbs(hi - 2359.26) < 1e-6);
        const Channel sc = channelFromDbcSignal(speed, QStringLiteral("Wheel Speed"));
        CHECK(sc.decimalPlaces == 3);
        CHECK(sc.dataType == QStringLiteral("u32"));
        CHECK(qAbs(sc.maxValue - 2359.26) < 1e-6); // NOT clamped to 65.535
        // The chosen type really can represent the whole range at 3 dp.
        CHECK(sc.maxValue <= 4294967295.0 * sc.baseResolution + 1e-6);

        // An UNSIGNED field with a negative offset is a signed channel: the
        // classic temperature signal spans -40..215, which no u8 can hold and
        // which s8 (max 127) cannot either — s16 is the smallest that fits.
        DbcSignal temp;
        temp.bitLength = 8;
        temp.isSigned = false;
        temp.factor = 1.0;
        temp.offset = -40.0;
        dbcPhysicalRange(temp, &lo, &hi);
        CHECK(qAbs(lo + 40.0) < 1e-9 && qAbs(hi - 215.0) < 1e-9);
        const Channel tc = channelFromDbcSignal(temp, QStringLiteral("Coolant Temp"));
        CHECK(tc.dataType == QStringLiteral("s16"));
        CHECK(qAbs(tc.minValue + 40.0) < 1e-9);

        // A plain 8-bit counter still lands on the smallest type.
        DbcSignal small;
        small.bitLength = 8;
        small.isSigned = false;
        small.factor = 1.0;
        CHECK(channelFromDbcSignal(small, QStringLiteral("Ctr")).dataType
              == QStringLiteral("u8"));

        // A declared [min|max] is the author's intent and still drives sizing:
        // a narrower declared range can legitimately fit a smaller type than
        // the field's full encodable span would need.
        DbcSignal declared = speed;
        declared.minValue = 0.0;
        declared.maxValue = 60.0; // 60000 counts at 3 dp — inside u16's 65535
        const Channel dc = channelFromDbcSignal(declared, QStringLiteral("Decl"));
        CHECK(qAbs(dc.maxValue - 60.0) < 1e-9);
        CHECK(dc.dataType == QStringLiteral("u16"));
        DbcSignal declaredWide = speed;
        declaredWide.minValue = 0.0;
        declaredWide.maxValue = 100.0; // 100000 counts at 3 dp — needs u32
        CHECK(channelFromDbcSignal(declaredWide, QStringLiteral("DeclW")).dataType
              == QStringLiteral("u32"));

        // A 1-bit flag stays boolean; a range no integer type can hold is float.
        DbcSignal flag;
        flag.bitLength = 1;
        flag.isSigned = false;
        flag.factor = 1.0;
        CHECK(channelFromDbcSignal(flag, QStringLiteral("Flag")).dataType
              == QStringLiteral("boolean"));

        // An IEEE754 signal's raw bits ARE the value, so the integer-span
        // derivation must NOT be applied to it — doing so would hand the
        // firmware a 0..4.29e9 clamp and pin every negative reading to 0.
        DbcSignal fsig;
        fsig.bitLength = 32;
        fsig.isSigned = false;
        fsig.factor = 1.0;
        fsig.valueType = 1; // SIG_VALTYPE_ float
        const Channel fc = channelFromDbcSignal(fsig, QStringLiteral("FloatSig"));
        CHECK(fc.dataType == QStringLiteral("float"));
        CHECK(fc.minValue < 0.0); // negative values must survive the clamp
        CHECK(qAbs(fc.minValue + 1e9) < 1.0 && qAbs(fc.maxValue - 1e9) < 1.0);

        // A span too wide for any integer type falls back to float, and the
        // range is kept INTACT: the dialogs' +/-1e9 float span is a display
        // convention, not a storage limit (min_val/max_val reach the device as
        // float32), so trimming to it would clamp away most of a wide signal —
        // the very failure this sizing work removes.
        DbcSignal odo;
        odo.bitLength = 32;
        odo.isSigned = false;
        odo.factor = 0.125; // 0..536870911.875 at 3 dp — past u32's reach
        const Channel oc = channelFromDbcSignal(odo, QStringLiteral("Odo"));
        CHECK(oc.dataType == QStringLiteral("float"));
        CHECK(qAbs(oc.maxValue - 536870911.875) < 1e-3);

        // J1939 High Resolution Total Vehicle Distance: 32-bit at 5 m/bit spans
        // 0..2.1e10 — an order of magnitude past the nominal float span. It must
        // survive whole, and its endpoints must stay correctly ordered (an
        // inverted pair makes the firmware pin every reading to max_val,
        // because it applies the min clamp first and the max clamp second).
        DbcSignal dist;
        dist.bitLength = 32;
        dist.isSigned = false;
        dist.factor = 5.0;
        const Channel dch = channelFromDbcSignal(dist, QStringLiteral("Distance"));
        CHECK(dch.dataType == QStringLiteral("float"));
        CHECK(dch.minValue < dch.maxValue);
        CHECK(dch.maxValue > 2.1e10);
        CHECK(storageTypeHoldsRange(dch.dataType, dch.minValue, dch.maxValue,
                                    dch.decimalPlaces)); // not falsely flagged

        // A span lying WHOLLY above the nominal float range (an epoch-second
        // base offset) must also come out well-ordered rather than inverted.
        DbcSignal epoch;
        epoch.bitLength = 32;
        epoch.isSigned = false;
        epoch.factor = 1.0;
        epoch.offset = 1500000000.0;
        const Channel ech = channelFromDbcSignal(epoch, QStringLiteral("AbsTime"));
        CHECK(ech.minValue < ech.maxValue);
        CHECK(ech.minValue >= 1.5e9);
        CHECK(storageTypeForRange(0.0, 1e12, 0) == QStringLiteral("float"));
        // Precision beyond a type's cap disqualifies it even when the range fits:
        // 0..2 at 4 dp is 20000 counts, past u8/s8's 2-dp cap, so u16 wins.
        CHECK(storageTypeForRange(0.0, 2.0, 4) == QStringLiteral("u16"));
        // Exact boundary: u16 at 3 dp reaches exactly 65.535.
        CHECK(storageTypeForRange(0.0, 65.535, 3) == QStringLiteral("u16"));
        CHECK(storageTypeForRange(0.0, 65.536, 3) == QStringLiteral("u32"));

        // End-to-end: the channel's range becomes the firmware's clamp
        // (CanSignalConfig::min_val/max_val, applied in applySignalScaling), so
        // the whole point of the sizing fix is that this reaches the device as
        // the signal's real span and not 65.535.
        Configuration scfg;
        scfg.bus[0].enabled = true;
        scfg.catalog().addOrUpdateUserChannel(sc);
        CommsSection srx;
        srx.name = QStringLiteral("Speed");
        srx.device = SectionDevice::ReceiveMessage;
        srx.alignment = SectionAlignment::WordSwap;
        srx.baseAddress = 0x200;
        srx.messageLengthBytes = 8;
        srx.rows.append(rowFromDbcSignal(speed, sc.name));
        scfg.bus[0].sections.append(srx);
        const MappingResult smr = mapToDevice(scfg);
        CHECK(smr.ok());
        const int sIdx = smr.channelToSignal.value(sc.name.toLower(), -1);
        CHECK(sIdx >= 0);
        if (sIdx >= 0) {
            const CanSignalConfig &ss = smr.tables.signalConfigs[sIdx];
            CHECK(qAbs(ss.factor - 0.036f) < 1e-6f);
            CHECK(ss.max_val > 2359.0f);  // was 65.535 before the sizing fix
            CHECK(qAbs(ss.min_val) < 1e-6f);
            // The top raw code must survive the clamp: 65535 * 0.036 = 2359.26.
            CHECK(65535.0f * ss.factor + ss.offset <= ss.max_val);
        }

        // The range became the USER'S to declare (the editor's two fields are
        // editable now), so a narrowed plausibility range - tighter than the
        // type span - must reach the device exactly as typed, not re-derived
        // back to what the type can carry.
        {
            DbcSignal rpmSig;
            rpmSig.bitLength = 16;
            rpmSig.isSigned = false;
            rpmSig.factor = 1.0;
            Channel rpm = channelFromDbcSignal(rpmSig, QStringLiteral("Engine RPM"));
            rpm.minValue = 0.0;    // the user's declared range,
            rpm.maxValue = 8000.0; // far inside u16's 0..65535
            Configuration ncfg;
            ncfg.bus[0].enabled = true;
            ncfg.catalog().addOrUpdateUserChannel(rpm);
            CommsSection nrx;
            nrx.name = QStringLiteral("RPM Rx");
            nrx.device = SectionDevice::ReceiveMessage;
            nrx.alignment = SectionAlignment::WordSwap; // the DBC row is Intel-coded
            nrx.baseAddress = 0x201;
            nrx.messageLengthBytes = 8;
            nrx.rows.append(rowFromDbcSignal(rpmSig, rpm.name));
            ncfg.bus[0].sections.append(nrx);
            const MappingResult nmr = mapToDevice(ncfg);
            CHECK(nmr.ok());
            const int nIdx = nmr.channelToSignal.value(rpm.name.toLower(), -1);
            CHECK(nIdx >= 0);
            if (nIdx >= 0) {
                const CanSignalConfig &ns = nmr.tables.signalConfigs[nIdx];
                CHECK(qAbs(ns.min_val) < 1e-6f);
                CHECK(qAbs(ns.max_val - 8000.0f) < 1e-3f);
            }

            // And an INVERTED range on a used channel is an Error before it
            // reaches hardware: the firmware applies the min clamp first and
            // the max clamp second, so min >= max pins every reading to max.
            rpm.minValue = 8000.0;
            rpm.maxValue = 0.0;
            ncfg.catalog().addOrUpdateUserChannel(rpm);
            bool rangeFlagged = false;
            for (const ValidationIssue &v : validateConfiguration(ncfg))
                if (v.severity == ValidationIssue::Error
                    && v.message.contains(QStringLiteral("pin every reading")))
                    rangeFlagged = true;
            CHECK(rangeFlagged);

            // An unused channel with the same inverted pair clamps nothing and
            // is not an Error - it reaches no signal row.
            Configuration ucfg;
            ucfg.bus[0].enabled = true;
            ucfg.catalog().addOrUpdateUserChannel(rpm);
            bool unusedFlagged = false;
            for (const ValidationIssue &v : validateConfiguration(ucfg))
                if (v.severity == ValidationIssue::Error
                    && v.message.contains(QStringLiteral("pin every reading")))
                    unusedFlagged = true;
            CHECK(!unusedFlagged);
        }

        // Get-from-device must size the reconstructed channel the same way, or
        // a Get would overwrite the catalogue with a bit-width guess that
        // contradicts the range it is paired with (and re-introduce the clamp
        // the next time the channel is edited).
        Configuration tcfg;
        tcfg.bus[0].enabled = true;
        tcfg.catalog().addOrUpdateUserChannel(tc); // Coolant Temp, -40..215
        CommsSection trx;
        trx.name = QStringLiteral("Temp");
        trx.device = SectionDevice::ReceiveMessage;
        trx.alignment = SectionAlignment::WordSwap;
        trx.baseAddress = 0x201;
        trx.messageLengthBytes = 8;
        trx.rows.append(rowFromDbcSignal(temp, tc.name));
        tcfg.bus[0].sections.append(trx);
        const MappingResult tmr = mapToDevice(tcfg);
        CHECK(tmr.ok());
        Configuration tback;
        mapFromDevice(tmr.tables, tback);
        const Channel rt = tback.catalog().findByName(tc.name);
        CHECK(rt.isValid());
        CHECK(rt.minValue < -39.0); // the negative range survived
        // Whatever type it came back as must be able to hold that range.
        CHECK(storageTypeForRange(rt.minValue, rt.maxValue, 0) == rt.dataType);
        CHECK(rt.dataType != QStringLiteral("u8")); // the old width guess

        // The Channel Editor flags an existing mis-sized channel using this
        // predicate, so it has to agree with the sizing rule in both
        // directions: what storageTypeForRange picks always holds the range,
        // and the too-small types it rejected never do.
        CHECK(storageTypeHoldsRange(QStringLiteral("u32"), 0.0, 2359.26, 3));
        CHECK(!storageTypeHoldsRange(QStringLiteral("u16"), 0.0, 2359.26, 3)); // the reported bug
        CHECK(!storageTypeHoldsRange(QStringLiteral("u8"), -40.0, 215.0, 0));  // negative offset
        CHECK(!storageTypeHoldsRange(QStringLiteral("u8"), 0.0, 102.0, 1));    // throttle 0.4%
        CHECK(storageTypeHoldsRange(QStringLiteral("u16"), 0.0, 65.535, 3));   // exact boundary
        CHECK(storageTypeHoldsRange(QStringLiteral("u16"), 0.0, 2.0, 4));      // 4 dp is u16's cap
        CHECK(!storageTypeHoldsRange(QStringLiteral("u8"), 0.0, 2.0, 4));      // past u8's 2 dp cap
        CHECK(storageTypeHoldsRange(QString(), 0.0, 1e9, 0));  // type not chosen yet: no warning
        CHECK(storageTypeHoldsRange(QStringLiteral("float"), -1e9, 1e9, 0));
        CHECK(storageTypeHoldsRange(QStringLiteral("boolean"), 0.0, 1.0, 0));
        CHECK(!storageTypeHoldsRange(QStringLiteral("boolean"), 0.0, 5.0, 0));
    }

    // --- Multiplexor selectors ---
    const DbcMessage &mux = byName("MuxMsg");
    CHECK(mux.hasMultiplexing());
    CHECK(mux.multiplexor() != nullptr);
    CHECK(mux.multiplexor()->name == QStringLiteral("Selector"));
    {
        int off = -1;
        quint32 id = 0, mask = 0;
        QString reason;
        CHECK(muxSelectorForValue(*mux.multiplexor(), 1, &off, &id, &mask, &reason));
        CHECK(off == 0 && mask == 0xFFu && id == 1u); // byte-0, full-byte selector
    }
    {
        // Sub-byte Intel multiplexor: bits 4..7 of byte 0.
        DbcSignal m;
        m.startBit = 4;
        m.bitLength = 4;
        m.bigEndian = false;
        int off = -1;
        quint32 id = 0, mask = 0;
        CHECK(muxSelectorForValue(m, 3, &off, &id, &mask, nullptr));
        CHECK(off == 0 && mask == 0xF0u && id == 0x30u); // 3 << 4
    }
    {
        // A Motorola multiplexor spanning multiple bytes can't be mapped.
        DbcSignal m;
        m.startBit = 7;
        m.bitLength = 16;
        m.bigEndian = true;
        int off = -1;
        quint32 id = 0, mask = 0;
        CHECK(!muxSelectorForValue(m, 0, &off, &id, &mask, nullptr));
    }

    // --- Full DBC multiplexed message -> compound section -> device tables ---
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection cs;
    cs.name = QStringLiteral("MuxMsg");
    cs.device = SectionDevice::ReceiveMessage;
    cs.alignment = SectionAlignment::WordSwap; // Intel
    cs.baseAddress = mux.canId;
    cs.messageLengthBytes = mux.dlc;
    cs.compound = true;
    // Compound has no always-present set: the non-muxed multiplexor "Selector"
    // is replicated into every identifier (each decodes under its own selector).
    QList<CommsChannelRow> commonRows;
    for (const DbcSignal &s : mux.signalList) {
        cfg.catalog().addOrUpdateUserChannel(channelFromDbcSignal(s, s.name));
        if (!s.isMultiplexed)
            commonRows.append(rowFromDbcSignal(s, s.name));
    }
    for (const DbcSignal &s : mux.signalList) {
        if (!s.isMultiplexed)
            continue;
        int off = 0;
        quint32 id = 0, mask = 0;
        QString reason;
        CHECK(muxSelectorForValue(*mux.multiplexor(), s.muxValue, &off, &id, &mask, &reason));
        CompoundIdentifier ci;
        ci.byteOffset = off;
        ci.id = id;
        ci.idMask = mask;
        ci.configured = true;
        ci.rows = commonRows; // replicate Selector
        ci.rows.append(rowFromDbcSignal(s, s.name));
        cs.identifiers.append(ci);
    }
    cfg.bus[0].sections.append(cs);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    // 2 identifiers × (Selector + one Val) = 4 signals; Selector appears twice
    // (a distinct gated receive signal per identifier). Plus the device
    // channels, which ride every Send regardless of what the document says.
    CHECK(mr.tables.signalConfigs.size() == 4 + DEVCH_COUNT);
    const int selIdx = mr.channelToSignal.value(QStringLiteral("selector"), -1);
    const int aIdx = mr.channelToSignal.value(QStringLiteral("vala"), -1);
    CHECK(selIdx >= 0 && aIdx >= 0);
    if (selIdx >= 0)
        CHECK(mr.tables.signalConfigs[selIdx].mux_mask == 0xFFu); // gated now, not always-present
    if (aIdx >= 0) {
        CHECK(mr.tables.signalConfigs[aIdx].mux_mask == 0xFFu);
        CHECK(mr.tables.signalConfigs[aIdx].mux_id == 0u); // ValA is m0
    }

    // Round-trip back through Get: the tables rebuild a compound section with
    // no always-present rows (Selector is inside each identifier).
    Configuration recon;
    mapFromDevice(mr.tables, recon);
    bool found = false;
    for (int bi = 0; bi < 3 && !found; ++bi)
        for (const CommsSection &s : recon.bus[bi].sections)
            if (s.baseAddress == mux.canId) {
                found = true;
                CHECK(s.compound);
                CHECK(s.rows.isEmpty());          // no always-present set
                CHECK(s.identifiers.size() == 2); // each has Selector + its Val
            }
    CHECK(found);

    // v10: a mux mask on a TRANSMIT signal reconstructs as a compound transmit
    // section (one identifier); the MSGFLAG_TX_SEQUENTIAL flag maps to the mode.
    {
        DeviceTables t;
        CanMessageConfig m{};
        m.can_id = 0x7A0;
        m.flags = MSGFLAG_ACTIVE | MSGFLAG_TRANSMIT | MSGFLAG_TX_SEQUENTIAL;
        m.src_bus = 2;
        m.dlc = 8;
        t.messages.append(m);
        CanSignalConfig s{};
        sigSetHeader(s, 0, 0, 1);
        sigSetBits(s, 0, 8, SIGNAL_TYPE_UINT8, 0, 0);
        s.factor = 1.0f;
        s.max_val = 255.0f;
        s.mux_mask = 0x0F; // gated transmit signal -> compound transmit variant
        s.mux_id = 1;
        std::memcpy(s.label, "Gated Tx", 8);
        t.signalConfigs.append(s);
        Configuration r2;
        mapFromDevice(t, r2);
        bool ok = false;
        for (int bi = 0; bi < 3; ++bi)
            for (const CommsSection &sec : r2.bus[bi].sections)
                if (sec.baseAddress == 0x7A0)
                    ok = sec.compound && sec.isTransmit() && sec.identifiers.size() == 1
                         && sec.compoundTxMode == CompoundTxMode::Sequential;
        CHECK(ok);
    }

    // A compound message that mixes an ungated (mux_mask 0) signal with a gated
    // one (a legacy/foreign layout) folds the ungated signal into the identifier
    // — compound sections carry channels only inside identifiers, so nothing is
    // stranded in the now-hidden section.rows.
    {
        DeviceTables t;
        CanMessageConfig m{};
        m.can_id = 0x210;
        m.flags = MSGFLAG_ACTIVE;
        m.src_bus = 1;
        m.dlc = 8;
        t.messages.append(m);
        CanSignalConfig common{}; // ungated (always-present, legacy)
        sigSetHeader(common, 0, 0, 1);
        sigSetBits(common, 0, 8, SIGNAL_TYPE_UINT8, 0, 0);
        common.factor = 1.0f;
        common.max_val = 255.0f;
        common.mux_mask = 0;
        std::memcpy(common.label, "Common", 6);
        t.signalConfigs.append(common);
        CanSignalConfig gated{}; // gated -> makes the message compound
        sigSetHeader(gated, 0, 0, 1);
        sigSetBits(gated, 8, 16, SIGNAL_TYPE_UINT16, 0, 0);
        gated.factor = 1.0f;
        gated.max_val = 65535.0f;
        gated.mux_id = 1;
        gated.mux_mask = 0xFF;
        std::memcpy(gated.label, "RPM", 3);
        t.signalConfigs.append(gated);
        Configuration recon;
        mapFromDevice(t, recon);
        bool ok = false;
        for (int bi = 0; bi < 3; ++bi)
            for (const CommsSection &s : recon.bus[bi].sections)
                if (s.baseAddress == 0x210)
                    ok = s.compound && s.rows.isEmpty() && s.identifiers.size() == 1
                         && s.identifiers[0].rows.size() == 2; // Common folded in + RPM
        CHECK(ok);
    }

    // A gated (compound) receive path still warns when two distinct over-long
    // channels truncate to the same device label (would merge on read-back).
    // The editors cap names now; this is the backstop for older documents.
    {
        Configuration cfg;
        cfg.bus[0].enabled = true;
        // 36 bytes each, identical for the first 31 — the pair has to be
        // rebuilt for every label width, because names that collided at 15
        // bytes ("...SensorBank1/2" shares its first 15) fit comfortably inside
        // 31 and no longer collide at all. That is the widening working, not
        // the check failing.
        const QString n1 = QStringLiteral("EngineCoolantTemperatureSensorsBank1"); // 36 bytes
        const QString n2 = QStringLiteral("EngineCoolantTemperatureSensorsBank2"); // same first 31
        CHECK(n1.toUtf8().size() == 36 && n2.toUtf8().size() == 36);
        CHECK(n1.left(MAX_CHANNEL_NAME_BYTES) == n2.left(MAX_CHANNEL_NAME_BYTES));
        for (const QString &n : {n1, n2}) {
            Channel c;
            c.name = n;
            c.userDefined = true;
            cfg.catalog().addOrUpdateUserChannel(c);
        }
        CommsSection cs;
        cs.name = QStringLiteral("Rx 0x400");
        cs.device = SectionDevice::ReceiveMessage;
        cs.alignment = SectionAlignment::WordSwap;
        cs.baseAddress = 0x400;
        cs.messageLengthBytes = 8;
        cs.compound = true;
        CompoundIdentifier i1;
        i1.byteOffset = 0; i1.id = 1; i1.idMask = 0xFF; i1.configured = true;
        CommsChannelRow r1; r1.channelName = n1; r1.startBit = 8; r1.bitLength = 16;
        i1.rows.append(r1);
        cs.identifiers.append(i1);
        CompoundIdentifier i2;
        i2.byteOffset = 0; i2.id = 2; i2.idMask = 0xFF; i2.configured = true;
        CommsChannelRow r2; r2.channelName = n2; r2.startBit = 8; r2.bitLength = 16;
        i2.rows.append(r2);
        cs.identifiers.append(i2);
        cfg.bus[0].sections.append(cs);
        const MappingResult mr = mapToDevice(cfg);
        bool warned = false;
        for (const QString &w : mr.warnings)
            if (w.contains(QStringLiteral("same 31-byte device label")))
                warned = true;
        CHECK(warned);
    }

    // A single over-long name has nothing to collide with, but it still loses
    // its tail on the device — say so rather than truncating silently. Names at
    // the budget pass clean. (The editors cap new names; this covers documents
    // saved before the cap.)
    {
        const auto mapOneChannel = [](const QString &name) {
            Configuration cfg;
            cfg.bus[0].enabled = true;
            Channel c;
            c.name = name;
            c.userDefined = true;
            cfg.catalog().addOrUpdateUserChannel(c);
            CommsSection cs;
            cs.name = QStringLiteral("Rx 0x401");
            cs.device = SectionDevice::ReceiveMessage;
            cs.alignment = SectionAlignment::WordSwap;
            cs.baseAddress = 0x401;
            cs.messageLengthBytes = 8;
            CommsChannelRow row; row.channelName = name; row.startBit = 8; row.bitLength = 16;
            cs.rows.append(row);
            cfg.bus[0].sections.append(cs);
            return mapToDevice(cfg);
        };
        const auto warnsAboutLength = [](const MappingResult &mr) {
            for (const QString &w : mr.warnings)
                if (w.contains(QStringLiteral("longer than the 31-byte device label")))
                    return true;
            return false;
        };

        // 36 bytes, clipped to the first 31 ("...Sensor" ends exactly there).
        const MappingResult tooLong =
            mapOneChannel(QStringLiteral("Manifold Air Temperature Sensor Rear"));
        CHECK(warnsAboutLength(tooLong));
        CHECK(!tooLong.tables.signalConfigs.isEmpty());
        CHECK(std::strcmp(tooLong.tables.signalConfigs[0].label,
                          "Manifold Air Temperature Sensor")
              == 0);

        // Exactly at the budget: 31 bytes, no warning, nothing lost. The name
        // that used to sit at this boundary ("Manifold Temp15") is now well
        // inside it, which is the whole user-visible point of the wider label.
        const QString atBudget = QStringLiteral("Manifold Air Temperature Rear32");
        CHECK(atBudget.toUtf8().size() == MAX_CHANNEL_NAME_BYTES);
        const MappingResult exact = mapOneChannel(atBudget);
        CHECK(!warnsAboutLength(exact));
        CHECK(!exact.tables.signalConfigs.isEmpty());
        CHECK(exact.tables.signalConfigs[0].label[31] == '\0'); // the 32nd byte is the NUL
        CHECK(QString::fromUtf8(exact.tables.signalConfigs[0].label) == atBudget);

        // The budget is UTF-8 BYTES, not characters. A 31-CHARACTER name whose
        // characters are not ASCII overruns the 31-byte label, and the clip has
        // to stop on a codepoint boundary rather than leaving half of one
        // behind — a truncated multi-byte sequence is not a shorter name, it is
        // an invalid string.
        QString accented;
        for (int i = 0; i < 31; ++i)
            accented.append(QChar(0x00E9)); // 'é', two UTF-8 bytes each = 62
        CHECK(accented.size() == 31);
        CHECK(accented.toUtf8().size() == 62);
        const MappingResult wide = mapOneChannel(accented);
        CHECK(warnsAboutLength(wide));
        CHECK(!wide.tables.signalConfigs.isEmpty());
        const QByteArray stored(wide.tables.signalConfigs[0].label);
        CHECK(stored.size() == 30); // 15 whole characters, not 31 half-eaten bytes
        CHECK(QString::fromUtf8(stored) == accented.left(15));
    }
}

// The clamp/roll-over choice, all the way out and all the way back.
//
// Four journeys, because the flag has four chances to be dropped: the .ct3, the
// device record, the read back from a device, and a file written before the
// option existed.
// An identifier the user filled in but gave no channels to must reach the
// device, survive a Get, and come back still empty.
static void testChannellessIdentifierStillTransmits()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel ch;
    ch.name = QStringLiteral("Payload Byte");
    ch.dataType = QStringLiteral("u8");
    ch.userDefined = true;
    ch.minValue = 0;
    ch.maxValue = 255;
    cfg.catalog().addOrUpdateUserChannel(ch);

    CommsSection tx;
    tx.device = SectionDevice::TransmitMessage;
    tx.name = QStringLiteral("Request Frames");
    tx.baseAddress = 0x400;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 10;
    tx.compound = true;
    const auto ident = [](int id, bool withRow, const QString &chName) {
        CompoundIdentifier ci;
        ci.byteOffset = 0;
        ci.id = quint32(id);
        ci.idMask = 0xFF;
        ci.configured = true;
        if (withRow) {
            CommsChannelRow r;
            r.channelName = chName;
            r.startBit = 8;
            r.bitLength = 8;
            r.dbcType = int(DbcType::Unsigned);
            r.dbcFactor = 1.0;
            ci.rows.append(r);
        }
        return ci;
    };
    tx.identifiers.append(ident(1, false, {}));            // selector only
    tx.identifiers.append(ident(2, true, ch.name));        // a real channel
    tx.identifiers.append(ident(3, false, {}));            // selector only
    // A slot the user never touched must NOT become a frame.
    CompoundIdentifier untouched;
    untouched.idMask = 0xFF;
    untouched.id = 4;
    untouched.configured = false;
    tx.identifiers.append(untouched);
    cfg.bus[0].sections.append(tx);

    const MappingResult mapped = mapToDevice(cfg);
    CHECK(mapped.errors.isEmpty());

    int selectorOnly = 0, realFields = 0;
    QList<int> declaredIds;
    for (const CanSignalConfig &sig : mapped.tables.signalConfigs) {
        if (sigMsgIdx(sig) == SIG_MSG_NONE)
            continue;
        if (sigSelectorOnly(sig)) {
            ++selectorOnly;
            declaredIds.append(int(sig.mux_id));
            CHECK(sig.mux_mask == 0xFF);
            CHECK(sigIsActive(sig) == 1);
        } else {
            ++realFields;
            CHECK(sig.mux_id == 2);
        }
    }
    // Two declared, one real field — and the unconfigured slot produced nothing.
    CHECK(selectorOnly == 2);
    CHECK(realFields == 1);
    std::sort(declaredIds.begin(), declaredIds.end());
    CHECK(declaredIds == (QList<int>{1, 3}));

    // Get: the identifiers come back, the empty ones still empty, and no
    // phantom channel is invented for a signal that names none.
    Configuration back;
    mapFromDevice(mapped.tables, back, nullptr);
    CHECK(back.bus[0].sections.size() == 1);
    const CommsSection &got = back.bus[0].sections[0];
    CHECK(got.compound);
    CHECK(got.identifiers.size() == 3); // 1, 2, 3 — never the untouched 4
    int emptyBack = 0;
    for (const CompoundIdentifier &ci : got.identifiers) {
        CHECK(ci.idMask == 0xFF);
        if (ci.rows.isEmpty()) {
            ++emptyBack;
            CHECK(ci.id == 1 || ci.id == 3);
        } else {
            CHECK(ci.id == 2);
            CHECK(ci.rows.size() == 1);
            CHECK(ci.rows[0].channelName == ch.name);
        }
    }
    CHECK(emptyBack == 2);
    for (const Channel &c : back.catalog().userChannels())
        CHECK(!c.name.startsWith(QStringLiteral("Signal ")));

    // And re-sending what came back produces the same thing — the round trip is
    // stable, not merely lossless once.
    const MappingResult again = mapToDevice(back);
    CHECK(again.errors.isEmpty());
    int againSelectorOnly = 0;
    for (const CanSignalConfig &sig : again.tables.signalConfigs)
        if (sigMsgIdx(sig) != SIG_MSG_NONE && sigSelectorOnly(sig))
            ++againSelectorOnly;
    CHECK(againSelectorOnly == 2);
}

static void testClampToRangeSurvivesEveryTrip()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel ch;
    ch.name = QStringLiteral("Rolling Count");
    ch.dataType = QStringLiteral("u16");
    ch.userDefined = true;
    ch.minValue = 0;
    ch.maxValue = 255;
    cfg.catalog().addOrUpdateUserChannel(ch);

    CommsSection tx;
    tx.device = SectionDevice::TransmitMessage;
    tx.name = QStringLiteral("Counter Out");
    tx.baseAddress = 0x300;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 10;
    CommsChannelRow rolling;
    rolling.channelName = ch.name;
    rolling.startBit = 0;
    rolling.bitLength = 8;
    rolling.dbcType = int(DbcType::Unsigned);
    rolling.dbcFactor = 1.0;
    rolling.clampToRange = false; // the whole point of the row
    tx.rows.append(rolling);
    CommsChannelRow clamped = rolling;
    clamped.channelName = ch.name;
    clamped.startBit = 8;
    clamped.clampToRange = true;
    tx.rows.append(clamped);
    cfg.bus[0].sections.append(tx);

    // 1. Through a real file.
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("clamp.ct3"));
    QString err;
    CHECK(cfg.saveToFile(path, &err));
    {
        Configuration back;
        CHECK(back.loadFromFile(path, &err));
        CHECK(back.bus[0].sections.size() == 1);
        const QList<CommsChannelRow> &rows = back.bus[0].sections[0].rows;
        CHECK(rows.size() == 2);
        CHECK(!rows[0].clampToRange);
        CHECK(rows[1].clampToRange);
    }

    // 2. Into the device record — and INVERTED, because the wire bit means
    //    wrap so that an all-zero legacy record means clamp.
    const MappingResult mapped = mapToDevice(cfg);
    CHECK(mapped.errors.isEmpty());
    int wrapping = 0, clamping = 0;
    for (const CanSignalConfig &sig : mapped.tables.signalConfigs) {
        if (sigMsgIdx(sig) == SIG_MSG_NONE)
            continue; // the channel's own value slot, not a field in the frame
        if (sigTxWrap(sig)) {
            ++wrapping;
            CHECK(sigStartBit(sig) == 0);
        } else {
            ++clamping;
            CHECK(sigStartBit(sig) == 8);
        }
        // The flag must not have disturbed the fields sharing its word.
        CHECK(sigIsActive(sig) == 1);
        CHECK(sigBitLength(sig) == 8);
    }
    CHECK(wrapping == 1);
    CHECK(clamping == 1);

    // 3. Back out of a device.
    {
        Configuration fromDevice;
        mapFromDevice(mapped.tables, fromDevice, nullptr);
        CHECK(fromDevice.bus[0].sections.size() == 1);
        const QList<CommsChannelRow> &rows = fromDevice.bus[0].sections[0].rows;
        CHECK(rows.size() == 2);
        for (const CommsChannelRow &r : rows)
            CHECK(r.clampToRange == (r.startBit == 8));
    }

    // 4. A row from before the option existed. An absent key is not "false" and
    //    not undefined — it is a file that could only ever clamp, and it has to
    //    keep clamping or a saved configuration changes behaviour on load.
    {
        QJsonObject legacy;
        legacy["channel"] = ch.name;
        legacy["startBit"] = 0;
        legacy["bitLength"] = 8;
        legacy["dbcType"] = int(DbcType::Unsigned);
        legacy["dbcFactor"] = 1.0;
        legacy["dbcOffset"] = 0.0;
        CHECK(!legacy.contains(QStringLiteral("clampToRange")));
        CHECK(CommsChannelRow::fromJson(legacy).clampToRange);
    }

    // And the key really is written, so a file that STATES it can be told from
    // one that predates it.
    CHECK(rolling.toJson().contains(QStringLiteral("clampToRange")));
    CHECK(!rolling.toJson()[QStringLiteral("clampToRange")].toBool(true));

    // 5. A receive row never carries it, whatever the model says, because the
    //    device would ignore it — and validation says so rather than letting
    //    the file and the device quietly disagree.
    Configuration rxCfg;
    rxCfg.bus[0].enabled = true;
    rxCfg.catalog().addOrUpdateUserChannel(ch);
    CommsSection rx;
    rx.device = SectionDevice::ReceiveMessage;
    rx.name = QStringLiteral("Counter In");
    rx.baseAddress = 0x301;
    rx.messageLengthBytes = 8;
    CommsChannelRow rxRow = rolling; // clampToRange == false, on a receive row
    rx.rows.append(rxRow);
    rxCfg.bus[0].sections.append(rx);
    const MappingResult rxMapped = mapToDevice(rxCfg);
    CHECK(rxMapped.errors.isEmpty());
    for (const CanSignalConfig &sig : rxMapped.tables.signalConfigs)
        CHECK(!sigTxWrap(sig));
    // That loop alone cannot fail — nothing in a receive map ever sets the bit —
    // so the falsifiable half is asserted directly: the receive emit CLEARS a
    // bit rather than trusting the slot to arrive clean, because the slot it
    // writes is one signalFor() may have created earlier. Drop the
    // sigSetTxWrap(sig, false) from device_mapper and this is what notices.
    CanSignalConfig dirty{};
    sigSetTxWrap(dirty, true);
    CHECK(sigTxWrap(dirty));
    sigSetHeader(dirty, 3, 0, 1);
    CHECK(sigTxWrap(dirty)); // a header write must not silently clean it either
    sigSetTxWrap(dirty, false);
    CHECK(!sigTxWrap(dirty));
    CHECK(sigMsgIdx(dirty) == 3);
    bool warned = false;
    for (const ValidationIssue &i : validateConfiguration(rxCfg))
        if (i.message.contains(QStringLiteral("receive row")))
            warned = true;
    CHECK(warned);

    // 6. The other contradiction: roll-over asked for on an IEEE754 field,
    //    which has no range to roll over.
    Configuration floatCfg;
    floatCfg.bus[0].enabled = true;
    floatCfg.catalog().addOrUpdateUserChannel(ch);
    CommsSection ftx;
    ftx.device = SectionDevice::TransmitMessage;
    ftx.name = QStringLiteral("Float Out");
    ftx.baseAddress = 0x302;
    ftx.messageLengthBytes = 8;
    ftx.transmitRateHz = 10;
    CommsChannelRow fRow;
    fRow.channelName = ch.name;
    fRow.startBit = 0;
    fRow.bitLength = 32;
    fRow.dbcType = int(DbcType::IEEE754);
    fRow.clampToRange = false;
    ftx.rows.append(fRow);
    floatCfg.bus[0].sections.append(ftx);
    bool floatWarned = false;
    for (const ValidationIssue &i : validateConfiguration(floatCfg))
        if (i.message.contains(QStringLiteral("IEEE754")))
            floatWarned = true;
    CHECK(floatWarned);

    // 7. And the two places a human is told about it. The Config Summary says
    //    so for a transmit row and stays quiet for a receive row, which is the
    //    same test the section editor's channel list applies — the two views of
    //    one row must not disagree.
    const QString txReport = configSummaryText(cfg);
    CHECK(txReport.contains(QStringLiteral("rolls over")));
    const QString rxReport = configSummaryText(rxCfg);
    CHECK(!rxReport.contains(QStringLiteral("rolls over")));
}

static void testConfigReport()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[0].termination = true;

    const auto addCh = [&cfg](const char *name, const char *dataType, const char *unit) {
        Channel c;
        c.name = QString::fromUtf8(name);
        c.dataType = QString::fromUtf8(dataType);
        c.unit = QString::fromUtf8(unit);
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
    };
    addCh("Engine RPM", "u16", "rpm");
    addCh("Engine Temp", "s16", "C");
    addCh("Orphan Channel", "u8", "");     // referenced nowhere -> unused
    addCh("Dormant Channel", "u8", "");    // referenced only by an Off section
    addCh("Diag Channel", "u8", "");       // Off section's diagnostic channel
    addCh("Zero Mask Value", "u16", "");   // row of a zero-mask (skipped) identifier

    // Receive section generating Engine RPM + Engine Temp.
    CommsSection rx;
    rx.name = QStringLiteral("Receive 0x640");
    rx.device = SectionDevice::ReceiveMessage;
    rx.alignment = SectionAlignment::WordSwap;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    rx.defaultValueOnTimeout = true;
    rx.receiveTimeoutMs = 2200;
    CommsChannelRow rpm;
    rpm.channelName = QStringLiteral("Engine RPM");
    rpm.startBit = 0;
    rpm.bitLength = 16;
    rpm.dbcFactor = 0.25;
    rpm.defaultValue = 1234;
    rx.rows.append(rpm);
    CommsChannelRow temp;
    temp.channelName = QStringLiteral("Engine Temp");
    temp.startBit = 16;
    temp.bitLength = 16;
    temp.dbcType = int(DbcType::Signed);
    temp.dbcFactor = 0.1;
    temp.dbcOffset = -40;
    rx.rows.append(temp);
    cfg.bus[0].sections.append(rx);

    // Transmit section using a channel NOBODY generates -> incomplete.
    CommsSection tx;
    tx.name = QStringLiteral("Transmit 0x700");
    tx.device = SectionDevice::TransmitMessage;
    tx.alignment = SectionAlignment::WordSwap;
    tx.baseAddress = 0x700;
    tx.messageLengthBytes = 8;
    CommsChannelRow txRow;
    txRow.channelName = QStringLiteral("Ghost Output");
    txRow.startBit = 0;
    txRow.bitLength = 16;
    tx.rows.append(txRow);
    cfg.bus[0].sections.append(tx);

    // Off section referencing Dormant Channel — protected from "unused" but
    // neither generated nor used.
    CommsSection off;
    off.name = QStringLiteral("Disabled 0x123");
    off.device = SectionDevice::Off;
    off.diagnosticChannel = QStringLiteral("Diag Channel"); // still a reference
    CommsChannelRow offRow;
    offRow.channelName = QStringLiteral("Dormant Channel");
    off.rows.append(offRow);
    cfg.bus[0].sections.append(off);

    // Compound receive section (multiplexed).
    CommsSection cmp;
    cmp.name = QStringLiteral("Mux 0x200");
    cmp.device = SectionDevice::ReceiveMessage;
    cmp.alignment = SectionAlignment::WordSwap;
    cmp.baseAddress = 0x200;
    cmp.messageLengthBytes = 8;
    cmp.compound = true;
    CompoundIdentifier ident;
    ident.byteOffset = 0;
    ident.id = 1;
    ident.idMask = 0xFF;
    ident.configured = true;
    CommsChannelRow gated;
    gated.channelName = QStringLiteral("Gated Value");
    gated.startBit = 8;
    gated.bitLength = 16;
    ident.rows.append(gated);
    cmp.identifiers.append(ident);
    // Zero-mask identifier: the mapper skips it, so its rows must be dormant
    // (not generated, but protected from cleanup) and the report must say so.
    CompoundIdentifier zeroMask;
    zeroMask.byteOffset = 0;
    zeroMask.id = 0;
    zeroMask.idMask = 0;
    zeroMask.configured = true;
    CommsChannelRow zeroRow;
    zeroRow.channelName = QStringLiteral("Zero Mask Value");
    zeroRow.startBit = 24;
    zeroRow.bitLength = 8;
    zeroMask.rows.append(zeroRow);
    cmp.identifiers.append(zeroMask);
    cfg.bus[0].sections.append(cmp);

    // Math using Engine RPM; condition using the math result.
    MathRow m;
    m.op = MATH_OP_MUL;
    m.aIsChannel = true;
    m.aChannel = QStringLiteral("Engine RPM");
    m.bIsChannel = false;
    m.bConst = 0.5;
    m.destChannel = QStringLiteral("Half RPM");
    cfg.mathRows.append(m);
    ConditionRow c;
    c.setTerms[0].aChannel = QStringLiteral("Half RPM");
    c.setTerms[0].op = COND_OP_GT;
    c.setTerms[0].bConst = 1000;
    giveInverseReset(c);
    c.outputChannel = QStringLiteral("RPM High");
    cfg.conditionRows.append(c);
    ConstantRow k;
    k.name = QStringLiteral("Boost Target");
    k.dataType = QStringLiteral("float");
    k.decimalPlaces = 1;
    k.value = 12.5;
    cfg.constantRows.append(k);

    // ---- usage analysis ----
    const ChannelUsage usage = analyzeChannelUsage(cfg);
    CHECK(usage.generators.contains(QStringLiteral("engine rpm")));
    CHECK(usage.generators.contains(QStringLiteral("gated value")));   // compound rows count
    CHECK(usage.generators.contains(QStringLiteral("half rpm")));
    CHECK(usage.generators.contains(QStringLiteral("boost target")));
    CHECK(usage.users.contains(QStringLiteral("ghost output")));       // transmit consumes
    CHECK(usage.users.contains(QStringLiteral("engine rpm")));         // math input
    CHECK(usage.incomplete == QStringList{QStringLiteral("Ghost Output")});
    CHECK(usage.unused == QStringList{QStringLiteral("Orphan Channel")}); // dormant excluded
    CHECK(!usage.used.contains(QStringLiteral("Dormant Channel")));
    CHECK(usage.used.contains(QStringLiteral("Engine RPM")));
    // An Off section's diagnostic channel is dormant — never "unused"/removable.
    CHECK(!usage.unused.contains(QStringLiteral("Diag Channel")));
    // Zero-mask identifier rows are skipped by the mapper: not generated,
    // but protected from cleanup.
    CHECK(!usage.generators.contains(QStringLiteral("zero mask value")));
    CHECK(!usage.unused.contains(QStringLiteral("Zero Mask Value")));

    // ---- report content ----
    const QString text = configSummaryText(cfg);
    CHECK(text.contains(QStringLiteral("Channel Summary Report")));
    CHECK(text.contains(QStringLiteral("Summary Information")));
    CHECK(text.contains(QStringLiteral("Used Channels")));
    CHECK(text.contains(QStringLiteral("Channels By Function")));
    CHECK(text.contains(QStringLiteral("Incomplete Channels")));
    CHECK(text.contains(QStringLiteral("Unused Channels")));
    CHECK(text.contains(QStringLiteral("Orphan Channel")));
    CHECK(text.contains(QStringLiteral("Ghost Output")));
    // Extraction detail line for Engine RPM (factor 0.25, timeout default 1234).
    CHECK(text.contains(QStringLiteral("bit: 0, len: 16, unsigned, res: 0.25, +: 0, dflt: 1234")));
    CHECK(text.contains(QStringLiteral("bit: 16, len: 16, signed, res: 0.1, +: -40")));
    // Compound identifier group.
    CHECK(text.contains(QStringLiteral("Id[1]  offset 0, id 0x1, mask 0xFF")));
    CHECK(text.contains(QStringLiteral("[mask 0 - skipped, not sent to the device]")));
    // Calculation headline + constants table.
    CHECK(text.contains(QStringLiteral("Half RPM = Engine RPM * 0.5")));
    CHECK(text.contains(QStringLiteral("RPM High = set (Half RPM > 1000)")));
    CHECK(text.contains(QStringLiteral("constant 12.5 (float, 1 dp)")));
    // Off section is listed but contributes no channel tables.
    CHECK(text.contains(QStringLiteral("[off]")));
    // Bus setup reports the termination resistor state.
    CHECK(text.contains(QStringLiteral("termination ON")));

    // HTML variant carries the same content, escaped and styled.
    const QString html = configSummaryHtml(cfg);
    CHECK(html.contains(QStringLiteral("<pre")));
    CHECK(html.contains(QStringLiteral("Channel Summary Report")));
    CHECK(html.contains(QStringLiteral("Orphan Channel")));

    // Validation surfaces the unused channel as Info (and only that one).
    int unusedInfos = 0;
    const auto issues = validateConfiguration(cfg);
    for (const ValidationIssue &issue : issues)
        if (issue.severity == ValidationIssue::Info
            && issue.message.contains(QStringLiteral("unused")))
            ++unusedInfos;
    CHECK(unusedInfos == 1);
}

static void testTransmitOrder()
{
    // The section list order is the transmit order: mapToDevice appends messages
    // in section order, and the firmware services its transmit table in index
    // order, so the top section is pushed to the bus first. Moving a section
    // (Move Up / Move Down) reorders the transmit table.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("TxCh");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);
    ConstantRow k; // generates TxCh so the transmit source exists
    k.name = QStringLiteral("TxCh");
    k.dataType = QStringLiteral("u16");
    k.value = 1;
    cfg.constantRows.append(k);

    const auto makeTx = [](quint32 id) {
        CommsSection s;
        s.name = QStringLiteral("Transmit 0x%1").arg(id, 0, 16);
        s.device = SectionDevice::TransmitMessage;
        s.alignment = SectionAlignment::WordSwap;
        s.baseAddress = id;
        s.messageLengthBytes = 8;
        s.transmitRateHz = 50;
        CommsChannelRow r;
        r.channelName = QStringLiteral("TxCh");
        r.startBit = 0;
        r.bitLength = 16;
        s.rows.append(r);
        return s;
    };
    cfg.bus[0].sections.append(makeTx(0x100));
    cfg.bus[0].sections.append(makeTx(0x200));
    cfg.bus[0].sections.append(makeTx(0x300));

    const auto txIds = [](const MappingResult &mr) {
        QList<quint32> ids;
        for (const CanMessageConfig &m : mr.tables.messages)
            if (m.flags & MSGFLAG_TRANSMIT)
                ids.append(m.can_id);
        return ids;
    };

    CHECK(txIds(mapToDevice(cfg)) == (QList<quint32>{0x100, 0x200, 0x300}));

    // Move the first section (0x100) to the bottom, as Move Down would.
    cfg.bus[0].sections.move(0, 2);
    CHECK(txIds(mapToDevice(cfg)) == (QList<quint32>{0x200, 0x300, 0x100}));
}

static void testMessageRelay()
{
    // A Message Relay (v11) maps to a RelayConfig, reconstructs to a
    // MessageRelay section, and survives a JSON round-trip.
    Configuration cfg;
    cfg.bus[1].enabled = true; // relay lives on CAN 2 (busIdx 1)

    CommsSection relay;
    relay.name = QStringLiteral("Relay 0x18F");
    relay.device = SectionDevice::MessageRelay;
    relay.extended = true;
    relay.baseAddress = 0x18FF0000;
    relay.relayBitmask = 0x1FFF0000;
    relay.relayInvert = true;
    relay.routeBusMask = (1 << 0) | (1 << 2); // CAN 1 + CAN 3 (source CAN 2 excluded)
    cfg.bus[1].sections.append(relay);

    // ---- map to device ----
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.messages.isEmpty()); // a relay is NOT a message
    CHECK(mr.tables.relays.size() == 1);
    if (mr.tables.relays.size() == 1) {
        const RelayConfig &rl = mr.tables.relays[0];
        CHECK(rl.address == 0x18FF0000);
        CHECK(rl.bitmask == 0x1FFF0000);
        CHECK(rl.src_bus == 2);
        CHECK(rl.forward_bus_mask == 0x5); // CAN 1 (bit0) + CAN 3 (bit2)
        CHECK(rl.flags & RELAYFLAG_ACTIVE);
        CHECK(rl.flags & RELAYFLAG_EXTENDED);
        CHECK(rl.flags & RELAYFLAG_INVERT);
    }

    // ---- reconstruct from device ----
    Configuration back;
    mapFromDevice(mr.tables, back);
    const auto &sec = back.bus[1].sections;
    CHECK(sec.size() == 1);
    if (sec.size() == 1) {
        CHECK(sec[0].isRelay());
        CHECK(sec[0].extended);
        CHECK(sec[0].baseAddress == 0x18FF0000);
        CHECK(sec[0].relayBitmask == 0x1FFF0000);
        CHECK(sec[0].relayInvert);
        CHECK(sec[0].routeBusMask == ((1 << 0) | (1 << 2)));
    }

    // ---- JSON round-trip ----
    const QJsonObject o = relay.toJson();
    const CommsSection r2 = CommsSection::fromJson(o, kCurrentSchemaVersion);
    CHECK(r2.device == SectionDevice::MessageRelay);
    CHECK(r2.extended);
    CHECK(r2.baseAddress == 0x18FF0000);
    CHECK(r2.relayBitmask == 0x1FFF0000);
    CHECK(r2.relayInvert);
    CHECK(r2.routeBusMask == ((1 << 0) | (1 << 2)));

    // ---- validation: a relay with no forward target is an error ----
    Configuration bad;
    bad.bus[0].enabled = true;
    CommsSection noTarget = relay;
    noTarget.extended = false;
    noTarget.baseAddress = 0x100;
    noTarget.relayBitmask = 0x7FF;
    noTarget.routeBusMask = 0; // nothing selected
    bad.bus[0].sections.append(noTarget);
    bool sawNoTargetError = false;
    for (const ValidationIssue &vi : validateConfiguration(bad))
        if (vi.severity == ValidationIssue::Error && vi.message.contains("forwards to no bus"))
            sawNoTargetError = true;
    CHECK(sawNoTargetError);

    // ---- report: a relay carrying a stray leftover channel row (e.g. a Receive
    // section re-typed to Relay) must treat that channel as dormant — protected
    // from cleanup but NOT counted as generated or used. ----
    Configuration stray;
    stray.bus[0].enabled = true;
    Channel leftover;
    leftover.name = QStringLiteral("StrayCh");
    leftover.userDefined = true;
    stray.catalog().addOrUpdateUserChannel(leftover);
    CommsSection relayWithRow;
    relayWithRow.name = QStringLiteral("Relay 0x201");
    relayWithRow.device = SectionDevice::MessageRelay;
    relayWithRow.baseAddress = 0x201;
    relayWithRow.relayBitmask = 0x7FF;
    relayWithRow.routeBusMask = (1 << 1);
    CommsChannelRow strayRow;
    strayRow.channelName = QStringLiteral("StrayCh");
    relayWithRow.rows.append(strayRow); // stale row a relay should ignore
    stray.bus[0].sections.append(relayWithRow);
    const ChannelUsage usage = analyzeChannelUsage(stray);
    CHECK(!usage.used.contains(QStringLiteral("StrayCh")));      // not generated/used
    CHECK(!usage.generators.contains(QStringLiteral("StrayCh"))); // device emits nothing
    CHECK(!usage.unused.contains(QStringLiteral("StrayCh")));     // dormant -> not cleanup-flagged
}

// Triggered transmit, host side: the .ct3 round trip, the device round trip,
// and the two refusals.
//
// The device round trip is the one with teeth. A section names its User
// Condition by the condition's OUTPUT CHANNEL; the wire carries an INDEX. So the
// document -> device -> document path has to survive two translations, and a
// SECOND, EARLIER condition sits in front of the one under test on purpose —
// with a single condition, a mapper that hard-wired index 0 would pass.
static void testTriggeredTransmit()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;

    // The channels the conditions read and write.
    for (const char *name : {"Oil Pressure", "Decoy Flag", "Send Now"}) {
        Channel ch;
        ch.name = QString::fromLatin1(name);
        ch.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(ch);
    }

    // Condition 1 (index 0 on the device) — the decoy.
    ConditionRow decoy;
    decoy.setTerms[0].aChannel = QStringLiteral("Oil Pressure");
    decoy.setTerms[0].op = COND_OP_LT;
    decoy.setTerms[0].bConst = 10.0;
    giveInverseReset(decoy);
    decoy.outputChannel = QStringLiteral("Decoy Flag");
    cfg.conditionRows.append(decoy);

    // Condition 2 (index 1) — the one the message is triggered on.
    ConditionRow trigger;
    trigger.setTerms[0].aChannel = QStringLiteral("Oil Pressure");
    trigger.setTerms[0].op = COND_OP_GT;
    trigger.setTerms[0].bConst = 90.0;
    giveInverseReset(trigger);
    trigger.outputChannel = QStringLiteral("Send Now");
    cfg.conditionRows.append(trigger);

    CommsSection tx;
    tx.name = QStringLiteral("Alarm");
    tx.device = SectionDevice::TransmitMessage;
    tx.baseAddress = 0x420;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 1;
    tx.cyclic = false;
    tx.transmitCondition = QStringLiteral("Send Now");
    cfg.bus[0].sections.append(tx);

    // ---- The file ----
    QString error;
    QTemporaryFile f;
    CHECK(f.open());
    const QString path = f.fileName();
    f.close();
    CHECK(cfg.saveToFile(path, &error));
    Configuration loaded;
    CHECK(loaded.loadFromFile(path, &error));
    CHECK(loaded.bus[0].sections.size() == 1);
    if (!loaded.bus[0].sections.isEmpty()) {
        const CommsSection &s = loaded.bus[0].sections[0];
        CHECK(!s.cyclic);
        CHECK(s.transmitCondition == QStringLiteral("Send Now"));
    }

    // ---- The device ----
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.conditions.size() == 2);
    CHECK(mr.tables.messages.size() == 1);
    if (!mr.tables.messages.isEmpty()) {
        const CanMessageConfig &m = mr.tables.messages[0];
        CHECK(m.tx_trigger_flags == TXTRIG_ENABLED);
        // The SECOND condition, not the first — the decoy is what makes this
        // assertion mean something.
        CHECK(m.tx_trigger_cond == 1);
        // And the byte beside them holds no password slot: this message carries
        // no marking, so there is nothing for one to point at. It is no longer a
        // RETIRED byte - store v16 made it password_slot - so "scrubbed to zero"
        // became "zero because this message is unmarked".
        CHECK(m.password_slot == 0);
    }

    Configuration back;
    mapFromDevice(mr.tables, back);
    CHECK(back.bus[0].sections.size() == 1);
    if (!back.bus[0].sections.isEmpty()) {
        const CommsSection &s = back.bus[0].sections[0];
        // Triggered used to be destroyed by every Get — forced back to Cyclic
        // because it reached the device nowhere. It survives now.
        CHECK(!s.cyclic);
        CHECK(s.transmitCondition == QStringLiteral("Send Now"));
    }

    // ---- A Cyclic message carries no trigger, and says so with the sentinel
    // rather than a bare zero: 0 is a perfectly good condition index. ----
    {
        Configuration plain;
        cfg.copyContentTo(plain);
        plain.bus[0].sections[0].cyclic = true;
        const MappingResult pr = mapToDevice(plain);
        CHECK(pr.ok());
        if (!pr.tables.messages.isEmpty()) {
            CHECK(pr.tables.messages[0].tx_trigger_flags == 0);
            CHECK(pr.tables.messages[0].tx_trigger_cond == TX_TRIGGER_COND_NONE);
        }
    }

    // ---- Both refusals. A message set to speak only on a condition must never
    // silently become one that never stops, so each of these is an ERROR that
    // blocks the Send rather than a fallback to cyclic. ----
    {
        Configuration noneNamed;
        cfg.copyContentTo(noneNamed);
        noneNamed.bus[0].sections[0].transmitCondition.clear();
        CHECK(!mapToDevice(noneNamed).ok());
        bool flagged = false;
        for (const ValidationIssue &v : validateConfiguration(noneNamed))
            if (v.severity == ValidationIssue::Error && v.message.contains(QStringLiteral("no User Condition")))
                flagged = true;
        CHECK(flagged);
    }
    {
        Configuration dangling;
        cfg.copyContentTo(dangling);
        dangling.bus[0].sections[0].transmitCondition = QStringLiteral("Gone Away");
        CHECK(!mapToDevice(dangling).ok());
        bool flagged = false;
        for (const ValidationIssue &v : validateConfiguration(dangling))
            if (v.severity == ValidationIssue::Error && v.message.contains(QStringLiteral("Gone Away")))
                flagged = true;
        CHECK(flagged);
    }

    // ---- Renaming the condition's output channel repoints the message. This
    // is the whole reason the binding is a channel name and not a row index. ----
    {
        Configuration renamed;
        cfg.copyContentTo(renamed);
        renamed.renameChannelReferences(QStringLiteral("Send Now"),
                                        QStringLiteral("Fire When Ready"));
        CHECK(renamed.bus[0].sections[0].transmitCondition
              == QStringLiteral("Fire When Ready"));
        CHECK(renamed.conditionRows[1].outputChannel == QStringLiteral("Fire When Ready"));
        CHECK(mapToDevice(renamed).ok());
    }
}

// Every User Condition's output channel is Boolean, including in documents
// written before that was true, and including after a Get.
//
// The Get half is the one that used to fail in a way nobody would notice.
// Boolean and u8 are the same eight bits on the wire, so the type cannot be
// recovered from the signal record — it is recovered from the CONDITION TABLE,
// which is unambiguous evidence that the channel is a condition output.
static void testConditionOutputsAreBoolean()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;

    // A channel typed the way a pre-existing document would have it: float, with
    // the wide default range, and decimals it has no business having.
    Channel wrong;
    wrong.name = QStringLiteral("Over Temp");
    wrong.userDefined = true;
    wrong.dataType = QStringLiteral("float");
    wrong.minValue = -1000.0;
    wrong.maxValue = 1000.0;
    wrong.decimalPlaces = 2;
    cfg.catalog().addOrUpdateUserChannel(wrong);

    Channel src;
    src.name = QStringLiteral("Coolant");
    src.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(src);

    ConditionRow c;
    c.setTerms[0].aChannel = QStringLiteral("Coolant");
    c.setTerms[0].op = COND_OP_GT;
    c.setTerms[0].bConst = 105.0;
    giveInverseReset(c);
    c.outputChannel = QStringLiteral("Over Temp");
    cfg.conditionRows.append(c);

    const auto isBoolean = [](const Configuration &doc, const char *name) {
        const Channel ch = doc.catalog().findByName(QString::fromLatin1(name));
        return ch.isValid() && ch.dataType == QLatin1String("boolean")
               && ch.minValue == 0.0 && ch.maxValue == 1.0 && ch.decimalPlaces == 0;
    };

    // Directly.
    CHECK(cfg.forceConditionOutputsBoolean() == 1);
    CHECK(isBoolean(cfg, "Over Temp"));
    // Idempotent: a second call changes nothing.
    CHECK(cfg.forceConditionOutputsBoolean() == 0);
    // And it leaves other channels alone.
    CHECK(cfg.catalog().findByName(QStringLiteral("Coolant")).dataType != QLatin1String("boolean"));

    // On load, for a file that stored the wrong type.
    {
        Configuration stale;
        stale.bus[0].enabled = true;
        stale.catalog().addOrUpdateUserChannel(wrong);
        stale.catalog().addOrUpdateUserChannel(src);
        stale.conditionRows.append(c);
        QString error;
        QTemporaryFile f;
        CHECK(f.open());
        const QString path = f.fileName();
        f.close();
        // Saved without coercion, so the file genuinely holds "float".
        CHECK(stale.saveToFile(path, &error));
        Configuration loaded;
        CHECK(loaded.loadFromFile(path, &error));
        CHECK(isBoolean(loaded, "Over Temp"));
    }

    // Through the device. The slot goes out typed, and comes back boolean —
    // which it could not do from the wire alone.
    {
        const MappingResult mr = mapToDevice(cfg);
        CHECK(mr.ok());
        CHECK(mr.tables.conditions.size() == 1);
        if (!mr.tables.conditions.isEmpty()) {
            const int dest = mr.tables.conditions[0].dest_signal_idx;
            CHECK(dest < mr.tables.signalConfigs.size());
            // Conditions used to leave this at 0, the "untyped" marker, which
            // is what made a condition output come home with no type at all.
            CHECK(sigValueType(mr.tables.signalConfigs[dest]) == SIGNAL_TYPE_UINT8);
        }
        Configuration back;
        mapFromDevice(mr.tables, back);
        CHECK(back.conditionRows.size() == 1);
        CHECK(isBoolean(back, "Over Temp"));
    }
}

// A Transmit CRC8 section is a transmit message that stamps a checksum, and it
// must survive both round trips whole: the .ct3 (the schema-16 recipe keys) and
// the device (one Crc8Config bound to its message record by index). The recipe
// used here is deliberately fully loaded — J1850 parameters, both reflections,
// one element of each kind — so a field this test does not check is a field the
// section does not carry. A plain transmit message sits AHEAD of the stamped
// one on purpose: msg_idx must be the stamped message's own record index (1),
// and with a single message a mapper that hard-wired zero would still pass.
// Every configuration in the field is migrated by inverting its one expression
// into a Reset, so the inversion has to be EXACTLY right: the migrated Reset
// must be true precisely when the Set is false, for every shape of expression
// a pre-modes file could contain. "De Morgan says so" is a proof about algebra,
// not about this code.
//
// So this evaluates both expressions against every combination that matters —
// all 6 operators in each of up to 3 comparison slots, both joiners in each of
// up to 2 gaps, and a spread of operand values chosen to straddle every
// boundary — and asserts the two never agree. 6^3 x 2^2 x 5^3 term-value
// combinations for the three-term case alone.
//
// The evaluator here is deliberately written OUT OF THE FIRMWARE'S SOURCE
// rather than calling it: if both sides shared an implementation, a fold that
// bracketed right-to-left would satisfy the test while disagreeing with the
// device. This mirrors engine_core.c's foldConditionExpr by hand, including
// the epsilon on equality, and the firmware-link test is what pins the device
// to the same shape.
static bool evalTermHere(const ct::ConditionTermRow &t, double a)
{
    const double b = t.bConst;
    switch (t.op) {
    case int(ct::COND_OP_EQ):  return std::fabs(a - b) < 0.0001;
    case int(ct::COND_OP_NEQ): return std::fabs(a - b) >= 0.0001;
    case int(ct::COND_OP_LT):  return a < b;
    case int(ct::COND_OP_LTE): return a <= b;
    case int(ct::COND_OP_GT):  return a > b;
    case int(ct::COND_OP_GTE): return a >= b;
    }
    return false;
}

static bool evalExprHere(const QList<ct::ConditionTermRow> &terms, const QList<int> &joiners,
                         const QList<double> &values)
{
    bool met = evalTermHere(terms[0], values[0]);
    for (int i = 1; i < terms.size(); ++i) {
        const bool rhs = evalTermHere(terms[i], values[i]);
        const bool isOr = joiners.value(i - 1, int(ct::COND_JOIN_AND)) == int(ct::COND_JOIN_OR);
        met = isOr ? (met || rhs) : (met && rhs);
    }
    return met;
}


static void testMigratedResetIsTheExactInverse()
{
    const QList<int> ops = {int(ct::COND_OP_EQ),  int(ct::COND_OP_NEQ), int(ct::COND_OP_LT),
                            int(ct::COND_OP_LTE), int(ct::COND_OP_GT),  int(ct::COND_OP_GTE)};
    const QList<int> joins = {int(ct::COND_JOIN_AND), int(ct::COND_JOIN_OR)};
    // Straddling the constant on both sides, exactly on it, and well clear —
    // the equality epsilon and the four inequalities all change answer here.
    const QList<double> values = {-1.0, 9.9999, 10.0, 10.0001, 42.0};
    const double kConst = 10.0;

    int checked = 0;
    // One, two and three comparisons: the fold has a different shape at each
    // length, and the two-gap case is the only one where the bracketing of the
    // accumulated left side can be got wrong.
    for (int n = 1; n <= COND_MAX_TERMS; ++n) {
        QList<int> opIdx(n, 0);
        while (true) {
            QList<int> joinIdx(qMax(0, n - 1), 0);
            while (true) {
                QList<ct::ConditionTermRow> setTerms;
                QList<int> setJoiners;
                for (int i = 0; i < n; ++i) {
                    ct::ConditionTermRow t;
                    t.aChannel = QStringLiteral("A%1").arg(i);
                    t.op = ops[opIdx[i]];
                    t.bIsChannel = false;
                    t.bConst = kConst;
                    setTerms.append(t);
                }
                for (int g = 0; g < n - 1; ++g)
                    setJoiners.append(joins[joinIdx[g]]);

                QList<ct::ConditionTermRow> resetTerms;
                QList<int> resetJoiners;
                CHECK(ct::invertConditionExpr(setTerms, setJoiners, &resetTerms, &resetJoiners));
                CHECK(resetTerms.size() == setTerms.size());
                CHECK(resetJoiners.size() == setJoiners.size());

                // Every combination of operand values across the n slots.
                QList<int> valIdx(n, 0);
                while (true) {
                    QList<double> vals;
                    for (int i = 0; i < n; ++i)
                        vals.append(values[valIdx[i]]);
                    const bool s = evalExprHere(setTerms, setJoiners, vals);
                    const bool inv = evalExprHere(resetTerms, resetJoiners, vals);
                    // THE assertion: never both, never neither.
                    CHECK(s != inv);
                    ++checked;
                    int k = n - 1;
                    for (; k >= 0; --k) {
                        if (++valIdx[k] < values.size())
                            break;
                        valIdx[k] = 0;
                    }
                    if (k < 0)
                        break;
                }

                int g = n - 2;
                for (; g >= 0; --g) {
                    if (++joinIdx[g] < joins.size())
                        break;
                    joinIdx[g] = 0;
                }
                if (g < 0 || n < 2)
                    break;
            }
            int i = n - 1;
            for (; i >= 0; --i) {
                if (++opIdx[i] < ops.size())
                    break;
                opIdx[i] = 0;
            }
            if (i < 0)
                break;
        }
    }
    // A guard on the guard: a loop that fell through would pass every CHECK
    // above by never running one.
    CHECK(checked == 5 * 6 + 25 * 36 * 2 + 125 * 216 * 4);

    // Inverting twice is the identity, which is a different claim from the one
    // above and catches an operator whose negation is not its own inverse.
    for (int op : ops)
        CHECK(int(ct::condOpNegate(ct::condOpNegate(quint8(op)))) == op);

    // A message operator has no negation, so the migration REFUSES rather than
    // producing an expression that is quietly wrong. Unreachable for a real
    // pre-modes file — the operators did not exist then — but the refusal is
    // what makes that true by construction rather than by history.
    ct::ConditionTermRow msg;
    msg.op = int(ct::COND_OP_MSG_RX);
    msg.aMessageBus = 1;
    msg.aMessage = QStringLiteral("Engine Data");
    QList<ct::ConditionTermRow> outT;
    QList<int> outJ;
    CHECK(!ct::invertConditionExpr({msg}, {}, &outT, &outJ));
}

static void testTransmitCrc8()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;

    CommsSection plain;
    plain.name = QStringLiteral("Plain TX");
    plain.device = SectionDevice::TransmitMessage;
    plain.alignment = SectionAlignment::WordSwap;
    plain.baseAddress = 0x300;
    plain.messageLengthBytes = 8;
    plain.transmitRateHz = 50;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Counter Out");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    plain.rows.append(row);
    cfg.bus[0].sections.append(plain);

    CommsSection sec = plain;
    sec.name = QStringLiteral("Stamped TX");
    sec.device = SectionDevice::TransmitCrc8;
    sec.baseAddress = 0x321;
    sec.crcChannel = QStringLiteral("Frame CRC");
    sec.crcByteLocation = 7;
    sec.crcPolynomial = 0x1D; // SAE J1850: 0x1D/0xFF/0xFF
    sec.crcInitValue = 0xFF;
    sec.crcFinalXor = 0xFF;
    sec.crcRefIn = true;
    sec.crcRefOut = true;
    sec.crcElements.append({CommsSection::CrcElement::Id, 0});    // (id >> 0) & 0xFF
    sec.crcElements.append({CommsSection::CrcElement::Data, 2});  // frame byte 2
    sec.crcElements.append({CommsSection::CrcElement::Raw, 0x5A}); // literal
    cfg.bus[0].sections.append(sec);

    // ---- .ct3 round trip: the whole recipe survives save/load ----
    QTemporaryFile file;
    CHECK(file.open());
    const QString path = file.fileName();
    file.close();
    QString error;
    CHECK(cfg.saveToFile(path, &error));
    Configuration loaded;
    CHECK(loaded.loadFromFile(path, &error));
    CHECK(loaded.bus[0].sections.size() == 2);
    if (loaded.bus[0].sections.size() == 2) {
        const CommsSection &s = loaded.bus[0].sections[1];
        CHECK(s.isCrc8());
        CHECK(s.isTransmit()); // a CRC8 section IS a transmit message
        CHECK(s.crcChannel == QStringLiteral("Frame CRC"));
        CHECK(s.crcByteLocation == 7);
        CHECK(s.crcPolynomial == 0x1D);
        CHECK(s.crcInitValue == 0xFF);
        CHECK(s.crcFinalXor == 0xFF);
        CHECK(s.crcRefIn);
        CHECK(s.crcRefOut);
        CHECK(s.crcElements.size() == 3);
        if (s.crcElements.size() == 3) {
            CHECK(s.crcElements[0].type == CommsSection::CrcElement::Id);
            CHECK(s.crcElements[0].value == 0);
            CHECK(s.crcElements[1].type == CommsSection::CrcElement::Data);
            CHECK(s.crcElements[1].value == 2);
            CHECK(s.crcElements[2].type == CommsSection::CrcElement::Raw);
            CHECK(s.crcElements[2].value == 0x5A);
        }
        // Byte-exact, not merely field-equal: re-serialising what was loaded
        // must reproduce the JSON the original produced, or the hex spellings
        // and element encoding are drifting on every save.
        CHECK(s.toJson() == sec.toJson());
        // The ordinary neighbour carries no trace of the feature.
        CHECK(!loaded.bus[0].sections[0].toJson().contains(QStringLiteral("crcChannel")));
    }

    // ---- device round trip: one Crc8Config, bound and restored ----
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.messages.size() == 2);
    if (mr.tables.messages.size() == 2)
        CHECK(mr.tables.messages[1].flags & MSGFLAG_TRANSMIT); // an ordinary transmit record
    CHECK(mr.tables.crc8.size() == 1);
    if (mr.tables.crc8.size() == 1) {
        const Crc8Config &cc = mr.tables.crc8[0];
        CHECK(cc.msg_idx == 1); // the stamped message's own record, not slot 0
        CHECK(cc.flags & CRC8FLAG_ACTIVE);
        CHECK(cc.flags & CRC8FLAG_REF_IN);
        CHECK(cc.flags & CRC8FLAG_REF_OUT);
        CHECK(cc.byte_location == 7);
        CHECK(cc.polynomial == 0x1D);
        CHECK(cc.init_value == 0xFF);
        CHECK(cc.final_xor == 0xFF);
        CHECK(cc.element_count == 3);
        CHECK(cc.elem_type[0] == CRC8_ELEM_ID && cc.elem_value[0] == 0);
        CHECK(cc.elem_type[1] == CRC8_ELEM_DATA && cc.elem_value[1] == 2);
        CHECK(cc.elem_type[2] == CRC8_ELEM_RAW && cc.elem_value[2] == 0x5A);
        CHECK(cc.elem_type[3] == 0 && cc.elem_value[3] == 0); // zero-filled tail
        // dest is the CRC channel's value slot, allocated through the same
        // path a receive row uses — so the monitor's signal map labels it.
        CHECK(cc.dest_signal_idx < mr.tables.signalConfigs.size());
        CHECK(mr.signalToChannel.value(cc.dest_signal_idx) == QStringLiteral("Frame CRC"));
        // Device-written, not message-extracted: the slot stays virtual.
        CHECK(sigMsgIdx(mr.tables.signalConfigs[cc.dest_signal_idx]) == SIG_MSG_NONE);
    }

    Configuration back;
    mapFromDevice(mr.tables, back);
    CHECK(back.bus[0].sections.size() == 2);
    if (back.bus[0].sections.size() == 2) {
        // The flip lands on the stamped message only.
        CHECK(back.bus[0].sections[0].device == SectionDevice::TransmitMessage);
        const CommsSection &s = back.bus[0].sections[1];
        CHECK(s.isCrc8());
        CHECK(s.baseAddress == 0x321);
        CHECK(s.rows.size() == 1);
        CHECK(s.crcChannel == QStringLiteral("Frame CRC"));
        CHECK(s.crcByteLocation == 7);
        CHECK(s.crcPolynomial == 0x1D);
        CHECK(s.crcInitValue == 0xFF);
        CHECK(s.crcFinalXor == 0xFF);
        CHECK(s.crcRefIn);
        CHECK(s.crcRefOut);
        CHECK(s.crcElements.size() == 3);
        if (s.crcElements.size() == 3) {
            CHECK(s.crcElements[0].type == CommsSection::CrcElement::Id);
            CHECK(s.crcElements[1].type == CommsSection::CrcElement::Data);
            CHECK(s.crcElements[1].value == 2);
            CHECK(s.crcElements[2].type == CommsSection::CrcElement::Raw);
            CHECK(s.crcElements[2].value == 0x5A);
        }
    }

    // ---- a hand-edited recipe with no CRC channel refuses to map ----
    // The editor cannot produce one, so this only reaches the mapper from a
    // hand-edited .ct3 — and it must be refused rather than mapped to a rule
    // that stamps the wire and publishes nowhere.
    Configuration blankCh;
    blankCh.bus[0].enabled = true;
    CommsSection noCh = sec;
    noCh.crcChannel.clear();
    blankCh.bus[0].sections.append(noCh);
    const MappingResult refused = mapToDevice(blankCh);
    CHECK(!refused.ok());
    bool sawNoChannel = false;
    for (const QString &e : refused.errors)
        sawNoChannel = sawNoChannel || e.contains(QStringLiteral("no CRC channel"));
    CHECK(sawNoChannel);
    CHECK(refused.tables.crc8.isEmpty()); // refused means not half-mapped

    // ---- capacity: the 21st stamped message is an error naming the limit ----
    Configuration many;
    many.bus[0].enabled = true;
    for (int i = 0; i <= MAX_CRC8_MESSAGES; ++i) {
        CommsSection s2 = sec;
        s2.name = QStringLiteral("Stamped %1").arg(i + 1);
        s2.baseAddress = 0x400 + i;
        many.bus[0].sections.append(s2);
    }
    const MappingResult over = mapToDevice(many);
    CHECK(!over.ok());
    CHECK(over.tables.crc8.size() == MAX_CRC8_MESSAGES);
    bool sawTableFull = false;
    for (const QString &e : over.errors)
        sawTableFull = sawTableFull
                       || e.contains(QStringLiteral("CRC8 table is full (%1)")
                                         .arg(MAX_CRC8_MESSAGES));
    CHECK(sawTableFull);
    bool sawLimitError = false;
    for (const ValidationIssue &vi : validateConfiguration(many))
        sawLimitError = sawLimitError
                        || (vi.severity == ValidationIssue::Error
                            && vi.message.contains(QStringLiteral("at most %1 CRC8")
                                                       .arg(MAX_CRC8_MESSAGES)));
    CHECK(sawLimitError);

    // ---- validation: the stamp-vs-layout lint ----
    // One section, three deliberate mistakes: the CRC byte inside a channel
    // row's span (the stamp overwrites it), a Data element reading the CRC's
    // own byte (pre-stamp value), and one indexing past the frame (feeds 0).
    Configuration lint;
    lint.bus[0].enabled = true;
    CommsSection bad = sec;
    bad.crcByteLocation = 1; // inside Counter Out's bytes 0-1
    bad.crcElements.clear();
    bad.crcElements.append({CommsSection::CrcElement::Data, 1});
    bad.crcElements.append({CommsSection::CrcElement::Data, 12});
    lint.bus[0].sections.append(bad);
    // A CHANNEL IN THE STAMPED BYTE IS AN ERROR, not a warning, and that is a
    // deliberate change of contract. It used to be a Warning on the grounds
    // that "the section still maps" — which judged it by whether the mapper
    // survived rather than by what leaves the device. It maps, and then
    // transmits a channel the stamp has already overwritten, so the frame does
    // not carry what the configuration says it carries. See frame_layout.h.
    //
    // The other two stay Warnings: reading the CRC's own byte as a Data element
    // feeds the pre-stamp value (odd, occasionally intended) and an element
    // past the frame feeds 0. Neither corrupts a channel.
    int overlapError = 0, selfReadWarn = 0, pastEndWarn = 0;
    for (const ValidationIssue &vi : validateConfiguration(lint)) {
        if (vi.severity == ValidationIssue::Error
            && vi.message.contains(QStringLiteral("the CRC8 is stamped into")))
            ++overlapError;
        if (vi.severity != ValidationIssue::Warning)
            continue;
        if (vi.message.contains(QStringLiteral("the CRC's own byte")))
            ++selfReadWarn;
        if (vi.message.contains(QStringLiteral("past the 8-byte message")))
            ++pastEndWarn;
    }
    CHECK(overlapError == 1);
    CHECK(selfReadWarn == 1);
    CHECK(pastEndWarn == 1);

    // ---- zero elements: a real spelling, warned about, and round-trippable ----
    // The editor's Element Count starts at 0 and the wire carries it as
    // element_count 0 — the stamp degenerates to the constant init/final-XOR
    // value. It must map (not clamp to a phantom element), warn (a checksum
    // that checks nothing is usually a half-finished recipe), and survive the
    // device round trip as an EMPTY list rather than growing an element.
    Configuration zero;
    zero.bus[0].enabled = true;
    CommsSection none = sec;
    none.crcElements.clear();
    zero.bus[0].sections.append(none);
    const MappingResult zr = mapToDevice(zero);
    CHECK(zr.ok());
    CHECK(zr.tables.crc8.size() == 1);
    if (zr.tables.crc8.size() == 1) {
        CHECK(zr.tables.crc8[0].element_count == 0);
        CHECK(zr.tables.crc8[0].elem_type[0] == 0 && zr.tables.crc8[0].elem_value[0] == 0);
    }
    bool sawEmptyWarn = false;
    for (const ValidationIssue &vi : validateConfiguration(zero))
        sawEmptyWarn = sawEmptyWarn
                       || (vi.severity == ValidationIssue::Warning
                           && vi.message.contains(QStringLiteral("element count is 0")));
    CHECK(sawEmptyWarn);
    Configuration zeroBack;
    mapFromDevice(zr.tables, zeroBack);
    CHECK(zeroBack.bus[0].sections.size() == 1);
    if (zeroBack.bus[0].sections.size() == 1) {
        CHECK(zeroBack.bus[0].sections[0].isCrc8());
        CHECK(zeroBack.bus[0].sections[0].crcElements.isEmpty());
    }
}

static void testLookupTables()
{
    // Lookup tables map to device, reconstruct, and JSON round-trip: the v13
    // 1-axis 2x16, and the 2-axis 8x8 that replaced the v12 4x4.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    // Axis inputs must be generated somewhere — use constants as sources.
    ConstantRow kx;
    kx.name = QStringLiteral("AxisX");
    kx.dataType = QStringLiteral("u16");
    kx.value = 15;
    cfg.constantRows.append(kx);
    ConstantRow ky;
    ky.name = QStringLiteral("AxisY");
    ky.dataType = QStringLiteral("u16");
    ky.value = 15;
    cfg.constantRows.append(ky);

    Table2x16Row t1;
    t1.outputChannel = QStringLiteral("Duty");
    t1.dataType = QStringLiteral("u8");
    t1.xChannel = QStringLiteral("AxisX");
    t1.xInterp = false; // discrete-centered
    for (int k = 0; k < TABLE_2X16_SITES; ++k) { // full v13 width
        t1.xSites.append(k * 10);
        t1.outputs.append(k * 100);
    }
    cfg.table2x16Rows.append(t1);

    // The 8x8 that replaced the 4x4: 64 cells, row-major over the X width.
    // cell(x, y) = x + 10*y, so every cell names itself and a transposed or
    // mis-strided grid is obvious rather than plausible.
    Table8x8Row t2;
    t2.outputChannel = QStringLiteral("Ign");
    t2.xChannel = QStringLiteral("AxisX");
    t2.yChannel = QStringLiteral("AxisY");
    t2.xInterp = true;
    t2.yInterp = false;
    for (int k = 0; k < TABLE_8X8_SITES; ++k) {
        t2.xSites.append(k * 10);
        t2.ySites.append(k * 10);
    }
    for (int y = 0; y < TABLE_8X8_SITES; ++y)
        for (int x = 0; x < TABLE_8X8_SITES; ++x)
            t2.outputs.append(x + 10 * y);
    cfg.table8x8Rows.append(t2);

    // ---- map to device ----
    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.tables2x16Def.size() == 1);
    // The split records are emitted in lockstep — one Out for every Def.
    CHECK(mr.tables.tables2x16Out.size() == mr.tables.tables2x16Def.size());
    CHECK(mr.tables.tables8x8Def.size() == 1);
    // One Def, EIGHT rows. The device addresses table t's grid as rows
    // t*8..t*8+7, so the row vector is not "one per table" and a mapper that
    // emitted fewer would leave the device evaluating against un-programmed
    // flash — the exact condition the torn-upload guard refuses to evaluate.
    CHECK(mr.tables.tables8x8Row.size() == TABLE_8X8_SITES);
    if (mr.tables.tables2x16Def.size() == 1 && mr.tables.tables2x16Out.size() == 1) {
        const Table2x16Def &def = mr.tables.tables2x16Def[0];
        const Table2x16Out &out = mr.tables.tables2x16Out[0];
        CHECK(def.flags & TABLEFLAG_ACTIVE);
        CHECK(!(def.flags & TABLEFLAG_X_INTERP)); // discrete
        CHECK(def.x_count == TABLE_2X16_SITES);
        CHECK(qFuzzyCompare(def.x_sites[3], 30.0f));
        CHECK(qFuzzyCompare(out.outputs[3], 300.0f));
        // The sites beyond the old 8-wide limit must survive the split.
        CHECK(qFuzzyCompare(def.x_sites[15], 150.0f));
        CHECK(qFuzzyCompare(out.outputs[15], 1500.0f));
    }
    if (mr.tables.tables8x8Def.size() == 1
        && mr.tables.tables8x8Row.size() == TABLE_8X8_SITES) {
        const Table8x8Def &tc = mr.tables.tables8x8Def[0];
        CHECK(tc.flags & TABLEFLAG_ACTIVE);
        CHECK(tc.flags & TABLEFLAG_X_INTERP);
        CHECK(!(tc.flags & TABLEFLAG_Y_INTERP));
        CHECK(tc.x_count == TABLE_8X8_SITES);
        CHECK(tc.y_count == TABLE_8X8_SITES);
        CHECK(qFuzzyCompare(tc.x_sites[7], 70.0f));
        CHECK(qFuzzyCompare(tc.y_sites[7], 70.0f));
        // Row y holds the cells of grid row y — the corners, plus one interior
        // cell whose row and column differ so a transposition cannot pass.
        CHECK(qFuzzyCompare(mr.tables.tables8x8Row[0].v[0], 0.0f));
        CHECK(qFuzzyCompare(mr.tables.tables8x8Row[0].v[7], 7.0f));
        CHECK(qFuzzyCompare(mr.tables.tables8x8Row[7].v[0], 70.0f));
        CHECK(qFuzzyCompare(mr.tables.tables8x8Row[7].v[7], 77.0f));
        CHECK(qFuzzyCompare(mr.tables.tables8x8Row[3].v[5], 35.0f));
    }

    // ---- reconstruct from device ----
    Configuration back;
    mapFromDevice(mr.tables, back);
    CHECK(back.table2x16Rows.size() == 1);
    CHECK(back.table8x8Rows.size() == 1);
    if (back.table2x16Rows.size() == 1) {
        const Table2x16Row &t = back.table2x16Rows[0];
        CHECK(t.outputChannel == QStringLiteral("Duty"));
        CHECK(t.xChannel == QStringLiteral("AxisX"));
        CHECK(!t.xInterp);
        CHECK(qFuzzyCompare(t.outputs.value(3), 300.0));
    }
    if (back.table8x8Rows.size() == 1) {
        const Table8x8Row &t = back.table8x8Rows[0];
        CHECK(t.xChannel == QStringLiteral("AxisX"));
        CHECK(t.yChannel == QStringLiteral("AxisY"));
        CHECK(t.xInterp);
        CHECK(!t.yInterp);
        CHECK(t.xSites.size() == TABLE_8X8_SITES);
        CHECK(t.ySites.size() == TABLE_8X8_SITES);
        CHECK(t.outputs.size() == TABLE_8X8_SITES * TABLE_8X8_SITES);
        CHECK(qFuzzyCompare(t.outputs.value(7 * TABLE_8X8_SITES + 7), 77.0));
    }

    // ---- map -> unmap -> map is byte-identical on BOTH 8x8 records ----
    // The grid crosses the wire as nine separate records that only mean
    // anything together, so "the document survived" is not the interesting
    // claim; "the same nine records come back out" is. A row dropped, padded
    // or reordered by the reconstruction would show here and nowhere else,
    // because a Get feeds this path and a Verify compares these exact bytes.
    {
        const MappingResult again = mapToDevice(back);
        CHECK(again.ok());
        CHECK(again.tables.tables8x8Def.size() == mr.tables.tables8x8Def.size());
        CHECK(again.tables.tables8x8Row.size() == mr.tables.tables8x8Row.size());
        for (int i = 0; i < qMin(again.tables.tables8x8Def.size(),
                                 mr.tables.tables8x8Def.size());
             ++i)
            CHECK(std::memcmp(&again.tables.tables8x8Def[i], &mr.tables.tables8x8Def[i],
                              sizeof(Table8x8Def))
                  == 0);
        for (int i = 0; i < qMin(again.tables.tables8x8Row.size(),
                                 mr.tables.tables8x8Row.size());
             ++i)
            CHECK(std::memcmp(&again.tables.tables8x8Row[i], &mr.tables.tables8x8Row[i],
                              sizeof(Table8x8GridRow))
                  == 0);
    }

    // ---- JSON round-trip ----
    const Table2x16Row r1 = Table2x16Row::fromJson(t1.toJson());
    CHECK(r1.outputChannel == QStringLiteral("Duty"));
    CHECK(!r1.xInterp);
    CHECK(qFuzzyCompare(r1.xSites.value(5), 50.0));
    CHECK(qFuzzyCompare(r1.outputs.value(5), 500.0));
    const Table8x8Row r2 = Table8x8Row::fromJson(t2.toJson());
    CHECK(r2.xInterp && !r2.yInterp);
    CHECK(r2.xSites.size() == TABLE_8X8_SITES);
    CHECK(r2.ySites.size() == TABLE_8X8_SITES);
    CHECK(r2.outputs.size() == 64);
    CHECK(qFuzzyCompare(r2.outputs.value(63), 77.0));

    // ---- validation: an axis with no generator is a NOTE, not an error ----
    // An axis reads its channel, so referencing one nothing writes yet is legal
    // (it reads its default value) and must not block Send.
    Configuration bad;
    bad.bus[0].enabled = true;
    Table2x16Row tb;
    tb.outputChannel = QStringLiteral("Z");
    tb.xChannel = QStringLiteral("NoSuchChannel");
    tb.xSites = {1.0, 2.0}; // non-empty so the axis check runs
    tb.outputs = {10.0, 20.0};
    bad.table2x16Rows.append(tb);
    bool sawUngenerated = false;
    for (const ValidationIssue &vi : validateConfiguration(bad)) {
        if (vi.severity == ValidationIssue::Info && vi.message.contains("has no generator"))
            sawUngenerated = true;
        CHECK(vi.severity != ValidationIssue::Error);
    }
    CHECK(sawUngenerated);

    // ---- a BLANK axis is still an error: the lookup has no input at all ----
    Configuration blankAxis;
    Table2x16Row noAxis;
    noAxis.outputChannel = QStringLiteral("Z");
    noAxis.xSites = {1.0, 2.0};
    noAxis.outputs = {10.0, 20.0};
    blankAxis.table2x16Rows.append(noAxis);
    bool sawBlankAxis = false;
    for (const ValidationIssue &vi : validateConfiguration(blankAxis))
        if (vi.severity == ValidationIssue::Error
            && vi.message.contains(QStringLiteral("no input axis channel selected")))
            sawBlankAxis = true;
    CHECK(sawBlankAxis);

    // ---- renameChannelReferences must follow a table's axis inputs + output ----
    Configuration rn;
    Table2x16Row rt;
    rt.outputChannel = QStringLiteral("Out");
    rt.xChannel = QStringLiteral("RPM");
    rn.table2x16Rows.append(rt);
    Table8x8Row rt8;
    rt8.outputChannel = QStringLiteral("Out8");
    rt8.xChannel = QStringLiteral("RPM");
    rt8.yChannel = QStringLiteral("TPS");
    rn.table8x8Rows.append(rt8);
    rn.renameChannelReferences(QStringLiteral("RPM"), QStringLiteral("EngineSpeed"));
    CHECK(rn.table2x16Rows[0].xChannel == QStringLiteral("EngineSpeed"));
    CHECK(rn.table8x8Rows[0].xChannel == QStringLiteral("EngineSpeed"));
    CHECK(rn.table8x8Rows[0].yChannel == QStringLiteral("TPS")); // untouched
    rn.renameChannelReferences(QStringLiteral("Out"), QStringLiteral("Duty"));
    CHECK(rn.table2x16Rows[0].outputChannel == QStringLiteral("Duty"));

    // ---- partial table: only 3 populated sites survive map + reconstruct ----
    Configuration part;
    ConstantRow px;
    px.name = QStringLiteral("PX");
    px.dataType = QStringLiteral("u16");
    part.constantRows.append(px);
    Table2x16Row pt;
    pt.outputChannel = QStringLiteral("POut");
    pt.xChannel = QStringLiteral("PX");
    pt.xSites = {10.0, 20.0, 30.0}; // 3 of 16
    pt.outputs = {1.0, 2.0, 3.0};
    part.table2x16Rows.append(pt);
    const MappingResult pmr = mapToDevice(part);
    CHECK(pmr.ok());
    CHECK(pmr.tables.tables2x16Def.size() == 1);
    CHECK(pmr.tables.tables2x16Out.size() == 1);
    if (pmr.tables.tables2x16Def.size() == 1 && pmr.tables.tables2x16Out.size() == 1) {
        CHECK(pmr.tables.tables2x16Def[0].x_count == 3);
        CHECK(qFuzzyCompare(pmr.tables.tables2x16Def[0].x_sites[2], 30.0f));
        CHECK(pmr.tables.tables2x16Def[0].x_sites[3] == 0.0f);  // unused site zeroed
        CHECK(pmr.tables.tables2x16Out[0].outputs[3] == 0.0f);  // unused output zeroed
        CHECK(pmr.tables.tables2x16Def[0].x_sites[15] == 0.0f); // through the full width
    }
    Configuration pback;
    mapFromDevice(pmr.tables, pback);
    CHECK(pback.table2x16Rows.size() == 1);
    if (pback.table2x16Rows.size() == 1) {
        CHECK(pback.table2x16Rows[0].xSites.size() == 3);
        CHECK(pback.table2x16Rows[0].outputs.size() == 3);
    }

    // A partial 8x8 (2 X sites, 3 Y sites) keeps its 2x3 grid row-major. The
    // document packs a partial grid tight — outputs[y*xSites.size() + x] — and
    // the wire always uses the full width, outputs[y*8 + x], so the two strides
    // differ and the mapper has to re-lay the rows rather than copy them. Every
    // partial table would be silently sheared if it did not.
    Configuration part8;
    ConstantRow qx, qy;
    qx.name = QStringLiteral("QX");
    qx.dataType = QStringLiteral("u16");
    qy.name = QStringLiteral("QY");
    qy.dataType = QStringLiteral("u16");
    part8.constantRows.append(qx);
    part8.constantRows.append(qy);
    Table8x8Row qt;
    qt.outputChannel = QStringLiteral("QOut");
    qt.xChannel = QStringLiteral("QX");
    qt.yChannel = QStringLiteral("QY");
    qt.xSites = {1.0, 2.0};         // 2 X sites
    qt.ySites = {10.0, 20.0, 30.0}; // 3 Y sites
    qt.outputs = {1, 2, 3, 4, 5, 6}; // row-major over X width 2: [y*2 + x]
    part8.table8x8Rows.append(qt);
    const MappingResult qmr = mapToDevice(part8);
    CHECK(qmr.ok());
    CHECK(qmr.tables.tables8x8Def.size() == 1);
    // A partial table still occupies all eight row slots: the device's grid is
    // addressed at a fixed stride of 8, so table t always owns rows t*8..t*8+7
    // whether or not it fills them.
    CHECK(qmr.tables.tables8x8Row.size() == TABLE_8X8_SITES);
    if (qmr.tables.tables8x8Def.size() == 1
        && qmr.tables.tables8x8Row.size() == TABLE_8X8_SITES) {
        CHECK(qmr.tables.tables8x8Def[0].x_count == 2);
        CHECK(qmr.tables.tables8x8Def[0].y_count == 3);
        CHECK(qmr.tables.tables8x8Def[0].x_sites[2] == 0.0f); // unused site zeroed
        CHECK(qmr.tables.tables8x8Def[0].y_sites[3] == 0.0f);
        // model [y*2+x] -> wire row y, column x: cell (x=1,y=2) is model[5]=6.
        CHECK(qFuzzyCompare(qmr.tables.tables8x8Row[2].v[1], 6.0f));
        CHECK(qFuzzyCompare(qmr.tables.tables8x8Row[0].v[0], 1.0f));
        CHECK(qmr.tables.tables8x8Row[0].v[2] == 0.0f); // unused cell zeroed
        CHECK(qmr.tables.tables8x8Row[7].v[7] == 0.0f); // ...through the full width
    }
    Configuration q8back;
    mapFromDevice(qmr.tables, q8back);
    CHECK(q8back.table8x8Rows.size() == 1);
    if (q8back.table8x8Rows.size() == 1) {
        CHECK(q8back.table8x8Rows[0].xSites.size() == 2);
        CHECK(q8back.table8x8Rows[0].ySites.size() == 3);
        CHECK(q8back.table8x8Rows[0].outputs.size() == 6); // packed back to 2x3
        CHECK(qFuzzyCompare(q8back.table8x8Rows[0].outputs.value(5), 6.0)); // [y=2*2 + x=1]
    }

    // ---- one 8x8 too many is an error, not a silent truncation. Worth its own
    // case because this table has TWO capacities: a ninth table would overflow
    // the 8 Def slots and, less obviously, push the row vector past
    // MAX_TABLE_8X8_ROWS — and rows past 64 do not land in some other table's
    // grid, they land nowhere. ----
    {
        Configuration crowded;
        ConstantRow cx;
        cx.name = QStringLiteral("CX");
        cx.dataType = QStringLiteral("u16");
        crowded.constantRows.append(cx);
        for (int i = 0; i < MAX_TABLES_8X8 + 1; ++i) {
            Table8x8Row g;
            g.outputChannel = QStringLiteral("Grid %1").arg(i);
            g.xChannel = QStringLiteral("CX");
            g.yChannel = QStringLiteral("CX");
            g.xSites = {1.0, 2.0};
            g.ySites = {1.0, 2.0};
            g.outputs = {1, 2, 3, 4};
            crowded.table8x8Rows.append(g);
        }
        const MappingResult cmr = mapToDevice(crowded);
        CHECK(!cmr.ok());
        CHECK(cmr.tables.tables8x8Def.size() == MAX_TABLES_8X8);
        CHECK(cmr.tables.tables8x8Row.size() == MAX_TABLE_8X8_ROWS);
    }

    // ---- an active but EMPTY table generates nothing (matches the mapper, which
    // skips empty tables): a downstream consumer must be NOTED, not blocked ----
    Configuration emptyTab;
    Table2x16Row et;
    et.outputChannel = QStringLiteral("EmptyOut");
    et.xChannel = QStringLiteral("SomeCh"); // no sites -> generates nothing
    emptyTab.table2x16Rows.append(et);
    CHECK(!emptyTab.generatedChannelNames().contains(QStringLiteral("EmptyOut"),
                                                     Qt::CaseInsensitive));
    MathRow em;
    em.aIsChannel = true;
    em.aChannel = QStringLiteral("EmptyOut");
    em.destChannel = QStringLiteral("MathOut");
    emptyTab.mathRows.append(em);
    bool sawEmptyUngenerated = false;
    for (const ValidationIssue &vi : validateConfiguration(emptyTab)) {
        if (vi.severity == ValidationIssue::Info && vi.message.contains(QStringLiteral("EmptyOut"))
            && vi.message.contains(QStringLiteral("has no generator")))
            sawEmptyUngenerated = true;
        CHECK(vi.severity != ValidationIssue::Error);
    }
    CHECK(sawEmptyUngenerated);
}

// Referencing a channel vs. writing it. Reading a channel — transmitting it,
// feeding it to a calculation, driving a table axis — leaves its value alone,
// so any number of sites may read the same channel and none of that is an error
// or a warning. Writing it twice IS a conflict: the device has one slot per
// channel, so two writers overwrite each other.
static void testChannelReferenceVsWrite()
{
    const QString rpm = QStringLiteral("Engine RPM");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[1].enabled = true;
    const auto declare = [&](const QString &name) {
        Channel c;
        c.name = name;
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
    };
    declare(rpm);

    // The single writer: one receive row.
    CommsSection rx;
    rx.name = QStringLiteral("Receive 0x640");
    rx.device = SectionDevice::ReceiveMessage;
    rx.alignment = SectionAlignment::WordSwap;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    CommsChannelRow rxRow;
    rxRow.channelName = rpm;
    rxRow.startBit = 0;
    rxRow.bitLength = 16;
    rx.rows.append(rxRow);
    cfg.bus[0].sections.append(rx);

    // Reader 1 + 2: the same channel transmitted in two different messages, on
    // two different buses. This is the case the old guard rails made awkward.
    const auto makeTx = [&](quint32 id) {
        CommsSection s;
        s.name = QStringLiteral("Transmit 0x%1").arg(id, 0, 16);
        s.device = SectionDevice::TransmitMessage;
        s.alignment = SectionAlignment::WordSwap;
        s.baseAddress = id;
        s.messageLengthBytes = 8;
        s.transmitRateHz = 50;
        CommsChannelRow r;
        r.channelName = rpm;
        r.startBit = 0;
        r.bitLength = 16;
        s.rows.append(r);
        return s;
    };
    cfg.bus[0].sections.append(makeTx(0x100));
    cfg.bus[1].sections.append(makeTx(0x200));

    // Readers 3..9: both math inputs, a condition, a counter trigger, a timer
    // start, a table axis and an integrator input — all on the same channel.
    MathRow m;
    m.op = MATH_OP_MUL;
    m.aIsChannel = true;
    m.aChannel = rpm;
    m.bIsChannel = true;
    m.bChannel = rpm; // read twice by ONE row, which is also fine
    m.destChannel = QStringLiteral("RPM Squared");
    cfg.mathRows.append(m);

    ConditionRow cond;
    cond.setTerms[0].aChannel = rpm;
    cond.setTerms[0].op = COND_OP_GT;
    cond.setTerms[0].bConst = 6000;
    giveInverseReset(cond);
    cond.outputChannel = QStringLiteral("Over Rev");
    cfg.conditionRows.append(cond);

    CounterRow cnt;
    cnt.outputChannel = QStringLiteral("Rev Events");
    cnt.upChannel = rpm;
    cfg.counterRows.append(cnt);

    TimerRow tmr;
    tmr.outputChannel = QStringLiteral("Rev Time");
    // Schema 20: the trigger is a comparison. This is the migration's own
    // shape — "that channel is non-zero" — so the row means exactly what it
    // meant when it was a bare channel name.
    tmr.startTerm.aChannel = rpm;
    tmr.startTerm.op = COND_OP_NEQ;
    cfg.timerRows.append(tmr);

    Table2x16Row tab;
    tab.outputChannel = QStringLiteral("Fuel Trim");
    tab.xChannel = rpm;
    tab.xSites = {0.0, 1000.0, 7000.0};
    tab.outputs = {0.0, 1.0, 2.0};
    cfg.table2x16Rows.append(tab);

    IntegratorRow itg;
    itg.outputChannel = QStringLiteral("Rev Total");
    itg.inputChannel = rpm;
    itg.resetChannel = QStringLiteral("Over Rev");
    cfg.integratorRows.append(itg);

    // Nine readers, one writer: nothing to report about Engine RPM at all, and
    // nothing anywhere that would block Send.
    for (const ValidationIssue &vi : validateConfiguration(cfg)) {
        CHECK(vi.severity != ValidationIssue::Error);
        if (vi.message.contains(rpm))
            CHECK(vi.severity != ValidationIssue::Warning);
    }
    // ...and the mapper expresses it: every reader resolves to the ONE value
    // slot the receive row writes, which is what makes many readers harmless.
    const MappingResult many = mapToDevice(cfg);
    CHECK(many.ok());
    const int rpmSlot = many.channelToSignal.value(rpm.toLower(), -1);
    CHECK(rpmSlot >= 0);
    CHECK(many.tables.math.size() == 1);
    if (many.tables.math.size() == 1) {
        CHECK(many.tables.math[0].input_a_idx == rpmSlot);
        CHECK(many.tables.math[0].input_b_idx == rpmSlot); // same slot, read twice
    }
    CHECK(many.tables.conditions.size() == 1);
    if (many.tables.conditions.size() == 1)
        CHECK(many.tables.conditions[0].set_terms[0].input_a_signal_idx == rpmSlot);
    CHECK(many.tables.counters.size() == 1);
    if (many.tables.counters.size() == 1)
        CHECK(many.tables.counters[0].up_signal_idx == rpmSlot);
    CHECK(many.tables.tables2x16Def.size() == 1);
    if (many.tables.tables2x16Def.size() == 1)
        CHECK(many.tables.tables2x16Def[0].x_signal_idx == rpmSlot);
    CHECK(many.tables.integrators.size() == 1);
    if (many.tables.integrators.size() == 1)
        CHECK(many.tables.integrators[0].input_signal_idx == rpmSlot);

    // The same configuration with NOTHING writing the channel still maps: every
    // reader resolves to a virtual slot holding the default. This is the case
    // the old guard rails refused outright, at the picker and at the mapper.
    Configuration noWriter;
    cfg.copyContentTo(noWriter);
    noWriter.bus[0].sections.removeFirst(); // drop the only receive row
    const MappingResult openRefs = mapToDevice(noWriter);
    CHECK(openRefs.ok());
    CHECK(openRefs.tables.math.size() == 1);
    CHECK(openRefs.tables.conditions.size() == 1);
    CHECK(openRefs.tables.counters.size() == 1);
    CHECK(openRefs.tables.timers.size() == 1);
    CHECK(openRefs.tables.tables2x16Def.size() == 1);
    CHECK(openRefs.tables.integrators.size() == 1);
    // Still one shared slot, and still virtual (no message owns it).
    const int openSlot = openRefs.channelToSignal.value(rpm.toLower(), -1);
    CHECK(openSlot >= 0);
    if (openSlot >= 0)
        CHECK(sigMsgIdx(openRefs.tables.signalConfigs[openSlot]) == SIG_MSG_NONE);
    if (openRefs.tables.math.size() == 1)
        CHECK(openRefs.tables.math[0].input_a_idx == openSlot);
    // Validation says the same thing: notes, no errors, no warnings.
    for (const ValidationIssue &vi : validateConfiguration(noWriter)) {
        CHECK(vi.severity != ValidationIssue::Error);
        if (vi.message.contains(rpm))
            CHECK(vi.severity == ValidationIssue::Info);
    }

    // Add a SECOND writer and the warning appears — naming both writers.
    MathRow clash;
    clash.aIsChannel = false;
    clash.aConst = 1;
    clash.bIsChannel = false;
    clash.bConst = 2;
    clash.destChannel = rpm;
    cfg.mathRows.append(clash);
    int rpmWarnings = 0;
    for (const ValidationIssue &vi : validateConfiguration(cfg)) {
        CHECK(vi.severity != ValidationIssue::Error); // a warning, never a block
        if (vi.severity == ValidationIssue::Warning && vi.message.contains(rpm)
            && vi.message.contains(QStringLiteral("written by 2 things"))) {
            ++rpmWarnings;
            CHECK(vi.message.contains(QStringLiteral("CAN 1 · Receive 0x640")));
            CHECK(vi.message.contains(QStringLiteral("Math 2")));
        }
    }
    CHECK(rpmWarnings == 1); // one warning for the pair, not one per writer

    // Table outputs count as writers too — a table and a math row aimed at the
    // same channel is the same conflict, and used to go unreported.
    Configuration dup;
    Table2x16Row dt;
    dt.outputChannel = QStringLiteral("Duty");
    dt.xChannel = QStringLiteral("Load");
    dt.xSites = {0.0, 100.0};
    dt.outputs = {0.0, 100.0};
    dup.table2x16Rows.append(dt);
    MathRow dm;
    dm.aIsChannel = false;
    dm.bIsChannel = false;
    dm.destChannel = QStringLiteral("Duty");
    dup.mathRows.append(dm);
    bool sawTableClash = false;
    for (const ValidationIssue &vi : validateConfiguration(dup))
        if (vi.severity == ValidationIssue::Warning
            && vi.message.contains(QStringLiteral("Table 2x16 1"))
            && vi.message.contains(QStringLiteral("Math 1")))
            sawTableClash = true;
    CHECK(sawTableClash);

    // An INACTIVE row writes nothing on the device, so it is not a second
    // writer — deactivating one of a clashing pair is a real fix.
    dup.mathRows[0].active = false;
    for (const ValidationIssue &vi : validateConfiguration(dup))
        CHECK(!vi.message.contains(QStringLiteral("written by")));

    // A row left half-finished is still an error: "channel" selected, no channel.
    Configuration blank;
    MathRow unfinished;
    unfinished.aIsChannel = true; // no aChannel
    unfinished.destChannel = QStringLiteral("Out");
    blank.mathRows.append(unfinished);
    bool sawUnfinished = false;
    for (const ValidationIssue &vi : validateConfiguration(blank))
        if (vi.severity == ValidationIssue::Error
            && vi.message.contains(QStringLiteral("no input A channel selected")))
            sawUnfinished = true;
    CHECK(sawUnfinished);
}

// rowBitPositions: the walk behind the section editor's frame layout map AND
// the overlap warning. It has to agree with computeExtraction exactly, or the
// picture the user is shown and the problem they are told about are different
// things.
static void testBitLayout()
{
    const auto make = [](int startBit, int bitLength, DbcType type = DbcType::Unsigned) {
        CommsChannelRow r;
        r.channelName = QStringLiteral("Ch");
        r.startBit = startBit;
        r.bitLength = bitLength;
        r.dbcType = int(type);
        return r;
    };

    // Intel / Word Swap: bits ascend straight through the frame.
    const QList<int> intel = rowBitPositions(make(0, 16), SectionAlignment::WordSwap);
    CHECK(intel.size() == 16);
    for (int i = 0; i < intel.size(); ++i)
        CHECK(intel[i] == i);

    // Motorola / Normal: the start bit is still the signal's LSB, but the walk
    // steps BACKWARDS a byte at a time — so "start bit 8, 16 bits" is byte 1
    // then byte 0, which is the shape two numbers alone never convey.
    const QList<int> motorola = rowBitPositions(make(8, 16), SectionAlignment::Normal);
    CHECK(motorola.size() == 16);
    CHECK(motorola.first() == 8);
    CHECK(motorola.value(7) == 15);
    CHECK(motorola.value(8) == 0);  // stepped down into byte 0
    CHECK(motorola.value(15) == 7);

    // The same Motorola field started one byte lower walks off the front of the
    // frame. It comes back SHORT rather than wrapping, which is how the map
    // detects "this doesn't fit" — and computeExtraction must refuse it too.
    const QList<int> under = rowBitPositions(make(0, 16), SectionAlignment::Normal);
    CHECK(under.size() == 8);
    for (int pos : under)
        CHECK(pos >= 0 && pos < 8);
    CHECK(!computeExtraction(make(0, 16), SectionAlignment::Normal, 8, nullptr, nullptr));
    CHECK(computeExtraction(make(8, 16), SectionAlignment::Normal, 8, nullptr, nullptr));

    // IEEE754 is 32 bits whatever the row's bit length says, matching
    // computeExtraction's override.
    CHECK(rowBitPositions(make(0, 8, DbcType::IEEE754), SectionAlignment::WordSwap).size() == 32);

    // A 64-byte CAN FD frame is the ceiling; nothing may be reported past it.
    const QList<int> last = rowBitPositions(make(504, 8), SectionAlignment::WordSwap);
    CHECK(last.size() == 8);
    CHECK(last.last() == MAX_FRAME_BITS - 1);
    CHECK(rowBitPositions(make(508, 8), SectionAlignment::WordSwap).size() == 4); // half over

    // Nonsense lengths yield nothing rather than a partial run.
    CHECK(rowBitPositions(make(0, 0), SectionAlignment::WordSwap).isEmpty());
    CHECK(rowBitPositions(make(0, 65), SectionAlignment::WordSwap).isEmpty());
    CHECK(rowBitPositions(make(-1, 8), SectionAlignment::WordSwap).isEmpty());

    // Overlap: what the map paints red is exactly what validation warns about.
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection s;
    s.name = QStringLiteral("Receive 0x100");
    s.device = SectionDevice::ReceiveMessage;
    s.alignment = SectionAlignment::WordSwap;
    s.baseAddress = 0x100;
    s.messageLengthBytes = 8;
    CommsChannelRow a = make(0, 16);
    a.channelName = QStringLiteral("A");
    CommsChannelRow b = make(8, 16); // shares bits 8..15 with A
    b.channelName = QStringLiteral("B");
    s.rows = {a, b};
    cfg.bus[0].sections.append(s);

    QSet<int> aBits;
    for (int pos : rowBitPositions(a, s.alignment))
        aBits.insert(pos);
    int shared = 0;
    for (int pos : rowBitPositions(b, s.alignment))
        if (aBits.contains(pos))
            ++shared;
    CHECK(shared == 8);
    bool warned = false;
    for (const ValidationIssue &vi : validateConfiguration(cfg))
        if (vi.severity == ValidationIssue::Warning
            && vi.message.contains(QStringLiteral("overlap")))
            warned = true;
    CHECK(warned);

    // Non-overlapping neighbours must NOT be flagged — an off-by-one in the
    // walk would show up here as a phantom conflict in the map.
    cfg.bus[0].sections[0].rows[1].startBit = 16;
    for (const ValidationIssue &vi : validateConfiguration(cfg))
        CHECK(!vi.message.contains(QStringLiteral("overlap")));
}

// The crypto primitives, which now serve the .ct3s container rather than the
// retired configuration password. Two things are being checked: that they are
// the real algorithms (known-answer vectors, so a broken build is caught rather
// than a self-consistently wrong one), and that the authenticated encryption
// built on them actually protects — wrong keys rejected, tampering detected,
// and the three derived keys genuinely independent of one another.
//
// The password machinery that used to sit on top of this is gone; these
// functions are not, because writeSecureFile() seals every .ct3s body with
// exactly them. Deleting this coverage along with the feature would have left
// the secure format resting on untested code.
static void testCryptoPrimitives()
{
    // ---- HMAC-SHA256 against RFC 4231 test case 1 ----
    // Everything here is built on this one primitive; if it is not real HMAC,
    // nothing above it means anything.
    {
        const QByteArray key(20, char(0x0b));
        const QByteArray data = QByteArrayLiteral("Hi There");
        const QByteArray mac =
            QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
        CHECK(mac.toHex()
              == QByteArrayLiteral(
                     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    }

    // ---- PBKDF2-HMAC-SHA256 published vector: P="password", S="salt", c=1 ----
    {
        const QByteArray dk = QPasswordDigestor::deriveKeyPbkdf2(
            QCryptographicHash::Sha256, QByteArrayLiteral("password"),
            QByteArrayLiteral("salt"), 1, 32);
        CHECK(dk.toHex()
              == QByteArrayLiteral(
                     "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    }

    const QString pass = QStringLiteral("correct horse battery staple");

    // ---- derivation is deterministic, salted, and yields independent keys ----
    {
        const QByteArray salt(kLockSaltBytes, char(0x5A));
        const ConfigKeys a = deriveKeys(pass, salt, 1000);
        const ConfigKeys b = deriveKeys(pass, salt, 1000);
        CHECK(a.isValid());
        CHECK(a.encKey == b.encKey && a.macKey == b.macKey && a.verifier == b.verifier);
        // The three must differ, or storing the verifier would hand over the
        // encryption key with it.
        CHECK(a.encKey != a.macKey);
        CHECK(a.encKey != a.verifier);
        CHECK(a.macKey != a.verifier);
        // A different salt gives different keys, so two configs sharing a
        // password do not share a keystream.
        QByteArray other(kLockSaltBytes, char(0x5A));
        other[0] = char(0x5B);
        CHECK(deriveKeys(pass, other, 1000).encKey != a.encKey);
        // Iteration count is part of the derivation.
        CHECK(deriveKeys(pass, salt, 1001).encKey != a.encKey);
        // Malformed input yields nothing usable rather than a weak key.
        CHECK(!deriveKeys(pass, QByteArray(4, 'x'), 1000).isValid());
        CHECK(!deriveKeys(pass, salt, 0).isValid());
    }

    // ---- seal / open ----
    {
        const QByteArray salt(kLockSaltBytes, char(0x11));
        const ConfigKeys keys = deriveKeys(pass, salt, 1000);
        // Deliberately longer than one 32-byte keystream block, and not a
        // multiple of it, so a block-boundary bug shows up.
        QByteArray plain;
        for (int i = 0; i < 1000; ++i)
            plain.append(char('A' + (i % 26)));

        const QByteArray sealed = sealPayload(plain, keys);
        CHECK(sealed.size() == plain.size() + kLockNonceBytes + 32);
        // The body must not be sitting there in the clear.
        CHECK(!sealed.contains(plain.left(64)));

        QByteArray out;
        QString error;
        CHECK(openPayload(sealed, keys, &out, &error));
        CHECK(out == plain);

        // A fresh nonce per seal: the same plaintext under the same key must
        // not produce the same bytes twice.
        CHECK(sealPayload(plain, keys) != sealed);

        // Empty payload is still a valid sealed blob.
        const QByteArray emptySealed = sealPayload(QByteArray(), keys);
        QByteArray emptyOut(1, 'x');
        CHECK(openPayload(emptySealed, keys, &emptyOut, nullptr));
        CHECK(emptyOut.isEmpty());

        // The wrong password cannot open it, and fails on the tag rather than
        // handing back garbage.
        const ConfigKeys wrong = deriveKeys(QStringLiteral("nope"), salt, 1000);
        QByteArray junk;
        CHECK(!openPayload(sealed, wrong, &junk, &error));
        CHECK(!error.isEmpty());

        // Tamper detection: every byte is covered, nonce and tag included.
        const QList<int> spots = {0, kLockNonceBytes, int(sealed.size() / 2),
                                  int(sealed.size() - 1)};
        for (int i : spots) {
            QByteArray bad = sealed;
            bad[i] = char(bad[i] ^ 0x01);
            CHECK(!openPayload(bad, keys, &junk, nullptr));
        }
        // Truncation is rejected, not silently short-read.
        CHECK(!openPayload(sealed.left(sealed.size() - 1), keys, &junk, nullptr));
        CHECK(!openPayload(QByteArray(), keys, &junk, nullptr));
        CHECK(!openPayload(sealed, ConfigKeys{}, &junk, nullptr));
    }

    // ---- constant-time compare still compares correctly ----
    {
        CHECK(constantTimeEquals(QByteArrayLiteral("abc"), QByteArrayLiteral("abc")));
        CHECK(!constantTimeEquals(QByteArrayLiteral("abc"), QByteArrayLiteral("abd")));
        CHECK(!constantTimeEquals(QByteArrayLiteral("abc"), QByteArrayLiteral("ab")));
        CHECK(constantTimeEquals(QByteArray(), QByteArray()));
    }
}

// The three access passwords. The device compares four bytes and nothing else,
// so all of this is about those four bytes being the RIGHT four: identical on
// every machine that types the password (a fleet update depends on it),
// different for every password that is not it, and never the "no password"
// sentinel by accident.
//
// The file-side verifier is checked alongside, because the pair only works if
// the two derivations stay unrelated: a .ct3 lying on a desk must be able to
// answer "was that the right password" without carrying anything that opens
// hardware.
static void testAccessKeys()
{
    // The three are a wire enum as well as a UI list, so their order and their
    // stable keys are part of the format. A renamed key silently orphans the
    // verifier in every file already written.
    const AccessFunction *all = allAccessFunctions();
    for (int i = 0; i < kAccessFunctionCount; ++i)
        CHECK(int(all[i]) == i);
    CHECK(int(AccessFunction::SendConfiguration) == ACCESS_FN_SEND);
    CHECK(int(AccessFunction::GetConfiguration) == ACCESS_FN_GET);
    CHECK(int(AccessFunction::EditProtectedComms) == ACCESS_FN_EDIT_COMMS);
    CHECK(kAccessFunctionCount == ACCESS_FN_COUNT);
    CHECK(kAccessKeyBytes == ACCESS_KEY_LEN);
    CHECK(kAccessChallengeBytes == ACCESS_CHALLENGE_LEN);
    CHECK(accessFunctionKey(AccessFunction::SendConfiguration)
          == QStringLiteral("sendConfiguration"));
    CHECK(accessFunctionKey(AccessFunction::GetConfiguration)
          == QStringLiteral("getConfiguration"));
    CHECK(accessFunctionKey(AccessFunction::EditProtectedComms)
          == QStringLiteral("editProtectedComms"));
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        CHECK(!accessFunctionLabel(all[i]).isEmpty());
        CHECK(!accessFunctionDescription(all[i]).isEmpty());
    }

    const QString pass = QStringLiteral("edit-protected-comms");

    // ---- deriveAccessKey ----
    const AccessKey key = deriveAccessKey(pass);
    // The fixed application salt is what makes one password fold to one key on
    // every installation, which is the whole reason a single .ct3s can update a
    // hundred devices. A per-machine salt would break the fleet story silently.
    CHECK(deriveAccessKey(pass) == key);
    CHECK(key != kNoAccessKey);
    const AccessKey sendKey = deriveAccessKey(QStringLiteral("send-a-configuration"));
    CHECK(sendKey != kNoAccessKey);
    CHECK(sendKey != key);
    // Case is part of a password, not decoration.
    CHECK(deriveAccessKey(QStringLiteral("Edit-Protected-Comms")) != key);
    // An empty password is how every caller says "no password here". If it
    // stretched into a real key, clearing a password would set one instead.
    CHECK(deriveAccessKey(QString()) == kNoAccessKey);
    CHECK(deriveAccessKey(QStringLiteral("")) == kNoAccessKey);

    // ---- the four bytes on the wire ----
    const QByteArray bytes = accessKeyBytes(key);
    CHECK(bytes.size() == kAccessKeyBytes);
    CHECK(accessKeyFromBytes(bytes) == key);
    // Big-endian, so the key reads the same way here, in the firmware's
    // AccessKeyRecord and in a hex dump of flash.
    CHECK(quint8(bytes[0]) == quint8((key >> 24) & 0xFF));
    CHECK(quint8(bytes[3]) == quint8(key & 0xFF));
    CHECK(accessKeyFromBytes(bytes.left(3)) == kNoAccessKey);
    CHECK(accessKeyFromBytes(QByteArray()) == kNoAccessKey);

    // ---- accessResponse IS HMAC-SHA256 over those four bytes ----
    // Computed here straight from Qt rather than by calling the same function
    // twice: the device computes it from the key it stored, so the two halves
    // of the exchange only agree if this is the exact construction.
    {
        const QByteArray challenge(kAccessChallengeBytes, char(0xA5));
        const QByteArray expected = QMessageAuthenticationCode::hash(
            challenge, accessKeyBytes(key), QCryptographicHash::Sha256);
        const QByteArray answer = accessResponse(key, challenge);
        CHECK(answer.size() == 32);
        CHECK(answer == expected);

        // A fresh nonce gives a different answer, which is what makes a serial
        // capture worthless on the next connection.
        QByteArray other = challenge;
        other[0] = char(other[0] ^ 0x01);
        CHECK(accessResponse(key, other) != answer);
        // A different key gives a different answer, which is what makes the
        // three passwords independent rather than three names for one gate.
        CHECK(accessResponse(sendKey, challenge) != answer);

        // No key, no answer. Returning an empty response rather than a
        // well-formed HMAC over four zero bytes is deliberate: a caller holding
        // nothing must not be able to put something plausible on the wire by
        // accident, and an empty result is a mistake it cannot help but notice.
        CHECK(accessResponse(kNoAccessKey, challenge).isEmpty());
        CHECK(accessResponse(key, QByteArray()).isEmpty());
        CHECK(accessResponse(key, challenge.left(kAccessChallengeBytes - 1)).isEmpty());
        CHECK(accessResponse(key, challenge + QByteArray(1, '\0')).isEmpty());
    }

    // ---- AccessKeySet: three slots that know nothing about each other ----
    {
        AccessKeySet set;
        CHECK(!set.any());
        CHECK(set.mask() == 0);
        CHECK(set.key(AccessFunction::SendConfiguration) == kNoAccessKey);

        set.setKey(AccessFunction::GetConfiguration, key);
        CHECK(set.any());
        CHECK(set.isSet(AccessFunction::GetConfiguration));
        // Holding one proves nothing about the others. This is the property the
        // whole design turns on — v18's single password made the weakest of
        // them the only one that mattered.
        CHECK(!set.isSet(AccessFunction::SendConfiguration));
        CHECK(!set.isSet(AccessFunction::EditProtectedComms));
        CHECK(set.mask() == ACCESS_MASK_GET);

        set.setKey(AccessFunction::SendConfiguration, sendKey);
        CHECK(set.mask() == (ACCESS_MASK_SEND | ACCESS_MASK_GET));
        CHECK(set.key(AccessFunction::SendConfiguration) == sendKey);
        CHECK(set.key(AccessFunction::GetConfiguration) == key);
        set.setKey(AccessFunction::GetConfiguration, kNoAccessKey);
        CHECK(set.mask() == ACCESS_MASK_SEND);
        set.clear();
        CHECK(!set.any() && set.mask() == 0);
    }

    // ---- AccessVerifier: what a FILE stores, and what it cannot become ----
    {
        const AccessVerifier v = AccessVerifier::make(pass);
        CHECK(v.isSet() && v.isValid());
        CHECK(v.salt.size() == kAccessVerifierSaltBytes);
        CHECK(v.verifier.size() == kAccessVerifierBytes);
        CHECK(v.iterations == kAccessVerifierIterations);
        CHECK(v.verify(pass));
        CHECK(!v.verify(QStringLiteral("nope")));
        // A random per-file salt, unlike the device key's fixed one: two
        // documents locked with the same password must not advertise the fact.
        const AccessVerifier second = AccessVerifier::make(pass);
        CHECK(second.salt != v.salt);
        CHECK(second.verifier != v.verifier);
        // ...and the verifier is not the key in disguise. A file left on a desk
        // must not hand over the four bytes that open hardware.
        CHECK(!v.verifier.contains(accessKeyBytes(key)));

        // An unset or malformed verifier verifies NOTHING. "No password" must
        // never be mistaken for "correct password", in either direction.
        CHECK(!AccessVerifier().isSet());
        CHECK(!AccessVerifier().isValid());
        CHECK(!AccessVerifier().verify(pass));
        CHECK(!AccessVerifier().verify(QString()));
        CHECK(!AccessVerifier::make(QString()).isSet());
        AccessVerifier broken = v;
        broken.verifier.chop(1);
        CHECK(!broken.isValid());
        CHECK(!broken.verify(pass));

        // The clear-text record round-trips through JSON and still verifies.
        const AccessVerifier back = AccessVerifier::fromJson(v.toJson());
        CHECK(back.salt == v.salt);
        CHECK(back.iterations == v.iterations);
        CHECK(back.verifier == v.verifier);
        CHECK(back.verify(pass));

        // The set only writes the functions that carry a password, so an
        // unprotected document does not read as a protected one.
        AccessVerifierSet set;
        CHECK(!set.any());
        set.setVerifier(AccessFunction::EditProtectedComms, v);
        CHECK(set.any());
        CHECK(set.isSet(AccessFunction::EditProtectedComms));
        CHECK(!set.isSet(AccessFunction::SendConfiguration));
        const QJsonObject json = set.toJson();
        CHECK(json.contains(accessFunctionKey(AccessFunction::EditProtectedComms)));
        CHECK(!json.contains(accessFunctionKey(AccessFunction::SendConfiguration)));
        const AccessVerifierSet reloaded = AccessVerifierSet::fromJson(json);
        CHECK(reloaded.isSet(AccessFunction::EditProtectedComms));
        CHECK(!reloaded.isSet(AccessFunction::GetConfiguration));
        CHECK(reloaded.verifier(AccessFunction::EditProtectedComms).verifier == v.verifier);
        set.clear();
        CHECK(!set.any());
    }
}

// The .ct3s container. The format's whole claim is that the bytes on disk say
// nothing, so the sharpest test here is the opacity one: a marker string sitting
// in the plaintext must not survive anywhere in the file. Everything else —
// round trip, wrong password, tampering — is what makes that opacity worth
// having rather than a party trick.
static void testSecureFile()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString err;

    // A body shaped like the JSON a .ct3 would carry, with a marker no
    // encoding could plausibly leave intact by accident and enough bulk to
    // spread across many carrier chunks.
    const QByteArray marker = QByteArrayLiteral("PROPRIETARY-ECU-FEED-0x7AB-MARKER");
    QByteArray body = QByteArrayLiteral("{\"fileType\":\"CANTripleConfig\",\"comments\":\"");
    body += marker;
    body += QByteArrayLiteral("\",\"filler\":\"");
    for (int i = 0; i < 600; ++i)
        body.append(char('a' + (i % 26)));
    body += QByteArrayLiteral("\"}");

    // ---- standard mode: opens for anyone holding this app ----
    const QString standard = dir.filePath(QStringLiteral("standard.ct3s"));
    SecureSaveOptions opts;
    opts.embeddedCommsKey = AccessKey(0x1A2B3C4Du);
    CHECK(writeSecureFile(standard, body, opts, &err));
    CHECK(isSecureFile(standard));

    SecureFileInfo peek;
    CHECK(peekSecureFile(standard, &peek, &err));
    CHECK(!peek.requiresPassword);
    CHECK(peek.formatVersion == kSecureFormatVersion);
    // Peeking reads the header alone; the key lives in the carrier, so it
    // cannot come back from a peek even though the file carries one.
    CHECK(peek.embeddedCommsKey == kNoAccessKey);

    QByteArray out;
    SecureFileInfo info;
    CHECK(readSecureFile(standard, QString(), &out, &info, &err));
    CHECK(out == body); // byte-identical, not merely parseable
    CHECK(!info.requiresPassword);
    CHECK(info.embeddedCommsKey == opts.embeddedCommsKey);
    // A password nobody asked for is ignored rather than refused: this mode is
    // meant to open unattended, on a customer's machine, with nothing typed.
    out.clear();
    CHECK(readSecureFile(standard, QStringLiteral("irrelevant"), &out, &info, &err));
    CHECK(out == body);

    // ---- opacity: the test the format exists to pass ----
    QFile f(standard);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();
    CHECK(raw.size() > kSecureHeaderBytes);
    CHECK(raw.startsWith(QByteArray(reinterpret_cast<const char *>(kSecureMagic),
                                    kSecureMagicBytes)));
    CHECK(!raw.contains(marker));
    CHECK(!raw.contains(QByteArrayLiteral("CANTripleConfig")));
    CHECK(!raw.contains(QByteArrayLiteral("fileType")));
    CHECK(!raw.contains(body.mid(60, 48))); // an arbitrary interior run
    // Not even the four embedded key bytes are lying there in the clear.
    CHECK(!raw.contains(accessKeyBytes(opts.embeddedCommsKey)));
    // The magic is deliberately not ASCII: a file that announces itself in a
    // hex dump invites the next question.
    for (int i = 0; i < kSecureMagicBytes; ++i)
        CHECK(!(kSecureMagic[i] >= 0x20 && kSecureMagic[i] < 0x7F));

    // ---- non-determinism: saving twice must not produce the same file ----
    const QString twin = dir.filePath(QStringLiteral("standard-again.ct3s"));
    CHECK(writeSecureFile(twin, body, opts, &err));
    QFile g(twin);
    CHECK(g.open(QIODevice::ReadOnly));
    const QByteArray raw2 = g.readAll();
    g.close();
    // Fresh salt, fresh file key, fresh noise. Identical bytes would mean two
    // customers shipped the same configuration could tell it was the same one,
    // and that a diff of two saves would show where the changes are.
    CHECK(raw2 != raw);
    CHECK(raw2.mid(12, 16) != raw.mid(12, 16)); // the salt itself
    CHECK(!raw2.contains(marker));
    QByteArray out2;
    CHECK(readSecureFile(twin, QString(), &out2, &info, &err));
    CHECK(out2 == body); // ...and both still open

    // ---- password mode: no password, no file ----
    {
        const QString locked = dir.filePath(QStringLiteral("locked.ct3s"));
        const QString pass = QStringLiteral("fleet-owner-only");
        SecureSaveOptions pw;
        pw.requirePassword = true;
        pw.password = pass;
        pw.embeddedCommsKey = AccessKey(0x0BADC0DEu);
        CHECK(writeSecureFile(locked, body, pw, &err));

        // The header says a password is needed before anything is disturbed,
        // so the prompt can happen while the open document is still intact.
        CHECK(peekSecureFile(locked, &peek, &err));
        CHECK(peek.requiresPassword);

        QByteArray got;
        CHECK(readSecureFile(locked, pass, &got, &info, &err));
        CHECK(got == body);
        CHECK(info.requiresPassword);
        CHECK(info.embeddedCommsKey == pw.embeddedCommsKey);

        // A wrong password FAILS. It must not yield a truncated document, an
        // empty one, or anything at all — the tag is checked before a single
        // byte is handed back, so `plainBody` is left exactly as it was.
        QByteArray junk = QByteArrayLiteral("untouched");
        CHECK(!readSecureFile(locked, QStringLiteral("fleet-owner-onlY"), &junk, &info, &err));
        CHECK(junk == QByteArrayLiteral("untouched"));
        CHECK(!err.isEmpty());
        CHECK(!readSecureFile(locked, QString(), &junk, &info, &err));
        CHECK(junk == QByteArrayLiteral("untouched"));
        // There is no recovery path, by design: nothing about the file offers a
        // way in without the password, not even the standard-mode reader.
        QFile lf(locked);
        CHECK(lf.open(QIODevice::ReadOnly));
        const QByteArray lockedRaw = lf.readAll();
        lf.close();
        CHECK(!lockedRaw.contains(marker));
    }

    // ---- tampering ----
    // Every byte the format actually USES is covered by the payload's HMAC tag.
    // The ones that are not are noise by construction, and they are counted
    // here rather than glossed over: the chaff around the wrapped access key,
    // the zero padding in the final payload chunk, and any unclaimed noise slot.
    // The bound is computed from the file's OWN header, so it stays honest if
    // the writer's noise ratio changes — and if a future change stopped
    // covering something else, the count would climb past it.
    {
        SecureSaveOptions tight = opts;
        tight.noiseRatio = 0.0; // claim every slot, so the bound below is tight
        QByteArray small = QByteArrayLiteral("{\"comments\":\"");
        small += marker;
        small += QByteArrayLiteral("\"}");
        const QString tpath = dir.filePath(QStringLiteral("tamper.ct3s"));
        CHECK(writeSecureFile(tpath, small, tight, &err));

        QFile tf(tpath);
        CHECK(tf.open(QIODevice::ReadOnly));
        const QByteArray good = tf.readAll();
        tf.close();
        CHECK(!good.contains(marker));

        // Rewrite the file with one bit flipped at `offset`; true when the
        // reader refused it.
        const auto flipRejected = [&](int offset) {
            QByteArray bad = good;
            bad[offset] = char(bad[offset] ^ 0x01);
            QFile bf(tpath);
            if (!bf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return false;
            bf.write(bad);
            bf.close();
            QByteArray scratch;
            SecureFileInfo si;
            return !readSecureFile(tpath, QString(), &scratch, &si, nullptr);
        };

        const auto headerU32 = [&good](int off) {
            return quint32(quint8(good[off])) | (quint32(quint8(good[off + 1])) << 8)
                   | (quint32(quint8(good[off + 2])) << 16)
                   | (quint32(quint8(good[off + 3])) << 24);
        };
        const int carrierLength = int(headerU32(32));
        const int payloadLength = int(headerU32(36));
        CHECK(carrierLength == good.size() - kSecureHeaderBytes);
        CHECK(payloadLength > 0 && payloadLength <= carrierLength);

        // The cleartext header fields the reader depends on. The salt matters
        // most: it seeds both the key wrap and the chunk placement, so one
        // flipped bit relocates every chunk in the file.
        bool headerCovered = true;
        for (int i = 0; i < kSecureMagicBytes; ++i) // magic
            headerCovered = flipRejected(i) && headerCovered;
        for (int i = 8; i < 10; ++i) // format version
            headerCovered = flipRejected(i) && headerCovered;
        for (int i = 12; i < 28; ++i) // salt
            headerCovered = flipRejected(i) && headerCovered;
        for (int i = 32; i < 40; ++i) // carrier / payload lengths
            headerCovered = flipRejected(i) && headerCovered;
        CHECK(headerCovered);
        // Claiming a password the writer never applied cannot open it either:
        // the wrap mask gains a term the file was not built with.
        CHECK(flipRejected(10));

        // Now every byte of the carrier.
        const auto roundUp = [](int n) {
            return ((n + kSecureChunkBytes - 1) / kSecureChunkBytes) * kSecureChunkBytes;
        };
        const int claimed = 2 * kSecureChunkBytes // wrapped file key
                            + roundUp(payloadLength);
        // Only two kinds of byte in this file may be flipped without the reader
        // noticing: a slot no chunk ever claimed, and the zero padding in the
        // final payload chunk. Notably NOT the embedded access key — it lives
        // inside the sealed plaintext, so the payload's tag covers it like
        // everything else, and a flip there must be caught.
        const int slack = qMax(0, carrierLength - claimed)            // unclaimed noise
                          + (roundUp(payloadLength) - payloadLength); // final-chunk padding
        int undetected = 0;
        for (int i = kSecureHeaderBytes; i < good.size(); ++i)
            if (!flipRejected(i))
                ++undetected;
        CHECK(undetected <= slack);

        // ...and the sweep has to have been worth running: a test where slack
        // covered the whole file would pass without detecting anything.
        //
        // This deliberately does NOT assert "most of the carrier was detected".
        // That was the first formulation and it was FLAKY: the noise a save
        // draws is randomised (a floor of kMinNoiseChunks plus a random spread,
        // so two saves of one document differ in length), and a small
        // configuration legitimately ends up with more unclaimed slots than
        // claimed ones. It passed or failed on the draw, which is the worst kind
        // of test — it had already passed once here before failing on a clean
        // build of identical code.
        //
        // The honest invariant does not mention the noise at all: there must be
        // a real amount of material, and every byte of it must have been caught.
        const int padding = roundUp(payloadLength) - payloadLength;
        CHECK(claimed >= 3 * kSecureChunkBytes);       // key + at least some payload
        CHECK(carrierLength - undetected >= claimed - padding);
    }

    // ---- a file that is not one of ours ----
    {
        const QString bogus = dir.filePath(QStringLiteral("plain.ct3"));
        QFile bf(bogus);
        CHECK(bf.open(QIODevice::WriteOnly));
        bf.write(QByteArrayLiteral("{\"fileType\":\"CANTripleConfig\"}\n"));
        bf.close();
        CHECK(!isSecureFile(bogus));
        CHECK(!peekSecureFile(bogus, &peek, &err));
        QByteArray scratch;
        CHECK(!readSecureFile(bogus, QString(), &scratch, &info, &err));
        CHECK(!isSecureFile(dir.filePath(QStringLiteral("no-such-file.ct3s"))));
    }
}

// The fleet identity block: which fleet a configuration is FOR. Three things
// here are worth testing directly — the byte budget on the two strings, what
// counts as "the same fleet", and the one rule the JSON has to obey, which is
// that the fleet key is the fleet's only secret and a plain .ct3 is a text
// file people mail around.
static void testFleetIdentity()
{
    // ---- clampToWire: at most N BYTES, and never half a character ----
    // The whole reason this exists rather than QString::left(). The wire field
    // is fixed-width UTF-8, so sixteen CHARACTERS can be forty-eight bytes, and
    // a truncation that counted QChars would cut the last one in half and hand
    // the device a field it cannot decode. The damage from that is invisible:
    // the unit simply reports a vendor no configuration matches, months later.
    //
    // Every case asserts the same things: the result is a byte-for-byte PREFIX
    // of the input, it fits the budget, it is exactly as long as it should be,
    // and the cut landed on a character boundary. The expected lengths are
    // written out per case rather than derived, so the arithmetic the function
    // is supposed to do is stated here and not repeated from it.
    {
        const auto checkClamp = [](const QString &input, int maxBytes, int expectedChars,
                                   int expectedBytes) {
            const QString clamped = FleetIdentity::clampToWire(input, maxBytes);
            const QByteArray clampedUtf8 = clamped.toUtf8();
            const QByteArray inputUtf8 = input.toUtf8();
            CHECK(clampedUtf8.size() <= maxBytes);
            CHECK(clampedUtf8.size() == expectedBytes);
            CHECK(clamped.size() == expectedChars);
            CHECK(clamped == input.left(expectedChars));
            CHECK(inputUtf8.startsWith(clampedUtf8));
            // The cut landed on a character boundary: the next byte of the
            // input, where there is one, STARTS a sequence rather than
            // continuing the one that was just severed (continuation bytes are
            // 10xxxxxx). This is the single check the function exists to pass.
            if (clampedUtf8.size() < inputUtf8.size())
                CHECK((quint8(inputUtf8[clampedUtf8.size()]) & 0xC0) != 0x80);
            // ...and re-decoding gives back exactly what was clamped. A split
            // sequence would come back as U+FFFD instead.
            CHECK(QString::fromUtf8(clampedUtf8) == clamped);
            CHECK(!clamped.contains(QChar(QChar::ReplacementCharacter)));
        };

        // Sixteen 3-byte characters (U+6F22) = 48 bytes. Five fit in sixteen
        // bytes; a sixth would need eighteen. This is the case the function
        // exists for, and the one QString::left(16) gets catastrophically wrong.
        QString han;
        for (int i = 0; i < 16; ++i)
            han += QString::fromUtf8("\xE6\xBC\xA2");
        CHECK(han.size() == 16 && han.toUtf8().size() == 48);
        checkClamp(han, kFleetVendorIdBytes, 5, 15);

        // Sixteen 2-byte characters (U+00E9) = 32 bytes; eight fit exactly.
        QString accented;
        for (int i = 0; i < 16; ++i)
            accented += QString::fromUtf8("\xC3\xA9");
        CHECK(accented.size() == 16 && accented.toUtf8().size() == 32);
        checkClamp(accented, kFleetModelIdBytes, 8, 16);

        // Astral plane (U+1F3CE): four UTF-8 bytes AND a surrogate PAIR in the
        // QString, so a clamp that stopped at a QChar boundary would leave a
        // lone surrogate — something Qt cannot encode at all.
        QString cars;
        for (int i = 0; i < 6; ++i)
            cars += QString::fromUtf8("\xF0\x9F\x8F\x8E");
        CHECK(cars.size() == 12 && cars.toUtf8().size() == 24);
        checkClamp(cars, kFleetVendorIdBytes, 8, 16); // four whole cars

        // Plain ASCII: exactly sixteen is legal — the field is NUL-PADDED, not
        // NUL-terminated, so all sixteen bytes are usable and a sixteen-byte
        // name must not lose its last character to a terminator that is not
        // there.
        checkClamp(QStringLiteral("0123456789ABCDEF"), kFleetVendorIdBytes, 16, 16);
        checkClamp(QStringLiteral("0123456789ABCDEFG"), kFleetVendorIdBytes, 16, 16);
        CHECK(FleetIdentity::clampToWire(QString(), kFleetVendorIdBytes).isEmpty());
        CHECK(FleetIdentity::clampToWire(QStringLiteral("x"), 0).isEmpty());
    }

    FleetIdentity id;
    CHECK(!id.isSet()); // what an unprovisioned unit reports
    id.vendorId = QStringLiteral("Detailed Motor");
    id.modelId = QStringLiteral("CAN Triple");
    id.serialNumber = 0x00000123u;
    id.configVersion = 42;
    id.flags = 0;
    id.fleetKey = AccessKey(0xDEADBEEFu);
    CHECK(id.isSet());
    // A serial on its own says which UNIT this is and nothing about which fleet
    // it belongs to, so it must not make an otherwise blank identity read as
    // provisioned — that unit would match no policy in existence.
    {
        FleetIdentity serialOnly;
        serialOnly.serialNumber = 0x99u;
        CHECK(!serialOnly.isSet());
    }

    // ---- sameFleetAs: vendor and model, exactly ----
    {
        CHECK(id.sameFleetAs(id));
        // Serial and version are the targeting and ordering questions, asked
        // separately. Folding them in here would refuse every update to a
        // device that is merely out of date — which is every update.
        FleetIdentity other = id;
        other.serialNumber = 0x00000999u;
        other.configVersion = 99;
        CHECK(id.sameFleetAs(other));
        CHECK(other.sameFleetAs(id));

        // Vendor and model are identifiers, not display names: case and spacing
        // are part of them. A case-insensitive match would let "acme" collect
        // updates built for "ACME", and the two are not obliged to be the same
        // company.
        FleetIdentity cased = id;
        cased.vendorId = id.vendorId.toLower();
        CHECK(!id.sameFleetAs(cased));
        FleetIdentity spaced = id;
        spaced.modelId = QStringLiteral("CANTriple");
        CHECK(!id.sameFleetAs(spaced));
        // A different product line is a different fleet. There is deliberately
        // no separate series field to distinguish config lines — the model name
        // carries that, so this is the comparison that has to do the work.
        FleetIdentity elsewhere = id;
        elsewhere.modelId = id.modelId + QStringLiteral(" X");
        CHECK(!id.sameFleetAs(elsewhere));

        // Two blank identities are not "the same fleet"; they are two things
        // nobody has told anything about. Matching them would wave every update
        // through on unprovisioned hardware, which is the one case where the
        // check has to be strictest.
        CHECK(!FleetIdentity().sameFleetAs(id));
        CHECK(!id.sameFleetAs(FleetIdentity()));
        CHECK(!FleetIdentity().sameFleetAs(FleetIdentity()));
    }

    // ---- toJson(false) must not carry the key under ANY name ----
    // Asserting that the object has no "fleetKey" member would only prove it
    // is absent under the name we happen to expect. The whole serialised object
    // is searched instead, for every form the four bytes could be written in.
    {
        const QJsonObject o = id.toJson(false);
        const QByteArray text = QJsonDocument(o).toJson(QJsonDocument::Compact);
        const QByteArray keyBytes = accessKeyBytes(id.fleetKey);
        CHECK(!text.contains(keyBytes.toBase64()));
        CHECK(!text.contains(keyBytes.toHex()));
        CHECK(!text.contains(keyBytes.toHex().toUpper()));
        CHECK(!text.contains(keyBytes));
        CHECK(!text.contains(QByteArray::number(qlonglong(id.fleetKey))));
        CHECK(!text.contains(QByteArray::number(qlonglong(id.fleetKey), 16)));

        // What IS there still names the fleet — a plain .ct3 that could not be
        // checked against a device would be no use at all.
        const FleetIdentity back = FleetIdentity::fromJson(o);
        CHECK(back.vendorId == id.vendorId);
        CHECK(back.modelId == id.modelId);
        CHECK(back.serialNumber == id.serialNumber);
        CHECK(back.configVersion == id.configVersion);
        CHECK(back.flags == id.flags);
        CHECK(back.fleetKey == kNoAccessKey);
        CHECK(back.sameFleetAs(id));
    }

    // ---- toJson(true): a .ct3s body is sealed, so it may carry the key ----
    {
        const FleetIdentity back = FleetIdentity::fromJson(id.toJson(true));
        CHECK(back.fleetKey == id.fleetKey);
        CHECK(back.sameFleetAs(id));
        CHECK(back.serialNumber == id.serialNumber);
        CHECK(back.configVersion == id.configVersion);
    }

    // An object with nothing in it is an unprovisioned identity rather than a
    // half-populated one, so a file written before the block existed loads as
    // "no fleet" instead of as a fleet whose name is the empty string.
    CHECK(!FleetIdentity::fromJson(QJsonObject()).isSet());
}

// The uploader's rulebook. It travels inside the configuration, so a .ct3s
// handed to a customer carries its own restrictions — which means the defaults
// matter as much as the values: a policy key missing from an older file must
// read as the STRICT answer, never as permission.
static void testUploadPolicy()
{
    // ---- an empty allow-list is "any serial in the fleet" ----
    UploadPolicy any;
    CHECK(!any.pinsSerial());
    CHECK(any.allowsSerial(0));
    CHECK(any.allowsSerial(1));
    CHECK(any.allowsSerial(0xFFFFFFFFu));
    CHECK(any.requireFleetKey);
    CHECK(any.warnOnOlderVersion);

    // ---- a populated one permits exactly what it names ----
    // This is what stops a one-off replacement configuration, built for the car
    // that broke, from installing on the rest of a customer's fleet.
    UploadPolicy pinned;
    pinned.allowedSerials = QList<quint32>{0x100u, 0x102u, 0x105u};
    CHECK(pinned.pinsSerial());
    CHECK(pinned.allowsSerial(0x100u));
    CHECK(pinned.allowsSerial(0x102u));
    CHECK(pinned.allowsSerial(0x105u));
    CHECK(!pinned.allowsSerial(0x101u));
    CHECK(!pinned.allowsSerial(0x106u));
    // Serial 0 is what an unprovisioned unit reports. A pinned policy must not
    // treat that as a wildcard, or the one device that has told you nothing
    // would be the one device every restricted update installs on.
    CHECK(!pinned.allowsSerial(0));

    // ---- JSON ----
    {
        const UploadPolicy back = UploadPolicy::fromJson(pinned.toJson());
        CHECK(back.allowedSerials == pinned.allowedSerials);
        CHECK(back.allowsSerial(0x102u));
        CHECK(!back.allowsSerial(0x101u));
        CHECK(back.requireFleetKey);
        CHECK(back.warnOnOlderVersion);
    }
    // Absent means TRUE for both flags. Read the other way round, a file that
    // predates the field — or one an editor stripped a key out of — would
    // silently stop demanding the attestation and stop refusing a downgrade,
    // and nothing on screen would say the policy had been relaxed.
    {
        const UploadPolicy fromNothing = UploadPolicy::fromJson(QJsonObject());
        CHECK(fromNothing.requireFleetKey);
        CHECK(fromNothing.warnOnOlderVersion);
        CHECK(!fromNothing.pinsSerial());
        CHECK(fromNothing.allowsSerial(0x1234u));
    }
    // ...and an explicit false has to survive, or turning a check off would be
    // inexpressible: deliberately reflashing an older configuration is a real
    // thing to want to do.
    {
        UploadPolicy relaxed;
        relaxed.requireFleetKey = false;
        relaxed.warnOnOlderVersion = false;
        const UploadPolicy back = UploadPolicy::fromJson(relaxed.toJson());
        CHECK(!back.requireFleetKey);
        CHECK(!back.warnOnOlderVersion);
    }
}

// A retained device script as it reaches disk.
//
// This is the ONLY copy of that script in existence: the device stores bytecode
// and bytecode does not decompile, so there is no source to rebuild it from.
// Losing it in the file loses it entirely — which is what makes the schema bump
// below load-bearing rather than bookkeeping.
//
// The image is assembled here rather than compiled, because this binary has no
// Lua in it and does not need any: what is on trial is whether the BYTES survive
// a file, and a hand-built image that the device's own verifier accepts proves
// that better than a compiler dependency would.
static QByteArray minimalScriptImage()
{
    ScriptInstr halt{};
    halt.op = SCRIPT_OP_HALT;

    QByteArray image;
    image.resize(int(sizeof(ScriptHeader) + sizeof(ScriptInstr)));
    std::memcpy(image.data() + sizeof(ScriptHeader), &halt, sizeof(halt));

    ScriptHeader h{};
    h.magic = SCRIPT_MAGIC;
    h.version = SCRIPT_BYTECODE_VERSION;
    h.num_state = 0;
    h.code_bytes = SCRIPT_INSTR_SIZE;
    h.entry_tick = 0;
    h.entry_rx = SCRIPT_NO_ENTRY;
    h.entry_tx = SCRIPT_NO_ENTRY;
    h.reserved = 0;
    h.code_crc32 = script_crc32(image.constData() + sizeof(ScriptHeader), h.code_bytes);
    std::memcpy(image.data(), &h, sizeof(h));

    // If this ever stops holding, every check below is testing base64 rather
    // than a script.
    CHECK(validateScriptImage(image, nullptr));
    return image;
}

static void testRetainedScriptDocument()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString error;

    const QByteArray image = minimalScriptImage();

    Configuration cfg;
    cfg.setScriptBytecode(image);
    CHECK(cfg.scriptBytecode() == image);

    // ---- plain .ct3 ----
    const QString plain = dir.filePath(QStringLiteral("retained.ct3"));
    CHECK(cfg.saveToFile(plain, &error));

    const QJsonObject root = configBodyOf(plain);
    // The bump is the point: a shipped 2.3.2 build knows nothing of
    // "scriptBytecode", would load this document as scriptless, and the next
    // Send would STRIP THE SCRIPT off the unit. The version guard refuses the
    // file to that build instead.
    CHECK(root.value(QStringLiteral("fileVersion")).toInt() == kCurrentSchemaVersion);
    CHECK(root.contains(QStringLiteral("scriptBytecode")));
    // No source is written beside it — the two are mutually exclusive, so a
    // file cannot record the stale pair the document cannot hold.
    CHECK(!root.contains(QStringLiteral("scriptSource")));

    Configuration loaded;
    CHECK(loaded.loadFromFile(plain, &error));
    // The BYTES, not the length: a file that round-tripped a re-encoding would
    // pass a size check and put a different script on the unit.
    CHECK(loaded.scriptBytecode() == image);
    CHECK(loaded.scriptSource().isEmpty());

    // ---- secure .ct3s ----
    // The secure container has to carry it too. It did not come free: the body
    // is buildBody()'s output, so this is the check that the two writers share
    // one body rather than two that drift.
    const QString secure = dir.filePath(QStringLiteral("retained.ct3s"));
    SecureSaveOptions opts;
    CHECK(cfg.saveSecureToFile(secure, opts, &error));
    Configuration reopened;
    CHECK(reopened.loadFromFile(secure, &error));
    CHECK(reopened.scriptBytecode() == image);
    CHECK(reopened.scriptSource().isEmpty());

    // ---- a source document writes no bytecode key at all ----
    Configuration sourceCfg;
    sourceCfg.setScriptSource(QStringLiteral("function on_tick()\nend\n"));
    const QString sourcePath = dir.filePath(QStringLiteral("source.ct3"));
    CHECK(sourceCfg.saveToFile(sourcePath, &error));
    const QJsonObject sourceRoot = configBodyOf(sourcePath);
    CHECK(sourceRoot.contains(QStringLiteral("scriptSource")));
    CHECK(!sourceRoot.contains(QStringLiteral("scriptBytecode")));

    // ---- a file carrying BOTH resolves through the same precedence rule ----
    // Nothing this application writes looks like this; a hand-merge does. The
    // loader must not be the one place a stale pair can enter the document.
    QJsonObject mixed = root;
    mixed[QStringLiteral("scriptSource")] = QStringLiteral("function on_tick()\nend\n");
    const QString mixedPath = dir.filePath(QStringLiteral("mixed.ct3"));
    QFile mf(mixedPath);
    CHECK(mf.open(QIODevice::WriteOnly));
    mf.write(QJsonDocument(mixed).toJson());
    mf.close();
    Configuration mixedLoaded;
    CHECK(mixedLoaded.loadFromFile(mixedPath, &error));
    CHECK(mixedLoaded.scriptSource() == QLatin1String("function on_tick()\nend\n"));
    CHECK(mixedLoaded.scriptBytecode().isEmpty());

    // ---- a schema-14 file still opens ----
    // The bump refuses NEWER files to OLDER builds. It must not refuse older
    // files to this one, which is the mistake a version guard makes when the
    // comparison slips from > to >=.
    QJsonObject old = root;
    old.remove(QStringLiteral("scriptBytecode"));
    old[QStringLiteral("fileVersion")] = 14;
    const QString oldPath = dir.filePath(QStringLiteral("schema14.ct3"));
    QFile of(oldPath);
    CHECK(of.open(QIODevice::WriteOnly));
    of.write(QJsonDocument(old).toJson());
    of.close();
    Configuration oldLoaded;
    CHECK(oldLoaded.loadFromFile(oldPath, &error));
    CHECK(oldLoaded.scriptBytecode().isEmpty());
}


// ---------------------------------------------------------------- format 2
//
// The .ct3 container itself: the preamble, the refusals, and — the case that
// matters most to anyone upgrading — that every format-1 file still opens.
//
// v1.1.3 shipped JSON .ct3 files to real users. If this function ever starts
// failing, those people cannot open their configurations, so it is written to
// build the old format by hand rather than by asking any current code to
// produce it: a migration test whose "old" input comes from the new writer
// tests nothing.
static void testBinaryConfigFormat()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString error;

    Configuration cfg;
    cfg.setConfigTitle(QStringLiteral("Format Two"));
    Channel ch;
    ch.name = QStringLiteral("Distinctive Channel Name");
    ch.unit = QStringLiteral("rpm");
    ch.quantity = QStringLiteral("Rotational Speed");
    ch.dataType = QStringLiteral("u16");
    cfg.catalog().addOrUpdateUserChannel(ch);

    const QString path = dir.filePath(QStringLiteral("fmt2.ct3"));
    CHECK(cfg.saveToFile(path, &error));

    // ---- the preamble says what it should, and is readable without the key --
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    f.close();

    CHECK(raw.startsWith(ct::kConfigPreambleMagic));
    CHECK(raw.size() > ct::kConfigPreambleBytes);
    const QByteArray preamble = raw.left(ct::kConfigPreambleBytes);
    CHECK(preamble.contains("format 2\r\n"));
    CHECK(preamble.contains("schema "));
    CHECK(preamble.contains("written-by "));
    // DOS end-of-file, so `type` at a command prompt stops here instead of
    // spraying ciphertext over the terminal.
    CHECK(preamble.contains('\x1a'));
    // Everything after the preamble is the sealed container, which begins with
    // the .ct3s magic — the same bytes, deliberately, because it is the same
    // container and there is only one implementation of it.
    CHECK(!raw.mid(ct::kConfigPreambleBytes).isEmpty());

    ct::ConfigFileInfo info;
    CHECK(ct::peekBinaryConfigFile(path, &info, &error));
    CHECK(info.formatVersion == ct::kConfigFileFormatVersion);
    CHECK(info.schemaVersion == kCurrentSchemaVersion);
    CHECK(!info.writtenBy.isEmpty());

    // The channel name is in the document and not in the file.
    Configuration back;
    CHECK(back.loadFromFile(path, &error));
    CHECK(back.configTitle() == QStringLiteral("Format Two"));
    CHECK(back.catalog().findByName(QStringLiteral("Distinctive Channel Name")).isValid());
    CHECK(!raw.contains("Distinctive Channel Name"));

    // ---- routing: a .ct3 is not a .ct3s and neither is mistaken for the other
    CHECK(ct::isBinaryConfigFile(path));
    CHECK(!ct::isSecureFile(path));
    Configuration::FilePeek peek;
    CHECK(Configuration::peekFile(path, &peek, &error));
    CHECK(!peek.secure);
    CHECK(!peek.requiresPassword); // a .ct3 has never needed one and still does not

    const QString securePath = dir.filePath(QStringLiteral("also.ct3s"));
    SecureSaveOptions opts;
    CHECK(cfg.saveSecureToFile(securePath, opts, &error));
    CHECK(ct::isSecureFile(securePath));
    CHECK(!ct::isBinaryConfigFile(securePath));

    // ---- a damaged body is refused WHOLE, not half-parsed -------------------
    //
    // Aimed at the SALT rather than at a byte picked by position, and the first
    // draft of this test got that wrong in an instructive way: most of the
    // carrier is unclaimed CSPRNG noise, so a bit flipped at the midpoint has
    // roughly even odds of landing somewhere that carries nothing, and the file
    // opens — correctly, because none of its content changed. A tamper test
    // that passes at random is worse than none.
    //
    // The salt is material by construction: it seeds both the chunk placement
    // and the wrap mask, so changing it moves every chunk and produces a wrong
    // file key. It sits at offset 12 of the container's own header, which
    // begins where the preamble ends.
    const auto writeFile = [&](const QString &name, const QByteArray &bytes) {
        const QString p = dir.filePath(name);
        QFile out(p);
        CHECK(out.open(QIODevice::WriteOnly));
        out.write(bytes);
        out.close();
        return p;
    };
    {
        QByteArray torn = raw;
        const int saltByte = ct::kConfigPreambleBytes + 12;
        torn[saltByte] = char(torn.at(saltByte) ^ 0x40);
        const QString tornPath = writeFile(QStringLiteral("torn.ct3"), torn);

        Configuration ruined;
        QString why;
        CHECK(!ruined.loadFromFile(tornPath, &why));
        CHECK(!why.isEmpty());
        // Still recognisably a configuration file, so the message is about
        // damage rather than about the file being some other thing entirely.
        CHECK(ct::isBinaryConfigFile(tornPath));
    }

    // NOT tested here, deliberately: that a byte changed in a slot the
    // container never claimed leaves the file openable. It is true, and it is
    // the reason the case above had to aim at the salt — but there is no slot a
    // test can name as unclaimed without reimplementing buildPlacement, because
    // the placement is a shuffle and material can land in the last slot as
    // readily as the first. An attempt at it here picked the final sixteen
    // bytes and failed, which is the flaky test this note exists to stop
    // somebody rewriting.

    // ---- truncation is refused too -----------------------------------------
    {
        const QString cutPath = dir.filePath(QStringLiteral("cut.ct3"));
        QFile cf(cutPath);
        CHECK(cf.open(QIODevice::WriteOnly));
        cf.write(raw.left(raw.size() / 2));
        cf.close();
        Configuration ruined;
        QString why;
        CHECK(!ruined.loadFromFile(cutPath, &why));
    }

    // ---- a preamble claiming the future is refused before anything decrypts -
    {
        QByteArray future = raw;
        future.replace("format 2\r\n", "format 9\r\n");
        const QString futurePath = dir.filePath(QStringLiteral("future.ct3"));
        QFile ff(futurePath);
        CHECK(ff.open(QIODevice::WriteOnly));
        ff.write(future);
        ff.close();
        Configuration ruined;
        QString why;
        CHECK(!ruined.loadFromFile(futurePath, &why));
        CHECK(why.contains(QLatin1String("newer version")));
    }

    // ---- FORMAT 1 STILL OPENS ----------------------------------------------
    //
    // Format 1 was an INDENTED JSON object: fileType, fileVersion, writtenBy,
    // and then the body's own keys at the top level. The body is unchanged
    // between the two formats by design — only the container moved — so it is
    // taken from the real writer here and re-wrapped, rather than re-spelled by
    // hand. Re-spelling it is what the first draft did, and it tested the
    // spelling: it named the catalog "channels" when the key is "userChannels",
    // so the channel silently did not survive a load that otherwise looked
    // fine. The hand-built old BODIES that pin the schema migrations belong to
    // those migrations' own tests and stay there.
    {
        QJsonObject legacy = configBodyOf(path);
        CHECK(legacy.value(QStringLiteral("fileType")).toString()
              == QLatin1String("CANTripleConfig"));
        legacy[QStringLiteral("writtenBy")] = QStringLiteral("1.1.3");
        legacy[QStringLiteral("configTitle")] = QStringLiteral("Written By The Old Build");

        const QString legacyPath = dir.filePath(QStringLiteral("format1.ct3"));
        QFile lf(legacyPath);
        CHECK(lf.open(QIODevice::WriteOnly));
        lf.write(QJsonDocument(legacy).toJson(QJsonDocument::Indented));
        lf.close();
        // Legible, which is the whole reason this is being left behind.
        QFile check1(legacyPath);
        CHECK(check1.open(QIODevice::ReadOnly));
        CHECK(check1.readAll().contains("Distinctive Channel Name"));
        check1.close();

        // Not mistaken for either binary format on the way in.
        CHECK(!ct::isBinaryConfigFile(legacyPath));
        CHECK(!ct::isSecureFile(legacyPath));
        Configuration::FilePeek oldPeek;
        CHECK(Configuration::peekFile(legacyPath, &oldPeek, &error));
        CHECK(!oldPeek.secure);
        CHECK(!oldPeek.requiresPassword);

        Configuration opened;
        CHECK(opened.loadFromFile(legacyPath, &error));
        CHECK(opened.configTitle() == QStringLiteral("Written By The Old Build"));
        CHECK(opened.catalog().findByName(QStringLiteral("Distinctive Channel Name")).isValid());

        // ---- and saving it converts it, with no user action ----------------
        // The whole migration story is this line: open the old one, save, and
        // the file on disk is format 2. There is deliberately no way back.
        const QString convertedPath = dir.filePath(QStringLiteral("converted.ct3"));
        CHECK(opened.saveToFile(convertedPath, &error));
        CHECK(ct::isBinaryConfigFile(convertedPath));
        QFile cvf(convertedPath);
        CHECK(cvf.open(QIODevice::ReadOnly));
        const QByteArray convertedRaw = cvf.readAll();
        cvf.close();
        CHECK(!convertedRaw.contains("Distinctive Channel Name"));
        CHECK(!convertedRaw.contains("Written By The Old Build"));

        Configuration reopened;
        CHECK(reopened.loadFromFile(convertedPath, &error));
        CHECK(reopened.configTitle() == QStringLiteral("Written By The Old Build"));
        CHECK(reopened.catalog()
                  .findByName(QStringLiteral("Distinctive Channel Name"))
                  .isValid());

        // Saving IN PLACE over the original works too, which is what File >
        // Save does to an old file and the one path a user actually takes.
        CHECK(opened.saveToFile(legacyPath, &error));
        CHECK(ct::isBinaryConfigFile(legacyPath));
        Configuration inPlace;
        CHECK(inPlace.loadFromFile(legacyPath, &error));
        CHECK(inPlace.configTitle() == QStringLiteral("Written By The Old Build"));
    }

    // ---- a file something else holds open still saves -----------------------
    // The reason config_file.cpp writes in place instead of using QSaveFile: an
    // atomic replace cannot overwrite a target another handle owns, which on
    // Windows means any folder OneDrive or Dropbox is syncing. QTemporaryFile
    // is the smallest honest stand-in for that, and this exact shape is what
    // caught the mistake the first time.
    {
        QTemporaryFile held;
        CHECK(held.open());
        const QString heldPath = held.fileName();
        held.close(); // closed, but the QTemporaryFile still owns the path
        CHECK(cfg.saveToFile(heldPath, &error));
        Configuration fromHeld;
        CHECK(fromHeld.loadFromFile(heldPath, &error));
        CHECK(fromHeld.configTitle() == QStringLiteral("Format Two"));
    }
}

// SCHEMA 20: a timer's trigger became a comparison, and every timer already on
// a user's disk is a channel name. The migration has to be exact rather than
// approximate — "start when this channel is non-zero" IS (channel NEQ 0) — and
// this is the case that proves it, because getting it wrong silently changes
// when somebody's timer runs.
static void testTimerTriggerMigration()
{
    // The pre-20 shape, spelled by hand. Taking it from the current writer
    // would test nothing: the writer no longer emits these keys.
    QJsonObject legacy;
    legacy[QStringLiteral("output")] = QStringLiteral("Lap Time");
    legacy[QStringLiteral("start")] = QStringLiteral("Beacon");
    legacy[QStringLiteral("stop")] = QStringLiteral("Pit Entry");
    legacy[QStringLiteral("countDown")] = false;
    legacy[QStringLiteral("active")] = true;

    const TimerRow t = TimerRow::fromJson(legacy);
    CHECK(t.outputChannel == QStringLiteral("Lap Time"));
    CHECK(t.startTerm.aChannel == QStringLiteral("Beacon"));
    CHECK(t.startTerm.op == COND_OP_NEQ);
    CHECK(!t.startTerm.bIsChannel);
    CHECK(qFuzzyIsNull(t.startTerm.bConst));
    CHECK(t.stopTerm.aChannel == QStringLiteral("Pit Entry"));
    CHECK(t.stopTerm.op == COND_OP_NEQ);

    // An ABSENT half stays absent. An empty "start" must not become a term
    // comparing the empty channel against zero, which would be a trigger the
    // mapper then refuses on a document that used to map cleanly.
    QJsonObject halfLegacy;
    halfLegacy[QStringLiteral("output")] = QStringLiteral("Stint");
    halfLegacy[QStringLiteral("start")] = QStringLiteral("Green Flag");
    const TimerRow half = TimerRow::fromJson(halfLegacy);
    CHECK(half.startTerm.aChannel == QStringLiteral("Green Flag"));
    CHECK(half.stopTerm.aChannel.isEmpty());
    CHECK(!half.stopTerm.isMessageOp());

    // And the new form round-trips, including a message operator, so the
    // written file says what was configured.
    TimerRow modern;
    modern.outputChannel = QStringLiteral("Since Frame");
    modern.startTerm.op = COND_OP_MSG_RX;
    modern.startTerm.aMessageBus = 2;
    modern.startTerm.aMessage = QStringLiteral("ECU Status");
    modern.stopTerm.aChannel = QStringLiteral("Engine RPM");
    modern.stopTerm.op = COND_OP_GT;
    modern.stopTerm.bConst = 4000;
    const TimerRow back = TimerRow::fromJson(modern.toJson());
    CHECK(back.startTerm.op == COND_OP_MSG_RX);
    CHECK(back.startTerm.aMessageBus == 2);
    CHECK(back.startTerm.aMessage == QStringLiteral("ECU Status"));
    CHECK(back.stopTerm.aChannel == QStringLiteral("Engine RPM"));
    CHECK(back.stopTerm.op == COND_OP_GT);
    CHECK(qAbs(back.stopTerm.bConst - 4000.0) < 0.0001);
}

// The "for" qualifier as it reaches disk. A new persisted field is worth its own
// case: absent must mean OFF, because that is what every file written before it
// existed says, and a default of anything else silently changes when those
// conditions fire.
// A hold time can only be asked of something that can HOLD. The Conditions
// editor refuses to tick one onto a message comparison, but the editor is only
// one road to the device: a file written before that rule, a package installed
// without opening the row, a readback from a device an older build configured.
// mapToDevice() is the chokepoint they all cross, so the rule has to hold here
// or it does not really hold at all.
// A COUNTER'S UNUSED INPUTS ARE DORMANT, NOT LIVE.
//
// Switching a counter's type keeps the inputs the old type used, so that
// switching back does not destroy what was typed. That only works if everything
// downstream agrees on which inputs the CURRENT type reads. They did not: the
// mapper, the validator and both cross-reference walks each excluded Rate and
// let Follow and Up/Down see each other's leftovers.
// RENAMING A MESSAGE REPOINTS WHAT NAMED IT.
//
// Message references are held by name and bus — there is no stable id to hold
// instead — so a rename in Communications Setup used to orphan every condition,
// timer trigger and counter input that keyed off it, silently. The row still
// read "when Engine Data is received" for a message that no longer existed and
// the first report was the Send refusing the whole configuration.
static void testRenamingAMessageRepointsWhatNamedIt()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    cfg.bus[1].enabled = true;

    CommsSection rx;
    rx.name = QStringLiteral("Engine Data");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    cfg.bus[0].sections.append(rx);
    // A SAME-NAMED MESSAGE ON ANOTHER BUS, so the walk has to key on the bus and
    // not just the name. Renaming this one at the end must leave bus 1 alone.
    CommsSection other = rx;
    other.baseAddress = 0x641;
    cfg.bus[1].sections.append(other);

    for (const char *n : {"Frame Seen", "Since Frame", "Frames"}) {
        Channel ch;
        ch.name = QString::fromLatin1(n);
        ch.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(ch);
    }

    ConditionRow c;
    c.mode = ConditionMode::Momentary;
    c.outputChannel = QStringLiteral("Frame Seen");
    c.setTerms[0].op = COND_OP_MSG_RX;
    c.setTerms[0].aMessageBus = 1;
    c.setTerms[0].aMessage = QStringLiteral("Engine Data");
    cfg.conditionRows.append(c);

    TimerRow t;
    t.outputChannel = QStringLiteral("Since Frame");
    t.startTerm.op = COND_OP_MSG_RX;
    t.startTerm.aMessageBus = 1;
    t.startTerm.aMessage = QStringLiteral("Engine Data");
    cfg.timerRows.append(t);

    CounterRow n;
    n.outputChannel = QStringLiteral("Frames");
    n.mode = int(COUNTER_MODE_UPDOWN);
    n.upSource.kind = int(COUNTER_SRC_MSG_RX);
    n.upSource.messageBus = 1;
    n.upSource.message = QStringLiteral("Engine Data");
    cfg.counterRows.append(n);

    CHECK(mapToDevice(cfg).ok()); // sound before the rename

    // The rename exactly as Communications Setup commits one: the whole bus,
    // through applyBusSections.
    QList<CommsSection> next = cfg.bus[0].sections;
    next[0].name = QStringLiteral("Engine Status");
    QString refusal;
    const bool applied = cfg.applyBusSections(0, next, &refusal);
    if (!applied)
        std::printf("FAIL  applyBusSections refused the rename: %s\n", qPrintable(refusal));
    CHECK(applied);

    CHECK(cfg.conditionRows[0].setTerms[0].aMessage == QStringLiteral("Engine Status"));
    CHECK(cfg.timerRows[0].startTerm.aMessage == QStringLiteral("Engine Status"));
    CHECK(cfg.counterRows[0].upSource.message == QStringLiteral("Engine Status"));
    // Which is the point: it still maps, and Check Channels is quiet.
    CHECK(mapToDevice(cfg).ok());
    for (const ValidationIssue &v : validateConfiguration(cfg))
        CHECK(!v.message.contains(QStringLiteral("Engine Data")));

    // BUS 1's same-named message is renamed now. Nothing above pointed at bus 2,
    // so nothing above may move.
    QList<CommsSection> next2 = cfg.bus[1].sections;
    next2[0].name = QStringLiteral("Something Else");
    CHECK(cfg.applyBusSections(1, next2, &refusal));
    CHECK(cfg.conditionRows[0].setTerms[0].aMessage == QStringLiteral("Engine Status"));
    CHECK(cfg.timerRows[0].startTerm.aMessage == QStringLiteral("Engine Status"));
    CHECK(cfg.counterRows[0].upSource.message == QStringLiteral("Engine Status"));

    // DELETING one is the case a rename cannot cover, and it must be reported at
    // the rows that have the problem rather than by the Send refusing. All three
    // are asked — timers used to defer this to the mapper and say nothing here.
    QList<CommsSection> gone;
    CHECK(cfg.applyBusSections(0, gone, &refusal));
    int condSaid = 0, timerSaid = 0, counterSaid = 0;
    for (const ValidationIssue &v : validateConfiguration(cfg)) {
        if (v.severity != ValidationIssue::Error
            || !v.message.contains(QStringLiteral("has no message named")))
            continue;
        if (v.location.startsWith(QStringLiteral("User Condition")))
            ++condSaid;
        else if (v.location.startsWith(QStringLiteral("Timer")))
            ++timerSaid;
        else if (v.location.startsWith(QStringLiteral("Counter")))
            ++counterSaid;
    }
    CHECK(condSaid == 1);
    CHECK(timerSaid == 1);
    CHECK(counterSaid == 1);
}

static void testCounterUnusedInputsStayDormant()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;

    Channel out;
    out.name = QStringLiteral("Trip Count");
    out.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(out);
    Channel src;
    src.name = QStringLiteral("Door Sw");
    src.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(src);
    Channel fol;
    fol.name = QStringLiteral("Gear");
    fol.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(fol);

    // A Follow counter carrying an Up left over from when it was an Up/Down
    // counter — and the message that Up names does not exist. Resolving it
    // used to be an ERROR, and main_window treats any mapper error as fatal, so
    // the whole configuration could not be sent because of an input this
    // counter does not read.
    CounterRow c;
    c.outputChannel = QStringLiteral("Trip Count");
    c.mode = int(COUNTER_MODE_FOLLOW);
    c.followChannel = QStringLiteral("Gear");
    c.upSource.kind = int(COUNTER_SRC_MSG_RX);
    c.upSource.messageBus = 1;
    c.upSource.message = QStringLiteral("LongGone");
    c.downChannel = QStringLiteral("Door Sw");
    cfg.counterRows.append(c);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.counters.size() == 1);
    if (!mr.tables.counters.isEmpty()) {
        const CounterConfig &wc = mr.tables.counters[0];
        CHECK(wc.up_signal_idx == SIG_MSG_NONE);
        CHECK(wc.down_signal_idx == SIG_MSG_NONE);
        CHECK(wc.input_kinds == 0); // the dead message left no kind behind
        CHECK(wc.follow_signal_idx != SIG_MSG_NONE);
    }
    // Check Channels must not claim the counter reads Door Sw either, or the
    // channel can never be tidied away.
    for (const ValidationIssue &v : validateConfiguration(cfg))
        CHECK(!v.message.contains(QStringLiteral("LongGone")));

    // The mirror image: an Up/Down counter whose Follow is left over. A stale
    // Follow channel used to be resolved, which spent a device signal slot on
    // an input the counter does not read.
    cfg.counterRows[0].mode = int(COUNTER_MODE_UPDOWN);
    cfg.counterRows[0].upSource = CounterSource{};
    cfg.counterRows[0].upChannel = QStringLiteral("Door Sw");
    const MappingResult ud = mapToDevice(cfg);
    CHECK(ud.ok());
    if (!ud.tables.counters.isEmpty()) {
        CHECK(ud.tables.counters[0].follow_signal_idx == SIG_MSG_NONE);
        CHECK(ud.tables.counters[0].up_signal_idx != SIG_MSG_NONE);
    }
}

// Message inputs were never validated at all, so the first report of a broken
// one was the Send refusing the whole configuration and naming the mapper.
static void testCounterMessageInputsAreValidated()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel out;
    out.name = QStringLiteral("Frames");
    out.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(out);

    CommsSection rx;
    rx.name = QStringLiteral("Engine Data");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    cfg.bus[0].sections.append(rx);

    const auto errorsFor = [](const Configuration &doc, const QString &needle) {
        int n = 0;
        for (const ValidationIssue &v : validateConfiguration(doc))
            if (v.severity == ValidationIssue::Error && v.message.contains(needle))
                ++n;
        return n;
    };

    // A message kind with nothing selected. Reachable directly: the picker is
    // empty on a document with no sections, so OK is accepted with no message.
    CounterRow c;
    c.outputChannel = QStringLiteral("Frames");
    c.mode = int(COUNTER_MODE_UPDOWN);
    c.upSource.kind = int(COUNTER_SRC_MSG_RX);
    cfg.counterRows.append(c);
    CHECK(errorsFor(cfg, QStringLiteral("no message selected")) == 1);

    // A message that named a section which has since gone.
    cfg.counterRows[0].upSource.messageBus = 1;
    cfg.counterRows[0].upSource.message = QStringLiteral("Deleted Msg");
    CHECK(errorsFor(cfg, QStringLiteral("has no message named")) == 1);

    // A real one is clean — and specifically must NOT be warned about as a
    // counter with neither an Up nor a Down, which it plainly has.
    cfg.counterRows[0].upSource.message = QStringLiteral("Engine Data");
    CHECK(errorsFor(cfg, QStringLiteral("message")) == 0);
    for (const ValidationIssue &v : validateConfiguration(cfg))
        CHECK(!v.message.contains(QStringLiteral("neither an Up nor a Down")));
}

static void testQualifyMaskDropsMessageComparisonsAtTheMapper()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;

    CommsSection rx;
    rx.name = QStringLiteral("Engine Data");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x640;
    rx.messageLengthBytes = 8;
    cfg.bus[0].sections.append(rx);

    Channel out;
    out.name = QStringLiteral("Frame Seen");
    out.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(out);
    Channel src;
    src.name = QStringLiteral("Coolant");
    src.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(src);

    // Comparison 1 is a channel test, which CAN be held. Comparison 2 is a
    // received frame, which cannot — it is true for one evaluation pass by
    // construction. The document asks for both to be timed, which is the state
    // a pre-fix build could write.
    ConditionRow c;
    c.mode = ConditionMode::Momentary;
    c.outputChannel = QStringLiteral("Frame Seen");
    c.setTerms.clear();
    ConditionTermRow chan;
    chan.aChannel = QStringLiteral("Coolant");
    chan.op = COND_OP_GT;
    chan.bConst = 105.0;
    ConditionTermRow msg;
    msg.op = COND_OP_MSG_RX;
    msg.aMessageBus = 1;
    msg.aMessage = QStringLiteral("Engine Data");
    c.setTerms << chan << msg;
    c.setJoiners << int(COND_JOIN_AND);
    c.qualifySetMs = 2000;
    c.qualifySetTerms = 0x03;
    cfg.conditionRows.append(c);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.conditions.size() == 1);
    if (!mr.tables.conditions.isEmpty()) {
        const ConditionConfig &cc = mr.tables.conditions[0];
        // The duration survives...
        CHECK(cc.set_qualify_cs == 200);
        // ...the channel comparison keeps its bit, and the message comparison
        // loses it. 0x03 would have made the device wait forever.
        CHECK(cc.set_qualify_terms == 0x01);
    }

    // And a mask naming ONLY message comparisons comes out empty rather than
    // being passed along to a device that cannot satisfy any of it.
    cfg.conditionRows[0].qualifySetTerms = 0x02;
    const MappingResult only = mapToDevice(cfg);
    CHECK(only.ok());
    if (!only.tables.conditions.isEmpty())
        CHECK(only.tables.conditions[0].set_qualify_terms == 0x00);
}

static void testConditionQualifierPersists()
{
    QJsonObject legacy;
    legacy[QStringLiteral("output")] = QStringLiteral("Overheat");
    legacy[QStringLiteral("mode")] = 1;
    const ConditionRow old = ConditionRow::fromJson(legacy);
    CHECK(old.qualifySetMs == 0);
    CHECK(old.qualifyResetMs == 0);
    CHECK(old.qualifySetTerms == 0);
    CHECK(old.qualifyResetTerms == 0);

    // BOTH SIDES, INDEPENDENTLY — the shape this exists for is "set at once,
    // clear only after the fault has been gone a while", so a writer that
    // collapsed the two into one would pass a same-value test and fail this.
    ConditionRow c;
    c.outputChannel = QStringLiteral("Overheat");
    c.setTerms[0].aChannel = QStringLiteral("Coolant");
    c.setTerms[0].op = COND_OP_GT;
    c.setTerms[0].bConst = 105;
    c.resetTerms[0].aChannel = QStringLiteral("Coolant");
    c.resetTerms[0].op = COND_OP_LT;
    c.resetTerms[0].bConst = 95;
    c.qualifySetMs = 5000;
    c.qualifySetTerms = 0x01;
    c.qualifyResetMs = 30000;
    c.qualifyResetTerms = 0;
    const ConditionRow back = ConditionRow::fromJson(c.toJson());
    CHECK(back.qualifySetMs == 5000);
    CHECK(back.qualifySetTerms == 0x01);
    CHECK(back.qualifyResetMs == 30000);
    CHECK(back.qualifyResetTerms == 0);

    // The one-sided spelling this briefly had, never released, reads as the SET
    // side. Anything written in that window meant exactly that.
    QJsonObject oneSided;
    oneSided[QStringLiteral("output")] = QStringLiteral("Overheat");
    // WITH a mode, because the qualifier only ever existed in the modes form —
    // fromJson routes the older shapes down a different branch entirely, and a
    // fixture without it tests that branch rather than this migration.
    oneSided[QStringLiteral("mode")] = 1;
    oneSided[QStringLiteral("qualifyMs")] = 2500;
    oneSided[QStringLiteral("qualifyTerms")] = 0x02;
    const ConditionRow migrated = ConditionRow::fromJson(oneSided);
    CHECK(migrated.qualifySetMs == 2500);
    CHECK(migrated.qualifySetTerms == 0x02);
    CHECK(migrated.qualifyResetMs == 0);

    // Off carries NO trace of the feature, per side: a condition that qualifies
    // only its Set writes no Reset keys at all.
    ConditionRow setOnly = c;
    setOnly.qualifyResetMs = 0;
    setOnly.qualifyResetTerms = 0x01; // set, but meaningless without a duration
    const QJsonObject o = setOnly.toJson();
    CHECK(o.contains(QStringLiteral("qualifySetMs")));
    CHECK(!o.contains(QStringLiteral("qualifyResetMs")));
    CHECK(!o.contains(QStringLiteral("qualifyResetTerms")));
    CHECK(ConditionRow::fromJson(o).qualifyResetTerms == 0);
}

// The identity and the policy as they reach disk. The sharp edge is the fleet
// key: it is the fleet's only secret and only a .ct3s carries it, so the same
// document has to write two different things. Both files are sealed now, so the
// asymmetry is a deliberate choice rather than a consequence of one format
// being legible — see withFleetKey() for the reasons it survived format 2.
static void testFleetDocument()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString error;

    Configuration cfg;
    FleetIdentity id;
    id.vendorId = QStringLiteral("Detailed Motor");
    id.modelId = QStringLiteral("CAN Triple");
    id.serialNumber = 0x00000123u;
    id.configVersion = 12;
    id.fleetKey = AccessKey(0xDEADBEEFu);
    cfg.setFleetIdentity(id);

    UploadPolicy policy;
    policy.allowedSerials = QList<quint32>{0x00000123u, 0x00000124u};
    policy.warnOnOlderVersion = false; // deliberately not the default
    cfg.setUploadPolicy(policy);

    const QByteArray keyBytes = accessKeyBytes(id.fleetKey);
    const auto fileBytes = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    const auto carriesKey = [&keyBytes, &id](const QByteArray &blob) {
        return blob.contains(keyBytes) || blob.contains(keyBytes.toHex())
               || blob.contains(keyBytes.toHex().toUpper()) || blob.contains(keyBytes.toBase64())
               || blob.contains(QByteArray::number(qlonglong(id.fleetKey)));
    };

    // ---- a plain .ct3 carries the fleet, never the secret ----
    const QString plain = dir.filePath(QStringLiteral("fleet.ct3"));
    CHECK(cfg.saveToFile(plain, &error));
    const QByteArray plainRaw = fileBytes(plain);
    CHECK(!plainRaw.isEmpty());
    CHECK(!carriesKey(plainRaw));

    Configuration loaded;
    CHECK(loaded.loadFromFile(plain, &error));
    CHECK(loaded.fleetIdentity().vendorId == id.vendorId);
    CHECK(loaded.fleetIdentity().modelId == id.modelId);
    CHECK(loaded.fleetIdentity().serialNumber == id.serialNumber);
    CHECK(loaded.fleetIdentity().configVersion == id.configVersion);
    CHECK(loaded.fleetIdentity().sameFleetAs(id));
    CHECK(loaded.fleetIdentity().fleetKey == kNoAccessKey);
    CHECK(loaded.uploadPolicy().allowedSerials == policy.allowedSerials);
    CHECK(loaded.uploadPolicy().allowsSerial(0x00000124u));
    CHECK(!loaded.uploadPolicy().allowsSerial(0x00000125u));
    CHECK(!loaded.uploadPolicy().warnOnOlderVersion);
    CHECK(loaded.uploadPolicy().requireFleetKey);

    // ---- a .ct3s is sealed, so it may carry the key ----
    // Without this a customer's copy could never verify a device's attestation
    // by itself, and somebody would have to type the fleet secret in by hand on
    // every machine — which is how a shared secret stops being one.
    const QString secure = dir.filePath(QStringLiteral("fleet.ct3s"));
    SecureSaveOptions opts;
    CHECK(cfg.saveSecureToFile(secure, opts, &error));
    const QByteArray secureRaw = fileBytes(secure);
    CHECK(!secureRaw.isEmpty());
    // Sealed means sealed: the key is inside the encrypted body, so it must not
    // be visible in the file's bytes either.
    CHECK(!carriesKey(secureRaw));

    Configuration reopened;
    CHECK(reopened.loadFromFile(secure, &error));
    CHECK(reopened.fleetIdentity().fleetKey == id.fleetKey);
    CHECK(reopened.fleetIdentity().sameFleetAs(id));
    CHECK(reopened.fleetIdentity().serialNumber == id.serialNumber);
    CHECK(reopened.fleetIdentity().configVersion == id.configVersion);
    CHECK(reopened.uploadPolicy().allowedSerials == policy.allowedSerials);
    CHECK(!reopened.uploadPolicy().warnOnOlderVersion);
}

// The Protected tier as it reaches disk: what a viewer without the Edit
// Protected Comms password may see, what they may change, and that neither
// answer can be walked around through the loader.
//
// The channels half is the part worth labouring. Locking the message but
// leaving its channels editable would protect the secret and break the
// function: change a protected channel's resolution and the message silently
// starts decoding to different numbers, with nothing on screen to say why.
//
// STAGE E: as of 2.3.0 this function covers only CONCEALMENT. The single
// isChannelProtected() it was written against answered two questions that have
// since come apart — isChannelConcealed (a value is withheld; lifted by the
// password) and isChannelEditLocked (a control is disabled; NOT lifted by it,
// because the password buys the right to untick the tier and unticking is what
// unlocks editing). These lines inherited the first because that is the
// property they always asserted. Nothing here exercises the second, and nothing
// here exercises ReadOnly at all — the tier that is VISIBLE and still not
// editable, i.e. the one case where the two predicates disagree. That is
// plan.md §6.2's testChannelPredicateSplit and it is not yet written.
static void testProtectedDocument()
{
    const QString pass = QStringLiteral("vendor-comms-secret");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    for (const char *name : {"Secret RPM", "Secret Temp", "Open Speed"}) {
        Channel c;
        c.name = QString::fromUtf8(name);
        c.userDefined = true;
        cfg.catalog().addOrUpdateUserChannel(c);
    }

    CommsSection secret;
    secret.name = QStringLiteral("Proprietary ECU Feed");
    secret.device = SectionDevice::ReceiveMessage;
    secret.alignment = SectionAlignment::WordSwap;
    secret.baseAddress = 0x7AB;
    secret.messageLengthBytes = 8;
    secret.protection = CommsProtection::Protected;
    for (const auto &spec : {std::make_pair(QStringLiteral("Secret RPM"), 0),
                             std::make_pair(QStringLiteral("Secret Temp"), 16)}) {
        CommsChannelRow r;
        r.channelName = spec.first;
        r.startBit = spec.second;
        r.bitLength = 16;
        secret.rows.append(r);
    }
    cfg.bus[0].sections.append(secret);

    CommsSection open;
    open.name = QStringLiteral("Wheel Speeds");
    open.device = SectionDevice::ReceiveMessage;
    open.baseAddress = 0x123;
    open.messageLengthBytes = 8;
    CommsChannelRow openRow;
    openRow.channelName = QStringLiteral("Open Speed");
    openRow.startBit = 0;
    openRow.bitLength = 16;
    open.rows.append(openRow);
    cfg.bus[0].sections.append(open);

    // ---- a marking with NOTHING BEHIND IT conceals, even here ----
    // This block asserted the reverse until 2.3.2: "with no password nothing is
    // withheld, and that is deliberate — there is nobody a password-less
    // document could sensibly be withheld from." The reasoning failed in the one
    // case that matters. Every section a Get produces is exactly this shape — a
    // tier with no key, because the wire's field is reserved[4] — so the rule
    // handed back every message its author had marked. No password existing is
    // not a reason to show the protocol; it is the reason nothing can open it.
    CHECK(!cfg.hasCommsPassword());
    CHECK(cfg.commsRevealed());
    CHECK(cfg.bus[0].sections[0].messageKey == kNoAccessKey);
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    CHECK(cfg.concealedChannelNames().contains(QStringLiteral("Secret RPM")));
    // The unmarked message beside it is untouched throughout: the point is to
    // protect the protocol, not the outputs.
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Open Speed")));

    // The MARKING'S OWN password, which is what guards a Protected section as of
    // 2.3.1 and, as of 2.3.2, the only thing that does. The document-wide Edit
    // Protected Comms password reveals nothing at any tier now: a keyless
    // Protected section used to fall back to it, and since the wire carries no
    // key that arm published every section any Get produced.
    const AccessKey secretKey = deriveAccessKey(QStringLiteral("proprietary-own"));
    cfg.bus[0].sections[0].messageKey = secretKey;
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    CHECK(!cfg.bus[0].sections[0].isConcealed(cfg.isSectionRevealed(cfg.bus[0].sections[0])));

    // ---- setting the password ----
    CHECK(cfg.setCommsPassword(pass));
    CHECK(cfg.hasCommsPassword());
    CHECK(cfg.accessVerifiers().isSet(AccessFunction::EditProtectedComms));
    // Only that one function is a document concern. Send and Get say what a
    // UNIT will do for you, not what a file will show you, so a .ct3 has no
    // business holding them.
    CHECK(!cfg.accessVerifiers().isSet(AccessFunction::SendConfiguration));
    CHECK(!cfg.accessVerifiers().isSet(AccessFunction::GetConfiguration));
    // Setting a password must not lock its author out of the document they were
    // editing a moment ago.
    CHECK(cfg.commsRevealed());

    // ---- concealed: the message and its channels are both withheld ----
    // Every assertion below follows the CONCEALMENT half of the old
    // isChannelProtected(), which 2.3.0 split in two. The old predicate was
    // lifted by the password, and that is what these lines have always pinned,
    // so isChannelConcealed is the one that inherits them. The other half —
    // isChannelEditLocked, which the password does NOT lift — has no coverage
    // here; see the note at the head of this function.
    // concealProtectedComms() drops every per-section grant as well as the
    // document flag, which is what re-conceals the section here.
    cfg.concealProtectedComms();
    CHECK(!cfg.commsRevealed());
    CHECK(cfg.bus[0].sections[0].isConcealed(cfg.isSectionRevealed(cfg.bus[0].sections[0])));
    CHECK(!cfg.bus[0].sections[1].isConcealed(cfg.isSectionRevealed(cfg.bus[0].sections[1])));
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret Temp")));
    // A channel carried only by an unprotected message is withheld from nobody —
    // the point is to protect the protocol, not the outputs.
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Open Speed")));
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Nothing Uses This")));
    // Matching is by channel, not by spelling of the moment: the grids look
    // names up however the user typed them.
    CHECK(cfg.isChannelConcealed(QStringLiteral("secret rpm")));
    const QStringList locked = cfg.concealedChannelNames();
    CHECK(locked.size() == 2);
    CHECK(locked.contains(QStringLiteral("Secret RPM")));
    CHECK(!locked.contains(QStringLiteral("Open Speed")));

    // ---- the wrong document password changes nothing; the right one changes
    // ---- nothing either, and that is 2.3.2 ----
    // revealProtectedComms still verifies what it always verified, and it still
    // gates setCommsPassword. What it no longer does is open a section: this
    // message is guarded by ITS OWN password, and one password sitting in the
    // same file as every Protected message in it was never a proof about any of
    // them individually.
    CHECK(!cfg.revealProtectedComms(QStringLiteral("not it")));
    CHECK(!cfg.commsRevealed());
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    CHECK(cfg.revealProtectedComms(pass));
    CHECK(cfg.commsRevealed());
    CHECK(cfg.isChannelConcealed(QStringLiteral("Secret RPM")));

    // The section's own password, proved, is what withholds nothing any more.
    // Note what this does NOT say as of 2.3.0: those channels are still
    // EDIT-LOCKED, and stay so until the section's tier is actually lowered.
    // Unlocking buys viewing and the right to untick the box, nothing more.
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Secret RPM")));
    CHECK(!cfg.isChannelConcealed(QStringLiteral("Secret Temp")));
    CHECK(cfg.concealedChannelNames().isEmpty());

    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString err;
    const QString path = dir.filePath(QStringLiteral("protected.ct3"));

    // ---- the tier and the verifier survive a plain .ct3 round trip ----
    {
        CHECK(cfg.saveToFile(path, &err));
        Configuration back;
        CHECK(back.loadFromFile(path, &err));
        CHECK(back.bus[0].sections.size() == 2);
        if (back.bus[0].sections.size() == 2) {
            CHECK(back.bus[0].sections[0].protection == CommsProtection::Protected);
            // Not merely "less than Protected": None is spelled by OMITTING the
            // key, so an ordinary section must come back with no trace of the
            // feature rather than with some weaker tier read out of a default.
            CHECK(back.bus[0].sections[1].protection == CommsProtection::None);
        }
        // A session grant is not something a file can carry, so it opens
        // concealed however it was saved.
        CHECK(back.hasCommsPassword());
        CHECK(!back.commsRevealed());
        CHECK(back.isChannelConcealed(QStringLiteral("Secret RPM")));
        // ...and the document password still verifies out of the file's own
        // verifier, while opening nothing. The SECTION's key came through the
        // round trip too, which is what makes the message openable again — a
        // grant taken against the key the file carries.
        CHECK(back.revealProtectedComms(pass));
        CHECK(back.isChannelConcealed(QStringLiteral("Secret RPM")));
        CHECK(back.bus[0].sections[0].messageKey == secretKey);
        back.grantSectionAccess(0, back.bus[0].sections[0]);
        CHECK(!back.isChannelConcealed(QStringLiteral("Secret RPM")));

        // THE POINT OF FORMAT 2, asserted against the bytes on disk rather
        // than inferred from the fact that a container was used. A format-1
        // .ct3 carried every one of these strings in the clear; this file
        // carries none of them, so the channel name and the message structure
        // are no longer findable with a text search of the drive.
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        f.close();
        CHECK(!raw.contains("Secret RPM"));
        CHECK(!raw.contains("sections"));
        CHECK(!raw.contains("fileVersion"));
        // The CAN ID is checked as the JSON spelling rather than as the bare
        // three characters "7AB". Three bytes turn up in forty kilobytes of
        // CSPRNG noise about once in four hundred runs, and a suite that fails
        // one build in four hundred teaches people to re-run it.
        CHECK(!raw.contains("\"7AB\""));
        // Never in any format, and the reason this line predates the change:
        // the password is not in the file, sealed or otherwise.
        CHECK(!raw.contains(pass.toUtf8()));

        // The preamble IS legible, and only the preamble. That is deliberate:
        // a person, a support ticket or a script can see what the file is and
        // which schema it holds without this program and without the key.
        CHECK(raw.startsWith(ct::kConfigPreambleMagic));
        CHECK(raw.contains("schema "));
        CHECK(!raw.mid(ct::kConfigPreambleBytes).contains("schema "));
        // Two saves of the same document share no bytes: the container keys
        // and pads each one independently. Worth pinning, because a writer that
        // quietly became deterministic would be one that stopped encrypting.
        const QString twin = dir.filePath(QStringLiteral("twin.ct3"));
        CHECK(cfg.saveToFile(twin, &err));
        QFile tf(twin);
        CHECK(tf.open(QIODevice::ReadOnly));
        const QByteArray twinRaw = tf.readAll();
        tf.close();
        CHECK(twinRaw.mid(ct::kConfigPreambleBytes) != raw.mid(ct::kConfigPreambleBytes));
        // ...while the preamble, which says nothing secret, is identical.
        CHECK(twinRaw.left(ct::kConfigPreambleBytes) == raw.left(ct::kConfigPreambleBytes));

        Configuration::FilePeek fp;
        CHECK(Configuration::peekFile(path, &fp, &err));
        CHECK(!fp.secure);
        CHECK(!fp.requiresPassword);
        CHECK(fp.commsProtected); // what the Open path prompts off
    }

    // ---- a pre-v19 file wrote the flag as "hidden" ----
    // The rename is a source-level one. A file written by the old build has to
    // still open with its message protected, or every configuration already on
    // a customer's disk silently loses the protection it was saved with.
    //
    // 2.3.0 makes this the sharpest trap in the whole migration, so read the
    // assertion below carefully before "fixing" it: the legacy key is literally
    // spelled "hidden" and the new tier ladder has a Hidden in it, but they are
    // NOT the same thing. This key is the direct ancestor of protectedComms, so
    // it maps to Protected. Re-pointing it at CommsProtection::Hidden because
    // the names match would look like a tidy-up in review and would silently
    // downgrade every schema-7 file on disk.
    {
        QJsonObject section;
        section["name"] = QStringLiteral("Legacy Secret");
        section["device"] = QStringLiteral("receive");
        section["baseAddress"] = QStringLiteral("6A1");
        section["messageLength"] = 8;
        section["hidden"] = true;
        QJsonObject busObj;
        busObj["enabled"] = true;
        busObj["sections"] = QJsonArray{section};
        QJsonObject root;
        root["fileType"] = QStringLiteral("CANTripleConfig");
        root["fileVersion"] = 2;
        root["buses"] = QJsonArray{busObj};
        const QString legacyPath = dir.filePath(QStringLiteral("legacy-hidden.ct3"));
        QFile lf(legacyPath);
        CHECK(lf.open(QIODevice::WriteOnly));
        lf.write(QJsonDocument(root).toJson());
        lf.close();

        Configuration legacy;
        CHECK(legacy.loadFromFile(legacyPath, &err));
        CHECK(legacy.bus[0].sections.size() == 1);
        if (legacy.bus[0].sections.size() == 1)
            CHECK(legacy.bus[0].sections[0].protection == CommsProtection::Protected);
    }

    // ---- the same document as a .ct3s: nothing legible left on disk ----
    {
        const QString spath = dir.filePath(QStringLiteral("protected.ct3s"));
        SecureSaveOptions opts;
        opts.embeddedCommsKey = AccessKey(0x0BADC0DEu);

        // Saving secure is refused while the protected messages are concealed:
        // the body has to be assembled in full to be sealed, and a session that
        // cannot see those messages must not be the one that rewrites the file
        // carrying them.
        Configuration concealed;
        CHECK(concealed.loadFromFile(path, &err));
        CHECK(!concealed.commsRevealed());
        // KEYED and concealed, and it SAVES. The refusal this used to assert
        // is gone: a .ct3s was always sealed, so writing one discloses nothing
        // the session could read, and what comes back out carries the same tier
        // and the same messageKey. The marking is enforced at the editor, not
        // at the writer.
        CHECK(concealed.anyKeyedSectionConcealed());
        const QString stillLocked = dir.filePath(QStringLiteral("still-locked.ct3s"));
        CHECK(concealed.saveSecureToFile(stillLocked, opts, &err));
        {
            // And it comes back locked. A writer that quietly dropped the tier
            // or the key would pass the line above and be the actual disaster.
            Configuration reread;
            CHECK(reread.loadFromFile(stillLocked, &err));
            CHECK(reread.anyKeyedSectionConcealed());
            // Compared against what went IN rather than against a literal
            // count: this document's shape belongs to the case above and a
            // hard-coded 1 was simply wrong about it.
            for (int b = 0; b < 3; ++b) {
                CHECK(reread.bus[b].sections.size() == concealed.bus[b].sections.size());
                for (int i = 0; i < reread.bus[b].sections.size()
                     && i < concealed.bus[b].sections.size(); ++i) {
                    CHECK(reread.bus[b].sections[i].protection
                          == concealed.bus[b].sections[i].protection);
                    CHECK(reread.bus[b].sections[i].messageKey
                          == concealed.bus[b].sections[i].messageKey);
                }
            }
        }

        CHECK(!cfg.anyKeyedSectionConcealed()); // the grant taken above
        CHECK(cfg.saveSecureToFile(spath, opts, &err));
        CHECK(cfg.isSecureFile());
        CHECK(ct::isSecureFile(spath));
        QFile f(spath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        f.close();
        CHECK(!raw.contains("7AB"));
        CHECK(!raw.contains("Secret RPM"));
        CHECK(!raw.contains("CANTripleConfig"));
        // The unprotected message goes opaque with the rest of it: the format
        // hides the document, not the ticked boxes inside it.
        CHECK(!raw.contains("Wheel Speeds"));

        Configuration::FilePeek fp;
        CHECK(Configuration::peekFile(spath, &fp, &err));
        CHECK(fp.secure);
        CHECK(!fp.requiresPassword);

        Configuration reopened;
        CHECK(reopened.loadFromFile(spath, &err));
        CHECK(reopened.isSecureFile());
        CHECK(reopened.bus[0].sections.size() == 2);
        if (reopened.bus[0].sections.size() == 2)
            CHECK(reopened.bus[0].sections[0].protection == CommsProtection::Protected);
        // The file carries the 4-byte key so a customer can satisfy a DEVICE's
        // protected-comms gate without ever holding the password...
        CHECK(reopened.commsKey() == opts.embeddedCommsKey);
        // ...which is not the same as being allowed to read the protocol. The
        // document still opens concealed, and the key is no way round that.
        CHECK(!reopened.commsRevealed());
        CHECK(reopened.isChannelConcealed(QStringLiteral("Secret RPM")));
    }
}

// Two ways the protection used to come apart in the writer rather than in the
// format. Both were real, both were found by audit rather than by use, and
// neither had anything holding it down — which is why they are here.
static void testConcealedSaveGuards()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    QString err;

    const QString pass = QStringLiteral("first-comms-secret");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel c;
    c.name = QStringLiteral("Secret RPM");
    c.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(c);

    CommsSection secret;
    secret.name = QStringLiteral("Proprietary ECU Feed");
    secret.device = SectionDevice::ReceiveMessage;
    secret.baseAddress = 0x7AB;
    secret.messageLengthBytes = 8;
    secret.protection = CommsProtection::Protected;
    // The marking's own password: what actually guards it, and what makes the
    // refusal below the LAUNDERING case rather than the keyless one. A section
    // with no password in existence is concealed too, but writing it out hands
    // nobody anything they could not already read, so the writers allow it —
    // see test_protection's testSavingAfterAGet.
    secret.messageKey = deriveAccessKey(QStringLiteral("proprietary-own"));
    CommsChannelRow row;
    row.channelName = QStringLiteral("Secret RPM");
    row.startBit = 0;
    row.bitLength = 16;
    secret.rows.append(row);
    cfg.bus[0].sections.append(secret);
    CHECK(cfg.setCommsPassword(pass));
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);

    const QString plain = dir.filePath(QStringLiteral("guard.ct3"));
    CHECK(cfg.saveToFile(plain, &err));

    // ---- a plain Save As of a concealed document, and what it does NOT leak --
    //
    // This case used to be the bypass and the test used to assert the refusal
    // that closed it: saveToFile emitted indented JSON, so File > Save As with
    // no password known wrote the protected message's CAN ID and bit layout
    // where Notepad could read them. Format 2 removed the leak rather than the
    // ability to save, so the refusal went with it — and this now checks the
    // thing that actually matters, which is that the save still gives the
    // writer nothing.
    {
        Configuration concealed;
        CHECK(concealed.loadFromFile(plain, &err));
        CHECK(!concealed.commsRevealed());
        CHECK(concealed.anyKeyedSectionConcealed());

        const QString shut = dir.filePath(QStringLiteral("still-shut.ct3"));
        err.clear();
        CHECK(concealed.saveToFile(shut, &err));
        CHECK(QFile::exists(shut));

        // Nothing legible came out of it, which is the whole basis for allowing
        // the save at all.
        QFile sf(shut);
        CHECK(sf.open(QIODevice::ReadOnly));
        const QByteArray shutRaw = sf.readAll();
        sf.close();
        CHECK(!shutRaw.contains("Secret RPM"));
        CHECK(!shutRaw.contains("\"7AB\""));

        // And the message is still shut on the far side: same tier, same key,
        // still concealed to a session that has proved nothing. Saving moved a
        // locked message; it did not open one.
        Configuration reopened;
        CHECK(reopened.loadFromFile(shut, &err));
        CHECK(reopened.anyKeyedSectionConcealed());
        CHECK(!reopened.commsRevealed());
        CHECK(reopened.bus[0].sections.size() == 1);
        CHECK(reopened.bus[0].sections.first().messageKey
              == concealed.bus[0].sections.first().messageKey);

        // The reveal path is unchanged and is still the only thing that opens
        // it: the document-wide password alone opens no message at any tier,
        // and the section grant is what does.
        CHECK(reopened.revealProtectedComms(pass));
        CHECK(reopened.anyKeyedSectionConcealed());
        reopened.grantSectionAccess(0, reopened.bus[0].sections[0]);
        CHECK(!reopened.anyKeyedSectionConcealed());
    }

    // ---- changing the password used to re-seal under the OLD one ----
    // setCommsPassword rewrote the verifier and the key but left the cached
    // secure-save options alone, so the next .ct3s Save wrapped the file key
    // under the previous password while the document's verifier expected the
    // new one. The file that came out opened with neither.
    {
        const QString spath = dir.filePath(QStringLiteral("reseal.ct3s"));
        SecureSaveOptions opts;
        opts.requirePassword = true;
        opts.password = pass;
        opts.embeddedCommsKey = cfg.commsKey();
        CHECK(cfg.saveSecureToFile(spath, opts, &err));

        const QString second = QStringLiteral("second-comms-secret");
        CHECK(cfg.commsRevealed());
        CHECK(cfg.setCommsPassword(second));
        // Deliberately re-saving with the options the DOCUMENT now carries,
        // which is what a plain Save does — not with the `opts` above, which
        // still names the old password.
        CHECK(cfg.saveSecureToFile(spath, cfg.secureOptions(), &err));

        Configuration reopened;
        CHECK(reopened.loadFromFile(spath, &err, second));
        CHECK(reopened.isSecureFile());
        CHECK(reopened.bus[0].sections.size() == 1);
        // The embedded key moved with the password too, or the file would still
        // be handing a device the key derived from the retired one.
        CHECK(reopened.commsKey() == deriveAccessKey(second));

        Configuration stale;
        CHECK(!stale.loadFromFile(spath, &err, pass));
    }
}

// Protected messages: the protocol detail is withheld, the channel names are
// not. The report is the sharpest test of it — it is printable and exportable,
// so anything that leaks there leaks in the most portable possible form.
static void testProtectedMessage()
{
    const QString pass = QStringLiteral("vendor-secret");

    Configuration cfg;
    cfg.bus[0].enabled = true;
    Channel ch;
    ch.name = QStringLiteral("Engine RPM");
    ch.userDefined = true;
    cfg.catalog().addOrUpdateUserChannel(ch);

    CommsSection secret;
    secret.name = QStringLiteral("Proprietary ECU Feed"); // deliberately not the ID
    secret.device = SectionDevice::ReceiveMessage;
    secret.alignment = SectionAlignment::WordSwap;
    secret.baseAddress = 0x7AB;
    secret.messageLengthBytes = 8;
    secret.protection = CommsProtection::Protected;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Engine RPM");
    row.startBit = 24;
    row.bitLength = 12;
    row.dbcFactor = 0.25;
    row.dbcOffset = -1000;
    secret.rows.append(row);
    cfg.bus[0].sections.append(secret);

    // ---- a marking with NOTHING BEHIND IT withholds from the report too ----
    // This block used to read "with no password there is nothing to withhold, so
    // the report says everything ... ticking the box on a document nobody has
    // locked is a tidiness feature". It is not a tidiness feature: a Get returns
    // every section keyless, so that rule printed the CAN ID and bit layout of
    // every message the author had marked, into the one artefact of this
    // application that is designed to be printed and emailed.
    CHECK(cfg.commsRevealed());
    CHECK(cfg.bus[0].sections[0].messageKey == kNoAccessKey);
    {
        const QString report = configSummaryText(cfg);
        CHECK(!report.contains(QStringLiteral("0x7AB")));
        // The channel NAME survives at every tier and in every state: the
        // customer has to be able to use it.
        CHECK(report.contains(QStringLiteral("Engine RPM")));
    }

    // The marking's own Message Password, and a grant for it — which is what the
    // section editor records once it has run every challenge the tier demands.
    // As of 2.3.2 that is the only thing that opens a marked section, so it has
    // to exist for the report to have anything to give back.
    const AccessKey secretKey = deriveAccessKey(QStringLiteral("proprietary-own"));
    cfg.bus[0].sections[0].messageKey = secretKey;
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    {
        const QString report = configSummaryText(cfg);
        CHECK(report.contains(QStringLiteral("0x7AB")));
        CHECK(report.contains(QStringLiteral("bit: 24")));
    }

    // ---- once a password is set and the session forgets it, the detail goes --
    CHECK(cfg.setCommsPassword(pass));
    cfg.concealProtectedComms();
    CHECK(!cfg.commsRevealed());
    {
        const QString report = configSummaryText(cfg);
        // The channel name survives: the customer must still be able to use it.
        CHECK(report.contains(QStringLiteral("Engine RPM")));
        CHECK(report.contains(QStringLiteral("Proprietary ECU Feed")));
        // ...and the report says WHY the rest is missing, rather than printing
        // a blank where a CAN ID should be.
        CHECK(report.contains(QStringLiteral("protected"), Qt::CaseInsensitive));
        // Everything that describes the protocol is gone.
        CHECK(!report.contains(QStringLiteral("0x7AB")));
        CHECK(!report.contains(QStringLiteral("bit: 24")));
        CHECK(!report.contains(QStringLiteral("len: 12")));
        CHECK(!report.contains(QStringLiteral("0.25")));   // factor
        CHECK(!report.contains(QStringLiteral("-1000")));  // offset
        CHECK(!report.contains(QStringLiteral("Word Swap")));
    }

    // ---- the SECTION's password reveals it again ----
    // The document's does not, and the check is here rather than implied: one
    // verifier sitting in the same file as every Protected message in it was
    // never a proof about any of them individually.
    CHECK(cfg.revealProtectedComms(pass));
    CHECK(!configSummaryText(cfg).contains(QStringLiteral("0x7AB")));
    cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
    {
        const QString report = configSummaryText(cfg);
        CHECK(report.contains(QStringLiteral("0x7AB")));
        CHECK(report.contains(QStringLiteral("bit: 24")));
    }

    // ---- an ordinary message alongside it is unaffected ----
    cfg.concealProtectedComms();
    CommsSection open;
    open.name = QStringLiteral("Dash Out");
    open.device = SectionDevice::TransmitMessage;
    open.baseAddress = 0x123;
    open.messageLengthBytes = 8;
    open.transmitRateHz = 50;
    CommsChannelRow openRow;
    openRow.channelName = QStringLiteral("Engine RPM");
    openRow.startBit = 0;
    openRow.bitLength = 16;
    open.rows.append(openRow);
    cfg.bus[0].sections.append(open);
    {
        const QString report = configSummaryText(cfg);
        CHECK(report.contains(QStringLiteral("0x123")));   // visible message intact
        CHECK(!report.contains(QStringLiteral("0x7AB")));  // protected one still is
    }

    // ---- the tier survives a save/load round trip ----
    {
        QTemporaryDir dir;
        CHECK(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("protected.ct3"));
        QString err;
        CHECK(cfg.revealProtectedComms(pass));
        // The writer refuses while a KEYED section is concealed from this
        // session, so the section has to be open for the save to happen at all —
        // which is the guard doing its job rather than a step of the fixture.
        cfg.grantSectionAccess(0, cfg.bus[0].sections[0]);
        CHECK(cfg.saveToFile(path, &err));
        Configuration back;
        CHECK(back.loadFromFile(path, &err));
        CHECK(back.bus[0].sections.size() == 2);
        if (back.bus[0].sections.size() == 2) {
            CHECK(back.bus[0].sections[0].protection == CommsProtection::Protected);
            CHECK(back.bus[0].sections[1].protection == CommsProtection::None);
        }
        // Loaded without the password, so it opens concealed.
        CHECK(!back.commsRevealed());
        CHECK(!configSummaryText(back).contains(QStringLiteral("0x7AB")));
    }

    // ---- a BROKEN protected message must not describe its own fault ----
    // Check Channels is the sharpest leak of all: it quotes start bits, widths
    // and frame lengths verbatim in order to be useful. A protected message
    // that is misconfigured therefore has to report that it is misconfigured
    // WITHOUT saying how.
    {
        Configuration broken;
        broken.bus[0].enabled = true;
        CommsSection bad;
        bad.name = QStringLiteral("Proprietary ECU Feed");
        bad.device = SectionDevice::ReceiveMessage;
        bad.alignment = SectionAlignment::WordSwap;
        bad.baseAddress = 0x7AB;
        bad.messageLengthBytes = 8;
        bad.protection = CommsProtection::Protected;
        CommsChannelRow overflow;
        overflow.channelName = QStringLiteral("Engine RPM");
        overflow.startBit = 60; // 16 bits from bit 60 runs off an 8-byte frame
        overflow.bitLength = 16;
        bad.messageKey = deriveAccessKey(QStringLiteral("broken-own"));
        bad.rows.append(overflow);
        broken.bus[0].sections.append(bad);
        CHECK(broken.setCommsPassword(pass));

        // Unlocked with the section's own password — the only thing that opens a
        // marked section — so the vendor gets the full diagnosis.
        broken.grantSectionAccess(0, broken.bus[0].sections[0]);
        {
            bool sawDetail = false;
            for (const ValidationIssue &vi : validateConfiguration(broken))
                if (vi.message.contains(QStringLiteral("start bit 60")))
                    sawDetail = true;
            CHECK(sawDetail);
        }

        // Concealed: one entry, still an Error so Send stays blocked, and not a
        // word about the layout.
        broken.concealProtectedComms();
        int sectionIssues = 0;
        bool blocking = false;
        for (const ValidationIssue &vi : validateConfiguration(broken)) {
            const QString all = vi.location + QLatin1Char(' ') + vi.message;
            CHECK(!all.contains(QStringLiteral("start bit")));
            CHECK(!all.contains(QStringLiteral("0x7AB")));
            CHECK(!all.contains(QStringLiteral("60")));
            CHECK(!all.contains(QStringLiteral("8-byte")));
            if (vi.location.contains(QStringLiteral("Proprietary ECU Feed"))) {
                ++sectionIssues;
                if (vi.severity == ValidationIssue::Error)
                    blocking = true;
                // It has to say WHY the detail is missing rather than printing a
                // blank where a CAN ID should be. The word was "protected" until
                // 2.3.0 gave Hidden and Protected separate wording; the collapse
                // text now covers both tiers and says "withheld".
                CHECK(vi.message.contains(QStringLiteral("withheld"), Qt::CaseInsensitive));
            }
        }
        CHECK(sectionIssues == 1); // collapsed, not merely reworded
        CHECK(blocking);           // a concealed fault still refuses to be sent
    }

    // ---- displayDetail is the one place that decides what a protected section
    // may say, so check both sides of it directly ----
    CHECK(secret.isConcealed(false));
    CHECK(!secret.isConcealed(true));
    CHECK(!open.isConcealed(false)); // not protected, so never concealed
    CHECK(secret.displayDetail(true).contains(QStringLiteral("0x7AB")));
    CHECK(!secret.displayDetail(false).contains(QStringLiteral("7AB")));
    // It still says WHAT the message is: a customer needs to know one exists,
    // and which way it flows, to make sense of the channels it produces.
    CHECK(secret.displayDetail(false).contains(QStringLiteral("receive")));
    CHECK(secret.displayDetail(false).contains(QStringLiteral("protected"),
                                               Qt::CaseInsensitive));
    CHECK(!open.displayDetail(false).contains(QStringLiteral("protected"),
                                              Qt::CaseInsensitive));
    CHECK(open.displayDetail(false).contains(QStringLiteral("0x123")));
}

// The monitor stream has two valid wire shapes and the Manager must read both.
//
// Firmware now sends MONITOR_HEADER_BYTES + data_len instead of the whole
// struct: the struct reserves the CAN FD maximum, so a fixed-size frame spent 76
// bytes describing a 4-byte message, and trimming takes that frame from 85 bytes
// on the wire to 25. That is the difference between a link that can describe a
// busy bus and one that drops two thirds of it.
//
// The compatibility direction that matters is a NEW Manager against an OLD
// device: an installer ships, someone runs it before updating firmware, and if
// this only accepted the trimmed shape their CAN viewer would show nothing at
// all. The reverse pairing cannot be helped -- an old Manager checks for the
// fixed size and a trimmed frame will not match -- which is exactly why the
// trim was not done quietly.
static void testMonitorPayloadShapes()
{
    const auto build = [](int dataLen, int padTo) {
        QByteArray p(MONITOR_HEADER_BYTES + dataLen, '\0');
        quint32 ts = 0x11223344;
        std::memcpy(p.data(), &ts, 4);
        p[4] = char(2);            // bus_idx
        p[5] = char(1);            // direction: Tx
        quint32 id = 0x1AB;
        std::memcpy(p.data() + 6, &id, 4);
        p[10] = char(MONFLAG_GAP); // flags
        p[11] = char(dataLen);     // data_len
        for (int i = 0; i < dataLen; ++i)
            p[MONITOR_HEADER_BYTES + i] = char(0xA0 + i);
        if (padTo > p.size())
            p.append(QByteArray(padTo - p.size(), '\0')); // legacy fixed-size form
        return p;
    };

    MonitorStreamPayload f{};

    // Trimmed, the normal case now.
    CHECK(DeviceLink::parseMonitorPayload(build(4, 0), f));
    CHECK(f.bus_idx == 2 && f.direction == 1 && f.can_id == 0x1AB);
    CHECK(f.data_len == 4);
    CHECK((f.flags & MONFLAG_GAP) != 0);
    // quint8, not char: data[] is unsigned, and char(0xA7) is negative here,
    // so the comparison would promote the two operands to different ints.
    CHECK(f.data[0] == quint8(0xA0) && f.data[3] == quint8(0xA3));
    // Everything past data_len must read as absent rather than as whatever the
    // caller's struct held -- a trace showing invented payload bytes is worse
    // than one showing none.
    CHECK(f.data[4] == 0x00 && f.data[63] == 0x00);

    // A zero-length frame is the shortest legal payload.
    CHECK(DeviceLink::parseMonitorPayload(build(0, 0), f));
    CHECK(f.data_len == 0);

    // A full FD frame trims to exactly the legacy size, and both readings agree.
    CHECK(DeviceLink::parseMonitorPayload(build(64, 0), f));
    CHECK(f.data_len == 64 && f.data[63] == quint8(0xA0 + 63));

    // The legacy fixed-size form still parses -- this is the assertion that
    // keeps a new Manager working against a device nobody has updated.
    CHECK(DeviceLink::parseMonitorPayload(build(8, int(sizeof(MonitorStreamPayload))), f));
    CHECK(f.data_len == 8 && f.data[7] == quint8(0xA7));

    // Rejections. Short of the header, a length the struct cannot hold, and a
    // size that agrees with neither shape -- guessing at any of these would put
    // bytes the bus never carried into a trace.
    CHECK(!DeviceLink::parseMonitorPayload(build(4, 0).left(MONITOR_HEADER_BYTES - 1), f));
    QByteArray tooLong = build(4, 0);
    tooLong[11] = char(65);
    CHECK(!DeviceLink::parseMonitorPayload(tooLong, f));
    QByteArray mismatched = build(4, 0);
    mismatched[11] = char(8); // says 8 bytes, carries 4
    CHECK(!DeviceLink::parseMonitorPayload(mismatched, f));
}

static void testDeviceLinkReadCommands()
{
    // Every table's READ command must be classified as a data-bearing read by
    // DeviceLink, or its reply is dropped and every verify/Get of that table
    // fails with "device data does not match what was sent". This is the exact
    // class of bug that made relay (and constant) verification fail on-device;
    // the host firmware-link test can't catch it because it bypasses DeviceLink.
    const quint8 reads[] = {
        CMD_GET_STATUS,      CMD_READ_MSG_CFG,   CMD_READ_SIG_CFG,   CMD_READ_MATH_CFG,
        CMD_READ_COND_CFG,   CMD_READ_COUNTER_CFG, CMD_READ_TIMER_CFG, CMD_READ_CONST_CFG,
        CMD_READ_RELAY_CFG,
        CMD_READ_TABLE2X16_DEF, CMD_READ_TABLE2X16_OUT,
        // The 8x8 pair that replaced the 4x4. TWO new read commands, and both
        // have to be here: a Get that classified the Def but not the Row would
        // recover a table's axes and then hang waiting for a grid reply it had
        // already thrown away.
        CMD_READ_TABLE8X8_DEF, CMD_READ_TABLE8X8_ROW,
        CMD_READ_CONFIG_NAME,
        // v16 — integrators were added to the protocol without being classified
        // here, so every Get/Verify of that table waited for an ACK the device
        // never sends and failed after retries. Exactly the bug this list is
        // for; it was not in it.
        CMD_READ_INTEG_CFG,
        CMD_GET_DEVICE_ID, // v18 device binding
        // Access passwords + fleet identity. All four answer with a payload:
        // which keys are set, the device's nonce, the identity block, and the
        // device's HMAC over the host's own challenge.
        CMD_READ_ACCESS_KEYS, CMD_ACCESS_CHALLENGE,
        CMD_READ_FLEET_ID,    CMD_FLEET_ID_PROVE,
        // v9's CRC8 table — added AFTER it repeated the family failure in the
        // field ("Reading CRC8 rules (no response)"), because this list is as
        // hand-maintained as the one it guards. Sixth occurrence.
        CMD_READ_CRC8_CFG,
    };
    for (quint8 cmd : reads)
        CHECK(DeviceLink::isReadResponse(cmd));

    // Writes and control commands complete on an ACK, not a data echo.
    const quint8 nonReads[] = {
        CMD_WRITE_MSG_CFG,   CMD_WRITE_SIG_CFG,  CMD_WRITE_CONST_CFG, CMD_WRITE_RELAY_CFG,
        CMD_CLEAR_CONFIG,    CMD_SAVE_TO_FLASH,  CMD_CONTROL_CAN,     CMD_RESET_DEVICE,
        CMD_WRITE_INTEG_CFG, CMD_WRITE_CONFIG_BINDING,
        // ACCESS_RESPONSE is answered ACK/NACK — listing it as a read would
        // leave requestSync waiting for a payload that never comes.
        CMD_WRITE_ACCESS_KEYS, CMD_ACCESS_RESPONSE,
    };
    for (quint8 cmd : nonReads)
        CHECK(!DeviceLink::isReadResponse(cmd));

    // 0x25-0x28 were the v18 single configuration password and are retired
    // rather than reused. Nothing may still classify them: a device that
    // answered one would be speaking a protocol this build no longer knows, and
    // routing its reply as data would make that look like success.
    //
    // 0x30 was retired alongside them for a while — it was the write half of the
    // update identity, before the identity moved into the firmware build. It has
    // since been REUSED for CMD_READ_CAN_SETUP, which is safe here in a way it
    // would not be in a shipped protocol: this is v1, nothing is deployed, and
    // no host in the world holds the older meaning.
    //
    // 0x1D/0x1E join them: they were the 4x4 lookup table, replaced by the 8x8
    // at fresh ids (0x34-0x37) rather than in place. The 4x4's record was 105
    // bytes and the 8x8's Def is 73, so a stale host and a new device would not
    // even fail on length reliably — retiring the ids is what makes the
    // mismatch impossible instead of unlikely.
    for (quint8 cmd : {quint8(0x25), quint8(0x26), quint8(0x27), quint8(0x28),
                       quint8(0x1D), quint8(0x1E)})
        CHECK(!DeviceLink::isReadResponse(cmd));

    // Reads whose reply carries no echo of the request. Getting this wrong is
    // not a compile error and not a wrong answer — the reply is silently
    // discarded and the command times out, which is how the whole family was
    // found broken after Send started using it. Each of these has an empty or
    // non-echoing request, so none may be classified as a range read.
    // CMD_READ_DEVICE_CHANNELS earned its place here the standard way: it
    // shipped absent from isReadResponse(), the device answered in under a
    // millisecond, the GUI dropped the reply, and every Get timed out on its
    // last step — found on hardware the next day. This list exists so that the
    // NEXT read command fails HERE instead of in the field.
    for (quint8 cmd : {CMD_GET_STATUS, CMD_READ_CONFIG_NAME, CMD_GET_DEVICE_ID,
                       CMD_READ_ACCESS_KEYS, CMD_ACCESS_CHALLENGE, CMD_READ_FLEET_ID,
                       CMD_FLEET_ID_PROVE, CMD_READ_CAN_SETUP, CMD_READ_DEVICE_CHANNELS}) {
        CHECK(DeviceLink::isReadResponse(cmd));
        CHECK(!DeviceLink::echoesRequestRange(cmd));
    }
    // ...and the range reads, which do echo and must keep the guard.
    for (quint8 cmd : {CMD_READ_MSG_CFG, CMD_READ_SIG_CFG, CMD_READ_MATH_CFG,
                       CMD_READ_COND_CFG, CMD_READ_COUNTER_CFG, CMD_READ_TIMER_CFG,
                       CMD_READ_CONST_CFG, CMD_READ_RELAY_CFG, CMD_READ_TABLE2X16_DEF,
                       CMD_READ_TABLE2X16_OUT, CMD_READ_TABLE8X8_DEF, CMD_READ_TABLE8X8_ROW,
                       CMD_READ_INTEG_CFG, CMD_READ_CRC8_CFG}) {
        CHECK(DeviceLink::isReadResponse(cmd));
        CHECK(DeviceLink::echoesRequestRange(cmd));
    }

    // THE STRUCTURAL HALF. Every list above is hand-maintained — including the
    // ones in this very test — and six read commands have now shipped
    // unclassified because the human adding a command to config_transfer.cpp
    // did not also visit device_link.cpp and this file. So ask the transfer
    // machinery itself: these build the REAL Send-with-verify and Get plans
    // (every table populated, so every read step materialises) and run each
    // step's declared intent against the classification lists. A new table
    // read added to either plan is checked HERE from the day it exists,
    // whether or not anyone remembered the lists — which is precisely what
    // "the seventh time" must mean: a red test, not a bench session.
    CHECK(ConfigTransfer::planClassificationFaultForTest(false).isEmpty());
    CHECK(ConfigTransfer::planClassificationFaultForTest(true).isEmpty());
}

// Configuration::buildLiveView — what the channel pickers judge "is this
// channel generated, and by what?" against. Every grid dialog edits a working
// copy and writes back on OK, so the document alone is stale in BOTH
// directions mid-session; the patch has to fix both, or the input filter hides
// channels that exist and the duplicate-source warning fires on channels that
// no longer do.
// Device channels ride EVERY Send, referenced or not.
//
// This is the property that makes them usable. Monitor Channels lists what the
// mapping publishes, so a device channel that is only mapped when something
// reads it is a channel you cannot look at until you have already gone back,
// referenced it, and re-sent — at the exact moment a bus is misbehaving and you
// want an error counter in front of you. The old on-demand behaviour was right
// for a user channel and wrong for a diagnostic.
static void testDeviceChannelsAlwaysMapped()
{
    // A document that mentions no device channel anywhere.
    Configuration doc;
    CommsSection rx;
    rx.name = QStringLiteral("Receive 0x200");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x200;
    rx.messageLengthBytes = 8;
    rx.alignment = SectionAlignment::WordSwap;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Coolant Temp");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    rx.rows.append(row);
    doc.bus[0].sections.append(rx);

    const MappingResult mapped = mapToDevice(doc);
    CHECK(mapped.ok());

    // Every catalogue entry has a destination, and they are all distinct — a
    // duplicate would mean two channels fighting over one slot, with the device
    // overwriting it twice per tick.
    // Not named `slots`: Qt #defines that as a keyword, and the variable would
    // vanish into a declaration that declares nothing.
    QSet<int> seenSlots;
    for (const Channel &dev : ChannelCatalog::deviceChannels()) {
        const quint16 slot = mapped.tables.deviceChannels.signal_idx[dev.deviceChannelId];
        CHECK(slot < MAX_SIGNALS);
        CHECK(!seenSlots.contains(int(slot)));
        seenSlots.insert(int(slot));
        // And the slot is named, so Monitor Channels has a row to label. This
        // is the half that actually reaches the window: the dialog builds its
        // rows from signalToChannel, not from the device-channels record.
        CHECK(mapped.signalToChannel.value(int(slot)).compare(
                  dev.name, Qt::CaseInsensitive) == 0);
    }
    CHECK(seenSlots.size() == DEVCH_COUNT);

    // The document's own channel is still there and still first — device
    // channels are appended, so nothing the document owns shifts.
    CHECK(mapped.tables.signalConfigs.size() == 1 + DEVCH_COUNT);
    CHECK(mapped.channelToSignal.value(QStringLiteral("coolant temp"), -1) == 0);

    // Referencing one must not allocate a SECOND slot for it: signalFor()
    // returns the existing index, and the count is what proves it.
    // Rebuilt rather than copied — Configuration is a QObject and non-copyable.
    Configuration doc2;
    doc2.bus[0].sections.append(rx);
    MathRow readsDevice;
    readsDevice.op = MATH_OP_ADD;
    readsDevice.aIsChannel = true;
    readsDevice.aChannel = ChannelCatalog::deviceOnTimeName();
    readsDevice.bIsChannel = false;
    readsDevice.bConst = 0.0;
    readsDevice.destChannel = QStringLiteral("Uptime Copy");
    doc2.mathRows.append(readsDevice);

    const MappingResult m2 = mapToDevice(doc2);
    CHECK(m2.ok());
    // Exactly ONE more slot, for the math destination. Device OnTime is now
    // read by the document AND mapped as a device channel, and it must still
    // occupy a single slot: the comms pass allocates it first and the device
    // block then finds it rather than making another. Double-allocation would
    // show up here as +2, and on the device as two labels for one reading.
    CHECK(m2.tables.signalConfigs.size() == mapped.tables.signalConfigs.size() + 1);
    CHECK(m2.tables.deviceChannels.signal_idx[DEVCH_ONTIME]
          == quint16(m2.channelToSignal.value(
                 ChannelCatalog::deviceOnTimeName().toLower(), -1)));
}

// A transmit row whose channel IS a device channel — "Device CAN1 Bus Off"
// broadcast onto the bus so other nodes can watch this unit's health — is
// fully supported on Send and validation-clean, so it must come back from a
// Get. It used to vanish: the signal walk dropped every signal whose label
// names a device channel BEFORE the section-row rebuild, the section came
// back with no rows, and the next Send transmitted zeros where the
// diagnostic had been. Only the catalogue half of that skip was ever right,
// and the last CHECK pins that half staying right.
static void testDeviceChannelTransmitRowSurvivesGet()
{
    Configuration cfg;
    cfg.bus[0].enabled = true;
    CommsSection tx;
    tx.name = QStringLiteral("Bus Health Tx");
    tx.device = SectionDevice::TransmitMessage;
    tx.alignment = SectionAlignment::WordSwap;
    tx.baseAddress = 0x7E0;
    tx.messageLengthBytes = 8;
    tx.transmitRateHz = 50;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Device CAN1 Bus Off");
    row.startBit = 0;
    row.bitLength = 8;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    tx.rows.append(row);
    cfg.bus[0].sections.append(tx);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    CHECK(mr.tables.messages.size() == 1);

    Configuration back;
    mapFromDevice(mr.tables, back);
    CHECK(back.bus[0].sections.size() == 1);
    if (!back.bus[0].sections.isEmpty()) {
        const CommsSection &s = back.bus[0].sections[0];
        CHECK(s.isTransmit());
        CHECK(s.rows.size() == 1);
        if (s.rows.size() == 1) {
            CHECK(s.rows[0].channelName == QStringLiteral("Device CAN1 Bus Off"));
            CHECK(s.rows[0].startBit == 0);
            CHECK(s.rows[0].bitLength == 8);
        }
    }
    // The rule the skip existed for still holds: the device channel must NOT
    // have come back as an editable user-channel duplicate of itself.
    CHECK(!back.catalog().findByName(QStringLiteral("Device CAN1 Bus Off")).userDefined);
}

// Sections whose bus is Off upload deactivated — no ACTIVE flag — on purpose
// (mapToDevice warns so). A Get used to read that flag as "empty slot" and
// destroy them: message and relay both, gone from the document that had just
// been sent. A real record is told from the zero-fill by src_bus (1..3 in
// every record either side writes, 0 only past the used prefix), so the
// sections must come back — onto a bus that STAYS Off, because a deactivated
// record is testimony that its bus was down, not evidence it is running.
static void testDisabledBusSectionsSurviveGet()
{
    Configuration cfg; // every bus at the Off default
    CommsSection rx;
    rx.name = QStringLiteral("Bench RX");
    rx.device = SectionDevice::ReceiveMessage;
    rx.alignment = SectionAlignment::WordSwap;
    rx.baseAddress = 0x500;
    rx.messageLengthBytes = 8;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Oil Temp");
    row.startBit = 0;
    row.bitLength = 16;
    row.dbcType = int(DbcType::Unsigned);
    row.dbcFactor = 1.0;
    rx.rows.append(row);
    cfg.bus[1].sections.append(rx);
    CommsSection relay;
    relay.name = QStringLiteral("Bench Relay");
    relay.device = SectionDevice::MessageRelay;
    relay.baseAddress = 0x300;
    relay.relayBitmask = 0x7FF;
    relay.routeBusMask = (1 << 2);
    cfg.bus[1].sections.append(relay);

    const MappingResult mr = mapToDevice(cfg);
    CHECK(mr.ok());
    bool warnedDeactivated = false;
    for (const QString &w : mr.warnings)
        if (w.contains(QStringLiteral("upload deactivated")))
            warnedDeactivated = true;
    CHECK(warnedDeactivated);
    CHECK(mr.tables.messages.size() == 1);
    if (!mr.tables.messages.isEmpty()) {
        CHECK(!(mr.tables.messages[0].flags & MSGFLAG_ACTIVE));
        CHECK(mr.tables.messages[0].src_bus == 2);
    }
    CHECK(mr.tables.relays.size() == 1);
    if (!mr.tables.relays.isEmpty())
        CHECK(!(mr.tables.relays[0].flags & RELAYFLAG_ACTIVE));

    Configuration back;
    mapFromDevice(mr.tables, back);
    CHECK(back.bus[1].sections.size() == 2);
    if (back.bus[1].sections.size() == 2) {
        const CommsSection &s = back.bus[1].sections[0];
        CHECK(!s.isTransmit() && !s.isRelay());
        CHECK(s.baseAddress == 0x500);
        CHECK(s.rows.size() == 1);
        if (s.rows.size() == 1)
            CHECK(s.rows[0].channelName == QStringLiteral("Oil Temp"));
        const CommsSection &r = back.bus[1].sections[1];
        CHECK(r.isRelay());
        CHECK(r.baseAddress == 0x300);
        CHECK(r.relayBitmask == 0x7FF);
        CHECK(r.routeBusMask == (1 << 2));
    }
    // Neither the source bus nor the relay's forward target may have been
    // switched on — that is the state the missing ACTIVE flags record.
    CHECK(!back.bus[0].enabled);
    CHECK(!back.bus[1].enabled);
    CHECK(!back.bus[2].enabled);

    // And a genuinely empty slot — the all-zero record the ACTIVE skip was
    // guarding against — still reconstructs nothing.
    DeviceTables padded = mr.tables;
    padded.messages.append(CanMessageConfig{});
    padded.relays.append(RelayConfig{});
    Configuration again;
    mapFromDevice(padded, again);
    CHECK(again.bus[0].sections.size() == 0);
    CHECK(again.bus[1].sections.size() == 2);
    CHECK(again.bus[2].sections.size() == 0);
}

static void testLiveView()
{
    Configuration doc;
    CommsSection rx;
    rx.name = QStringLiteral("Receive 0x100");
    rx.device = SectionDevice::ReceiveMessage;
    rx.baseAddress = 0x100;
    CommsChannelRow row;
    row.channelName = QStringLiteral("Wheel Speed");
    row.bitLength = 16;
    row.dbcFactor = 1.0;
    rx.rows.append(row);
    doc.bus[0].sections.append(rx);

    MathRow math;
    math.active = true;
    math.destChannel = QStringLiteral("Speed Avg");
    doc.mathRows.append(math);

    // No patch: the live view is just the document.
    Configuration live;
    doc.buildLiveView(live, {});
    CHECK(live.generatedChannelNames().contains(QStringLiteral("Wheel Speed")));
    CHECK(live.generatedChannelNames().contains(QStringLiteral("Speed Avg")));
    CHECK(analyzeChannelUsage(live).generators.contains(QStringLiteral("wheel speed")));

    // ADDED: a math row the grid is holding but has not written back counts as
    // a generator, so an input picker offers it.
    MathRow pending;
    pending.active = true;
    pending.destChannel = QStringLiteral("Slip Ratio");
    QList<MathRow> working = doc.mathRows;
    working.append(pending);
    doc.buildLiveView(live, [working](Configuration &c) { c.mathRows = working; });
    CHECK(live.generatedChannelNames().contains(QStringLiteral("Slip Ratio")));
    CHECK(analyzeChannelUsage(live).generators.value(QStringLiteral("slip ratio")).size() == 1);
    CHECK(doc.generatedChannelNames().contains(QStringLiteral("Slip Ratio")) == false);

    // DELETED: a math row removed in the grid stops counting straight away —
    // the case that used to leave a stale "already generated by Math 1".
    doc.buildLiveView(live, [](Configuration &c) { c.mathRows.clear(); });
    CHECK(!live.generatedChannelNames().contains(QStringLiteral("Speed Avg")));
    CHECK(!analyzeChannelUsage(live).generators.contains(QStringLiteral("speed avg")));

    // RE-POINTED: a receive row edited to a different channel frees the old
    // name, so re-using it on another row is not flagged as a second source.
    doc.buildLiveView(live, [](Configuration &c) {
        c.bus[0].sections[0].rows[0].channelName = QStringLiteral("Wheel Speed FL");
    });
    CHECK(!analyzeChannelUsage(live).generators.contains(QStringLiteral("wheel speed")));
    CHECK(analyzeChannelUsage(live).generators.contains(QStringLiteral("wheel speed fl")));

    // A live view is scratch state, never a document: it must not pick up the
    // file path or dirty flag, or a caller could mistake it for one.
    doc.setDirty(true);
    doc.buildLiveView(live, {});
    CHECK(live.filePath().isEmpty());
    CHECK(!live.isDirty());

    // Rebuilding is idempotent — the picker re-runs it after New…/Edit….
    const QStringList once = live.generatedChannelNames();
    doc.buildLiveView(live, {});
    CHECK(live.generatedChannelNames() == once);
}

// The bus rate vocabulary: the kbps<->Hz pair, the display label, the FD test,
// and the warning that keeps a hand-edited degenerate FD honest.
static void testBusRates()
{
    // 83 is the label that lies — GMLAN's 83.333 kbit/s (1 Mbit / 12). Both
    // directions pin to their literals so neither can drift to 83,000 alone.
    CHECK(ct::busRateHz(83) == 83333u);
    CHECK(ct::busRateKbpsFromHz(83333u) == 83);
    CHECK(ct::busRateHz(800) == 800000u);
    CHECK(ct::busRateHz(0) == 0u); // classic: no FD data rate
    // Every base rate the dialog offers survives the there-and-back.
    for (int kbps : {1000, 800, 500, 250, 200, 125, 100, 83, 50})
        CHECK(ct::busRateKbpsFromHz(ct::busRateHz(kbps)) == kbps);
    CHECK(busRateLabel(83) == QStringLiteral("83.3"));
    CHECK(busRateLabel(500) == QStringLiteral("500"));

    // isFd is the DEVICE's test — data EXCEEDS nominal — not "a rate is set".
    BusConfig b;
    b.rateKbps = 500;
    b.dataRateKbps = 1000;
    CHECK(b.isFd());
    b.rateKbps = 1000;
    CHECK(!b.isFd()); // equal rates bring the bus up classic
    b.dataRateKbps = 0;
    CHECK(!b.isFd());

    // The file carries 83 as a plain int — no schema movement.
    BusConfig save;
    save.enabled = true;
    save.rateKbps = 83;
    save.dataRateKbps = 1000;
    const BusConfig loadedBus = BusConfig::fromJson(save.toJson(), kCurrentSchemaVersion);
    CHECK(loadedBus.rateKbps == 83);
    CHECK(loadedBus.dataRateKbps == 1000);
    CHECK(loadedBus.isFd());

    // A hand-edited FD data rate at the base rate cannot come from the dialog
    // (those choices are disabled there); validation must say what the device
    // will actually do with it.
    Configuration deg;
    deg.bus[0].enabled = true;
    deg.bus[0].rateKbps = 1000;
    deg.bus[0].dataRateKbps = 1000;
    CommsSection fdSec;
    fdSec.name = QStringLiteral("FD Frame");
    fdSec.device = SectionDevice::TransmitMessage;
    fdSec.baseAddress = 0x100;
    fdSec.fd = true;
    fdSec.messageLengthBytes = 12;
    deg.bus[0].sections.append(fdSec);
    bool sawClassicWarn = false;
    for (const ValidationIssue &vi : validateConfiguration(deg))
        sawClassicWarn = sawClassicWarn
                         || (vi.severity == ValidationIssue::Warning
                             && vi.message.contains(QStringLiteral("runs this bus classic")));
    CHECK(sawClassicWarn);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Report mode: "test_roundtrip <file.ct3>" prints the Channel Summary
    // Report for a saved configuration (handy for eyeballing real configs).
    if (argc > 1) {
        Configuration cfg;
        QString error;
        if (!cfg.loadFromFile(QString::fromLocal8Bit(argv[1]), &error)) {
            std::printf("Could not load %s: %s\n", argv[1], qPrintable(error));
            return 1;
        }
        std::printf("%s", qPrintable(configSummaryText(cfg)));
        return 0;
    }

    testCrc();
    testCobs();
    testFramer();
    testExtraction();
    testMapper();
    testAdvancedMath();
    testConfigJson();
    testDeviceLock();
    testConstants();
    testIntegrators();
    testDbcImport();
    testChannellessIdentifierStillTransmits();
    testClampToRangeSurvivesEveryTrip();
    testConfigReport();
    testTransmitOrder();
    testMessageRelay();
    testTransmitCrc8();
    testMigratedResetIsTheExactInverse();
    testTriggeredTransmit();
    testConditionOutputsAreBoolean();
    testBusRates();
    testLookupTables();
    testChannelReferenceVsWrite();
    testBitLayout();
    testCryptoPrimitives();
    testAccessKeys();
    testSecureFile();
    testBinaryConfigFormat();
    testTimerTriggerMigration();
    testRenamingAMessageRepointsWhatNamedIt();
    testCounterUnusedInputsStayDormant();
    testCounterMessageInputsAreValidated();
    testQualifyMaskDropsMessageComparisonsAtTheMapper();
    testConditionQualifierPersists();
    testFleetIdentity();
    testUploadPolicy();
    testFleetDocument();
    testRetainedScriptDocument();
    testProtectedDocument();
    testConcealedSaveGuards();
    testProtectedMessage();
    testMonitorPayloadShapes();
    testDeviceLinkReadCommands();
    testAscLog();
    testDeviceChannelsAlwaysMapped();
    testDeviceChannelTransmitRowSurvivesGet();
    testDisabledBusSectionsSurviveGet();
    testLiveView();
    if (failures == 0)
        std::printf("ALL TESTS PASSED\n");
    else
        std::printf("%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
