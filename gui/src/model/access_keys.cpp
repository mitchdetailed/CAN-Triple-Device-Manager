#include "access_keys.h"

#include "config_lock.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QObject>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QtEndian>

namespace ct {

namespace {

// The salt for the 4-byte device key. Fixed, application-wide, and never
// rotated — which is the opposite of the usual advice, so it is worth saying
// plainly why.
//
// The device stores the KEY, not the password, and it has no room for a salt.
// For one .ct3s to update a hundred units, the same password must fold to the
// same four bytes on every one of them; a per-user or per-file salt would make
// "the Protected Comms password" mean a different key on each machine that
// typed it, and the fleet story falls apart. So the salt is a constant of the
// protocol, exactly like kAccessKeyIterations.
//
// The cost is the one a fixed salt always carries: a table built once serves
// every installation of this app. kAccessKeyIterations is what makes building
// that table expensive, and the 2^32 key space the four bytes impose is the
// lower ceiling anyway — see the header, which does not pretend otherwise.
//
// The bytes themselves are arbitrary. They are not a secret and never were:
// this file is shipped, and the firmware header points at it by name.
constexpr unsigned char kAccessKeySalt[kAccessVerifierSaltBytes] = {
    0x9d, 0x4c, 0x1f, 0xa7, 0x63, 0x0e, 0xb8, 0x52,
    0x27, 0xd1, 0x6a, 0xf4, 0x08, 0x9b, 0x3e, 0xc5,
};

QByteArray accessKeySalt()
{
    return QByteArray(reinterpret_cast<const char *>(kAccessKeySalt), int(sizeof(kAccessKeySalt)));
}

// `n` must be a multiple of 4; every caller here passes kAccessVerifierSaltBytes.
QByteArray randomBytes(int n)
{
    QByteArray out(n, Qt::Uninitialized);
    // The system CSPRNG, not the default (deterministically seeded) generator.
    // A verifier salt that another process can predict is no salt at all, and
    // the default generator is reproducible by design.
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(out.data()),
                                         reinterpret_cast<quint32 *>(out.data() + n));
    return out;
}

// The enum arrives from JSON and from the wire, so it is not automatically one
// of the three values the type names.
bool validFunction(AccessFunction fn)
{
    return int(fn) >= 0 && int(fn) < kAccessFunctionCount;
}

} // namespace

const AccessFunction *allAccessFunctions()
{
    static const AccessFunction fns[kAccessFunctionCount] = {
        AccessFunction::SendConfiguration,
        AccessFunction::GetConfiguration,
        AccessFunction::EditProtectedComms,
    };
    return fns;
}

QString accessFunctionLabel(AccessFunction fn)
{
    switch (fn) {
    case AccessFunction::SendConfiguration:
        return QObject::tr("Send a Configuration");
    case AccessFunction::GetConfiguration:
        return QObject::tr("Get a Configuration");
    case AccessFunction::EditProtectedComms:
        return QObject::tr("Protected Comms");
    }
    return {};
}

QString accessFunctionDescription(AccessFunction fn)
{
    // Each one says what is PREVENTED, not what the password is. A user setting
    // a lock is deciding what to take away from whoever holds the device next,
    // and phrasing it any other way makes them guess.
    switch (fn) {
    case AccessFunction::SendConfiguration:
        return QObject::tr("Prevents a configuration being sent to the device without this "
                           "password.");
    case AccessFunction::GetConfiguration:
        return QObject::tr("Prevents a configuration being read back off the device without "
                           "this password.");
    case AccessFunction::EditProtectedComms:
        // Names what it actually gates now: not "editing", which is unlocked by
        // unticking a marking rather than by any password, but the MOVE itself.
        // On the device this is the one thing it is for — a Protect Communication
        // marking is not applied or lowered until a unit confirms this password,
        // and that round trip is what makes that tier stronger than Hidden.
        //
        // NECESSARY, NOT SUFFICIENT, and not a master key over anything. Since
        // 2.3.1 a Protect Communication message carries its own Message Password
        // as well and needs both halves; Hidden and Read Only answer to each
        // section's own password and to this one never. This comment used to
        // claim a master over all three, which is the substitution the model
        // refuses — isSectionRevealed, maySectionLower, proofsRequiredFor and the
        // section editor's tier ladder all decline it, and a comment describing
        // code we deleted is worse than no comment.
        return QObject::tr("Prevents proprietary CAN messages being unprotected without this "
                           "password. The device confirms it before this application will move "
                           "a message's Protect Communication marking — alongside that "
                           "message's own Message Password, which this one never replaces.");
    }
    return {};
}

QString accessFunctionKey(AccessFunction fn)
{
    // Written into files and settings, so these strings are frozen: renaming one
    // orphans the verifier in every .ct3 already saved.
    switch (fn) {
    case AccessFunction::SendConfiguration:
        return QStringLiteral("sendConfiguration");
    case AccessFunction::GetConfiguration:
        return QStringLiteral("getConfiguration");
    case AccessFunction::EditProtectedComms:
        return QStringLiteral("editProtectedComms");
    }
    return {};
}

