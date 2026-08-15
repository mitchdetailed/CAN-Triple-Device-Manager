// Reading order for the .ct3s writer and reader.
//
// secure_file.h is the specification: byte offsets, labels, what every field is
// for. What it cannot give you is the ORDER the pieces come together in, and
// that is what you need to follow this code without holding the whole layout in
// your head. So, in order:
//
//   Writing
//     1. Seal the body, with the four embedded-access-key bytes in front of it.
//        A fresh 32-byte fileKey is drawn, encKey/macKey fall out of it by HMAC,
//        and sealPayload() from config_lock.h does the actual encryption.
//        Everything after this step is shuffling opaque bytes.
//     2. Wrap the fileKey — XOR it with a mask derived from the salt, and, only
//        when the file requires a password, with PBKDF2 of that password too.
//        A standard file's mask is reproducible by anyone holding the salt,
//        which is precisely what "the key travels inside the file" means.
//     3. Size the carrier: two chunks of overhead, one per 16 bytes of sealed
//        payload, plus noise, rounded up to whole 16-byte slots.
//     4. Fill the WHOLE carrier with CSPRNG noise before writing anything real
//        into it, so slots that never get claimed are indistinguishable from
//        the slots that do.
//     5. Shuffle. Fisher-Yates over the slot indices, driven by a keystream
//        derived from the salt. placement[k] is the slot chunk k lands in.
//     6. Overwrite the claimed slots: wrapped key, then payload.
//     7. Header, then carrier, to disk in one atomic replace.
//
//   Reading is that list backwards, with one ordering rule that matters more
//   than the rest: nothing is trusted until the payload's tag verifies. The
//   header's lengths are checked against the file's actual size first — a
//   crafted header must not be able to walk us off the end of the buffer or
//   spin for an hour on a huge iteration count — then chunks are gathered, then
//   the key is unwrapped, and only then does openPayload() decide whether any of
//   it was real. There is no partial success: the caller's QByteArray is written
//   once, at the very end, or never.
//
// One thing worth repeating in the same plain words the header uses: the scatter
// is obfuscation, and the encryption is the security. Everything needed to
// reverse the scatter derives from `salt`, and `salt` sits in the clear at
// offset 12 — un-scattering a file is half a page of code for anyone holding
// this source. What it buys is a file that gives up nothing to a hex editor, to
// strings(1), or to a grep for a CAN ID. That is a real property, and a much
// smaller one than encryption.
#include "secure_file.h"

#include <QCryptographicHash>
#include <QFile>
#include <QList>
#include <QMessageAuthenticationCode>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QVarLengthArray>
#include <QtEndian>

#include <cmath>
#include <cstring>

#include "access_keys.h"  // kMaxKdfIterations — the shared PBKDF2 round ceiling
#include "config_lock.h"

