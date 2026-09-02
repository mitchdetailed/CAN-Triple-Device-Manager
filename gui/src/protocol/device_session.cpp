#include "device_session.h"

#include <QRandomGenerator>
#include <QtEndian>

#include <cstring>

#include "../model/config_lock.h"

namespace ct {
namespace device_session {

namespace {

// AccessFunction lives in the model layer and ACCESS_FN_* is the wire, and
// nothing but these assertions keeps the two in step — every function number
// this file sends is a plain cast of the enum. Reordering the enum to reorder
// the dialog's list would otherwise set the wrong password on every unit in a
// fleet, and the device would accept it without complaint.
static_assert(int(AccessFunction::SendConfiguration) == ACCESS_FN_SEND, "wire order");
static_assert(int(AccessFunction::GetConfiguration) == ACCESS_FN_GET, "wire order");
static_assert(int(AccessFunction::EditProtectedComms) == ACCESS_FN_EDIT_COMMS, "wire order");
static_assert(kAccessFunctionCount == ACCESS_FN_COUNT, "wire order");
static_assert(kAccessKeyBytes == ACCESS_KEY_LEN, "wire size");
static_assert(kAccessChallengeBytes == ACCESS_CHALLENGE_LEN, "wire size");

// AccessKeyWritePayload is blitted onto the wire as a raw struct, so its packed
// layout is load-bearing here in a way it is not anywhere else. The size is
// asserted at the point of use rather than only next to the declaration,
// because this is the file that would break. It is now the only such record:
// the fleet identity is unpacked field by field below, and there is nothing
// left in this file that writes one.
static_assert(sizeof(AccessKeyWritePayload) == 7, "must match firmware");

// A NACK of ERR_INVALID_CMD means this firmware predates the command, which is
// not a failure — it is a device with no access passwords, no fleet identity
// and no binding. Anything else is a real error the caller should see.
bool unsupported(quint8 errCode)
{
    return errCode == ERR_INVALID_CMD;
}

// The enum's values ARE the wire's, per the assertions above. This exists so
// the cast is written once and reads as a deliberate conversion at each of the
// places that puts a function number in a payload.
quint8 functionByte(AccessFunction fn)
{
    return quint8(fn);
}

// One of the fleet identity's fixed-width strings. The field is NUL-PADDED
// rather than NUL-terminated, so all `len` bytes are usable and a 16-byte name
// carries no terminator at all. QString::fromUtf8(p) would therefore keep
// reading — straight through model_id and into the serial number — for exactly
// the longest names, which is the case least likely to be tried on a bench.
// Take the bytes up to the first NUL, or the whole field if there is none.
QString paddedField(const char *p, int len)
{
    int used = 0;
    while (used < len && p[used] != '\0')
        ++used;
    return QString::fromUtf8(p, used);
}

// Is this field a non-answer? Both fill bytes mean the same thing and are kept
// distinct nowhere: 0xFF is virgin OTP on a part nobody has burned, and 0x00 is
// a field a programmer zeroed rather than filled. A field is unknown only when
// EVERY byte is one of them — "Minton Performance" followed by NUL padding is a
// perfectly good answer, and testing the padding alone would throw it away.
bool otpFieldUnknown(const char *p, int len)
{
    bool allFF = true;
    bool allZero = true;
    for (int i = 0; i < len; ++i) {
        const quint8 b = quint8(p[i]);
        if (b != 0xFFu)
            allFF = false;
        if (b != 0x00u)
            allZero = false;
    }
    return allFF || allZero;
}

// One OTP text field: unknown, or the bytes up to the first NUL.
//
// Anything unprintable is dropped rather than shown. These are ASCII fields by
// convention and nothing enforces it, so a mis-burned record can hold arbitrary
// bytes — and a control character reaching a QLabel is at best invisible and at
// worst reformats the dialog around it. The raw bytes stay on DeviceInfo::raw
// for anyone who needs to see what is really there.
QString otpText(const char *p, int len)
{
    if (otpFieldUnknown(p, len))
        return QString();
    int used = 0;
    while (used < len && p[used] != '\0')
        ++used;
    QString text;
    text.reserve(used);
    for (const QChar c : QString::fromUtf8(p, used)) {
        if (c.isPrint())
            text.append(c);
    }
    return text.trimmed();
}

// Shared body of writeAccessKey/clearAccessKey. Both send the same record and
// differ only in one byte, and keeping the round trip in one place keeps their
// error text identical — a user who clears a password and a user who changes
// one hit the same device refusal and should read the same sentence.
bool sendAccessKeyWrite(DeviceLink *link, AccessFunction fn, AccessKey key, bool clear,
                        QString *error, int slot = 1)
{
    if (!link)
        return false;
    AccessKeyWritePayload record{};
    record.function = functionByte(fn);
    record.clear = clear ? 1 : 0;
    record.slot = quint8(qBound(1, slot, 4));
    if (!clear) {
        const QByteArray bytes = accessKeyBytes(key);
        if (bytes.size() != ACCESS_KEY_LEN) {
            if (error)
                *error = QStringLiteral("The password did not produce a usable key.");
            return false;
        }
        memcpy(record.key, bytes.constData(), ACCESS_KEY_LEN);
    }
    // record.key stays zero when clearing. The device ignores it, but an
    // all-zero key is also how the device spells "no password", so sending the
    // real bytes there would put a live key on the wire to say the opposite.

    const QByteArray payload(reinterpret_cast<const char *>(&record), sizeof(record));
    quint8 code = 0;
    if (!link->requestSync(CMD_WRITE_ACCESS_KEYS, payload, nullptr, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                *error = QStringLiteral("This firmware does not support access passwords.");
        } else if (code == ERR_LOCKED) {
            if (error)
                *error = QStringLiteral("The device refused the change: the password currently "
                                        "in force has not been proved on this connection.");
        }
        return false;
    }
    return true;
}

} // namespace

QString Identity::uidText() const
{
    if (uid.size() != CONFIG_UID_LEN)
        return {};
    // Most significant byte first, so the printed string orders the same way a
    // human would compare two of them.
    QString text;
    for (int i = uid.size() - 1; i >= 0; --i)
        text += QStringLiteral("%1").arg(quint8(uid[i]), 2, 16, QLatin1Char('0')).toUpper();
    return text;
}

bool parseIdentity(const QByteArray &payload, Identity *out)
{
    if (!out)
        return false;
    *out = Identity{};
    if (payload.size() < CONFIG_UID_LEN + 1)
        return false;
    out->uid = payload.left(CONFIG_UID_LEN);
    out->configStatus = quint8(payload[CONFIG_UID_LEN]);
    out->supported = true;
    return true;
}

bool parseAccessState(const QByteArray &payload, AccessState *out)
{
    if (!out)
        return false;
    *out = AccessState{};
    if (payload.isEmpty())
        return false;
    // One byte, one bit per function, bit i being function i — which is what
    // the ACCESS_MASK_* assertions at the top of this file pin down, and why
    // this can be a loop instead of three named assignments.
    const quint8 mask = quint8(payload[0]);
    for (int i = 0; i < kAccessFunctionCount; ++i)
        out->set[i] = (mask & (1u << i)) != 0;
    // v17: the second byte, when the firmware sends one, is the Protected Comms
    // slot mask. Absent on older firmware; protSlots then stays 0 and callers
    // fall back to the single-password reading of isSet().
    if (payload.size() >= 2)
        out->protSlots = quint8(payload[1]);
    out->supported = true;
    return true;
}

// The OTP manufacturing record. Field by field, and each one independently
// allowed to be missing — see the DeviceInfo comment for why a half-burned part
// is normal rather than broken.
bool parseDeviceInfo(const QByteArray &payload, DeviceInfo *out)
{
    if (!out)
        return false;
    const bool wasSupported = out->supported;
    *out = DeviceInfo{};
    out->supported = wasSupported;
    if (payload.size() < int(OTP_INFO_LEN))
        return false;
    out->raw = payload.left(int(OTP_INFO_LEN));

    const char *p = out->raw.constData();
    out->manufacturer = otpText(p + OTP_INFO_MANUFACTURER_AT, OTP_INFO_MANUFACTURER_LEN);
    out->product = otpText(p + OTP_INFO_PRODUCT_AT, OTP_INFO_PRODUCT_LEN);
    out->hardwareVersion = otpText(p + OTP_INFO_HW_VERSION_AT, OTP_INFO_HW_VERSION_LEN);

    // BIG-endian, unlike every other multi-byte field in this protocol, which is
    // little. That is the burn tool's convention and not a choice available here
    // — the record is already in silicon on any part worth reading — so it is
    // spelled out rather than left to qFromBigEndian to imply.
    if (!otpFieldUnknown(p + OTP_INFO_SERIAL_AT, OTP_INFO_SERIAL_LEN)) {
        quint64 serial = 0;
        for (int i = 0; i < OTP_INFO_SERIAL_LEN; ++i)
            serial = (serial << 8) | quint8(p[OTP_INFO_SERIAL_AT + i]);
        out->serialNumber = serial;
        out->serialKnown = true;
    }

    out->dateText = otpText(p + OTP_INFO_DATE_AT, OTP_INFO_DATE_LEN);
    if (!out->dateText.isEmpty()) {
        // DDMMYYYY. QDate::fromString returns an invalid date for anything that
        // is not one, which is exactly the signal the dialog wants: it then
        // shows the characters as burned instead of inventing a day.
        out->date = QDate::fromString(out->dateText, QStringLiteral("ddMMyyyy"));
    }
    return true;
}

// The public licence record: three NUL-padded strings and a flags word. 74
// bytes, unpacked by hand for the same reason every wire record here is — the
// parser must depend on the documented layout, not on this compiler having
// packed a mirror struct the way it was asked to.
bool parseLicense(const QByteArray &payload, LicenseState *out)
{
    if (!out)
        return false;
    const bool wasSupported = out->supported;
    *out = LicenseState{};
    out->supported = wasSupported;

    constexpr int kManufacturerAt = 0;
    constexpr int kModelAt = kManufacturerAt + LICENSE_MANUFACTURER_LEN;
    constexpr int kVersionAt = kModelAt + LICENSE_MODEL_LEN;
    constexpr int kFlagsAt = kVersionAt + LICENSE_VERSION_LEN;
    constexpr int kPublicLen = kFlagsAt + 2; // 74
    if (payload.size() < kPublicLen)
        return false;

    const char *p = payload.constData();
    out->manufacturer = paddedField(p + kManufacturerAt, LICENSE_MANUFACTURER_LEN);
    out->model = paddedField(p + kModelAt, LICENSE_MODEL_LEN);
    out->firmwareVersion = paddedField(p + kVersionAt, LICENSE_VERSION_LEN);
    const quint16 flags = qFromLittleEndian<quint16>(p + kFlagsAt);
    out->keySet = (flags & LICENSE_FLAG_KEY_SET) != 0;
    out->updaterSet = (flags & LICENSE_FLAG_UPDATER_SET) != 0;
    return true;
}

bool parseConfigVersion(const QByteArray &payload, ConfigVersionState *out)
{
    if (!out)
        return false;
    const bool wasSupported = out->supported;
    *out = ConfigVersionState{};
    out->supported = wasSupported;
    if (payload.size() < 2)
        return false;
    out->version = qFromLittleEndian<quint16>(payload.constData());
    return true;
}

bool readIdentity(DeviceLink *link, Identity *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = Identity{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_GET_DEVICE_ID, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            return true; // old firmware: no identity, not an error
        }
        return false;
    }
    if (!parseIdentity(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned a short identity response.");
        return false;
    }
    return true;
}

bool readAccessState(DeviceLink *link, AccessState *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = AccessState{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_READ_ACCESS_KEYS, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            return true; // pre-v19 firmware: no access passwords exist to report
        }
        return false;
    }
    if (!parseAccessState(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned an empty access password response.");
        return false;
    }
    return true;
}

bool readConfigVersion(DeviceLink *link, ConfigVersionState *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = ConfigVersionState{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_READ_CONFIG_VERSION, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            return true; // older firmware: it cannot say
        }
        return false;
    }
    out->supported = true;
    if (!parseConfigVersion(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned a short configuration version.");
        return false;
    }
    return true;
}

bool readDeviceInfo(DeviceLink *link, DeviceInfo *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = DeviceInfo{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_GET_DEVICE_INFO, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            return true; // older firmware: it cannot read its own OTP for us
        }
        return false;
    }
    out->supported = true;
    if (!parseDeviceInfo(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned a short device info response.");
        return false;
    }
    return true;
}

bool readLicense(DeviceLink *link, LicenseState *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = LicenseState{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_READ_LICENSE, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            return true; // firmware older than licensing
        }
        return false;
    }
    out->supported = true;
    if (!parseLicense(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned a short licence response.");
        return false;
    }
    return true;
}

bool proveLicenseSecret(DeviceLink *link, const QByteArray &secret, QString *error,
                        bool *wrongSecret)
{
    if (wrongSecret)
        *wrongSecret = false;
    if (!link)
        return false;

    QByteArray challenge;
    quint8 code = 0;
    if (!link->requestSync(CMD_LICENSE_CHALLENGE, QByteArray(), &challenge, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code) && error)
            *error = QStringLiteral("This firmware has no licence to prove.");
        return false;
    }
    const QByteArray answer = licenseAuthResponse(secret, challenge);
    if (answer.isEmpty()) {
        // Empty means the derivation produced nothing usable, which for a
        // non-empty passphrase is a bug and for an empty one is the caller
        // asking to prove a secret it does not have. Either way, do not put a
        // malformed response on the wire and call the result a wrong password.
        if (error)
            *error = QStringLiteral("No licence secret to prove with.");
        return false;
    }
    QByteArray ignored;
    if (!link->requestSync(CMD_LICENSE_RESPONSE, answer, &ignored, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        // ERR_LOCKED is the device saying the HMAC did not match. That is a
        // wrong passphrase, not a broken link, and the two want opposite
        // reactions from the caller.
        if (code == ERR_LOCKED) {
            if (wrongSecret)
                *wrongSecret = true;
            if (error)
                *error = QStringLiteral("The device did not accept that as its FW Updater "
                                        "Password or its Firmware Key.");
        }
        return false;
    }
    return true;
}

bool proveLicenseKey(DeviceLink *link, const QByteArray &expectedKey, QString *error,
                     bool *mismatch)
{
    if (mismatch)
        *mismatch = false;
    if (!link || expectedKey.size() != kLicenseKeyBytes)
        return false;

    // The HOST picks the nonce here. That is the whole value of this exchange:
    // a device cannot pre-compute an answer to a challenge it has not been
    // given, so a reply that matches could only have come from a unit holding
    // the key. From the system CSPRNG, not the default generator, because a
    // predictable nonce would make a captured reply replayable.
    QByteArray challenge(kAccessChallengeBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(challenge.data()),
                                         reinterpret_cast<quint32 *>(challenge.data()
                                                                    + challenge.size()));

    QByteArray answer;
    quint8 code = 0;
    if (!link->requestSync(CMD_LICENSE_KEY_PROVE, challenge, &answer, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        // ERR_LOCKED is the device saying it holds no Firmware Key at all. That
        // is a unit with nothing to prove, not a broken link.
        if (code == ERR_LOCKED && error)
            *error = QStringLiteral("This unit holds no Firmware Key to prove.");
        return false;
    }

    const QByteArray expected = licenseProveExpected(expectedKey, challenge);
    if (expected.isEmpty() || answer != expected) {
        if (mismatch)
            *mismatch = true;
        if (error)
            *error = QStringLiteral("The device does not hold the expected Firmware Key.");
        return false;
    }
    return true;
}

bool writeLicense(DeviceLink *link, const LicenseWrite &write, QString *error)
{
    if (!link)
        return false;

    // Built field by field into a fixed-width, NUL-padded buffer. clipToUtf8Bytes
    // is deliberately NOT called here: the dialog clamps what can be typed, and a
    // silent truncation at this layer would put a different name on the device
    // than the one on screen. What this does guarantee is that an over-long
    // string cannot overrun its field.
    QByteArray payload(LICENSE_MANUFACTURER_LEN + LICENSE_MODEL_LEN + LICENSE_VERSION_LEN
                           + 2 * (1 + LICENSE_KEY_LEN),
                       '\0');
    const auto put = [&payload](int at, int len, const QString &text) {
        const QByteArray utf8 = text.toUtf8().left(len);
        std::memcpy(payload.data() + at, utf8.constData(), size_t(utf8.size()));
    };
    int at = 0;
    put(at, LICENSE_MANUFACTURER_LEN, write.manufacturer);
    at += LICENSE_MANUFACTURER_LEN;
    put(at, LICENSE_MODEL_LEN, write.model);
    at += LICENSE_MODEL_LEN;
    put(at, LICENSE_VERSION_LEN, write.firmwareVersion);
    at += LICENSE_VERSION_LEN;

    // KEEP unless asked otherwise, for both secrets and by the same rule, so
    // neither can quietly grow a different one. Editing a model name must
    // disturb neither password.
    const auto putSecret = [&](const QByteArray &secret, bool clear) {
        quint8 action = LICENSE_KEY_KEEP;
        if (clear)
            action = LICENSE_KEY_CLEAR;
        else if (secret.size() == kLicenseKeyBytes)
            action = LICENSE_KEY_SET;
        payload[at++] = char(action);
        if (action == LICENSE_KEY_SET)
            std::memcpy(payload.data() + at, secret.constData(), size_t(kLicenseKeyBytes));
        at += kLicenseKeyBytes;
    };
    putSecret(write.key, write.clearKey);
    putSecret(write.updaterKey, write.clearUpdater);

    QByteArray resp;
    quint8 code = 0;
    // THE LONG TIMEOUT. The device erases and reprograms a page in the bank it
    // is executing from, so it stalls for tens of milliseconds and answers late.
    // With the default timeout this reads as a dead device on a write that in
    // fact succeeded — and the retry would then be refused for want of a proof
    // the first write consumed.
    if (!link->requestSync(CMD_WRITE_LICENSE, payload, &resp, error, DeviceLink::kFlashTimeoutMs,
                           DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code) && error) {
            *error = QStringLiteral("This firmware cannot store a licence \u2014 it is older than "
                                    "the Firmware License Manager. Update the unit's firmware.");
        } else if (code == ERR_LOCKED && error) {
            *error = QStringLiteral("The device refused the write: its FW Updater Password has "
                                    "not been proved in this session.");
        }
        return false;
    }
    return true;
}

// The first of the two round trips a proof needs. It was shared by the function
// proof and the retired per-message one, because the device keeps ONE challenge
// slot and does not care which kind of answer spends it; only the function proof
// is left, and the slot is still the device's, so this stays a separate step.
//
// The challenge keeps the link's ordinary retries, but a retransmit is not
// free: the reply carries nothing tying it to its request — the payload IS the
// nonce, and a retransmitted CMD_ACCESS_CHALLENGE is byte-identical to the
// original, so not even the ACK-style CRC echo could tell their replies apart.
// When the first reply is merely LATE rather than lost, it arrives during the
// retransmit's wait and is consumed as the retransmit's answer, while the
// device holds the second nonce it minted; a proof computed from the stale one
// then fails ERR_LOCKED on a correct password. proveAccess absorbs that by
// believing ERR_LOCKED only after a second whole exchange. The ANSWER is what
// may not be retried; see the callers.
static bool fetchChallenge(DeviceLink *link, QByteArray *challenge, QString *error)
{
    quint8 code = 0;
    if (!link->requestSync(CMD_ACCESS_CHALLENGE, QByteArray(), challenge, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code) && error)
            *error = QStringLiteral("This firmware does not support access passwords.");
        return false;
    }
    if (challenge->size() != ACCESS_CHALLENGE_LEN) {
        if (error)
            *error = QStringLiteral("The device could not produce a challenge. Its random "
                                    "number generator may have failed.");
        return false;
    }
    return true;
}

bool proveAccess(DeviceLink *link, AccessFunction fn, AccessKey key, QString *error,
                 bool *wrongPassword)
{
    // Two round trips, and they must be consecutive: the device consumes the
    // challenge on the first response attempt, so nothing may be interleaved.

    if (wrongPassword)
        *wrongPassword = false;
    if (!link || key == kNoAccessKey) {
        if (error)
            *error = QStringLiteral("No password to prove.");
        return false;
    }

    // TWO whole exchanges before an ERR_LOCKED is believed. A challenge reply
    // cannot be correlated with its request (see fetchChallenge), so after a
    // timeout-and-retransmit the nonce in hand may be the stale first one while
    // the device holds the second — and the proof below then earns ERR_LOCKED
    // with a correctly typed password. Running the exchange again from the top
    // settles it: a fresh nonce cannot be stale, the device keeps no
    // failed-attempt counter (a wrong guess costs only the round trip), and a
    // genuinely wrong key simply NACKs a second time.
    for (int attempt = 0; attempt < 2; ++attempt) {
        QByteArray challenge;
        if (!fetchChallenge(link, &challenge, error))
            return false;
        quint8 code = 0;

        // Which function is being proved travels with the answer rather than
        // with the challenge, so the challenge stays a plain nonce and one
        // device-side slot serves all three.
        QByteArray body;
        body.append(char(functionByte(fn)));
        body.append(accessResponse(key, challenge));

        // The answer goes out with NO retries, which is the one place on this
        // link that asks for none. The nonce is spent by the first
        // CMD_ACCESS_RESPONSE the device sees, so a retransmission — which is
        // exactly what a lost ACK provokes — arrives at a device that has
        // already checked the password, accepted it, and thrown the challenge
        // away, and is answered ERR_LOCKED. From here that is indistinguishable
        // from a wrong key, so the retry would turn a correct password into
        // "Wrong password." and send the caller off to re-prompt somebody who
        // typed it correctly. One attempt, and a silence is reported as a
        // silence: the whole two-trip exchange is what gets run again, and that
        // is cheap.
        if (link->requestSync(CMD_ACCESS_RESPONSE, body, nullptr, error,
                              DeviceLink::kDefaultTimeoutMs, /*retries=*/0, &code))
            return true;
        // Only a NACK on this un-retried attempt can mean the key was wrong, so
        // only that may set wrongPassword — and only once the second exchange
        // has agreed (the loop comment above says why the first may lie).
        if (code == ERR_LOCKED) {
            if (attempt == 0)
                continue;
            if (wrongPassword)
                *wrongPassword = true;
            if (error)
                *error = QStringLiteral("Wrong password.");
        } else if (code == 0 && error) {
            // A link failure, and worth saying plainly, because the default
            // timeout text would leave the user staring at a password field
            // wondering whether to change what they typed. They should not: the
            // device may well have accepted it and only the reply went missing.
            *error = QStringLiteral("The device did not answer, so the password was neither "
                                    "accepted nor refused. This is a connection problem and "
                                    "not a wrong password — try again.");
        }
        return false;
    }
    return false; // not reachable: the second ERR_LOCKED returns inside the loop
}

bool writeAccessKey(DeviceLink *link, AccessFunction fn, AccessKey key, QString *error,
                    int slot)
{
    // A zero key is the device's own spelling of "no password", so accepting
    // one here would quietly turn "set this password" into "remove it". The
    // only way to reach this is an empty password, and the dialog is expected
    // to route that to clearAccessKey where the user can see what it means.
    if (key == kNoAccessKey) {
        if (error)
            *error = QStringLiteral("An empty password cannot be set. Clear the password "
                                    "instead if that is what was meant.");
        return false;
    }
    return sendAccessKeyWrite(link, fn, key, false, error, slot);
}

bool clearAccessKey(DeviceLink *link, AccessFunction fn, QString *error, int slot)
{
    return sendAccessKeyWrite(link, fn, kNoAccessKey, true, error, slot);
}

// The configuration version has no writer of its own, and that is deliberate:
// it moves as the optional payload of CMD_SAVE_TO_FLASH, so it commits with the
// tables it describes and can never name a revision the unit is not running.
// See ConfigTransfer::send and its configVersion parameter.

bool writeBinding(DeviceLink *link, const QByteArray &uid, QString *error)
{
    if (!link)
        return false;
    QByteArray payload(CONFIG_UID_LEN, char(0)); // all zero = unbound
    if (uid.size() == CONFIG_UID_LEN)
        payload = uid;
    else if (!uid.isEmpty()) {
        if (error)
            *error = QStringLiteral("The device identity is the wrong size.");
        return false;
    }
    quint8 code = 0;
    if (!link->requestSync(CMD_WRITE_CONFIG_BINDING, payload, nullptr, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                *error = QStringLiteral("This firmware does not support device binding, so the "
                                        "configuration was sent unbound.");
        }
        return false;
    }
    return true;
}

} // namespace device_session
} // namespace ct