QString passwordProblem(const QString &password)
{
    if (password.isEmpty())
        return QString(); // "no password" — always allowed, see the header
    if (password.length() < kMinPasswordLength)
        return QObject::tr("A password must be at least %1 characters. Leave the box empty "
                           "if you want no password at all.")
            .arg(kMinPasswordLength);
    return QString();
}

AccessKey deriveAccessKey(const QString &password)
{
    // An empty password is how every caller says "no password here", so it must
    // not stretch into a real key — otherwise clearing a lock would set one.
    if (password.isEmpty())
        return kNoAccessKey;
    // Not enforced here on purpose: this function is also how an EXISTING
    // password is turned into a key to prove it, and a device provisioned before
    // the minimum existed may hold a shorter one. Refusing it here would lock
    // that unit out of the tool that could change it. The rule belongs at the
    // points where a password is CHOSEN — see passwordProblem().

    // toUtf8() rather than any local encoding: the same password typed on a
    // machine with a different locale has to reach the same four bytes, or a
    // fleet built in one office cannot be updated from another.
    const QByteArray material =
        QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(),
                                           accessKeySalt(), kAccessKeyIterations, kAccessKeyBytes);

    AccessKey key = kNoAccessKey;
    if (material.size() == kAccessKeyBytes)
        key = qFromBigEndian<quint32>(material.constData());

    // Zero means "no password" everywhere else in this file, so a non-empty
    // password must never fold to it. A password that silently reads as no
    // password is a trapdoor: the dialog would report the function locked while
    // the device was left open. Roughly one derivation in four billion lands on
    // zero honestly; a derivation that failed outright lands there too, which is
    // why the nudge covers the whole path rather than only the arithmetic case.
    // Colliding one password onto key 1 costs nothing — the key space was never
    // the strong part, and a wrong key only ever fails a challenge.
    return key == kNoAccessKey ? AccessKey(1) : key;
}

QByteArray accessKeyBytes(AccessKey key)
{
    QByteArray out(kAccessKeyBytes, Qt::Uninitialized);
    // Big-endian so the key reads the same way in the firmware's AccessKeyRecord,
    // in a hex dump of flash and in this app. The wire is little-endian for
    // integers, but these four bytes are a byte STRING, not a number, and the
    // HMAC both ends compute is over the bytes in this order.
    qToBigEndian<quint32>(key, out.data());
    return out;
}

AccessKey accessKeyFromBytes(const QByteArray &bytes)
{
    if (bytes.size() != kAccessKeyBytes)
        return kNoAccessKey;
    return qFromBigEndian<quint32>(bytes.constData());
}

QByteArray accessResponse(AccessKey key, const QByteArray &challenge)
{
    // Both guards return empty rather than a well-formed answer. A caller that
    // holds no key must not be able to put a plausible-looking response on the
    // wire by accident; an empty result is a mistake it cannot help but notice.
    if (key == kNoAccessKey || challenge.size() != kAccessChallengeBytes)
        return {};
    return QMessageAuthenticationCode::hash(challenge, accessKeyBytes(key),
                                            QCryptographicHash::Sha256);
}

QByteArray deriveLicenseKey(const QString &passphrase)
{
    if (passphrase.isEmpty())
        return {};
    // Its own salt, so a phrase reused between a licence and an access password
    // does not produce two related secrets. See the note in the header.
    static const char kLicenseSalt[] = "CANTriple/license/v1";
    const QByteArray salt(kLicenseSalt, int(sizeof(kLicenseSalt) - 1));
    // toUtf8(), not a local encoding: the same phrase typed under a different
    // locale has to reach the same bytes or a licence stops verifying when the
    // laptop changes.
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, passphrase.toUtf8(),
                                              salt, kAccessKeyIterations, kLicenseKeyBytes);
}

namespace {
QByteArray licenseMac(const char *label, const QByteArray &key, const QByteArray &challenge)
{
    if (key.size() != kLicenseKeyBytes || challenge.size() != kAccessChallengeBytes)
        return {};
    QByteArray msg(label);
    msg.append(challenge);
    return QMessageAuthenticationCode::hash(msg, key, QCryptographicHash::Sha256);
}
} // namespace

QByteArray licenseAuthResponse(const QByteArray &key, const QByteArray &challenge)
{
    return licenseMac(kLicenseAuthLabel, key, challenge);
}

QByteArray licenseProveExpected(const QByteArray &key, const QByteArray &challenge)
{
    return licenseMac(kLicenseProveLabel, key, challenge);
}

AccessKey AccessKeySet::key(AccessFunction fn) const
{
    return validFunction(fn) ? keys[int(fn)] : kNoAccessKey;
}

void AccessKeySet::setKey(AccessFunction fn, AccessKey key)
{
    if (validFunction(fn))
        keys[int(fn)] = key;
}

bool AccessKeySet::isSet(AccessFunction fn) const
{
    return key(fn) != kNoAccessKey;
}