namespace ct {

// Not ASCII and not a word: a hex dump that spells out "CT3SECURE" answers "what
// is this file?" before anybody has to work for it. The last four bytes borrow
// PNG's trailer trick — CR LF ^Z LF catches a transfer that has helpfully
// "fixed" line endings, which in a binary body would otherwise surface much
// later as an integrity failure nobody can explain.
const unsigned char kSecureMagic[kSecureMagicBytes] = {0x8A, 0xC7, 0xB2, 0x1D,
                                                       0x0D, 0x0A, 0x1A, 0x0A};

namespace {

// ---------------------------------------------------------------------------
// Format constants

// Header field offsets, straight out of the table in secure_file.h. Named here
// so the two readers of a header — peek and open — cannot drift apart.
constexpr int kOffMagic = 0;
constexpr int kOffFormatVersion = 8;
constexpr int kOffFlags = 10;
constexpr int kOffSalt = 12;
constexpr int kOffIterations = 28;
constexpr int kOffCarrierLength = 32;
constexpr int kOffPayloadLength = 36;
constexpr int kOffPadding = 40;
constexpr int kPaddingBytes = 24;
constexpr int kSaltBytes = kOffIterations - kOffSalt;

static_assert(kOffPadding + kPaddingBytes == kSecureHeaderBytes,
              "the header fields must exactly fill kSecureHeaderBytes");
static_assert(kSaltBytes == 16, "the salt field is 16 bytes and the format fixes that");
static_assert(kSecureFileKeyBytes == kLockKeyBytes,
              "the file key is one SHA-256 output wide, which is what the HMACs produce");

// Chunks 0 and 1 carry the wrapped file key; the sealed payload starts here.
constexpr int kFirstPayloadChunk = 2;

// The embedded access key is the first four bytes of the SEALED PLAINTEXT, not a
// chunk of its own.
//
// It lived in its own chunk once, masked with a value derived from the salt.
// That was wrong twice over, and both faults are worth recording so nobody
// reinvents it. The mask came from the salt, and the salt is cleartext at offset
// 12 — so the "masked" key was recoverable by anyone holding this source, which
// is to say it was not protected at all. And nothing authenticated it: flipping
// one of those four bytes produced a file that opened perfectly and handed back
// a silently wrong key, which surfaces later as an unexplainable refusal from a
// device rather than as "this file is damaged".
//
// Inside the payload it is encrypted with everything else and covered by the
// payload's HMAC tag, so a flipped bit is caught with all the others. Nothing is
// lost by the move: the key has to be reachable in exactly the cases the payload
// is. A standard file opens without a password, so the key comes out without one
// too; a password-protected file needs the password, and anyone holding that can
// derive the key themselves and never wanted the embedded copy.
constexpr int kEmbeddedKeyBytes = kAccessKeyBytes;

// A floor on the noise, independent of noiseRatio. It matters as much as the
// ratio does: a two-message configuration seals to a few hundred bytes, and a
// file that size tells a bystander how little is in it before they open a thing.
constexpr int kMinNoiseChunks = 6;

// A ceiling on the iteration count a FILE may ask for. The header is
// attacker-controlled cleartext and PBKDF2 will happily grind for as many rounds
// as it is handed; without this, a two-byte edit turns opening a file into a
// hang nobody can attribute to anything. Shared with the access verifier and
// the legacy lock (access_keys.h) so the three PBKDF2 entry points cannot drift.
constexpr int kMaxIterations = kMaxKdfIterations;

// Domain-separation labels. Everything derived from one key has to say what it
// is for, or two derivations could collide and one secret would silently stand
// in for another. These strings are part of the file format: change one
// character and every .ct3s ever written stops opening.
const QByteArray kLabelPlacement = QByteArrayLiteral("ct3s/placement/v1");
const QByteArray kLabelWrap = QByteArrayLiteral("ct3s/wrap/v1");
const QByteArray kLabelEnc = QByteArrayLiteral("ct3s/enc/v1");
const QByteArray kLabelMac = QByteArrayLiteral("ct3s/mac/v1");
const QByteArray kLabelVerifier = QByteArrayLiteral("ct3s/vfy/v1");
// "ct3s/akey/v1" and "ct3s/akeyoff/v1" were the mask and offset for the
// standalone access-key chunk. Retired with it; see kEmbeddedKeyBytes. Listed
// here so the labels are not reused for something else in a v2 of the format.

// ---------------------------------------------------------------------------
// Small primitives

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QByteArray randomBytes(int n)
{
    // The system CSPRNG, never the default (deterministically seeded) generator:
    // a predictable salt, file key or carrier would each undo the point of
    // having one. The generator deals in 32-bit words, so draw whole words and
    // keep the prefix rather than leaving a tail uninitialised.
    const int words = (n + 3) / 4;
    QVarLengthArray<quint32, 512> buffer(words);
    QRandomGenerator::system()->fillRange(buffer.data(), words);
    QByteArray out(n, Qt::Uninitialized);
    std::memcpy(out.data(), buffer.data(), size_t(n));
    std::memset(buffer.data(), 0, size_t(words) * sizeof(quint32));
    return out;
}

// Overwrite before releasing. A QByteArray that merely goes out of scope leaves
// its contents in freed heap; ConfigKeys::clear() takes this care with the keys
// it owns, and the material that passes through here has earned the same.
void burn(QByteArray &b)
{
    b.fill('\0');
    b.clear();
}

void putU16(QByteArray &buf, int offset, quint16 value)
{
    qToLittleEndian<quint16>(value, buf.data() + offset);
}

void putU32(QByteArray &buf, int offset, quint32 value)
{
    qToLittleEndian<quint32>(value, buf.data() + offset);
}

quint16 getU16(const QByteArray &buf, int offset)
{
    return qFromLittleEndian<quint16>(buf.constData() + offset);
}

quint32 getU32(const QByteArray &buf, int offset)
{
    return qFromLittleEndian<quint32>(buf.constData() + offset);
}

// XOR the leading bytes of `mask` into `into`, in place.
void xorInto(QByteArray &into, const QByteArray &mask)
{
    const int n = qMin(into.size(), mask.size());
    for (int i = 0; i < n; ++i)
        into[i] = char(into[i] ^ mask[i]);
}

// ---------------------------------------------------------------------------
// Placement

// The keystream behind the shuffle: HMAC(salt, "ct3s/placement/v1" ||
// u32be(counter)), handed out four bytes at a time as a big-endian u32. It is
// derived from the salt and nothing else, which is what lets a reader holding
// only the cleartext header reproduce the same permutation the writer used.
class PlacementStream
{
public:
    explicit PlacementStream(const QByteArray &salt) : m_salt(salt) {}

