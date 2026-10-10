#pragma once

// THE STORED PROJECT, as this Manager writes it into a CAN Triple 2.0
// (wire_structs.h, CMD_WRITE_PROJECT). The unit keeps the bytes and reads none
// of them, so what they are is this file's to say:
//
//   offset 0     a 128-byte header, written LAST in a Send
//   offset 128   the body: the document, as Configuration::projectBody()
//                gives it (the .ct3 body, compressed, sealed like a .ct3)
//
// THE HEADER, little-endian:
//     0  u32     magic 'CTPJ'
//     4  u8      format (1)
//     5  u8      flags (0)
//     6  u16     reserved (0)
//     8  u32     body bytes
//    12  u32     CRC-32 of the body
//    16  u8[16]  the configuration's fingerprint: of what the Send that stored
//                it wrote (ConfigTransfer::configurationFingerprint), so a Get
//                can tell whether the unit still runs exactly that
//    32  i64     when it was sent, ms since 1970 UTC
//    40  u8[24]  the Manager version that sent it, UTF-8, zero-padded
//    64  u8[60]  the document's file name, UTF-8, zero-padded
//   124  u32     CRC-32 of the 124 bytes before it
//
// Header last, because a Send that stops part way through the project then
// leaves a copy with no header, which a Get reads as no project at all, never
// as a damaged one. CRC-32 is IEEE (zlib's), so a bench script can check a
// copy with Python's zlib.crc32.
//
// Header-only: config_transfer.cpp plans the Get's body reads from it, and
// every target that compiles that file gets it without a list to keep.

#include <QByteArray>
#include <QString>
#include <QtEndian>

#include <array>
#include <cstring>

#include "wire_structs.h" // PROJECT_BYTES

