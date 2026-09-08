// Relays a sealed install stream (src/model/sealed_stream.h) to a device.
//
// The counterpart of ConfigTransfer::send for a package the host is not
// allowed to read: no mapping, no verify reads, no password writes of its own
// — every one of those is inside the sealed frames. Three commands, in order:
// SEAL_BEGIN with the salt, one SEAL_FRAME per frame, SEAL_END. The device
// answers each frame with the inner command's own ACK or NACK, so the reply
// handling is the ordinary write handling.
//
// A frame the device refuses ends the session on the device (see seal.h), so
// there is no resuming: the whole package is sent again from BEGIN. That is
// also the answer to the one hazard a relay has that a plain Send does not — a
// lost ACK makes the retransmit carry an index the device has already moved
// past, which it refuses as out of step. The message says to retry.
#pragma once

#include <QObject>

#include "../model/sealed_stream.h"
#include "device_link.h"

namespace ct {

class SealedInstall : public QObject
{
    Q_OBJECT

public:
    // Parses `stream` and starts the relay on the event loop. `finished` fires
    // exactly once, after which the object deletes itself.
    static SealedInstall *run(DeviceLink *link, const QByteArray &stream, QObject *parent = nullptr);

    void cancel();

    // The wording for a refusal, shared with the tests so the sentence a
    // person reads is the one pinned. `cmd` is the outer command that failed.
    static QString describeFailure(quint8 cmd, quint8 errCode, const QString &linkError,
                                   int frameIndex, int frameCount);

signals:
    void progress(int done, int total, const QString &stage);
    void finished(bool ok, const QString &error);

private:
    explicit SealedInstall(DeviceLink *link, QObject *parent);
    void runNext();
    void finish(bool ok, const QString &error);

    DeviceLink *m_link;
    QByteArray m_salt;
    QList<SealedFrame> m_frames;
    int m_step = 0; // 0 = BEGIN, 1..N = frames, N+1 = END
    bool m_cancelled = false;
    bool m_done = false;
};

} // namespace ct
