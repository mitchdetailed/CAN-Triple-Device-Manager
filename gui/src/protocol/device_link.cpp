#include "device_link.h"

#include <QEventLoop>
#include <QPointer>

#include <cstring>
#include <memory>

namespace ct {

// Commands whose reply is a DATA frame that echoes the request command (rather
// than a bare ACK). Every CMD_READ_* belongs here — a read missing from this
// list has its reply rejected in handlePacket()'s default case, so its request
// completes via a stale ACK or a timeout and every verify/Get of that table
// fails ("device data does not match what was sent"). Add new READ commands here.
bool DeviceLink::isReadResponse(quint8 cmd)
{
    switch (cmd) {
    case CMD_GET_STATUS:
    case CMD_READ_MSG_CFG:
    case CMD_READ_SIG_CFG:
    case CMD_READ_MATH_CFG:
    case CMD_READ_COND_CFG:
    case CMD_READ_COUNTER_CFG:
    case CMD_READ_TIMER_CFG:
    case CMD_READ_CONST_CFG:      // v6 — was missing (constants verify/Get)
    case CMD_READ_RELAY_CFG:      // v11 — was missing (relay verify/Get)
    case CMD_READ_TABLE2X16_DEF:  // v13
    case CMD_READ_TABLE2X16_OUT:  // v13
    case CMD_READ_TABLE8X8_DEF:   // the 4x4's replacement (0x1E left the list with it)
    case CMD_READ_TABLE8X8_ROW:
    case CMD_READ_CONFIG_NAME:    // v7 — was missing (Get Configuration failed)
    case CMD_READ_INTEG_CFG:      // v16 — was missing (Get/Verify of integrators failed)
    case CMD_GET_DEVICE_ID:       // v18: answers uid + config status, not an ACK
    // The four access / fleet-identity commands that carry data back rather
    // than an ACK. The two that do NOT are deliberately absent: ACCESS_RESPONSE
    // and WRITE_ACCESS_KEYS are answered with a plain ACK or a NACK, and listing
    // one here would leave requestSync waiting for a payload that is never
    // coming. (There used to be a third, WRITE_UPDATE_ID. The fleet identity is
    // compiled into the firmware now, so there is no identity write left to get
    // wrong either way.)
    case CMD_READ_ACCESS_KEYS:
    case CMD_ACCESS_CHALLENGE:
    case CMD_READ_FLEET_ID:
    case CMD_FLEET_ID_PROVE:      // the device's HMAC over the host's challenge
    case CMD_READ_CAN_SETUP:      // ControlCanPayload[3]; empty request, no echo
    // Was missing on the day it shipped — the same mistake this list's own
    // history records for v6, v7, v11 and v16, made again. The device answered
    // 0x33 in under a millisecond; the GUI classified the pending request as
    // ACK-completed, dropped the data reply, and every Get timed out on its
    // final step. Fixed the day after, with the classification test in
    // test_roundtrip extended so the NEXT read command cannot repeat it.
    case CMD_READ_MSG_PASSWORDS:   // MessagePasswordRecord; fixed shape, no echo
    case CMD_READ_DEVICE_CHANNELS: // DeviceChannelsConfig; fixed shape, no echo
    // The SIXTH time. Shipped absent the same day the CRC8 table shipped —
    // the device answered 0x42 instantly (the bench harness proved it), the
    // GUI dropped the reply as unclassified, and every Get ended with
    // "Reading CRC8 rules (no response)" plus a whole timeout of dead air.
    // The entry below this comment is the fix; the pattern above it is the
    // warning nobody heeds: a new READ command is not done until it is in
    // BOTH lists in this file and both lists in the classification test.
    case CMD_READ_CRC8_CFG:
    // v2 bootloader. Added in the same edit as the command itself rather than
    // after the symptom appeared — the four entries above learned it the other
    // way round, and each cost a debugging session in which a device answering
    // instantly looked like a device answering not at all.
    case CMD_FW_UPDATE_STATUS:     // FwUpdateStatus; fixed shape, no echo
    case CMD_SCRIPT_STATUS:        // ScriptStatus; fixed shape, no echo
    // The script BYTECODE is a range read like any other table, so it goes in
    // the echoesRequestRange list below rather than here alongside the
    // fixed-shape replies.
    case CMD_READ_SCRIPT:
        return true;
    default:
        return false;
    }
}

static bool isReadCommand(quint8 cmd) { return DeviceLink::isReadResponse(cmd); }

// Range reads — and ONLY they — repeat the request's 4-byte start/count at the
// front of their reply. That echo is what lets a late duplicate from a retry be
// told apart from the answer actually being waited on.
//
// Every other read answers with a fixed-shape reply that echoes nothing:
// GET_STATUS, READ_CONFIG_NAME, GET_DEVICE_ID, READ_ACCESS_KEYS,
// ACCESS_CHALLENGE, READ_FLEET_ID, FLEET_ID_PROVE. Demanding an echo from those
// throws the reply away and lets the request time out.
//
// This used to be written as "every read except GET_STATUS and
// READ_CONFIG_NAME", which was true when those were the only two. Each read
// added since — GET_DEVICE_ID in v18, then the four access and fleet commands —
// silently joined the wrong side of that test and could never complete. Stated
// the other way round, as the property the check actually depends on, a new
// read command is broken only if someone deliberately claims it echoes a range.
bool DeviceLink::echoesRequestRange(quint8 cmd)
{
    switch (cmd) {
    case CMD_READ_MSG_CFG:
    case CMD_READ_SIG_CFG:
    case CMD_READ_MATH_CFG:
    case CMD_READ_COND_CFG:
    case CMD_READ_COUNTER_CFG:
    case CMD_READ_TIMER_CFG:
    case CMD_READ_CONST_CFG:
    case CMD_READ_RELAY_CFG:
    case CMD_READ_TABLE2X16_DEF:
    case CMD_READ_TABLE2X16_OUT:
    case CMD_READ_TABLE8X8_DEF:
    case CMD_READ_TABLE8X8_ROW:
    case CMD_READ_INTEG_CFG:
    case CMD_READ_SCRIPT:   // bytecode chunks, a range read like any other table
    case CMD_READ_CRC8_CFG: // a range read like any other table — see the
                            // sixth-time note in isReadResponse above
        return true;
    default:
        return false;
    }
}

DeviceLink::DeviceLink(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &DeviceLink::onTimeout);
    connect(&m_port, &QSerialPort::readyRead, this, &DeviceLink::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
        if (e == QSerialPort::ResourceError && m_port.isOpen()) {
            close(); // device unplugged
        }
    });
}