    quint32 next()
    {
        if (m_offset + 4 > m_block.size())
            refill();
        const quint32 value = qFromBigEndian<quint32>(m_block.constData() + m_offset);
        m_offset += 4;
        return value;
    }

private:
    void refill()
    {
        QByteArray input = kLabelPlacement;
        char counter[4];
        qToBigEndian<quint32>(m_counter++, counter);
        input.append(counter, 4);
        m_block = hmac(m_salt, input);
        m_offset = 0;
    }

    QByteArray m_salt;
    QByteArray m_block;
    int m_offset = 0;
    quint32 m_counter = 0;
};

// Which slot each chunk goes into. slotOrder[k] is the carrier slot holding chunk k;
// every slot appears exactly once, so the chunks that are never written keep
// their noise and the ones that are get scattered the length of the file.
QList<int> buildPlacement(const QByteArray &salt, int slotCount)
{
    QList<int> slotOrder(slotCount);
    for (int i = 0; i < slotCount; ++i)
        slotOrder[i] = i;

    // Fisher-Yates, downwards, so the range shrinks as it goes. The draw is
    // reduced with a plain modulo: a 32-bit draw against a range of at most a
    // few thousand skews the distribution by something on the order of 2^-20 of
    // a slot, which is not a number anything here can notice. More to the point,
    // correctness does not rest on the permutation being uniform at all — it
    // rests on the writer and the reader producing the SAME permutation, which a
    // deterministic keystream gives us whether the draw is biased or not.
    // Rejection sampling would buy a statistical property nothing depends on.
    PlacementStream stream(salt);
    for (int i = slotCount - 1; i > 0; --i) {
        const int j = int(stream.next() % quint32(i + 1));
        slotOrder.swapItemsAt(i, j);
    }
    return slotOrder;
}

QByteArray chunkAt(const QByteArray &carrier, int slot)
{
    return carrier.mid(slot * kSecureChunkBytes, kSecureChunkBytes);
}

void putChunk(QByteArray &carrier, int slot, const QByteArray &chunk)
{
    std::memcpy(carrier.data() + slot * kSecureChunkBytes, chunk.constData(),
                size_t(kSecureChunkBytes));
}

// ---------------------------------------------------------------------------
// Key wrapping

// The mask the file key is XORed with. Without a password this is derivable from
// the header alone — that IS the standard mode, and the header is candid about
// what it is worth. With one, PBKDF2 of the password is folded in, and the mask
// becomes unreachable without it.
QByteArray buildWrapMask(const QByteArray &salt, const QString &password, int iterations,
                         bool requirePassword)
{
    QByteArray mask = hmac(salt, kLabelWrap);
    if (!requirePassword)
        return mask;
    QByteArray stretched = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, kSecureFileKeyBytes);
    xorInto(mask, stretched);
    burn(stretched);
    return mask;
}

// encKey and macKey come straight out of the file key by HMAC, so the .ct3s body
// is sealed with exactly the primitives the rest of the app already relies on.
// The verifier is filled only because ConfigKeys::isValid() insists on all three
// — it is never stored and never compared against anything. A .ct3s does not
// check a password by verifier; it checks it by whether the payload's tag
// survives, which is the same answer arrived at without keeping a second thing
// on disk that a password could be tried against.
ConfigKeys keysFromFileKey(const QByteArray &fileKey)
{
    ConfigKeys keys;
    keys.encKey = hmac(fileKey, kLabelEnc);
    keys.macKey = hmac(fileKey, kLabelMac);
    keys.verifier = hmac(fileKey, kLabelVerifier);
    return keys;
}

// ---------------------------------------------------------------------------
// The header

struct Header
{
    quint16 formatVersion = 0;
    quint16 flags = 0;
    QByteArray salt;
    quint32 iterations = 0;
    quint32 carrierLength = 0;
    quint32 payloadLength = 0;

