// Access passwords — the three function locks behind "Online > Set Access
// Passwords", modelled on the same screen in MoTeC Dash Manager.
//
//   Send a Configuration   — the device refuses a Send without it.
//   Get a Configuration    — the device refuses a Get without it.
//   Edit Protected Comms   — reveals and edits messages marked "Protect
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

constexpr int kFleetVendorIdBytes = 16;
constexpr int kFleetModelIdBytes = 16;

// Who a unit is. Mirrors the firmware's FleetIdentityPublic; see protocol.h.
//
// Five of these six fields are COMPILED INTO the firmware
// (firmware/include/fleet_identity.h) and cannot be changed over the wire —
// re-badging a unit means building and flashing it. Only configVersion is
// runtime state, because it has to move every time a configuration is released.
//
// fleetKey is the one secret and is never read back off a device: a device
// PROVES it by answering a challenge.
//
// The two strings are at most 16 BYTES each, not 16 QChars — they cross the wire
// as fixed 16-byte NUL-padded fields, so a name with non-ASCII characters in it
// runs out of room sooner than its length suggests. clampToWire() is what every
// writer must use rather than QString::left().
struct FleetIdentity
{
    QString vendorId;                   // <= kFleetVendorIdBytes UTF-8 bytes
    QString modelId;                    // <= kFleetModelIdBytes UTF-8 bytes
    quint32 serialNumber = 0;
    quint16 configVersion = 0;
    quint16 flags = 0;
    AccessKey fleetKey = kNoAccessKey; // 0 = none set

    // An identity that names nothing matches nothing, and is what an
    // unprovisioned unit reports. Note serialNumber is NOT part of this: a
    // serial on its own says nothing about which fleet a unit belongs to.
    bool isSet() const { return !vendorId.isEmpty() || !modelId.isEmpty(); }
    // Same fleet? Vendor and model compare exactly — they are identifiers, not
    // display names, so case and spacing matter.
    // Version and serial are deliberately NOT compared — those are the ordering
    // and targeting questions, asked separately.
    bool sameFleetAs(const FleetIdentity &other) const;

    // Truncate to what the wire can carry, on a UTF-8 byte boundary so a
    // multi-byte character is never cut in half.
    static QString clampToWire(const QString &text, int maxBytes);

    // `includeFleetKey` is explicit at every call site on purpose. A .ct3s
    // body is encrypted and may carry the key, so the app can verify a device's
    // attestation without an operator typing it. A plain .ct3 is legible text
    // and must NEVER carry it — passing true there would put the fleet secret
    // in a file people mail around.
    QJsonObject toJson(bool includeFleetKey) const;
    static FleetIdentity fromJson(const QJsonObject &o);
};

// What a configuration demands of a device before it will install on it. This
// is the uploader's rulebook, and it travels inside the configuration so a
// .ct3s handed to a customer carries its own restrictions.
//
// Vendor and model are matched from the configuration's own FleetIdentity —
// there is no way to require a vendor other than the one the config is for, and
// making that separately settable would only create a way to get it wrong. What
// IS separately settable is how narrowly the serial is pinned.
struct UploadPolicy
{
    // Empty = any serial in the fleet. Otherwise the device's serial must be
    // one of these, which is how a one-off replacement config is stopped from
    // installing on the rest of a customer's cars.
    QList<quint32> allowedSerials;
    // Require the device to PROVE the fleet key rather than merely claim the
    // identity. On by default: without it the whole block is four strings a
    // look-alike can echo back.
    bool requireFleetKey = true;
    // WARN when the device is already running a NEWER revision than the package
    // — i.e. this install would move it backwards. On by default.
    //
    // A warning, not a refusal, and the distinction is deliberate. Reinstalling
    // the same revision is routine (a unit was replaced, a config was reloaded
    // after a clear), and deliberately going back to an older one is a normal
    // thing to do when the newer one turned out worse. Neither is an error, so
    // neither blocks. Only "you are about to go backwards" is worth saying, and
    // saying it once is enough.
    bool warnOnOlderVersion = true;

    bool pinsSerial() const { return !allowedSerials.isEmpty(); }
    bool allowsSerial(quint32 serial) const;

    QJsonObject toJson() const;
    static UploadPolicy fromJson(const QJsonObject &o);
};

} // namespace ct