bool DeviceLink::open(const QString &portName, qint32 baud, QString *error)
{
    close();
    m_port.setPortName(portName);
    m_port.setBaudRate(baud);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port.open(QIODevice::ReadWrite)) {
        if (error)
            *error = m_port.errorString();
        return false;
    }
    m_baud = baud;
    m_splitter.clear();
    emit connected();
    return true;
}

void DeviceLink::close()
{
    if (!m_port.isOpen())
        return;
    cancelAll();
    m_port.close();
    emit disconnected();
}

bool DeviceLink::isOpen() const
{
    return m_port.isOpen();
}

QString DeviceLink::portName() const
{
    return m_port.portName();
}

void DeviceLink::cancelAll()
{
    m_timer.stop();
    auto failAll = [](Pending &p) {
        if (p.handler)
            p.handler(false, 0, {}, QStringLiteral("Cancelled"));
    };
    if (m_current) {
        Pending p = std::move(*m_current);
        m_current.reset();
        failAll(p);
    }
    while (!m_queue.isEmpty()) {
        Pending p = m_queue.dequeue();
        failAll(p);
    }
}

void DeviceLink::request(quint8 cmd, const QByteArray &payload, ResponseHandler handler,
                         int timeoutMs, int retries)
{
    if (!m_port.isOpen()) {
        if (handler)
            handler(false, 0, {}, QStringLiteral("Not connected"));
        return;
    }
    Pending p;
    p.cmd = cmd;
    p.payload = payload;
    p.handler = std::move(handler);
    p.timeoutMs = timeoutMs;
    p.retriesLeft = retries;
    p.reqCrc = frameCrc(cmd, payload); // matched against the echo in this request's ACK
    m_queue.enqueue(std::move(p));
    if (!m_current)
        startNext();
}

