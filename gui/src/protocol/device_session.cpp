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
static_assert(sizeof(AccessKeyWritePayload) == 6, "must match firmware");

// The model layer sizes the two identity strings (kFleet*IdBytes) so the dialogs
// can clamp a typed name to what the wire can carry. These assertions say the
// two halves agree about how much room that is. If they drifted, names would be
// clamped to one width and read back at another, and the mismatch would show up
// as a fleet that no longer matches itself.
static_assert(kFleetVendorIdBytes == FLEET_VENDOR_ID_LEN, "wire size");
static_assert(kFleetModelIdBytes == FLEET_MODEL_ID_LEN, "wire size");

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

// Shared body of writeAccessKey/clearAccessKey. Both send the same record and
// differ only in one byte, and keeping the round trip in one place keeps their
// error text identical — a user who clears a password and a user who changes
// one hit the same device refusal and should read the same sentence.
bool sendAccessKeyWrite(DeviceLink *link, AccessFunction fn, AccessKey key, bool clear,
                        QString *error)
{
    if (!link)
        return false;
    AccessKeyWritePayload record{};
    record.function = functionByte(fn);
    record.clear = clear ? 1 : 0;
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
    out->supported = true;
    return true;
}

bool parseFleetIdentity(const QByteArray &payload, FleetIdentityState *out)
{
    if (!out)
        return false;
    *out = FleetIdentityState{};
    // FleetIdentityPublic on the wire: char vendor[16], char model[16],
    // u32 serial, u16 config_version, u16 flags, u8 key_present.
    constexpr int kVendorAt = 0;
    constexpr int kModelAt = kVendorAt + FLEET_VENDOR_ID_LEN;
    constexpr int kSerialAt = kModelAt + FLEET_MODEL_ID_LEN;
    constexpr int kVersionAt = kSerialAt + 4;
    constexpr int kFlagsAt = kVersionAt + 2;
    constexpr int kKeyPresentAt = kFlagsAt + 2;
    constexpr int kPublicLen = kKeyPresentAt + 1; // 41
    if (payload.size() < kPublicLen)
        return false;

    // Read field by field rather than casting the buffer onto the mirror
    // struct. Forty-one bytes cost nothing to unpack by hand, and doing it this
    // way means the parser — the half that test_firmware_link feeds real
    // firmware bytes to — depends only on the documented layout, not on this
    // compiler having packed the mirror the way it was asked to. The wire is
    // packed and little-endian; this side is neither, which is why every scalar
    // goes through qFromLittleEndian and the strings through paddedField.
    const char *p = payload.constData();
    out->identity.vendorId = paddedField(p + kVendorAt, FLEET_VENDOR_ID_LEN);
    out->identity.modelId = paddedField(p + kModelAt, FLEET_MODEL_ID_LEN);
    out->identity.serialNumber = qFromLittleEndian<quint32>(p + kSerialAt);
    out->identity.configVersion = qFromLittleEndian<quint16>(p + kVersionAt);
    out->identity.flags = qFromLittleEndian<quint16>(p + kFlagsAt);
    out->keyPresent = quint8(payload[kKeyPresentAt]) != 0;
    // The device never hands the fleet key back, so the parsed identity always
    // carries kNoAccessKey. Setting it explicitly keeps that visible to anyone
    // reading a call site rather than only to anyone reading the struct.
    out->identity.fleetKey = kNoAccessKey;
    out->supported = true;
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

bool readFleetIdentity(DeviceLink *link, FleetIdentityState *out, QString *error)
{
    if (!link || !out)
        return false;
    *out = FleetIdentityState{};
    QByteArray resp;
    quint8 code = 0;
    if (!link->requestSync(CMD_READ_FLEET_ID, QByteArray(), &resp, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code)) {
            if (error)
                error->clear();
            // A firmware that does not know the command was built without an
            // identity to report, so it belongs to no fleet. That is a fact
            // about the device, not a failure of the read — the uploader shows
            // it as an unrecognised unit rather than as a broken link.
            return true;
        }
        return false;
    }
    if (!parseFleetIdentity(resp, out)) {
        if (error)
            *error = QStringLiteral("The device returned a short fleet identity response.");
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

bool writeAccessKey(DeviceLink *link, AccessFunction fn, AccessKey key, QString *error)
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
    return sendAccessKeyWrite(link, fn, key, false, error);
}

bool clearAccessKey(DeviceLink *link, AccessFunction fn, QString *error)
{
    return sendAccessKeyWrite(link, fn, kNoAccessKey, true, error);
}

// There is deliberately no writeFleetIdentity() to match the reader. Every
// field of the identity except config_version is compiled into the firmware, so
// the wire has no command that could change one — CMD_WRITE_UPDATE_ID was
// deleted rather than left NACKing, and 0x30 is free. config_version does still
// move, and it moves where it belongs: as the optional payload of
// CMD_SAVE_TO_FLASH, so it commits with the tables it describes (see
// config_transfer.cpp).

bool proveFleetIdentity(DeviceLink *link, AccessKey expectedKey, QString *error, bool *mismatch)
{
    if (mismatch)
        *mismatch = false;
    if (!link || expectedKey == kNoAccessKey) {
        if (error)
            *error = QStringLiteral("There is no fleet key to check the device against.");
        return false;
    }

    // The one exchange that runs the other way, so the HOST supplies the nonce.
    // That is what the check is worth anything for: a look-alike replaying a
    // captured answer would have had to see these exact bytes before, and the
    // system generator is what makes that not worth attempting.
    static_assert(ACCESS_CHALLENGE_LEN % 4 == 0, "challenge must be a whole number of words");
    quint32 words[ACCESS_CHALLENGE_LEN / 4];
    QRandomGenerator::system()->fillRange(words);
    const QByteArray challenge(reinterpret_cast<const char *>(words), ACCESS_CHALLENGE_LEN);

    QByteArray reply;
    quint8 code = 0;
    if (!link->requestSync(CMD_FLEET_ID_PROVE, challenge, &reply, error,
                           DeviceLink::kDefaultTimeoutMs, DeviceLink::kDefaultRetries, &code)) {
        if (unsupported(code) && error)
            *error = QStringLiteral("This firmware cannot prove a fleet identity.");
        return false;
    }

    // The size test is not redundant with the constant-time compare: without it
    // a device that answered with nothing at all would match an expectation
    // that was also empty, and silence would read as proof.
    const QByteArray expected = accessResponse(expectedKey, challenge);
    if (expected.isEmpty() || reply.size() != expected.size()
        || !constantTimeEquals(reply, expected)) {
        if (mismatch)
            *mismatch = true;
        if (error)
            *error = QStringLiteral("The device did not prove the fleet key. It belongs to a "
                                    "different fleet, or holds a different key for this one.");
        return false;
    }
    return true;
}

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