    bool requiresPassword() const { return (flags & kSecureFlagRequiresPassword) != 0; }
};

bool hasMagic(const QByteArray &raw)
{
    return raw.size() >= kSecureMagicBytes
           && std::memcmp(raw.constData() + kOffMagic, kSecureMagic, size_t(kSecureMagicBytes))
                  == 0;
}

// Everything that can be decided from the 64 cleartext bytes. Deliberately does
// NOT validate the lengths against a file size — peek has no file size to check
// against, and open does that separately with the whole buffer in hand.
bool parseHeader(const QByteArray &raw, Header *out, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (raw.size() < kSecureHeaderBytes)
        return fail(QStringLiteral("This file is too short to be a secure configuration file."));
    if (!hasMagic(raw))
        return fail(QStringLiteral("This is not a secure configuration file."));

    Header h;
    h.formatVersion = getU16(raw, kOffFormatVersion);
    h.flags = getU16(raw, kOffFlags);
    h.salt = raw.mid(kOffSalt, kSaltBytes);
    h.iterations = getU32(raw, kOffIterations);
    h.carrierLength = getU32(raw, kOffCarrierLength);
    h.payloadLength = getU32(raw, kOffPayloadLength);

    // Version zero never existed, so it means a corrupt or hand-made header
    // rather than an old file; a higher version means a file this build has no
    // business guessing at.
    if (h.formatVersion == 0)
        return fail(QStringLiteral("This secure configuration file's header is damaged."));
    if (h.formatVersion > kSecureFormatVersion)
        return fail(QStringLiteral("This file was saved by a newer version of CAN Triple "
                                   "Device Manager and can't be opened."));

    *out = h;
    return true;
}

} // namespace

bool isSecureFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return hasMagic(f.read(kSecureMagicBytes));
}

bool peekSecureFile(const QString &path, SecureFileInfo *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    // The header is the whole point of peeking: it is cleartext, fixed size, and
    // says whether a password is needed. Reading further would mean holding the
    // carrier in memory to answer a question the first 64 bytes already answer.
    const QByteArray raw = f.read(kSecureHeaderBytes);
    f.close();

    Header h;
    if (!parseHeader(raw, &h, error))
        return false;
    if (out) {
        out->requiresPassword = h.requiresPassword();
        out->formatVersion = h.formatVersion;
        // embeddedCommsKey stays kNoAccessKey — it lives in the carrier, and
        // reaching it means unwrapping, which means opening the file.
        out->embeddedCommsKey = kNoAccessKey;
    }
    return true;
}