namespace ct {

constexpr int kStoredProjectHeaderBytes = 128;
constexpr quint32 kStoredProjectMagic = 0x4A505443u; // 'CTPJ' little-endian
constexpr quint8 kStoredProjectFormat = 1;
constexpr int kStoredProjectFingerprintBytes = 16;
constexpr int kStoredProjectMaxBody = PROJECT_BYTES - kStoredProjectHeaderBytes;

struct StoredProjectHeader {
    quint32 bodyBytes = 0;
    quint32 bodyCrc = 0;
    QByteArray fingerprint; // kStoredProjectFingerprintBytes
    qint64 sentMs = 0;      // ms since 1970 UTC
    QString writer;         // the Manager version that sent it
    QString fileName;       // the document's file name, no folder
};

enum class StoredProjectState {
    None,    // nothing stored: the unit came without a project
    Valid,   // a header that checks out, and (when asked of the whole) its body
    Damaged, // something there that is not a whole project
};

namespace stored_project_detail {

inline const std::array<quint32, 256> &crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// UTF-8 of `text` in at most `bytes` bytes, cut before a character it would
// split, zero-padded to exactly `bytes`.
inline QByteArray fixedUtf8(const QString &text, int bytes)
{
    QByteArray u = text.toUtf8();
    if (u.size() > bytes) {
        int cut = bytes;
        while (cut > 0 && (quint8(u.at(cut)) & 0xC0) == 0x80)
            --cut; // u[cut] continues a character: cut before its lead byte
        u.truncate(cut);
    }
    return u.leftJustified(bytes, '\0', true);
}

inline QString fromFixedUtf8(const char *p, int bytes)
{
    return QString::fromUtf8(p, int(qstrnlen(p, bytes)));
}

} // namespace stored_project_detail

// IEEE CRC-32, as zlib computes it.
inline quint32 storedProjectCrc32(const QByteArray &bytes)
{
    const auto &t = stored_project_detail::crcTable();
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : bytes)
        c = t[(c ^ quint8(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline QByteArray buildStoredProjectHeader(const StoredProjectHeader &h)
{
    using namespace stored_project_detail;
    QByteArray out(kStoredProjectHeaderBytes, '\0');
    auto *p = reinterpret_cast<uchar *>(out.data());
    qToLittleEndian<quint32>(kStoredProjectMagic, p);
    p[4] = kStoredProjectFormat;
    qToLittleEndian<quint32>(h.bodyBytes, p + 8);
    qToLittleEndian<quint32>(h.bodyCrc, p + 12);
    const QByteArray fp = h.fingerprint.left(kStoredProjectFingerprintBytes)
                              .leftJustified(kStoredProjectFingerprintBytes, '\0');
    std::memcpy(p + 16, fp.constData(), kStoredProjectFingerprintBytes);
    qToLittleEndian<qint64>(h.sentMs, p + 32);
    std::memcpy(p + 40, fixedUtf8(h.writer, 24).constData(), 24);
    std::memcpy(p + 64, fixedUtf8(h.fileName, 60).constData(), 60);
    qToLittleEndian<quint32>(storedProjectCrc32(out.left(124)), p + 124);
    return out;
}

// What a copy's first kStoredProjectHeaderBytes say. None: every byte 0xFF,
// nothing was stored. Valid: the magic, format and CRC check out and the body
// it declares fits a copy; `out` is filled. Damaged: anything else, including
// a header too short to judge.
inline StoredProjectState parseStoredProjectHeader(const QByteArray &bytes,
                                                   StoredProjectHeader *out)
{
    using namespace stored_project_detail;
    if (bytes.size() < kStoredProjectHeaderBytes)
        return StoredProjectState::Damaged;
    const QByteArray head = bytes.left(kStoredProjectHeaderBytes);
    if (head.count('\xFF') == kStoredProjectHeaderBytes)
        return StoredProjectState::None;
    const auto *p = reinterpret_cast<const uchar *>(head.constData());
    if (qFromLittleEndian<quint32>(p) != kStoredProjectMagic || p[4] != kStoredProjectFormat
        || qFromLittleEndian<quint32>(p + 124) != storedProjectCrc32(head.left(124)))
        return StoredProjectState::Damaged;
    const quint32 body = qFromLittleEndian<quint32>(p + 8);
    if (body > quint32(kStoredProjectMaxBody))
        return StoredProjectState::Damaged;
    if (out) {
        out->bodyBytes = body;
        out->bodyCrc = qFromLittleEndian<quint32>(p + 12);
        out->fingerprint = head.mid(16, kStoredProjectFingerprintBytes);
        out->sentMs = qFromLittleEndian<qint64>(p + 32);
        out->writer = fromFixedUtf8(head.constData() + 40, 24);
        out->fileName = fromFixedUtf8(head.constData() + 64, 60);
    }
    return StoredProjectState::Valid;
}

// Header then body, as a Send writes them; `header`'s body size and CRC are
// taken from `body`. Empty when the body is empty or will not fit a copy.
inline QByteArray buildStoredProject(const QByteArray &body, StoredProjectHeader header)
{
    if (body.isEmpty() || body.size() > kStoredProjectMaxBody)
        return {};
    header.bodyBytes = quint32(body.size());
    header.bodyCrc = storedProjectCrc32(body);
    return buildStoredProjectHeader(header) + body;
}

// A copy as a Get read it: Valid when its header checks out and the whole body
// is there with its CRC (`header` and `body` are then filled), None when
// nothing was stored, Damaged otherwise.
inline StoredProjectState openStoredProject(const QByteArray &bytes, StoredProjectHeader *header,
                                            QByteArray *body)
{
    StoredProjectHeader h;
    const StoredProjectState state = parseStoredProjectHeader(bytes, &h);
    if (state != StoredProjectState::Valid)
        return state;
    if (bytes.size() < kStoredProjectHeaderBytes + qint64(h.bodyBytes))
        return StoredProjectState::Damaged;
    const QByteArray b = bytes.mid(kStoredProjectHeaderBytes, int(h.bodyBytes));
    if (storedProjectCrc32(b) != h.bodyCrc)
        return StoredProjectState::Damaged;
    if (header)
        *header = h;
    if (body)
        *body = b;
    return StoredProjectState::Valid;
}

} // namespace ct
