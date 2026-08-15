// Serial transport to the CAN Triple: stop-and-wait command queue with
// timeout/retry, plus demux of the always-on monitor/value streams.
#pragma once

#include <QObject>
#include <QQueue>
#include <QSerialPort>
#include <QTimer>

#include <functional>
#include <optional>

#include "framer.h"
#include "wire_structs.h"

namespace ct {

class DeviceLink : public QObject
{
    Q_OBJECT
public:
    explicit DeviceLink(QObject *parent = nullptr);

    bool open(const QString &portName, qint32 baud, QString *error = nullptr);
    void close();
    bool isOpen() const;
    QString portName() const;
    qint32 baudRate() const { return m_baud; }

    static constexpr int kDefaultTimeoutMs = 250;
    // Covers the slowest flash operation, which is CLEAR_CONFIG: it erases the
    // WHOLE config region page by page before it ACKs, so it scales with the
    // region and is the number to re-measure whenever that grows.
    //
    // Measured on hardware at the 96 KB region: 1055-1106 ms (~22.9 ms per 2 KB
    // dual-bank page, twice the 11.9 ms the earlier 52 KB figure implied). The
    // margin matters more than usual for this step because its overrun is
    // uniquely destructive: a CLEAR that times out is RETRANSMITTED, the device
    // erases twice, and the transfer then runs a stale ACK ahead of the device
    // for its whole length — merging frames until one arrives as a fragment and
    // NACKs. That exact cascade cost a debugging session at the old 1500 ms and
    // its 1.36x margin; see the CLEAR step in config_transfer.cpp.
    //
    // Flash map v2 grew the region to 128 KB (64 pages). Measured there:
    // 1.43 s (twice), right on the 22.9 ms/page prediction. 4000 ms keeps the
    // ~2.8x margin the 96 KB/3000 ms pairing had, and costs nothing in the
    // normal case: the timeout only elapses when something has already gone
    // wrong.
    static constexpr int kFlashTimeoutMs = 4000;
    static constexpr int kDefaultRetries = 5;

    // ok=false + errCode!=0 means the device NACKed; ok=false + errCode==0
    // means a link failure (timeout after retries, port gone).
    using ResponseHandler =
        std::function<void(bool ok, quint8 errCode, const QByteArray &payload, const QString &error)>;

    // Queue a command. Reads (GET_STATUS/READ_*) complete on a packet echoing
    // the request cmd; everything else completes on ACK.
    void request(quint8 cmd, const QByteArray &payload, ResponseHandler handler,
                 int timeoutMs = kDefaultTimeoutMs, int retries = kDefaultRetries);

    // Convenience wrapper running a nested event loop until completion.
    // errCodeOut receives the device's NACK code, or 0 for a link failure —
    // callers that must tell "this firmware doesn't know the command"
    // (ERR_INVALID_CMD) from "wrong password" (ERR_LOCKED) need it, and the
    // error STRING cannot be pattern-matched for that safely.
    bool requestSync(quint8 cmd, const QByteArray &payload, QByteArray *responsePayload,
                     QString *error, int timeoutMs = kDefaultTimeoutMs,
                     int retries = kDefaultRetries, quint8 *errCodeOut = nullptr);

    int pendingCount() const { return (m_current ? 1 : 0) + m_queue.size(); }
    void cancelAll(); // fails all queued/in-flight requests

    static QString errorCodeText(quint8 code);

    // True when a reply carrying this command is a data frame that echoes the
    // request (every CMD_READ_*), as opposed to a bare ACK. Exposed for tests so
    // a newly-added READ command that is forgotten here is caught.
    static bool isReadResponse(quint8 cmd);

    // True only for the range reads, whose reply repeats the request's 4-byte
    // start/count. Exposed for the same reason as isReadResponse: the set has to
    // agree with the firmware's tableForRead(), and test_firmware_link is the
    // one place both sides are visible. A read wrongly listed here has its reply
    // discarded and times out — which is how it was found.
    static bool echoesRequestRange(quint8 cmd);

signals:
    void connected();
    void disconnected();
    void monitorFrame(const ct::MonitorStreamPayload &frame);
    void signalValues(const QList<ct::SignalValueEntry> &values);
    void logMessage(const QString &text); // v2 firmware CMD_LOG

private:
    void onReadyRead();
    void onTimeout();
    void transmitCurrent();
    void completeCurrent(bool ok, quint8 errCode, const QByteArray &payload, const QString &error);
    void startNext();
    void handlePacket(const Packet &p);

    struct Pending {
        quint8 cmd = 0;
        QByteArray payload;
        ResponseHandler handler;
        int timeoutMs = kDefaultTimeoutMs;
        int retriesLeft = kDefaultRetries;
        quint16 reqCrc = 0; // CRC the firmware echoes in this request's ACK/NACK
    };

public:
    // Should an ACK/NACK whose payload is `replyPayload` be accepted as the
    // answer to a request whose frame CRC is `requestCrc`? New firmware echoes
    // the request CRC in bytes 1-2, so a duplicate ACK left over from a
    // retransmit (which carries the PREVIOUS command's CRC) is rejected here
    // rather than completing the wrong command. Firmware that predates the echo
    // sends a 1-byte reply with no CRC to match, so those are accepted as before
    // — no protection, but no regression. Static and public so the rule can be
    // unit-tested without a serial port. See handlePacket().
    static bool replyEchoMatches(const QByteArray &replyPayload, quint16 requestCrc);

    // Decode a CMD_MONITOR_STREAM payload, or return false if it is not a shape
    // this build understands. TWO are valid: current firmware sends
    // MONITOR_HEADER_BYTES + data_len, firmware predating the trim sends the
    // whole struct every time, and a Manager has to read a device that has not
    // been updated yet. Static and public for the same reason as the rule above
    // — the shapes are worth testing without a serial port. See handlePacket().
    static bool parseMonitorPayload(const QByteArray &payload,
                                    ct::MonitorStreamPayload &out);

private:

    QSerialPort m_port;
    qint32 m_baud = 7372800;
    FrameSplitter m_splitter;
    QTimer m_timer;
    std::optional<Pending> m_current;
    QQueue<Pending> m_queue;
};

} // namespace ct