bool DeviceLink::requestSync(quint8 cmd, const QByteArray &payload, QByteArray *responsePayload,
                             QString *error, int timeoutMs, int retries, quint8 *errCodeOut)
{
    // The completion state lives on the HEAP, shared with the response handler,
    // and NOT captured by reference. The handler is stored in the request queue
    // and can outlive this stack frame: if the nested loop below returns for any
    // reason other than the handler firing — the application shutting down while
    // a firmware chunk or waitForDeviceToReturn is mid-flight is the real case —
    // this function returns, its frame is gone, and a `[&]` handler would then
    // write `result`, `*responsePayload` and quit a destroyed QEventLoop through
    // dangling references. Owning the state in a shared_ptr, and marking it
    // `abandoned` once we stop waiting, makes a late reply a no-op instead.
    struct SyncState {
        bool done = false;
        bool result = false;
        bool abandoned = false;
        QByteArray *responsePayload = nullptr;
        QString *error = nullptr;
        quint8 *errCodeOut = nullptr;
        QEventLoop loop;
    };
    auto st = std::make_shared<SyncState>();
    st->responsePayload = responsePayload;
    st->error = error;
    st->errCodeOut = errCodeOut;
    if (errCodeOut)
        *errCodeOut = 0;

    request(cmd, payload,
            [st](bool ok, quint8 errCode, const QByteArray &resp, const QString &err) {
                // Abandoned means requestSync already gave up and returned; its
                // out-params belong to a frame that no longer exists, so touch
                // nothing.
                if (st->abandoned)
                    return;
                st->result = ok;
                if (st->responsePayload)
                    *st->responsePayload = resp;
                if (st->errCodeOut)
                    *st->errCodeOut = errCode;
                if (st->error)
                    *st->error = ok ? QString()
                                    : (errCode
                                           ? QStringLiteral("Device error: %1").arg(errorCodeText(errCode))
                                           : err);
                st->done = true;
                st->loop.quit();
            },
            timeoutMs, retries);
    if (!st->done)
        st->loop.exec();
    // From here the handler must not write through the out-params or the loop:
    // both may be gone the moment this returns.
    st->abandoned = true;
    return st->result;
}

void DeviceLink::startNext()
{
    if (m_current || m_queue.isEmpty())
        return;
    m_current = m_queue.dequeue();
    transmitCurrent();
}

void DeviceLink::transmitCurrent()
{
    if (!m_current)
        return;
    const QByteArray frame = buildFrame(m_current->cmd, m_current->payload);
    // The frame, including both delimiters, must fit MAX_TX_WIRE_BYTES — 512
    // since the payload cap was raised, and bounded now by the firmware's RX
    // ring rather than by the v1 DMA defect that set the old 127 (FIRMWARE-NOTES
    // #5, and the note on MAX_TX_WIRE_BYTES in wire_structs.h).
    //
    // This was a Q_ASSERT, which compiles away in the Release build everyone
    // actually runs — so the one build that could have caught an oversized frame
    // was the one nobody uses, and in Release it would have gone out and
    // scrambled the device's RX instead. Refusing is strictly better: the
    // command fails with a message naming the cause, and the link survives.
    // wire_structs.h static_asserts that no chunk size can get here, so this is
    // the backstop for a payload assembled some other way.
    if (frame.size() > MAX_TX_WIRE_BYTES) {
        completeCurrent(false, 0, {},
                        QStringLiteral("Internal error: a %1-byte frame exceeds the %2-byte "
                                       "limit the device's receiver can accept.")
                            .arg(frame.size())
                            .arg(MAX_TX_WIRE_BYTES));
        return;
    }
    m_port.write(frame);
    m_timer.start(m_current->timeoutMs);
}

void DeviceLink::onTimeout()
{
    if (!m_current)
        return;
    if (m_current->retriesLeft-- > 0) {
        transmitCurrent();
        return;
    }
    completeCurrent(false, 0, {}, QStringLiteral("No response from device (timeout)"));
}

void DeviceLink::completeCurrent(bool ok, quint8 errCode, const QByteArray &payload,
                                 const QString &error)
{
    m_timer.stop();
    if (!m_current)
        return;
    Pending p = std::move(*m_current);
    m_current.reset();
    if (p.handler)
        p.handler(ok, errCode, payload, error);
    startNext();
}