bool AccessKeySet::any() const
{
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        if (keys[i] != kNoAccessKey)
            return true;
    }
    return false;
}

quint8 AccessKeySet::mask() const
{
    // Bit i is AccessFunction i, which is what makes this directly comparable
    // with the device's AccessKeyRecord::set_mask.
    quint8 m = 0;
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        if (keys[i] != kNoAccessKey)
            m = quint8(m | (1u << i));
    }
    return m;
}

void AccessKeySet::clear()
{
    for (int i = 0; i < kAccessFunctionCount; ++i)
        keys[i] = kNoAccessKey;
}

bool AccessVerifier::isValid() const
{
    // The upper bound is a DoS guard, not a correctness one: verify() runs the
    // KDF for `iterations` rounds, and a hostile file naming billions would hang
    // the prompt for an hour. isValid() is verify()'s gate, so rejecting the
    // absurd count here means the expensive derivation never starts — the file
    // reads as a malformed lock, i.e. wrong password. See kMaxKdfIterations.
    return salt.size() == kAccessVerifierSaltBytes && verifier.size() == kAccessVerifierBytes
           && iterations > 0 && iterations <= kMaxKdfIterations;
}

bool AccessVerifier::verify(const QString &password) const
{
    // Structure first: a file can carry a truncated or hand-edited verifier, and
    // "malformed" must read as "wrong", never as "no lock to check".
    if (!isValid())
        return false;
    const QByteArray candidate =
        QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(), salt,
                                           iterations, kAccessVerifierBytes);
    return constantTimeEquals(verifier, candidate);
}

AccessVerifier AccessVerifier::make(const QString &password)
{
    AccessVerifier v;
    // An empty password means "no password", the same as it does in
    // deriveAccessKey. Returning an unset verifier here is what makes
    // `setVerifier(fn, make(text))` do the right thing when the field is blank,
    // instead of locking the document behind the empty string.
    if (password.isEmpty())
        return v;

    v.salt = randomBytes(kAccessVerifierSaltBytes);
    v.iterations = kAccessVerifierIterations;
    // Per-file random salt, unlike the device key's fixed one: this value only
    // ever answers "was that the right password" on this machine, so nothing
    // needs it to match across units — and making it differ per file means two
    // documents locked with the same password do not advertise the fact.
    v.verifier = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(),
                                                    v.salt, v.iterations, kAccessVerifierBytes);
    if (v.verifier.size() != kAccessVerifierBytes)
        v = AccessVerifier(); // derivation failed: store nothing rather than half a lock
    return v;
}

QJsonObject AccessVerifier::toJson() const
{
    QJsonObject o;
    o["salt"] = QString::fromLatin1(salt.toBase64());
    o["iterations"] = iterations;
    o["verifier"] = QString::fromLatin1(verifier.toBase64());
    return o;
}

AccessVerifier AccessVerifier::fromJson(const QJsonObject &o)
{
    AccessVerifier v;
    v.salt = QByteArray::fromBase64(o["salt"].toString().toLatin1());
    v.iterations = o["iterations"].toInt(kAccessVerifierIterations);
    v.verifier = QByteArray::fromBase64(o["verifier"].toString().toLatin1());
    return v;
}

const AccessVerifier &AccessVerifierSet::verifier(AccessFunction fn) const
{
    // A shared unset verifier for an out-of-range function. isSet() is false on
    // it and verify() refuses it, so a bad index degrades to "no password
    // recorded" rather than to a dangling reference.
    static const AccessVerifier none;
    return validFunction(fn) ? verifiers[int(fn)] : none;
}

void AccessVerifierSet::setVerifier(AccessFunction fn, const AccessVerifier &v)
{
    if (validFunction(fn))
        verifiers[int(fn)] = v;
}

bool AccessVerifierSet::isSet(AccessFunction fn) const
{
    return verifier(fn).isSet();
}

bool AccessVerifierSet::any() const
{
    for (const AccessVerifier &v : verifiers) {
        if (v.isSet())
            return true;
    }
    return false;
}

void AccessVerifierSet::clear()
{
    for (AccessVerifier &v : verifiers)
        v = AccessVerifier();
}

QJsonObject AccessVerifierSet::toJson() const
{
    QJsonObject o;
    // Only the functions that actually carry a password are written. An unset
    // verifier is all-empty and says nothing, and emitting three of them into
    // every .ct3 would make an unprotected document look protected to anyone
    // reading the file.
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        const AccessFunction fn = allAccessFunctions()[i];
        if (verifiers[i].isSet())
            o[accessFunctionKey(fn)] = verifiers[i].toJson();
    }
    return o;
}

AccessVerifierSet AccessVerifierSet::fromJson(const QJsonObject &o)
{
    AccessVerifierSet set;
    for (int i = 0; i < kAccessFunctionCount; ++i) {
        const AccessFunction fn = allAccessFunctions()[i];
        const QJsonValue v = o[accessFunctionKey(fn)];
        if (v.isObject())
            set.verifiers[i] = AccessVerifier::fromJson(v.toObject());
    }
    return set;
}


} // namespace ct
