// A DeviceLink that answers from something other than a serial port.
//
// ConfigTransfer and FirmwareUpdater are the two classes that actually talk to
// a device, and until this existed neither could be tested: their whole surface
// is DeviceLink::request / requestSync, and DeviceLink needs a COM port. The
// tests reached around them instead — test_firmware_link drove the firmware
// with its own hand-rolled chunking, and test_roundtrip checked only the pure
// mapping layer — so the code that runs a real Send, tolerates an optional
// step, verifies a read-back, or chunks a firmware image was covered by nothing.
//
// The seam is two virtual methods and this subclass. What sits behind it is the
// caller's choice, and the interesting choice is the REAL FIRMWARE: a responder
// built on test_firmware_link's exchange() feeds each command to serial_proto
// and hands back what the device library actually said, so a ConfigTransfer
// here performs the same conversation it performs on a bench unit — including
// the parts a hand-rolled stub would get wrong by agreeing with the host.
//
// Header-only on purpose: it is test scaffolding, and a .cpp would mean adding
// it to every test target that ever wants it.
#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QTimer>

#include <functional>

#include "../src/protocol/device_link.h"

namespace ct {

class FakeDeviceLink : public DeviceLink
{
public:
    // One answer, in DeviceLink's own terms: ok=true is an ACK or a read
    // response; ok=false with errCode!=0 is a device NACK; ok=false with
    // errCode==0 is a link failure (a timeout past every retry).
    struct Reply {
        bool ok = true;
        quint8 errCode = 0;
        QByteArray payload;
        QString error;

        static Reply ack(const QByteArray &p = QByteArray()) { return Reply{true, 0, p, {}}; }
        static Reply nack(quint8 code) { return Reply{false, code, {}, QStringLiteral("NACK")}; }
        static Reply lost() { return Reply{false, 0, {}, QStringLiteral("no response")}; }
    };
    using Responder = std::function<Reply(quint8 cmd, const QByteArray &payload)>;

    explicit FakeDeviceLink(Responder responder, QObject *parent = nullptr)
        : DeviceLink(parent), m_responder(std::move(responder))
    {
    }

    // ---- what the caller sent, for assertions ---------------------------
    QList<QPair<quint8, QByteArray>> sent;

    int countOf(quint8 cmd) const
    {
        int n = 0;
        for (const auto &s : sent)
            if (s.first == cmd)
                ++n;
        return n;
    }
    bool sentAny(quint8 cmd) const { return countOf(cmd) > 0; }

    // ---- fault injection -------------------------------------------------
    // NACK a command with a given code, or lose its reply entirely. `after`
    // lets the Nth occurrence fail while earlier ones succeed, which is how a
    // chunked write is made to fail in the middle rather than at its start.
    void nackCommand(quint8 cmd, quint8 code, int after = 0)
    {
        m_faults.append(Fault{cmd, code, after, /*lose=*/false});
    }
    void loseReplyTo(quint8 cmd, int after = 0)
    {
        m_faults.append(Fault{cmd, 0, after, /*lose=*/true});
    }

    // Called before each command is answered — the hook a test uses to cancel
    // an upload mid-flight, or to watch the conversation as it happens.
    std::function<void(quint8 cmd, const QByteArray &payload)> beforeEach;

    // ---- the seam --------------------------------------------------------
    void request(quint8 cmd, const QByteArray &payload, ResponseHandler handler,
                 int timeoutMs = kDefaultTimeoutMs, int retries = kDefaultRetries) override
    {
        Q_UNUSED(timeoutMs);
        Q_UNUSED(retries);
        const Reply r = answer(cmd, payload);
        // POSTED, not called here. ConfigTransfer's step handler calls runNext()
        // which issues the next request, so answering inline would recurse one
        // stack frame per step and a real configuration has hundreds. The real
        // link is asynchronous for its own reasons; matching that keeps the
        // object under test in the same shape it has in the application.
        QTimer::singleShot(0, this, [handler, r]() {
            handler(r.ok, r.errCode, r.payload, r.error);
        });
    }

    bool requestSync(quint8 cmd, const QByteArray &payload, QByteArray *responsePayload,
                     QString *error, int timeoutMs = kDefaultTimeoutMs,
                     int retries = kDefaultRetries, quint8 *errCodeOut = nullptr) override
    {
        Q_UNUSED(timeoutMs);
        Q_UNUSED(retries);
        const Reply r = answer(cmd, payload);
        if (responsePayload)
            *responsePayload = r.payload;
        if (errCodeOut)
            *errCodeOut = r.errCode;
        if (!r.ok && error)
            *error = r.error;
        return r.ok;
    }

private:
    struct Fault {
        quint8 cmd;
        quint8 code;
        int after; // let this many occurrences through first
        bool lose;
    };

    Reply answer(quint8 cmd, const QByteArray &payload)
    {
        sent.append(qMakePair(cmd, payload));
        if (beforeEach)
            beforeEach(cmd, payload);
        for (Fault &f : m_faults) {
            if (f.cmd != cmd)
                continue;
            if (f.after > 0) {
                --f.after;
                continue;
            }
            // The fault stands for every later occurrence too: a device that
            // refuses a command once refuses it, and a test that wanted one
            // failure followed by success is describing a retry, which belongs
            // in the link rather than here.
            return f.lose ? Reply::lost() : Reply::nack(f.code);
        }
        return m_responder ? m_responder(cmd, payload) : Reply::lost();
    }

    Responder m_responder;
    QList<Fault> m_faults;
};

} // namespace ct