bool DeviceLink::parseMonitorPayload(const QByteArray &payload, MonitorStreamPayload &out)
{
    const int n = payload.size();
    if (n < MONITOR_HEADER_BYTES)
        return false;

    // Zeroed before the copy: bytes past data_len are ABSENT from a trimmed
    // frame, not merely uncopied, and a trace must not show whatever happened to
    // be in the caller's struct as though the bus had carried it.
    out = MonitorStreamPayload{};
    std::memcpy(&out, payload.constData(), size_t(qMin(n, int(sizeof(out)))));

    if (out.data_len > 64)
        return false; // a length the struct cannot hold is a corrupt frame

    // The length has to account for itself exactly. Anything that is neither the
    // trimmed shape nor the legacy fixed one is not a monitor frame this build
    // understands, and guessing would put invented bytes into a trace somebody
    // is reading to find out what the bus actually did.
    return n == MONITOR_HEADER_BYTES + int(out.data_len)
           || n == int(sizeof(MonitorStreamPayload));
}

bool DeviceLink::replyEchoMatches(const QByteArray &replyPayload, quint16 requestCrc)
{
    // A 3-byte reply is [status, crc_hi, crc_lo] from firmware that echoes the
    // request CRC: accept only if it echoes THIS request's CRC. A 1-byte reply
    // is [status] from firmware that predates the echo: there is nothing to
    // match, so accept it (old behaviour, no duplicate protection). Any other
    // length is malformed and is not a valid answer.
    if (replyPayload.size() == 1)
        return true;
    if (replyPayload.size() >= 3) {
        const quint16 echo = quint16((quint8(replyPayload[1]) << 8) | quint8(replyPayload[2]));
        return echo == requestCrc;
    }
    return false;
}

void DeviceLink::onReadyRead()
{
    const QList<Packet> packets = m_splitter.feed(m_port.readAll());
    for (const Packet &p : packets)
        handlePacket(p);
}