bool writeSecureFile(const QString &path, const QByteArray &plainBody,
                     const SecureSaveOptions &options, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (options.requirePassword && options.password.isEmpty())
        return fail(QStringLiteral("This file was asked to require a password, but no password "
                                   "was given."));

    const QByteArray salt = randomBytes(kSaltBytes);
    // Written whether or not a password is involved. It costs nothing, and a
    // file whose iteration count were zero when unprotected would be one more
    // place the header answers "is this worth attacking?" — the flag already
    // says so honestly, and one honest tell is enough.
    const int iterations = kLockDefaultIterations;

    // 1. Seal the body under a key that exists only for this one file. Two saves
    //    of the same document therefore share no key material at all.
    //
    //    The embedded access key goes in front of the body rather than anywhere
    //    else in the file, so it is encrypted and authenticated by the same tag
    //    as the configuration it belongs to — see kEmbeddedKeyBytes for what
    //    that replaced and why.
    QByteArray keyed = accessKeyBytes(options.embeddedCommsKey);
    if (keyed.size() != kEmbeddedKeyBytes)
        keyed = QByteArray(kEmbeddedKeyBytes, '\0');
    keyed.append(plainBody);

    QByteArray fileKey = randomBytes(kSecureFileKeyBytes);
    ConfigKeys keys = keysFromFileKey(fileKey);
    const QByteArray sealed = sealPayload(keyed, keys);
    keys.clear();
    burn(keyed);
    if (sealed.isEmpty()) {
        burn(fileKey);
        return fail(QStringLiteral("The configuration could not be encrypted."));
    }

    // 2. Wrap the file key.
    QByteArray wrapped = fileKey;
    QByteArray mask = buildWrapMask(salt, options.password, iterations, options.requirePassword);
    xorInto(wrapped, mask);
    burn(mask);
    burn(fileKey);

    // 3. Size the carrier.
    const int payloadChunks = (sealed.size() + kSecureChunkBytes - 1) / kSecureChunkBytes;
    const int materialChunks = kFirstPayloadChunk + payloadChunks;
    // The ratio is clamped rather than trusted: a caller that hands over a wild
    // value should get a large file, not an allocation failure.
    const double ratio = qBound(0.0, options.noiseRatio, 8.0);
    // Two independent reasons for noise and both matter. The RATIO keeps a large
    // configuration from being padded by what amounts to a rounding error; the
    // FLOOR keeps a small one from being an obviously small file, which the ratio
    // alone cannot do because a fraction of very little is still very little.
    const int noiseFloor = qMax(kMinNoiseChunks, int(std::ceil(materialChunks * ratio)));
    // Then a random spread of up to half again on top, so two saves of the same
    // document differ in LENGTH as well as in content. Without this the floor
    // pins every small configuration to the same file size, and a size that
    // never moves is a size worth measuring.
    const int spread = qMax(1, noiseFloor / 2);
    const int extraChunks = noiseFloor + int(QRandomGenerator::system()->bounded(spread + 1));
    const int slotCount = materialChunks + extraChunks;
    const int carrierLength = slotCount * kSecureChunkBytes;

    // 4. Noise first, everywhere. Writing the real chunks into a zeroed carrier
    //    would leave the unclaimed slots screaming which ones they were.
    QByteArray carrier = randomBytes(carrierLength);

    // 5. Where each chunk goes.
    const QList<int> slotOrder = buildPlacement(salt, slotCount);

    // 6a. The wrapped file key, two chunks of it.
    putChunk(carrier, slotOrder.at(0), wrapped.mid(0, kSecureChunkBytes));
    putChunk(carrier, slotOrder.at(1), wrapped.mid(kSecureChunkBytes, kSecureChunkBytes));
    burn(wrapped);

    // 6b. The payload, access key and all. The last chunk is zero-padded out to
    //     sixteen bytes; payloadLength in the header is what trims it back on
    //     the way in.
    for (int k = 0; k < payloadChunks; ++k) {
        QByteArray chunk(kSecureChunkBytes, '\0');
        const int taken = qMin(kSecureChunkBytes, sealed.size() - k * kSecureChunkBytes);
        std::memcpy(chunk.data(), sealed.constData() + k * kSecureChunkBytes, size_t(taken));
        putChunk(carrier, slotOrder.at(kFirstPayloadChunk + k), chunk);
    }

    // 7. Header, then carrier.
    QByteArray header(kSecureHeaderBytes, '\0');
    std::memcpy(header.data() + kOffMagic, kSecureMagic, size_t(kSecureMagicBytes));
    putU16(header, kOffFormatVersion, kSecureFormatVersion);
    putU16(header, kOffFlags, options.requirePassword ? kSecureFlagRequiresPassword : quint16(0));
    std::memcpy(header.data() + kOffSalt, salt.constData(), size_t(kSaltBytes));
    putU32(header, kOffIterations, quint32(iterations));
    putU32(header, kOffCarrierLength, quint32(carrierLength));
    putU32(header, kOffPayloadLength, quint32(sealed.size()));
    // The padding is random rather than zero for the same reason the carrier is:
    // a run of zeroes in an otherwise high-entropy file is a landmark, and a
    // landmark is where someone starts.
    const QByteArray padding = randomBytes(kPaddingBytes);
    std::memcpy(header.data() + kOffPadding, padding.constData(), size_t(kPaddingBytes));

    // QSaveFile rather than QFile: a half-written .ct3s is not a partly-readable
    // configuration, it is an unopenable one, and overwriting a good file with a
    // truncated one is the way somebody loses a config to a full disk.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(f.errorString());
    f.write(header);
    f.write(carrier);
    if (!f.commit())
        return fail(f.errorString());
    return true;
}

