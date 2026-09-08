#include "sealed_stream.h"

#include <QRandomGenerator>
#include <QtEndian>

#include <cstring>

#include "../protocol/wire_structs.h"
#include "access_keys.h"
#include "seal.h" // firmware/include, through ct_device_core

namespace ct {

namespace {

const char kMagic[4] = {'C', 'T', '3', 'I'};

QByteArray randomSalt()
{
    QByteArray salt(int(SEAL_SALT_LEN), Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(salt.data()),
                                         reinterpret_cast<quint32 *>(salt.data() + salt.size()));
    return salt;
}

void putU16(QByteArray &b, quint16 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}

void putU32(QByteArray &b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

quint16 getU16(const QByteArray &b, int at)
{
    return quint16(quint8(b[at]) | (quint8(b[at + 1]) << 8));
}

quint32 getU32(const QByteArray &b, int at)
{
    quint32 v = 0;
    for (int i = 3; i >= 0; --i)
        v = (v << 8) | quint8(b[at + i]);
    return v;
}

} // namespace

int sealedInnerPayloadBudget()
{
    return MAX_TX_PAYLOAD - int(SEAL_OVERHEAD) - 1;
}

QByteArray buildSealedStream(const QByteArray &fleetKey, const QList<PlannedFrame> &frames,
                             QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return QByteArray();
    };
    if (fleetKey.size() != kLicenseKeyBytes)
        return fail(QStringLiteral("The Firmware Key could not be derived."));
    if (frames.size() > 0xFFFFFF)
        return fail(QStringLiteral("The package has too many frames."));

    const QByteArray salt = randomSalt();
    SealKeys keys{};
    seal_derive(reinterpret_cast<const uint8_t *>(fleetKey.constData()),
                uint32_t(fleetKey.size()), reinterpret_cast<const uint8_t *>(salt.constData()),
                &keys);

    QByteArray out;
    out.append(kMagic, 4);
    out.append(char(kSealedStreamVersion));
    out.append(salt);
    putU32(out, quint32(frames.size()));

    QByteArray plain;
    QByteArray sealed;
    for (int i = 0; i < frames.size(); ++i) {
        const PlannedFrame &f = frames[i];
        if (f.payload.size() > sealedInnerPayloadBudget()) {
            seal_wipe(&keys);
            return fail(QStringLiteral("Frame %1 is too large to seal.").arg(i));
        }
        plain.clear();
        plain.append(char(f.cmd));
        plain.append(f.payload);
        sealed.resize(plain.size() + int(SEAL_OVERHEAD));
        const uint32_t n = seal_frame(&keys, uint32_t(i),
                                      reinterpret_cast<const uint8_t *>(plain.constData()),
                                      uint32_t(plain.size()),
                                      reinterpret_cast<uint8_t *>(sealed.data()));
        sealed.truncate(int(n));
        out.append(char(f.flashTimeout ? kFrameFlashTimeout : 0));
        putU16(out, quint16(sealed.size()));
        out.append(sealed);
    }
    plain.fill('\0');
    seal_wipe(&keys);
    return out;
}

bool parseSealedStream(const QByteArray &stream, QByteArray *salt, QList<SealedFrame> *frames,
                       QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const int headerBytes = 4 + 1 + int(SEAL_SALT_LEN) + 4;
    if (stream.size() < headerBytes || std::memcmp(stream.constData(), kMagic, 4) != 0)
        return fail(QStringLiteral("This package carries no install stream."));
    if (quint8(stream[4]) != kSealedStreamVersion)
        return fail(QStringLiteral("This package was built by a newer version of CAN Triple "
                                   "Device Manager and cannot be installed by this one."));
    const QByteArray s = stream.mid(5, int(SEAL_SALT_LEN));
    const quint32 count = getU32(stream, 5 + int(SEAL_SALT_LEN));
    QList<SealedFrame> out;
    int at = headerBytes;
    for (quint32 i = 0; i < count; ++i) {
        if (stream.size() < at + 3)
            return fail(QStringLiteral("This package's install stream is truncated."));
        SealedFrame f;
        f.flashTimeout = (quint8(stream[at]) & kFrameFlashTimeout) != 0;
        const int len = getU16(stream, at + 1);
        at += 3;
        if (len < int(SEAL_OVERHEAD) + 1 || stream.size() < at + len)
            return fail(QStringLiteral("This package's install stream is damaged."));
        f.bytes = stream.mid(at, len);
        f.stage = QStringLiteral("Installing (%1/%2)").arg(i + 1).arg(count);
        at += len;
        out.append(f);
    }
    if (at != stream.size())
        return fail(QStringLiteral("This package's install stream is damaged."));
    if (salt)
        *salt = s;
    if (frames)
        *frames = out;
    return true;
}

} // namespace ct
