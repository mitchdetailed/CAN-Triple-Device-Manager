#include "config_lock.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QtEndian>

namespace ct {

namespace {

constexpr int kTagBytes = 32; // HMAC-SHA256 output

QByteArray randomBytes(int n)
{
    QByteArray out(n, Qt::Uninitialized);
    // The system CSPRNG, not the default (deterministically seeded) generator —
    // a predictable salt or nonce would undo the point of having one.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(out.data()),
                                         reinterpret_cast<quint32 *>(out.data() + n));
    return out;
}

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

// HMAC-SHA256 in counter mode, XORed over the data in place. Symmetric: the
// same call encrypts and decrypts.
void applyKeystream(QByteArray &data, const QByteArray &encKey, const QByteArray &nonce)
{
    for (int offset = 0, counter = 0; offset < data.size(); offset += kTagBytes, ++counter) {
        QByteArray input = nonce;
        char be[4];
        qToBigEndian<quint32>(quint32(counter), be);
        input.append(be, 4);
        const QByteArray block = hmac(encKey, input);
        const int n = qMin(kTagBytes, data.size() - offset);
        for (int i = 0; i < n; ++i)
            data[offset + i] = char(data[offset + i] ^ block[i]);
    }
}

} // namespace

bool ConfigKeys::isValid() const
{
    return encKey.size() == kLockKeyBytes && macKey.size() == kLockKeyBytes
           && verifier.size() == kLockKeyBytes;
}

void ConfigKeys::clear()
{
    // Overwrite before releasing: a QByteArray that merely goes out of scope
    // leaves the key sitting in freed heap.
    for (QByteArray *k : {&encKey, &macKey, &verifier}) {
        k->fill('\0');
        k->clear();
    }
}

ConfigKeys deriveKeys(const QString &password, const QByteArray &salt, int iterations)
{
    ConfigKeys keys;
    if (salt.size() != kLockSaltBytes || iterations <= 0)
        return keys;
    // One PBKDF2 pass for all three keys, then split. Deriving them separately
    // would cost three times the stretching for no added strength.
    const QByteArray material = QPasswordDigestor::deriveKeyPbkdf2(
        QCryptographicHash::Sha256, password.toUtf8(), salt, iterations, 3 * kLockKeyBytes);
    if (material.size() != 3 * kLockKeyBytes)
        return keys;
    keys.encKey = material.mid(0, kLockKeyBytes);
    keys.macKey = material.mid(kLockKeyBytes, kLockKeyBytes);
    keys.verifier = material.mid(2 * kLockKeyBytes, kLockKeyBytes);
    return keys;
}

QByteArray sealPayload(const QByteArray &plain, const ConfigKeys &keys)
{
    if (!keys.isValid())
        return {};
    const QByteArray nonce = randomBytes(kLockNonceBytes);
    QByteArray cipher = plain;
    applyKeystream(cipher, keys.encKey, nonce);
    // Encrypt-then-MAC over nonce AND ciphertext: covering the nonce stops it
    // being swapped for one from another file.
    const QByteArray tag = hmac(keys.macKey, nonce + cipher);
    return nonce + cipher + tag;
}

bool openPayload(const QByteArray &sealed, const ConfigKeys &keys, QByteArray *plain,
                 QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (!keys.isValid())
        return fail(QStringLiteral("no key"));
    if (sealed.size() < kLockNonceBytes + kTagBytes)
        return fail(QStringLiteral("the encrypted section is truncated"));

    const QByteArray nonce = sealed.left(kLockNonceBytes);
    const QByteArray tag = sealed.right(kTagBytes);
    const QByteArray cipher =
        sealed.mid(kLockNonceBytes, sealed.size() - kLockNonceBytes - kTagBytes);

    // Authenticate BEFORE decrypting. Decrypting first and parsing the result
    // would hand attacker-chosen bytes to the JSON reader.
    if (!constantTimeEquals(tag, hmac(keys.macKey, nonce + cipher)))
        return fail(QStringLiteral("the encrypted section fails its integrity check — the file "
                                   "is damaged or has been altered"));

    QByteArray out = cipher;
    applyKeystream(out, keys.encKey, nonce);
    if (plain)
        *plain = out;
    return true;
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size())
        return false;
    // Accumulate every byte; never break early. volatile keeps the compiler
    // from reintroducing the short-circuit this exists to avoid.
    volatile unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff = diff | static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace ct
