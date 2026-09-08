#include "sealed_install.h"

#include <QPointer>

#include "wire_structs.h"

namespace ct {

SealedInstall::SealedInstall(DeviceLink *link, QObject *parent)
    : QObject(parent)
    , m_link(link)
{
}

SealedInstall *SealedInstall::run(DeviceLink *link, const QByteArray &stream, QObject *parent)
{
    auto *t = new SealedInstall(link, parent);
    QString error;
    if (!parseSealedStream(stream, &t->m_salt, &t->m_frames, &error)) {
        // Reported through the same signal as a device refusal, on the event
        // loop, so a caller connects once and handles one shape of failure.
        QMetaObject::invokeMethod(
            t, [t, error]() { t->finish(false, error); }, Qt::QueuedConnection);
        return t;
    }
    QMetaObject::invokeMethod(t, &SealedInstall::runNext, Qt::QueuedConnection);
    return t;
}

void SealedInstall::cancel()
{
    m_cancelled = true;
}

QString SealedInstall::describeFailure(quint8 cmd, quint8 errCode, const QString &linkError,
                                       int frameIndex, int frameCount)
{
    if (cmd == CMD_SEAL_BEGIN) {
        if (errCode == ERR_INVALID_CMD)
            return QStringLiteral("This unit's firmware is too old to install a sealed package. "
                                  "Update it to device firmware 1.0.8 or newer.");
        if (errCode == ERR_LOCKED)
            return QStringLiteral("This unit holds no Firmware Key, so it cannot decrypt the "
                                  "package. Issue its licence with the Firmware License Manager "
                                  "first.");
    }
    if (cmd == CMD_SEAL_FRAME && errCode == ERR_LOCKED) {
        return QStringLiteral("The device refused frame %1 of %2. Either this package was not "
                              "built for this unit's Firmware Key, or the transfer got out of "
                              "step; nothing after that frame was sent. Try the install again.")
            .arg(frameIndex + 1)
            .arg(frameCount);
    }
    if (errCode != 0)
        return QStringLiteral("The device refused frame %1 of %2: %3.")
            .arg(frameIndex + 1)
            .arg(frameCount)
            .arg(DeviceLink::errorCodeText(errCode));
    return linkError.isEmpty() ? QStringLiteral("The device did not answer.") : linkError;
}

void SealedInstall::finish(bool ok, const QString &error)
{
    if (m_done)
        return;
    m_done = true;
    emit finished(ok, error);
    deleteLater();
}

void SealedInstall::runNext()
{
    if (m_cancelled) {
        finish(false, QStringLiteral("Cancelled"));
        return;
    }
    const int total = m_frames.size() + 2;
    if (m_step >= total) {
        finish(true, {});
        return;
    }

    quint8 cmd = CMD_SEAL_FRAME;
    QByteArray payload;
    QString stage;
    int timeoutMs = DeviceLink::kDefaultTimeoutMs;
    const int frameIndex = m_step - 1;
    if (m_step == 0) {
        cmd = CMD_SEAL_BEGIN;
        payload = m_salt;
        stage = QStringLiteral("Opening the sealed session");
    } else if (frameIndex < m_frames.size()) {
        const SealedFrame &f = m_frames[frameIndex];
        payload = f.bytes;
        stage = f.stage;
        if (f.flashTimeout)
            timeoutMs = DeviceLink::kFlashTimeoutMs;
    } else {
        cmd = CMD_SEAL_END;
        stage = QStringLiteral("Closing the sealed session");
    }
    emit progress(m_step, total, stage);

    m_link->request(
        cmd, payload,
        [this, self = QPointer<SealedInstall>(this), cmd, frameIndex](
            bool ok, quint8 errCode, const QByteArray &, const QString &error) {
            if (!self)
                return;
            if (!ok) {
                finish(false, describeFailure(cmd, errCode, error, frameIndex, m_frames.size()));
                return;
            }
            ++m_step;
            runNext();
        },
        timeoutMs);
}

} // namespace ct
