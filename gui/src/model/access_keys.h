// Access passwords — the three function locks behind "Online > Set Access
// Passwords".
//
//   Send a Configuration   — the device refuses a Send without it.
//   Get a Configuration    — the device refuses a Get without it.
//   Protected Comms   — reveals and edits messages marked "Protect
//                            Communication", both in this app and on the device.
//
// Each is independent: holding one proves nothing about the others, and a
// session that has earned the right to Send has not thereby earned the right to
// Get.
//
// ---------------------------------------------------------------------------
// Two derivations from one password, and why there are two
//
// A password turns into two unrelated things:
//
//   deriveAccessKey()  -> a 4-byte key, PBKDF2 over a FIXED application salt.
//        This is what the hardware stores and compares. It has to be fixed-salt
//        because the same password must produce the same key on every unit in a
//        fleet — that is what lets one .ct3s update a hundred devices. It is
//        also what the device proves by challenge-response.
//
//   AccessVerifier     -> a 32-byte verifier, PBKDF2 over a RANDOM per-file
//        salt. This is what a .ct3/.ct3s stores so the app can check a typed
//        password offline. It deliberately cannot be turned into the 4-byte
//        key, so a configuration file lying around does not hand over the key
//        that opens the hardware.
//
// A file therefore leaks nothing usable, and the device holds the only copy of
// the thing that matters.
//
// ---------------------------------------------------------------------------
// What four bytes is worth — stated plainly, because a lock trusted further
// than it reaches is worse than no lock
//
//   * Against password GUESSING it is strong. kAccessKeyIterations rounds of
//     PBKDF2 make each candidate cost real time, and the device answers one
//     guess per serial round trip.
//   * Against a FLASH DUMP it is worth nothing: the key is right there, and the
//     attacker never needs the password. The backstop is STM32G4 readout
//     protection, a device programming decision rather than a code one.
//   * The key space is 2^32. Unreachable online; reachable offline by someone
//     who captures a challenge/response pair and is willing to spend the
//     compute. Four bytes is what the hardware compares, so this is the floor
//     the design sits on, not an oversight in it.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace ct {

// The three protected functions, in the order the dialog lists them. The
// numeric values are the wire's ACCESS_FN_* and are part of the protocol.
enum class AccessFunction { SendConfiguration = 0, GetConfiguration = 1, EditProtectedComms = 2 };

constexpr int kAccessFunctionCount = 3;
constexpr int kAccessKeyBytes = 4;
constexpr int kAccessChallengeBytes = 16;
constexpr int kAccessVerifierSaltBytes = 16;
constexpr int kAccessVerifierBytes = 32;

// PBKDF2 rounds. Matched to the file lock's, and stored alongside every
// verifier so raising it later does not orphan existing files. The 4-byte key
// derivation uses the same count but cannot store it — the device has no room
// and no need — so changing kAccessKeyIterations would invalidate every key
// already programmed into hardware. It is therefore fixed for the life of the
// protocol, which is why it is a separate constant rather than a shared one.
constexpr int kAccessKeyIterations = 210000;
constexpr int kAccessVerifierIterations = 210000;

// A CEILING on any PBKDF2 round count read from a file. The count is stored in
// the .ct3 so a future increase does not orphan old files (see above), which
// means a hand-edited file can name any count it likes — and PBKDF2 will
// faithfully grind for all of them. A value like 2,000,000,000 would freeze the
// UI for the better part of an hour at the password prompt: a denial of service
// out of a plain text edit. Real files use 210,000; anything past this is
// treated as a malformed lock (which reads as "wrong password", never as "no
// lock"). Matches the secure-file writer's own limit.
constexpr int kMaxKdfIterations = 4000000;

// Every function, for iterating the dialog and the JSON.
const AccessFunction *allAccessFunctions(); // kAccessFunctionCount entries

// "Send a Configuration" — what the user reads in the list.
QString accessFunctionLabel(AccessFunction fn);
// One sentence on what withholding this password actually prevents.
QString accessFunctionDescription(AccessFunction fn);
// Stable JSON / settings key, e.g. "sendConfiguration".
QString accessFunctionKey(AccessFunction fn);

// The 4-byte key the hardware holds. Zero is reserved to mean "no password", so
// a derivation that lands on zero is nudged off it — the alternative is a
// password that silently reads as no password at all.
using AccessKey = quint32;
constexpr AccessKey kNoAccessKey = 0;

// The shortest string this app accepts as a password. Four is not a security
// claim — the resistance here comes from kAccessKeyIterations and the four-byte
// key space, not from length — it is a floor under the passwords that are
// obviously accidents: one stray keystroke, or an initial typed before the
// person decided what to use.
constexpr int kMinPasswordLength = 4;

// Whether a password may be used. EMPTY IS ALWAYS ALLOWED and means "no
// password": every caller spells "remove this lock" as a blank field, and
// rejecting it would leave a lock that can be set but never cleared. Anything
// else must reach kMinPasswordLength.
//
// Returns the reason to show the user, empty when the password is fine, so no
// call site has to invent its own wording and the rule cannot drift between the
// half-dozen places a password is typed.
QString passwordProblem(const QString &password);

// PBKDF2-HMAC-SHA256(password, fixed salt, kAccessKeyIterations) folded to four
// bytes. Deliberately slow; call it once and keep the result for the session.
// An empty password yields kNoAccessKey.
AccessKey deriveAccessKey(const QString &password);

// Big-endian, so the same four bytes appear in the same order in the firmware,
// in a hex dump and in this app.
QByteArray accessKeyBytes(AccessKey key);
AccessKey accessKeyFromBytes(const QByteArray &bytes); // kNoAccessKey if not 4 bytes