void DeviceLink::handlePacket(const Packet &p)
{
    switch (p.cmd) {
    case CMD_MONITOR_STREAM: {
        MonitorStreamPayload frame;
        if (parseMonitorPayload(p.payload, frame))
            emit monitorFrame(frame);
        return;
    }
    case CMD_VALUE_STREAM: {
        if (p.payload.size() < 2)
            return;
        quint16 count = 0;
        std::memcpy(&count, p.payload.constData(), 2);
        const int available = (p.payload.size() - 2) / int(sizeof(SignalValueEntry));
        count = quint16(qMin<int>(count, available));
        QList<SignalValueEntry> values;
        values.reserve(count);
        for (int i = 0; i < count; ++i) {
            SignalValueEntry e;
            std::memcpy(&e, p.payload.constData() + 2 + i * int(sizeof(e)), sizeof(e));
            values.append(e);
        }
        if (!values.isEmpty())
            emit signalValues(values);
        return;
    }
    case CMD_LOG: {
        // v2 firmware frames its printf/debug output.
        emit logMessage(QString::fromUtf8(p.payload));
        return;
    }
    case CMD_ACK: {
        if (!(m_current && !isReadCommand(m_current->cmd)))
            return;
        // Match the echoed request CRC. Without this, a duplicate ACK left over
        // from a retransmit — the original and the retransmitted write both
        // ACK, arriving one after the other — would complete whatever command is
        // NOW in flight rather than the one it actually answered. On a no-verify
        // Send (a Get-guarded unit skips the read-back) that could complete
        // SAVE_TO_FLASH ~1.4 s before the flash write started, tearing the image
        // on a power cut in the gap. The firmware now echoes the request's CRC
        // in the ACK; a stale duplicate carries the PREVIOUS command's CRC and is
        // dropped here, leaving this command waiting for its own ACK (or its
        // timeout). Firmware that predates the echo sends a 1-byte ACK, which
        // replyEchoMatches accepts unconditionally — old behaviour, no
        // regression.
        if (!replyEchoMatches(p.payload, m_current->reqCrc))
            return;
        completeCurrent(true, 0, p.payload, {});
        return;
    }
    case CMD_NACK: {
        // 1 byte from old firmware, 3 from firmware that echoes the request CRC.
        if (!m_current || (p.payload.size() != 1 && p.payload.size() != 3))
            return;
        const quint8 code = quint8(p.payload[0]);

        // Two NACKs say nothing about the request and everything about the
        // line, and those two are retransmitted rather than believed:
        //
        //   ERR_INVALID_CRC — the bytes arrived mangled. WHY is not established
        //   (FIRMWARE-NOTES #5): the receive-to-idle re-arm this comment used to
        //   blame does not exist any more — the RX DMA is circular and never
        //   re-armed — leaving line-level bit errors at 7.37 Mbaud, the overrun
        //   path, and the firmware's rxBuffer being lapped when the USART IRQ is
        //   delayed. That last one is why rxBuffer goes to 1024 alongside the
        //   larger frames. It happens now and then on a long transfer, and this
        //   retry is what makes all three survivable rather than diagnosed.
        //
        //   ERR_INVALID_LEN — usually the same event wearing a different code.
        //   A fragment too short to even hold a CRC never reaches the CRC
        //   check; the firmware's frame-length sanity check answers first
        //   (hardware-observed: a merged RX burst NACKed INVALID_LEN mid-Send
        //   and killed a transfer whose every frame was well-formed). It CAN
        //   also be the device's considered answer — a host whose record sizes
        //   disagree with the firmware's — but every write payload this GUI
        //   sends is compile-time sized against the same wire_structs.h the
        //   verify byte-compares, so on a version mismatch the retries burn a
        //   second and then fail with the same honest code. Cheap insurance
        //   one way, a round of retransmissions the other.
        //
        // Still deliberately narrow. ERR_OUT_OF_BOUNDS cannot come off a bad
        // line — it requires a frame whose CRC PASSED — and retrying
        // ERR_LOCKED or ERR_BUS_BUSY would just spam a device that has already
        // said no for a reason.
        //
        // These two are also the one NACK class that carries NO echoed request
        // CRC — the firmware emits them for a frame it could not authenticate,
        // so it echoes 0 (see processChunk). They are matched by CODE, never by
        // CRC, which is why the echo check below deliberately sits after this
        // branch: applying it here would drop the retry trigger.
        if (code == ERR_INVALID_CRC || code == ERR_INVALID_LEN) {
            if (m_current->retriesLeft-- > 0) {
                m_timer.stop();
                transmitCurrent();
                return;
            }
            completeCurrent(false, code, {},
                            QStringLiteral("Device error: %1").arg(errorCodeText(code)));
            return;
        }

        // Every other NACK is the device's considered answer to a specific
        // request, and new firmware echoes that request's CRC. Drop a stale
        // duplicate exactly as an ACK is dropped, so a leftover NACK from a
        // retransmit cannot fail the command that is now in flight. (Old
        // firmware's 1-byte NACK has no CRC and is accepted.)
        if (!replyEchoMatches(p.payload, m_current->reqCrc))
            return;
        completeCurrent(false, code, {},
                        QStringLiteral("Device error: %1").arg(errorCodeText(code)));
        return;
    }
    default:
        // Data responses echo the request command. Because timeouts retransmit
        // and the firmware answers statelessly, a late response for a previous
        // identical-cmd request can arrive now; a range read's reply repeats the
        // start/count, so require that to match the in-flight request. A read
        // that echoes nothing can only be confused with another request of the
        // same command, which is benign — see echoesRequestRange().
        if (m_current && isReadCommand(m_current->cmd) && p.cmd == m_current->cmd) {
            if (DeviceLink::echoesRequestRange(m_current->cmd)
                && (p.payload.size() < 4 || p.payload.left(4) != m_current->payload.left(4)))
                return; // stale duplicate from a retry — keep waiting
            completeCurrent(true, 0, p.payload, {});
        }
        return;
    }
}

QString DeviceLink::errorCodeText(quint8 code)
{
    switch (code) {
    case ERR_OK: return QStringLiteral("OK");
    case ERR_INVALID_CMD: return QStringLiteral("invalid command (0x01)");
    case ERR_INVALID_LEN: return QStringLiteral("invalid length (0x02)");
    case ERR_INVALID_CRC: return QStringLiteral("invalid CRC (0x03)");
    case ERR_OUT_OF_BOUNDS: return QStringLiteral("out of bounds (0x04)");
    case ERR_FLASH_WRITE: return QStringLiteral("flash write failed (0x05)");
    case ERR_BUS_BUSY: return QStringLiteral("bus busy (0x06)");
    case ERR_LOCKED:
        return QStringLiteral("the device configuration is password protected (0x07)");
    default: return QStringLiteral("unknown error (0x%1)").arg(code, 2, 16, QLatin1Char('0'));
    }
}

} // namespace ct
