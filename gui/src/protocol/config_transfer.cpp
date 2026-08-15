#include "config_transfer.h"

#include <QPointer>

#include <cstring>

namespace ct {

namespace {

QByteArray rangeHeader(quint16 start, quint16 count)
{
    QByteArray b(4, 0);
    b[0] = char(start & 0xFF);
    b[1] = char(start >> 8);
    b[2] = char(count & 0xFF);
    b[3] = char(count >> 8);
    return b;
}

template <typename T>
QByteArray writePayload(quint16 start, const QVector<T> &items, int from, int count)
{
    QByteArray b = rangeHeader(start, quint16(count));
    const int bytes = count * int(sizeof(T));
    const int oldSize = b.size();
    b.resize(oldSize + bytes);
    std::memcpy(b.data() + oldSize, items.constData() + from, size_t(bytes));
    return b;
}

template <typename T>
bool appendItems(QVector<T> &dst, const QByteArray &payload)
{
    // Response payload: u16 start, u16 count, count*T. The echoed start must
    // be exactly the next index we expect — anything else is a stale or
    // out-of-order response and would silently corrupt the table.
    if (payload.size() < 4)
        return false;
    quint16 start = 0, count = 0;
    std::memcpy(&start, payload.constData(), 2);
    std::memcpy(&count, payload.constData() + 2, 2);
    if (start != dst.size())
        return false;
    const int available = (payload.size() - 4) / int(sizeof(T));
    const int n = qMin<int>(count, available);
    for (int i = 0; i < n; ++i) {
        T item;
        std::memcpy(&item, payload.constData() + 4 + i * int(sizeof(T)), sizeof(T));
        dst.append(item);
    }
    return true;
}

// Chunk sizes for the transmit-CRC8 table, derived exactly the way the
// WRITE_CHUNK_*/READ_CHUNK_* figures in wire_structs.h are. Writes: the
// largest n with 4 + n*40 <= MAX_TX_PAYLOAD (496), which is 12 — 4 + 12*40 =
// 484, and 13 would be 524. Reads: the whole table in one request, since
// 4 + 20*40 = 804 sits well inside the ~2030-byte response cap that bounds
// every READ_CHUNK_* — the same "capped by the table's own capacity" rule the
// counters and relays use.
constexpr int kWriteChunkCrc8 = 12;
constexpr int kReadChunkCrc8 = MAX_CRC8_MESSAGES;
static_assert(4 + kWriteChunkCrc8 * int(sizeof(Crc8Config)) <= MAX_TX_PAYLOAD,
              "CRC8 write chunk must fit the payload cap");

// The one sentence both routes to an abandoned read-back report, so a device
// known to guard Get before the transfer started and one that only said so when
// a read was tried tell the user the same thing. It goes in skippedStages()
// because that is the list every Send caller already prints; verifyWasSkipped()
// is for callers that need to stop saying the word "verified".
QString verifySkippedNote()
{
    return QStringLiteral("Verifying the sent configuration (this device requires its Get "
                          "Configuration password before it will read anything back)");
}

} // namespace

ConfigTransfer::ConfigTransfer(DeviceLink *link, QObject *parent)
    : QObject(parent)
    , m_link(link)
{
}

ConfigTransfer *ConfigTransfer::send(DeviceLink *link, const DeviceTables &tables, bool verify,
                                     const QVector<ControlCanPayload> &busSetups,
                                     bool saveToFlash, quint16 configVersion,
                                     const QString &configName, bool resetAfter, QObject *parent,
                                     const device_session::AccessState &deviceAccess)
{
    auto *t = new ConfigTransfer(link, parent);
    // A device that guards Get Configuration will refuse every CMD_READ_* on
    // this connection, because a Send proves only ACCESS_FN_SEND and is never
    // going to stop and ask for the other password. The read-back would fail on
    // its first step no matter how many were queued, so build none of them. This
    // is only the polish: runNext() survives the refusal on its own, which it
    // has to, since a caller that could not read the access state (old firmware,
    // a failed read) passes nothing and gets the old behaviour.
    const bool getGuarded =
        deviceAccess.supported && deviceAccess.isSet(AccessFunction::GetConfiguration);
    if (verify && getGuarded) {
        t->m_verifySkipped = true;
        t->m_skippedStages.append(verifySkippedNote());
    }
    t->buildSendSteps(tables, verify && !getGuarded, busSetups, saveToFlash, configVersion,
                      configName, resetAfter);
    // Run after the caller had a chance to connect signals.
    QMetaObject::invokeMethod(t, &ConfigTransfer::runNext, Qt::QueuedConnection);
    return t;
}

ConfigTransfer *ConfigTransfer::get(DeviceLink *link, QObject *parent)
{
    auto *t = new ConfigTransfer(link, parent);
    t->m_isGet = true;
    t->buildGetSteps();
    QMetaObject::invokeMethod(t, &ConfigTransfer::runNext, Qt::QueuedConnection);
    return t;
}

void ConfigTransfer::cancel()
{
    m_cancelled = true;
}

void ConfigTransfer::buildSendSteps(const DeviceTables &tables, bool verify,
                                    const QVector<ControlCanPayload> &busSetups, bool saveToFlash,
                                    quint16 configVersion, const QString &configName,
                                    bool resetAfter)
{
    Step ping;
    ping.cmd = CMD_GET_STATUS;
    ping.stage = QStringLiteral("Checking device");
    m_steps.append(ping);

    Step clear;
    clear.cmd = CMD_CLEAR_CONFIG;
    clear.stage = QStringLiteral("Clearing device configuration");
    // CLEAR erases the whole config region before ACKing — measured ~310 ms on
    // real hardware, past the 250 ms default, and the erase is larger now that
    // the region is 96 KB. On the default timeout every Send retransmitted
    // CLEAR, the device erased twice, and the transfer then ran one stale ACK
    // ahead of the device for its whole length: consecutive host frames lost
    // the stop-and-wait gap, merged into one over-long RX burst, and the
    // fragment NACKed ERR_INVALID_LEN mid-send. The burst limit that made a
    // merge fatal is gone — FIRMWARE-NOTES #5 records the v1 DMA defect as
    // fixed, which is what let MAX_TX_WIRE_BYTES go to 512 — but the double
    // erase is reason enough on its own. A flash operation gets the flash
    // timeout, same as SAVE_TO_FLASH below.
    clear.timeoutMs = DeviceLink::kFlashTimeoutMs;
    m_steps.append(clear);

    // Configuration name (CLEAR resets it on the device, so this comes after).
    // Old firmware NACKs the command — tolerated, the name just isn't stored.
    {
        const QByteArray utf8 = configName.toUtf8();
        QByteArray name = utf8.left(CONFIG_NAME_LEN);
        // If the 32-byte cut landed inside a multi-byte codepoint, drop that
        // codepoint's partial bytes rather than storing a broken tail.
        if (utf8.size() > CONFIG_NAME_LEN && (quint8(utf8[CONFIG_NAME_LEN]) & 0xC0) == 0x80) {
            while (!name.isEmpty() && (quint8(name.back()) & 0xC0) == 0x80)
                name.chop(1);
            if (!name.isEmpty())
                name.chop(1); // the lead byte of the split codepoint
        }
        name.append(CONFIG_NAME_LEN - name.size(), '\0');
        Step s;
        s.cmd = CMD_WRITE_CONFIG_NAME;
        s.payload = name;
        s.stage = QStringLiteral("Sending configuration name");
        s.optional = true;
        m_steps.append(s);
    }

    auto addWrites = [&](quint8 cmd, auto const &items, int chunk, const QString &what,
                         bool skipIfUnsupported = false) {
        for (int i = 0; i < items.size(); i += chunk) {
            const int count = qMin(chunk, int(items.size()) - i);
            Step s;
            s.cmd = cmd;
            s.payload = writePayload(quint16(i), items, i, count);
            s.stage = QStringLiteral("Sending %1 (%2/%3)").arg(what).arg(i + count).arg(items.size());
            s.skipIfUnsupported = skipIfUnsupported;
            m_steps.append(s);
        }
    };
    addWrites(CMD_WRITE_MSG_CFG, tables.messages, WRITE_CHUNK_MESSAGES, QStringLiteral("messages"));
    addWrites(CMD_WRITE_SIG_CFG, tables.signalConfigs, WRITE_CHUNK_SIGNALS, QStringLiteral("channels"));
    addWrites(CMD_WRITE_MATH_CFG, tables.math, WRITE_CHUNK_MATH, QStringLiteral("math"));
    addWrites(CMD_WRITE_COND_CFG, tables.conditions, WRITE_CHUNK_CONDITIONS,
              QStringLiteral("conditions"));
    addWrites(CMD_WRITE_COUNTER_CFG, tables.counters, WRITE_CHUNK_COUNTERS,
              QStringLiteral("counters"));
    addWrites(CMD_WRITE_TIMER_CFG, tables.timers, WRITE_CHUNK_TIMERS,
              QStringLiteral("timers"));
    addWrites(CMD_WRITE_CONST_CFG, tables.constants, WRITE_CHUNK_CONSTANTS,
              QStringLiteral("constants"));
    addWrites(CMD_WRITE_RELAY_CFG, tables.relays, WRITE_CHUNK_RELAYS,
              QStringLiteral("relays"));
    // OUTPUTS BEFORE DEFINITIONS, deliberately. The engine keeps evaluating
    // while the upload streams in, and the Def record is the one carrying
    // TABLEFLAG_ACTIVE and x_count — so a table goes live the instant its Def
    // lands. Writing the values first means it can only ever go live with its
    // outputs already resident. (The firmware also refuses to evaluate an index
    // missing from either table, so this is the second of two guards.)
    addWrites(CMD_WRITE_TABLE2X16_OUT, tables.tables2x16Out, WRITE_CHUNK_TABLES_2X16_OUT,
              QStringLiteral("2x16 table values"));
    addWrites(CMD_WRITE_TABLE2X16_DEF, tables.tables2x16Def, WRITE_CHUNK_TABLES_2X16_DEF,
              QStringLiteral("2x16 tables"));
    // ROWS BEFORE DEFINITIONS, for exactly the reason above and with one extra
    // tooth: an 8x8's grid arrives as eight separate records, so the window in
    // which a table could be half-uploaded is eight times wider than the 2x16's.
    // The Def is the only record carrying TABLEFLAG_ACTIVE, and the firmware
    // will not evaluate table t until it has both a Def at t and (t+1)*8 rows —
    // and unprogrammed flash reads 0xFF, which as a float is NaN. Send the rows
    // first and a table can only ever switch on over a grid that is already
    // there; send the Def first and one interrupted transfer feeds NaN into a
    // live output channel.
    addWrites(CMD_WRITE_TABLE8X8_ROW, tables.tables8x8Row, WRITE_CHUNK_TABLES_8X8_ROW,
              QStringLiteral("8x8 table values"));
    addWrites(CMD_WRITE_TABLE8X8_DEF, tables.tables8x8Def, WRITE_CHUNK_TABLES_8X8_DEF,
              QStringLiteral("8x8 tables"));
    addWrites(CMD_WRITE_INTEG_CFG, tables.integrators, WRITE_CHUNK_INTEGRATORS,
              QStringLiteral("integrators"));
    // Transmit-CRC8 rules, after the messages they bind to by msg_idx. No
    // torn-state ordering to design around beyond that: a rule landing early
    // stamps nothing, because CLEAR_CONFIG above emptied the message table and
    // an un-composed frame is never stamped. Not marked skipIfUnsupported the
    // way the script is, deliberately — firmware that lacks this table is
    // firmware with an older store version, and the Send path refuses those
    // units up front (EXPECTED_STORE_VERSION) instead of part-writing them.
    addWrites(CMD_WRITE_CRC8_CFG, tables.crc8, kWriteChunkCrc8,
              QStringLiteral("CRC8 rules"));
    // The compiled device script. Unlike the tables above there is no torn-state
    // window to design around: the firmware adopts a script only in
    // engine_load_script(), which runs on config LOAD, so a half-uploaded image
    // is never executed — and script_verify() checks the header's claimed length
    // against the chunk count, so a truncated one is refused rather than run off
    // the end. Empty when the document has no script, which writes zero chunks
    // and is what clears a script off a device (CLEAR_CONFIG already zeroed the
    // count; nothing here puts it back).
    //
    // skipIfUnsupported: firmware older than the v5 store does not know
    // CMD_WRITE_SCRIPT and NACKs ERR_INVALID_CMD. Without this flag that aborted
    // the whole transfer AFTER CLEAR_CONFIG had already erased the device —
    // turning "your firmware is too old for scripts" into "your device is now
    // blank and every retry blanks it again". Tolerated, the script is simply
    // omitted (the rest of the configuration is unaffected, exactly as it would
    // be on a device that never had a script) and the omission is reported
    // through skippedStages().
    addWrites(CMD_WRITE_SCRIPT, tables.scriptChunks, WRITE_CHUNK_SCRIPT,
              QStringLiteral("script"), /*skipIfUnsupported=*/true);

    // Device channels: one small record telling the firmware which slot to
    // publish its own values into. Sent unconditionally, including when nothing
    // reads them — the payload then says SIG_MSG_NONE, which is what clears a
    // destination a previous configuration set. Skipping the step when unused
    // would leave the old one in place on a device being reconfigured.
    {
        Step s;
        s.cmd = CMD_WRITE_DEVICE_CHANNELS;
        s.payload = QByteArray(reinterpret_cast<const char *>(&tables.deviceChannels),
                               sizeof(tables.deviceChannels));
        s.stage = QStringLiteral("Sending device channels");
        s.optional = true; // firmware without device channels NACKs — not fatal
        m_steps.append(s);
    }

    for (const ControlCanPayload &setup : busSetups) {
        Step s;
        s.cmd = CMD_CONTROL_CAN;
        s.payload = QByteArray(reinterpret_cast<const char *>(&setup), sizeof(setup));
        s.stage = QStringLiteral("Configuring CAN %1").arg(setup.bus_idx);
        s.optional = true; // v1 firmware NACKs CONTROL_CAN — not fatal
        m_steps.append(s);
    }

    // The read-back phase. It has to stay one unbroken run of isVerify steps
    // with the commit behind it, because runNext() abandons the phase by walking
    // forward over consecutive isVerify steps until it reaches the next thing —
    // which must be SAVE_TO_FLASH. Slipping a write in among these would get it
    // silently skipped along with them.
    if (verify) {
        auto addVerify = [&](quint8 cmd, auto const &items, int chunk, const QString &what,
                             bool skipIfUnsupported = false) {
            for (int i = 0; i < items.size(); i += chunk) {
                const int count = qMin(chunk, int(items.size()) - i);
                Step s;
                s.cmd = cmd;
                s.payload = rangeHeader(quint16(i), quint16(count));
                s.stage = QStringLiteral("Verifying %1").arg(what);
                s.expectedEcho = writePayload(quint16(i), items, i, count);
                s.isVerify = true;
                s.skipIfUnsupported = skipIfUnsupported;
                m_steps.append(s);
            }
        };
        addVerify(CMD_READ_MSG_CFG, tables.messages, READ_CHUNK_MESSAGES, QStringLiteral("messages"));
        addVerify(CMD_READ_SIG_CFG, tables.signalConfigs, READ_CHUNK_SIGNALS, QStringLiteral("channels"));
        addVerify(CMD_READ_MATH_CFG, tables.math, READ_CHUNK_MATH, QStringLiteral("math"));
        addVerify(CMD_READ_COND_CFG, tables.conditions, READ_CHUNK_CONDITIONS,
                  QStringLiteral("conditions"));
        addVerify(CMD_READ_COUNTER_CFG, tables.counters, READ_CHUNK_COUNTERS,
                  QStringLiteral("counters"));
        addVerify(CMD_READ_TIMER_CFG, tables.timers, READ_CHUNK_TIMERS,
                  QStringLiteral("timers"));
        addVerify(CMD_READ_CONST_CFG, tables.constants, READ_CHUNK_CONSTANTS,
                  QStringLiteral("constants"));
        addVerify(CMD_READ_RELAY_CFG, tables.relays, READ_CHUNK_RELAYS,
                  QStringLiteral("relays"));
        addVerify(CMD_READ_TABLE2X16_DEF, tables.tables2x16Def, READ_CHUNK_TABLES_2X16_DEF,
                  QStringLiteral("2x16 tables"));
        addVerify(CMD_READ_TABLE2X16_OUT, tables.tables2x16Out, READ_CHUNK_TABLES_2X16_OUT,
                  QStringLiteral("2x16 table values"));
        // Order is irrelevant here — nothing is being written, so there is no
        // torn state to protect. Def first to match the read order of a Get.
        addVerify(CMD_READ_TABLE8X8_DEF, tables.tables8x8Def, READ_CHUNK_TABLES_8X8_DEF,
                  QStringLiteral("8x8 tables"));
        addVerify(CMD_READ_TABLE8X8_ROW, tables.tables8x8Row, READ_CHUNK_TABLES_8X8_ROW,
                  QStringLiteral("8x8 table values"));
        addVerify(CMD_READ_INTEG_CFG, tables.integrators, READ_CHUNK_INTEGRATORS,
                  QStringLiteral("integrators"));
        // Not skipIfUnsupported, matching its write step: a unit old enough to
        // NACK this read was refused the whole Send before it started.
        addVerify(CMD_READ_CRC8_CFG, tables.crc8, kReadChunkCrc8,
                  QStringLiteral("CRC8 rules"));
        // skipIfUnsupported, to match the WRITE_SCRIPT step above: on pre-v5
        // firmware the write was skipped, so its read-back would NACK
        // ERR_INVALID_CMD too, and demanding it would abort a verify that has
        // otherwise entirely succeeded.
        addVerify(CMD_READ_SCRIPT, tables.scriptChunks, READ_CHUNK_SCRIPT,
                  QStringLiteral("script"), /*skipIfUnsupported=*/true);
    }

    // Persist the verified configuration to flash as the final step so a
    // power cycle reloads it. skipIfUnsupported (not optional): a genuine
    // flash-write error stays fatal and surfaces, but firmware too old to know
    // the command (ERR_INVALID_CMD) is tolerated and reported as skipped.
    //
    // The configuration's version travels as the commit's payload — two bytes,
    // little-endian like every other multi-byte field on this wire — rather than
    // as a command of its own. That is the whole reason it is here: a version
    // written in a separate round trip could land without its tables, or its
    // tables without it, and either way the device's answer to "what revision
    // are you running?" would be a confident lie. Riding the commit, the two
    // become the same flash write.
    //
    // Sent unconditionally, including zero. The protocol allows an empty payload
    // meaning "leave the stored version alone", but this path never wants that:
    // whatever is being committed IS the configuration now, so an unversioned
    // one must clear a stale number rather than inherit it.
    if (saveToFlash) {
        Step save;
        save.cmd = CMD_SAVE_TO_FLASH;
        save.payload.append(char(configVersion & 0xFF));
        save.payload.append(char((configVersion >> 8) & 0xFF));
        save.stage = QStringLiteral("Saving to flash");
        save.timeoutMs = DeviceLink::kFlashTimeoutMs;
        save.skipIfUnsupported = true;
        m_steps.append(save);
    }

    // Optional reboot as the very last step: the device ACKs then resets after
    // a short delay (so this ACK still arrives). skipIfUnsupported so older
    // firmware that doesn't know the command reports it skipped, not failed.
    if (resetAfter) {
        Step reset;
        reset.cmd = CMD_RESET_DEVICE;
        reset.stage = QStringLiteral("Resetting device");
        reset.skipIfUnsupported = true;
        m_steps.append(reset);
    }
    validateReadClassification();
}

void ConfigTransfer::buildGetSteps()
{
    Step ping;
    ping.cmd = CMD_GET_STATUS;
    ping.stage = QStringLiteral("Checking device");
    m_steps.append(ping);

    auto addReads = [&](quint8 cmd, int max, int chunk, int table, const QString &what) {
        for (int i = 0; i < max; i += chunk) {
            const int count = qMin(chunk, max - i);
            Step s;
            s.cmd = cmd;
            s.payload = rangeHeader(quint16(i), quint16(count));
            s.stage = QStringLiteral("Reading %1 (%2/%3)").arg(what).arg(i + count).arg(max);
            s.table = table;
            m_steps.append(s);
        }
    };
    addReads(CMD_READ_MSG_CFG, MAX_MESSAGES, READ_CHUNK_MESSAGES, 0, QStringLiteral("messages"));
    addReads(CMD_READ_SIG_CFG, MAX_SIGNALS, READ_CHUNK_SIGNALS, 1, QStringLiteral("channels"));
    addReads(CMD_READ_MATH_CFG, MAX_MATH_COMPUTATIONS, READ_CHUNK_MATH, 2, QStringLiteral("math"));
    addReads(CMD_READ_COND_CFG, MAX_CONDITIONS, READ_CHUNK_CONDITIONS, 3,
             QStringLiteral("conditions"));
    // Older firmware NACKs the counter/timer/constant reads — tolerated, tables
    // stay empty.
    const int firstOptionalStep = m_steps.size();
    addReads(CMD_READ_COUNTER_CFG, MAX_COUNTERS, READ_CHUNK_COUNTERS, 4,
             QStringLiteral("counters"));
    addReads(CMD_READ_TIMER_CFG, MAX_TIMERS, READ_CHUNK_TIMERS, 5,
             QStringLiteral("timers"));
    addReads(CMD_READ_CONST_CFG, MAX_CONSTANTS, READ_CHUNK_CONSTANTS, 6,
             QStringLiteral("constants"));
    addReads(CMD_READ_RELAY_CFG, MAX_RELAYS, READ_CHUNK_RELAYS, 7,
             QStringLiteral("relays"));
    addReads(CMD_READ_TABLE2X16_DEF, MAX_TABLES_2X16, READ_CHUNK_TABLES_2X16_DEF, 8,
             QStringLiteral("2x16 tables"));
    addReads(CMD_READ_TABLE2X16_OUT, MAX_TABLES_2X16, READ_CHUNK_TABLES_2X16_OUT, 9,
             QStringLiteral("2x16 table values"));
    // The 8x8 pair took the retired 4x4's slots 10 and 11, which pushed the
    // integrators to 12 — see the table-index note in runNext(). The row table
    // is read over its FLAT range 0..MAX_TABLE_8X8_ROWS-1, not per table: the
    // device stores it as one 64-entry table and the t*8 grouping is the
    // reader's arithmetic, not the wire's.
    addReads(CMD_READ_TABLE8X8_DEF, MAX_TABLES_8X8, READ_CHUNK_TABLES_8X8_DEF, 10,
             QStringLiteral("8x8 tables"));
    addReads(CMD_READ_TABLE8X8_ROW, MAX_TABLE_8X8_ROWS, READ_CHUNK_TABLES_8X8_ROW, 11,
             QStringLiteral("8x8 table values"));
    addReads(CMD_READ_INTEG_CFG, MAX_INTEGRATORS, READ_CHUNK_INTEGRATORS, 12,
             QStringLiteral("integrators"));
    // The script, table 13. Read back as BYTECODE, which is not the source and
    // cannot be turned into it — so a Get still cannot recover an EDITABLE
    // script. What it does now is KEEP the image: mapFromDevice validates these
    // chunks and stores them in Configuration::scriptBytecode(), so sending the
    // configuration back restores the same script byte for byte instead of
    // silently removing it.
    //
    // Read over the table's FULL capacity, like every other table, so the reply
    // carries the device's stored chunks followed by zeros for the rest
    // (engine_table_read zero-fills past the active prefix). The mapper is what
    // trims that back down to header + code_bytes.
    //
    // It would be read even without any of that, because the comparison in
    // main_window is byte-for-byte against what a Send would produce, and a
    // device whose script differs from the document's is exactly the difference
    // that comparison exists to report.
    addReads(CMD_READ_SCRIPT, MAX_SCRIPT_CHUNKS, READ_CHUNK_SCRIPT, 13,
             QStringLiteral("script"));
    // Transmit-CRC8 rules, table 14 (the firmware's ENGINE_TABLE_CRC8).
    // Optional like everything from the counters on: firmware without the
    // table NACKs ERR_INVALID_CMD, and the honest reading of that is "this
    // device stamps no checksums", not a failed Get.
    addReads(CMD_READ_CRC8_CFG, MAX_CRC8_MESSAGES, kReadChunkCrc8, 14,
             QStringLiteral("CRC8 rules"));
    // Configuration name (v7+; old firmware NACKs — tolerated).
    {
        Step s;
        s.cmd = CMD_READ_CONFIG_NAME;
        s.stage = QStringLiteral("Reading configuration name");
        s.captureName = true;
        m_steps.append(s);
    }
    // Bus modes and rates. Optional like the rest: firmware without
    // CMD_READ_CAN_SETUP NACKs ERR_INVALID_CMD and the mapper falls back to
    // assuming the bring-up rates, which is what every Get did before this.
    {
        Step s;
        s.cmd = CMD_READ_CAN_SETUP;
        s.stage = QStringLiteral("Reading bus setup");
        s.captureBusSetup = true;
        m_steps.append(s);
    }
    // Which slot the device publishes its own values into. Without it a Get
    // would rebuild that slot as an ordinary user channel — a duplicate of the
    // built-in Device OnTime that the user could then edit into disagreeing
    // with the firmware.
    {
        Step s;
        s.cmd = CMD_READ_DEVICE_CHANNELS;
        s.stage = QStringLiteral("Reading device channels");
        s.captureDeviceChannels = true;
        m_steps.append(s);
    }
    for (int i = firstOptionalStep; i < m_steps.size(); ++i)
        m_steps[i].optional = true;
    validateReadClassification();
}

/* The guard behind a six-entry history. DeviceLink::isReadResponse() and
 * echoesRequestRange() are hand-maintained lists, and six read commands have
 * now shipped absent from them — constants, config name, relays, integrators,
 * device channels, and most recently READ_CRC8_CFG. The symptom was identical
 * every time: the device answered instantly, the GUI dropped the reply as
 * unclassified, and the step burned its whole timeout ("no response" in the
 * field, found on hardware, six times).
 *
 * The step flags say which replies must carry data — and which of those echo
 * a range header — INDEPENDENTLY of the lists, so the two can be compared the
 * moment a plan is built, before anything is sent. A disagreement fails the
 * transfer instantly with the fix in the message instead of a mystery
 * timeout, and the classification test builds real plans through this exact
 * check, so the seventh omission fails in the test run, not on a bench. */
void ConfigTransfer::validateReadClassification()
{
    const auto fault = [&](quint8 cmd, const QString &stage, const QString &why) {
        m_buildFault = QStringLiteral("Internal error at \"%1\": command 0x%2 %3 — fix the "
                                      "classification lists in device_link.cpp before this "
                                      "command ships.")
                           .arg(stage, QString::number(cmd, 16).toUpper().rightJustified(2, '0'),
                                why);
    };
    for (const Step &s : std::as_const(m_steps)) {
        const bool awaitsData = s.table != -1 || s.isVerify || s.captureName
                                || s.captureBusSetup || s.captureDeviceChannels
                                || s.cmd == CMD_GET_STATUS;
        const bool rangeRead = s.table != -1 || s.isVerify;
        if (awaitsData && !DeviceLink::isReadResponse(s.cmd)) {
            fault(s.cmd, s.stage,
                  QStringLiteral("awaits a data reply that isReadResponse() would discard"));
            return;
        }
        if (rangeRead && !DeviceLink::echoesRequestRange(s.cmd)) {
            fault(s.cmd, s.stage,
                  QStringLiteral("is a range read that echoesRequestRange() does not know"));
            return;
        }
        if (!rangeRead && awaitsData && DeviceLink::echoesRequestRange(s.cmd)) {
            fault(s.cmd, s.stage,
                  QStringLiteral("has a fixed-shape reply that the range-echo check would "
                                 "discard"));
            return;
        }
    }
}

QString ConfigTransfer::planClassificationFaultForTest(bool getPlan)
{
    ConfigTransfer t(nullptr, nullptr);
    if (getPlan) {
        t.buildGetSteps();
    } else {
        // One record per table, because the verify phase only enqueues steps
        // for non-empty tables — an empty container would silently exempt its
        // command from the very check this seam exists to run.
        DeviceTables tables;
        tables.messages.resize(1);
        tables.signalConfigs.resize(1);
        tables.math.resize(1);
        tables.conditions.resize(1);
        tables.counters.resize(1);
        tables.timers.resize(1);
        tables.constants.resize(1);
        tables.relays.resize(1);
        tables.tables2x16Def.resize(1);
        tables.tables2x16Out.resize(1);
        tables.tables8x8Def.resize(1);
        tables.tables8x8Row.resize(1);
        tables.integrators.resize(1);
        tables.crc8.resize(1);
        tables.scriptChunks.resize(1);
        t.buildSendSteps(tables, /*verify=*/true, {}, /*saveToFlash=*/true,
                         /*configVersion=*/0, {}, /*resetAfter=*/false);
    }
    return t.m_buildFault;
}

void ConfigTransfer::runNext()
{
    if (m_cancelled) {
        emit finished(false, QStringLiteral("Cancelled"));
        deleteLater();
        return;
    }
    if (!m_buildFault.isEmpty()) {
        emit finished(false, m_buildFault);
        deleteLater();
        return;
    }
    if (m_index >= m_steps.size()) {
        if (m_isGet)
            emit tablesReady(m_readTables);
        emit finished(true, {});
        deleteLater();
        return;
    }
    const Step &step = m_steps[m_index];
    emit progress(m_index, m_steps.size(), step.stage);
    m_link->request(
        step.cmd, step.payload,
        [this, self = QPointer<ConfigTransfer>(this)](
            bool ok, quint8 errCode, const QByteArray &payload, const QString &error) {
            // The link holds this handler until the reply lands or the request
            // times out — which for a CLEAR can be tens of seconds with retries.
            // If the owning dialog is closed in that window, it takes this
            // ConfigTransfer (its child) with it, and the link then calls a
            // handler over freed memory. The QPointer, captured by value, is the
            // one thing that outlives the object: null here means we are gone,
            // so there is nothing left to drive and every `this` below would be
            // a use-after-free.
            if (!self)
                return;
            const Step &cur = m_steps[m_index];
            // An optional step tolerates ANY failure — a NACK and a timeout
            // alike. "Optional" means the transfer is complete without this
            // step's answer, and that is as true when the reply was lost as
            // when the device declined; errCode==0 (link failure) used to fall
            // through to the fatal path below, which made a single lost reply
            // on an optional read abort a whole Get. A skipIfUnsupported step
            // is narrower on purpose: only "the firmware doesn't know the
            // command" (ERR_INVALID_CMD) is tolerable — a real error like
            // ERR_FLASH_WRITE, or a timeout, stays fatal so it surfaces.
            const bool unsupported = errCode == ERR_INVALID_CMD;
            if (!ok && (cur.optional || (cur.skipIfUnsupported && unsupported))) {
                if (cur.cmd == CMD_SAVE_TO_FLASH)
                    m_flashSaveSkipped = true;
                // errCode == 0 is a LOST reply (a timeout past every retry), not
                // a device NACK. For an optional READ that means the table this
                // step would have filled is left EMPTY not because the device
                // has none but because we never heard the answer — a distinction
                // that is invisible in the result and dangerous to a caller
                // about to save it as a backup. Record it so such a caller can
                // refuse. (A device NACK, errCode != 0, is a truthful "not
                // present" and does not set this.)
                if (errCode == 0)
                    m_replyLost = true;
                m_skippedStages.append(QStringLiteral("%1 (%2)")
                                           .arg(cur.stage,
                                                errCode ? DeviceLink::errorCodeText(errCode)
                                                        : QStringLiteral("no response")));
                ++m_index;
                runNext();
                return;
            }
            // A verify read refused for want of the Get Configuration password
            // is not a failed verification — it is a verification that never
            // happened, and the two must not be treated alike.
            //
            // Everything ahead of this step was ACKed, so the device is holding
            // what was sent; the only thing missing is the proof, and this
            // connection cannot obtain it because a Send proves ACCESS_FN_SEND
            // and the reads are gated on ACCESS_FN_GET. Aborting here would be
            // the worst answer available: CLEAR_CONFIG has already erased the
            // unit and SAVE_TO_FLASH is still ahead, so the operation meant to
            // update the device would leave it wiped instead. Anyone who set a
            // Get password would have bricked their own updates. So drop the
            // rest of the read-back and carry on to the commit.
            //
            // A MISMATCH below stays fatal, and must. There the read SUCCEEDED
            // and came back with bytes other than the ones sent, which means the
            // device is not holding the configuration it was given — writing
            // that to flash would make a corrupt config the one it reloads at
            // every power-up. Silence about the contents is survivable; a
            // demonstrated disagreement about them is not.
            if (!ok && errCode == ERR_LOCKED && cur.isVerify) {
                m_verifySkipped = true;
                m_skippedStages.append(verifySkippedNote());
                while (m_index < m_steps.size() && m_steps[m_index].isVerify)
                    ++m_index;
                runNext();
                return;
            }
            if (!ok) {
                // Record WHY, so a caller can offer the right password instead
                // of repeating the device's word "locked" at the user. See
                // failedLocked() for why a Get can reach here having already
                // proved its own Get password.
                m_failedLocked = errCode == ERR_LOCKED;
                emit finished(false, QStringLiteral("%1 — %2")
                                         .arg(m_steps[m_index].stage,
                                              error.isEmpty() ? QStringLiteral("failed") : error));
                deleteLater();
                return;
            }
            const Step &step = m_steps[m_index];
            if (!step.expectedEcho.isEmpty() && payload != step.expectedEcho) {
                emit finished(false, QStringLiteral("%1 — device data does not match what was sent")
                                         .arg(step.stage));
                deleteLater();
                return;
            }
            if (step.captureName) {
                const int nul = payload.indexOf('\0');
                m_deviceConfigName = QString::fromUtf8(nul >= 0 ? payload.left(nul) : payload);
            }
            if (step.captureBusSetup) {
                // Three packed records, bus 1..3 in order. A short or oversized
                // reply is left as "not read" rather than half-applied: a
                // partially-believed bus map is worse than an admitted guess,
                // because the guess at least tells the user to go and look.
                if (payload.size() == 3 * int(sizeof(ControlCanPayload))) {
                    m_deviceBusSetup.resize(3);
                    std::memcpy(m_deviceBusSetup.data(), payload.constData(),
                                size_t(payload.size()));
                }
            }
            if (step.captureDeviceChannels && payload.size() > 0
                && payload.size() <= int(sizeof(DeviceChannelsConfig))
                && payload.size() % int(sizeof(quint16)) == 0) {
                // A SHORT reply is a PREFIX, not a failure — the mirror of what
                // the device does with a short write. Firmware that predates the
                // CAN diagnostic channels answers with just the two OnTime bytes,
                // and reading them is strictly better than discarding a
                // destination the device really is publishing into. Whatever the
                // reply does not cover keeps the field's SIG_MSG_NONE, which
                // reads as "this device publishes nothing there" — the same
                // answer a NACK gives, and the safe one either way.
                //
                // A reply that is not a whole number of destinations is treated
                // as unreadable rather than truncated: half a uint16 is a
                // corrupt index, not a short one.
                std::memcpy(&m_readTables.deviceChannels, payload.constData(),
                            size_t(payload.size()));
            }
            if (step.table >= 0) {
                bool placed = false;
                // These indices ARE the firmware's EngineTable enum values, and
                // the correspondence is not decorative: engine_core.c uses the
                // enum value as the flash-layout index, so the two lists have to
                // be read side by side whenever either changes. Retiring the 4x4
                // freed 10; the 8x8 Def and Row pair took 10 and 11, and the
                // integrators moved 11 -> 12 rather than leaving a hole, which
                // is what keeps the firmware's
                // "ENGINE_TABLE_INTEGRATORS + 1 == FLASH_NUM_TABLES" assertion
                // true at 13 tables.
                switch (step.table) {
                case 0: placed = appendItems(m_readTables.messages, payload); break;
                case 1: placed = appendItems(m_readTables.signalConfigs, payload); break;
                case 2: placed = appendItems(m_readTables.math, payload); break;
                case 3: placed = appendItems(m_readTables.conditions, payload); break;
                case 4: placed = appendItems(m_readTables.counters, payload); break;
                case 5: placed = appendItems(m_readTables.timers, payload); break;
                case 6: placed = appendItems(m_readTables.constants, payload); break;
                case 7: placed = appendItems(m_readTables.relays, payload); break;
                case 8: placed = appendItems(m_readTables.tables2x16Def, payload); break;
                case 9: placed = appendItems(m_readTables.tables2x16Out, payload); break;
                case 10: placed = appendItems(m_readTables.tables8x8Def, payload); break;
                case 11: placed = appendItems(m_readTables.tables8x8Row, payload); break;
                case 12: placed = appendItems(m_readTables.integrators, payload); break;
                case 13: placed = appendItems(m_readTables.scriptChunks, payload); break;
                case 14: placed = appendItems(m_readTables.crc8, payload); break;
                }
                if (!placed) {
                    emit finished(false, QStringLiteral("%1 — out-of-order device response")
                                             .arg(step.stage));
                    deleteLater();
                    return;
                }
            }
            ++m_index;
            runNext();
        },
        step.timeoutMs);
}

} // namespace ct