// The answer to a device challenge: HMAC-SHA256(key bytes, challenge). Empty
// when the key is unset or the challenge is the wrong size, so a caller cannot
// accidentally send a well-formed response for no key.
QByteArray accessResponse(AccessKey key, const QByteArray &challenge);

// ---------------------------------------------------------------------------
// The firmware licence key
//
// The same PBKDF2 construction as an access key, and deliberately WIDER: 16
// bytes rather than 4. The access key is four bytes because that is what the
// device's flash header can spare, and access_keys.h says plainly that four
// bytes is the floor the design sits on. The licence lives in its own page with
// two kilobytes to spend, so there was no reason to inherit the constraint —
// and a licence key is the one secret here that gates re-badging a unit.
//
// A DIFFERENT SALT from the access keys, so the same passphrase typed in both
// places produces unrelated keys. Sharing a salt would mean a licence key
// recovered from a dumped page also opened the Send password on every unit
// where somebody reused the phrase.
//
// Fixed salt for the same reason the access key's is fixed: one passphrase must
// produce one key on every machine, or a licence issued in one office cannot be
// applied from another.
constexpr int kLicenseKeyBytes = 16;

// Empty passphrase -> empty result, which every caller reads as "no key". Never
// a well-formed key, so clearing cannot accidentally set.
QByteArray deriveLicenseKey(const QString &passphrase);

// The two directions a licence secret is used in, and they are NOT the same
// computation. Each MACs its own label ahead of the nonce:
//
//   licenseAuthResponse   host proves a secret TO the device (CMD_LICENSE_RESPONSE)
//   licenseProveExpected  what the device must answer TO the host (CMD_LICENSE_KEY_PROVE)
//
// Kept separate because without the labels the device was a signing oracle for
// its own challenge — it would answer HMAC(key, N) for any host-chosen N,
// including the N it had just issued. The labels must match the firmware's
// LICENSE_AUTH_LABEL / LICENSE_PROVE_LABEL byte for byte; test_firmware_link
// asserts that they do.
//
// Empty on a bad key or a malformed challenge, so a caller holding nothing
// cannot put a plausible answer on the wire.
constexpr char kLicenseAuthLabel[] = "CT3/license/auth/v1";
constexpr char kLicenseProveLabel[] = "CT3/license/prove/v1";
QByteArray licenseAuthResponse(const QByteArray &key, const QByteArray &challenge);
QByteArray licenseProveExpected(const QByteArray &key, const QByteArray &challenge);

// Which keys a session is holding. Lives in memory only — never written to a
// file, never persisted between runs.
struct AccessKeySet
{
    AccessKey keys[kAccessFunctionCount] = {kNoAccessKey, kNoAccessKey, kNoAccessKey};

    AccessKey key(AccessFunction fn) const;
    void setKey(AccessFunction fn, AccessKey key);
    bool isSet(AccessFunction fn) const;
    bool any() const;
    // Bit per function, matching the wire's ACCESS_MASK_*.
    quint8 mask() const;
    void clear();
};

// What a FILE stores about one access password: enough to check a typed
// password, never enough to reach the 4-byte key. An unset verifier is the
// default and verifies nothing.
struct AccessVerifier
{
    QByteArray salt;     // kAccessVerifierSaltBytes
    int iterations = kAccessVerifierIterations;
    QByteArray verifier; // kAccessVerifierBytes

    bool isSet() const { return !verifier.isEmpty(); }
    // Structurally usable: right-sized salt and verifier, sane iteration count.
    bool isValid() const;
    // Constant-time. False for an unset or malformed verifier, so "no password"
    // can never be mistaken for "correct password".
    bool verify(const QString &password) const;

    static AccessVerifier make(const QString &password); // fresh random salt

    QJsonObject toJson() const;
    static AccessVerifier fromJson(const QJsonObject &o);
};

// The verifiers a document carries. Only EditProtectedComms is meaningful in a
// file today — Send and Get are device-side gates and a file has no business
// knowing them — but all three are stored so a future offline check does not
// need a file format change.
struct AccessVerifierSet
{
    AccessVerifier verifiers[kAccessFunctionCount];

    const AccessVerifier &verifier(AccessFunction fn) const;
    void setVerifier(AccessFunction fn, const AccessVerifier &v);
    bool isSet(AccessFunction fn) const;
    bool any() const;
    void clear();

    QJsonObject toJson() const;
    static AccessVerifierSet fromJson(const QJsonObject &o);
};

// FleetIdentity is GONE — vendor, model, serial, a config version and a key,
// held by a configuration to say which hardware it was for. Its device-side
// half was compiled into the firmware and was replaced by the writable licence
// (device_session.h, LicenseState) and the OTP hardware record (DeviceInfo);
// its file-side half was replaced by SecurePackagePolicy (secure_file.h),
// sealed into the .ct3s rather than written into a plain .ct3. The
// configuration version outlived it and now belongs to the PACKAGE, where a
// release actually happens. Old files keep their "fleetIdentity" object and it
// is simply never read.

// What a configuration demands of a device before it will install on it. This
// is the uploader's rulebook, and it travels inside the configuration so a
// .ct3s handed to a customer carries its own restrictions.
//
// UploadPolicy is GONE. It named which devices a .ct3 could be installed on — a
// serial allow-list, a require-the-fleet-key flag, a downgrade warning — and
// every one of those was read by Upload Configuration, which no longer exists.
//
// The same job is done by SecurePackagePolicy (secure_file.h), and better: it
// lives sealed inside the .ct3s rather than in a plain .ct3 anyone could edit,
// it matches against the writable firmware licence rather than an identity
// compiled into the binary, and its key is PROVED by challenge rather than
// claimed. Files written before the removal keep their "uploadPolicy" object
// and it is simply never read.

} // namespace ct