bool readSecureFile(const QString &path, const QString &password, QByteArray *plainBody,
                    SecureFileInfo *info, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const auto damaged = [&]() {
        return fail(QStringLiteral("This secure configuration file is damaged and cannot be "
                                   "opened."));
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    const QByteArray raw = f.readAll();
    f.close();

    Header h;
    if (!parseHeader(raw, &h, error))
        return false;

    // Every length is checked against the buffer we actually hold BEFORE a byte
    // of it is indexed. A .ct3s header is cleartext and therefore editable by
    // anyone, so treat it as a claim to be tested rather than a fact.
    if (h.carrierLength == 0 || h.carrierLength % kSecureChunkBytes != 0)
        return damaged();
    const qint64 needed = qint64(kSecureHeaderBytes) + qint64(h.carrierLength);
    if (qint64(raw.size()) < needed)
        return damaged();
    // A file LONGER than the header claims is not by itself evidence of damage —
    // something may have appended to it — and the payload's tag is what settles
    // authenticity anyway, so the surplus is ignored rather than fatal. A short
    // file is fatal: the chunks are simply not there.

    const int slotCount = int(h.carrierLength / kSecureChunkBytes);
    if (h.payloadLength == 0)
        return damaged();
    const qint64 payloadChunks =
        (qint64(h.payloadLength) + kSecureChunkBytes - 1) / kSecureChunkBytes;
    if (qint64(kFirstPayloadChunk) + payloadChunks > qint64(slotCount))
        return damaged();
    if (h.requiresPassword() && (h.iterations == 0 || h.iterations > quint32(kMaxIterations)))
        return damaged();

    const QByteArray carrier = raw.mid(kSecureHeaderBytes, int(h.carrierLength));
    const QList<int> slotOrder = buildPlacement(h.salt, slotCount);

    // Gather the wrapped key and undo the wrap. A wrong password produces a
    // wrong file key here without any complaint — nothing checks it, and nothing
    // can, because the only thing that knows the right key is the tag on the
    // payload.
    QByteArray fileKey = chunkAt(carrier, slotOrder.at(0)) + chunkAt(carrier, slotOrder.at(1));
    QByteArray mask = buildWrapMask(h.salt, password, int(h.iterations), h.requiresPassword());
    xorInto(fileKey, mask); // wrapped in, unwrapped out
    burn(mask);

    ConfigKeys keys = keysFromFileKey(fileKey);
    burn(fileKey);

    QByteArray sealed;
    sealed.reserve(int(payloadChunks) * kSecureChunkBytes);
    for (int k = 0; k < int(payloadChunks); ++k)
        sealed.append(chunkAt(carrier, slotOrder.at(kFirstPayloadChunk + k)));
    sealed.truncate(int(h.payloadLength)); // drops the zero padding in the last chunk

    QByteArray plain;
    QString why;
    const bool opened = openPayload(sealed, keys, &plain, &why);
    keys.clear();
    if (!opened) {
        burn(plain);
        // The tag failing means one of two things and there is no way to tell
        // them apart — a wrong key and a corrupted byte look identical to a MAC.
        // When the file requires a password, a wrong password is overwhelmingly
        // the likelier of the two, and saying so is more use than a message that
        // hedges; when it does not, no password was involved, so damage is the
        // only explanation left.
        if (h.requiresPassword()) {
            return fail(password.isEmpty()
                            ? QStringLiteral("This secure configuration requires a password.")
                            : QStringLiteral("That password is not correct."));
        }
        return damaged();
    }

    // Authenticated, so the length is now trustworthy — but check it anyway
    // before slicing. A file sealed by some other writer could carry a shorter
    // plaintext than this format allows, and a tag that verifies is not a
    // promise about structure.
    if (plain.size() < kEmbeddedKeyBytes) {
        burn(plain);
        return damaged();
    }

    // Only now, with the payload authenticated, is anything handed back. The
    // access key came out of the sealed body along with the configuration, so a
    // flipped bit anywhere in it — key included — has already been caught above.
    if (info) {
        info->requiresPassword = h.requiresPassword();
        info->formatVersion = h.formatVersion;
        info->embeddedCommsKey = accessKeyFromBytes(plain.left(kEmbeddedKeyBytes));
    }
    if (plainBody)
        *plainBody = plain.mid(kEmbeddedKeyBytes);
    burn(plain);
    return true;
}

} // namespace ct
