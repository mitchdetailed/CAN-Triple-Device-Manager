// The .ct3s container — "File > Save Secure Config".
//
// Marking a message Read Only, Hidden or Protected stops THIS APPLICATION from
// displaying and editing it. All three tiers are conventions of this app: any
// other serial tool talking to the device defeats them, and the device itself
// enforces nothing about message protection at all. That is fine for a
// configuration you own and useless for one you ship to a customer, which is
// what a .ct3s is for.
//
// WHAT A .ct3s STILL ADDS OVER A .ct3, now that both are opaque. As of format 2
// a plain .ct3 is this same sealed container behind a readable preamble (see
// config_file.h), so "the bytes are unreadable" is no longer the difference
// between them. What remains is:
//
//   - Concealment SURVIVES the round trip. Open a .ct3s and its Hidden and
//     Protected messages stay concealed in the UI; open a .ct3 and everything
//     in it is yours to read and edit. That is the difference that matters to
//     someone shipping a configuration to a customer, and it is a property of
//     how the document is treated, not of how the file is encoded.
//   - The embedded Protected Comms key, so a customer's copy can satisfy
//     the device's protected-comms gate without them ever typing the password.
//   - The password mode below, which a .ct3 has no equivalent of at all.
//
// ---------------------------------------------------------------------------
// What it actually protects against, and what it does not
//
// Two modes, and the difference between them is the whole story:
//
//   Standard (requirePassword = false)
//       The body is encrypted, but the key that decrypts it travels inside the
//       file, obfuscated. Anyone with CAN Triple Device Manager can open the
//       file, send it to a device and use its channels; nobody can read the
//       protocol detail out of the bytes, and the protected messages stay
//       concealed in the UI. This is the shipping format: a customer can deploy
//       and update without ever seeing the CAN layout.
//
//       Be honest about the limit: this is obfuscation over encryption. It
//       defeats a hex editor, a text search, a grep for "0x640", and any tool
//       that does not implement this format. It does NOT defeat someone who
//       reads this source or disassembles the app — the key is in the file, and
//       a determined reader will find it. If that reader is in your threat
//       model, use the second mode.
//
//   Password-protected (requirePassword = true)
//       The file key is additionally wrapped under the Protected Comms
//       password, so the file cannot be opened at all without it. There is no
//       recovery: no reset, no back door, no copy held anywhere. A lost
//       password is a lost configuration, and the save dialog says so before
//       the fact.
//
// This is the classic "Hide setup information" / "Require access password for
// use" pairing on a locked comms template, for the same reasons.
//
// ---------------------------------------------------------------------------
// File layout — all multi-byte integers LITTLE-ENDIAN
//
//   Header, 64 bytes, cleartext:
//       0   u8[8]  magic          kSecureMagic
//       8   u16    formatVersion  kSecureFormatVersion
//      10   u16    flags          bit 0 = requires password
//      12   u8[16] salt           random; seeds wrapping AND chunk placement
//      28   u32    iterations     PBKDF2 rounds for the password wrap
//      32   u32    carrierLength  bytes of carrier following the header
//      36   u32    payloadLength  bytes of sealed payload hidden in the carrier
//      40   u8[24] padding        random; ignored on read
//
//   Carrier, carrierLength bytes: CSPRNG noise, divided into 16-byte slots,
//   into which the secret material is SCATTERED rather than laid down in one
//   run. In order, the material is:
//       chunks 0-1  wrapped file key   (32 bytes)
//       chunks 2+   sealed payload     (payloadLength bytes, zero-padded to a
//                                       multiple of 16 in the final chunk)
//
//   The sealed payload's PLAINTEXT is the four embedded-access-key bytes
//   followed by the body. The key is therefore encrypted and covered by the
//   payload's tag like everything else, rather than sitting in a chunk of its
//   own — which is where it started, and was a mistake twice over: the mask it
//   wore derived from the cleartext salt, so it was not really hidden, and
//   nothing authenticated it, so a flipped bit yielded a file that opened
//   cleanly and produced a silently wrong key. Nothing is lost by folding it in,
//   because the key only ever needs to be reachable when the payload is.
//   Slot order comes from a Fisher-Yates shuffle of the slot indices driven by
//   the placement keystream, so consecutive chunks land nowhere near each other
//   and unclaimed slots stay indistinguishable noise. Everything a reader needs
//   to reverse this is derived from `salt`, which is in the clear — the scatter
//   buys unreadability, not secrecy, and the encryption is what buys secrecy.
//
//   Placement keystream: HMAC-SHA256(salt, "ct3s/placement/v1" || u32be(ctr)),
//   counter from 0, consumed 4 bytes at a time as a big-endian u32.
//
//   Wrapping:
//       fileKey        32 random bytes, generated per save
//       wrapMask       HMAC-SHA256(salt, "ct3s/wrap/v1")
//                        XOR PBKDF2(password, salt, iterations, 32)
//                            — the second term only when a password is required
//       wrapped        fileKey XOR wrapMask
//       encKey/macKey  HMAC-SHA256(fileKey, "ct3s/enc/v1" / "ct3s/mac/v1")
//                      fed to sealPayload()/openPayload() from config_lock.h,
//                      so the .ct3s body uses exactly the authenticated
//                      encryption the rest of the app already relies on.
//       accessKey      4 big-endian bytes, prepended to the plaintext before
//                      sealing — no separate wrapping, because the payload's
//                      own encryption and tag already cover it.
//
// A truncated, tampered or wrong-password file fails at the payload's HMAC tag
// and is rejected whole. Nothing is ever half-parsed.
#pragma once

#include <QByteArray>
#include <QString>

#include <QJsonObject>

#include "access_keys.h"

namespace ct {

constexpr int kSecureHeaderBytes = 64;
constexpr int kSecureMagicBytes = 8;
// v2 dropped the password-protected mode. A v1 file is REFUSED rather than
// read: the mode is gone, so a file that depends on it could not be opened
// anyway, and half-reading one would mean carrying the wrapping code forever to
// service files nobody is making. Packages are rebuilt from their .ct3, which is
// where the configuration actually lives.
constexpr quint16 kSecureFormatVersion = 2;
constexpr int kSecureFileKeyBytes = 32;
constexpr int kSecureChunkBytes = 16;

// The eight bytes that identify a .ct3s. Deliberately not ASCII: a file that
// announces itself as "CT3SECURE" in a hex dump invites the next question.
extern const unsigned char kSecureMagic[kSecureMagicBytes];

// WHAT A PACKAGE DEMANDS OF THE UNIT IT IS INSTALLED ON, and what it changes
// there. Built by the Secure Configuration Builder, sealed inside the .ct3s
// alongside the configuration, and enforced by Send Secure Configuration before
// a single record is written.
//
// This is the upload policy, rebuilt on the firmware licence. The one it
// replaces compared a document's fleet block against an identity compiled into
// the firmware; that identity is gone, and a licence is a better anchor anyway
// because it can be issued and revised after the board was built.
//
// ---------------------------------------------------------------------------
// THE KEY IS NOT OPTIONAL. Every package names a Firmware Key and every target
// must prove it. The three string matches are each optional — a package may
// decline to care about the model, say — but the key always applies, so an
// unlicensed unit takes no packages at all. The provisioning order that follows
// from this is deliberate: flash the firmware, issue a licence over serial with
// the Firmware License Manager, and only then can packages be installed. There
// is no deadlock in that, because issuing a licence needs no package.
//
// The key is stored DERIVED, never as the passphrase, and it is checked by
// challenge — the host picks a nonce, the device answers under the key it
// holds, and the two answers are compared. Nothing that travels reveals it.
//
// ---------------------------------------------------------------------------
// The password updates are the other half: a package may set the device's Send,
// Get and Protected Comms passwords as it installs. That reverses an older rule
// which said access passwords must never be a side effect of a Send, and the
// reversal is deliberate for two reasons.
//
// The first is that it is the only moment it reliably WORKS. Access keys live in
// the config store's write-once header, so a password set on an already
// configured unit survives only until the next power cycle — the documented
// "set the password, THEN send a configuration" trap. An install erases and
// re-commits that header anyway, so passwords applied here land atomically with
// the configuration.
//
// The second is that the Firmware Key authorises it. A package proving the key
// holds the manufacturer's secret, and the device lets it overwrite passwords it
// does not know (see license_store.h). Without that a package could only ever
// provision a blank unit.
//
// Each field is OPTIONAL and empty means "leave this one alone", which is what
// the Builder's checkboxes select. A field that is present but empty means
// REMOVE that password.
struct SecurePackagePolicy
{
    // Optional string matches against the device's licence. Empty = not checked.
    QString matchManufacturer;
    QString matchModel;
    QString matchVersion;

    // The revision this package stamps on the unit when it installs, read back
    // by Device Status. 0 means unversioned. Lives here because a PACKAGE is
    // the deployable revision — it used to be a document field, edited in a
    // dialog that no longer exists, and a bench Send has no business
    // renumbering a unit.
    quint16 configVersion = 0;

    // The derived Firmware Key, kLicenseKeyBytes long. MANDATORY: a policy
    // whose key is missing or the wrong length is not installable, and
    // isValid() is what every writer and reader checks.
    QByteArray key;

    // Device passwords to apply on install, AS DERIVED KEYS — never as the
    // passphrases. The device only ever needs the 4-byte PBKDF2 key, so that is
    // all a package carries; the phrase somebody typed into the Builder is
    // derived on the spot and discarded. This matters because the .ct3s key
    // travels in the file obfuscated: anyone reading this source can open any
    // package, and a package holding typed passwords would hand them every
    // passphrase a manufacturer ever used, reuse and all. A derived key opens
    // one function on one fleet and nothing else. Found in review.
    //
    // set* = false leaves that password alone. set* = true with kNoAccessKey
    // CLEARS it — safe as a sentinel because deriveAccessKey() never folds a
    // real phrase to zero. Send and Get are one each; the four Protected Comms
    // slots are separate because the device keys them that way.
    bool setSend = false;
    AccessKey sendKey = kNoAccessKey;
    bool setGet = false;
    AccessKey getKey = kNoAccessKey;
    bool setCommsSlot[4] = {false, false, false, false};
    AccessKey commsSlotKey[4] = {kNoAccessKey, kNoAccessKey, kNoAccessKey, kNoAccessKey};

    bool isValid() const { return key.size() == kLicenseKeyBytes; }
    bool changesPasswords() const
    {
        return setSend || setGet || setCommsSlot[0] || setCommsSlot[1] || setCommsSlot[2]
               || setCommsSlot[3];
    }

    QJsonObject toJson() const;
    static SecurePackagePolicy fromJson(const QJsonObject &o);
};

// THE INSTALL VERDICT, as a decision rather than as dialogs.
//
// Everything Send Secure Configuration can decide WITHOUT a round trip lives
// here, where a test can reach it. It used to live inline in the window, which
// made it the one gate in the install path with no test — the same shape that
// let the licence-key oracle ship, and the same shape protectedSendVerdict was
// extracted to fix. The window keeps only the wording.
//
// What this does NOT cover is the key proof, which is a round trip and the
// caller's next step — taken only when this verdict is ok(). The order is
// load-bearing: a package refused here never touches the device at all.
struct InstallMismatch
{
    QString field;  // "manufacturer", "model" or "version" — a key for the UI to word
    QString wanted; // what the package demands
    QString actual; // what the device reports
};

struct InstallVerdict
{
    // Either an older .ct3s or one whose sealed policy would not parse. Fatal on
    // its own: "this package makes no demands" is not a thing a package may be.
    bool noPolicy = false;
    // The unit's firmware predates licensing and cannot be matched. Fatal on its
    // own, and deliberately distinct from a mismatch: only one of them is fixed
    // by updating the firmware.
    bool deviceUnlicensed = false;
    // Every optional match that was asked for and did not hold — ALL of them,
    // not the first, so the person holding the laptop can see whether they have
    // the wrong file or the wrong unit in one reading.
    QList<InstallMismatch> mismatches;

    bool ok() const { return !noPolicy && !deviceUnlicensed && mismatches.isEmpty(); }
};

// `deviceSupported` is LicenseState::supported; the three strings are the
// device's licence fields (empty when it holds none). Passed as values rather
// than as a LicenseState because that type lives beside the serial link, and a
// decision over strings should not drag QSerialPort into the test that pins it.
InstallVerdict packageInstallVerdict(const SecurePackagePolicy &policy, bool deviceSupported,
                                     const QString &deviceManufacturer,
                                     const QString &deviceModel, const QString &deviceVersion);

// What "Save Secure Config" was asked for.
struct SecureSaveOptions
{
    // The 4-byte Protected Comms key carried in the file, so this app can
    // satisfy a device's protected-comms gate on the customer's behalf without
    // them ever typing the password. kNoAccessKey when there is none.
    AccessKey embeddedCommsKey = kNoAccessKey;
    // The package policy, sealed inside the file with the configuration. An
    // invalid one (no key) writes no policy at all, which is what every caller
    // that is not the Builder does — a plain .ct3 has no policy and neither does
    // a comms template.
    SecurePackagePolicy policy;
    // Extra noise beyond what the payload needs, as a fraction of payload size.
    // Non-zero so two saves of the same document differ in length as well as
    // content, and a file's size says nothing about how much configuration is
    // in it.
    double noiseRatio = 0.35;
};

// What a .ct3s says about itself before it is opened.
struct SecureFileInfo
{
    quint16 formatVersion = 0;
    // Only filled by readSecureFile, like the embedded key below: the policy is
    // sealed, so peeking cannot reach it. That is the point — a package's
    // demands are not readable off a file lying on a disk.
    SecurePackagePolicy policy;
    // Only filled by readSecureFile — peeking cannot reach it, because it lives
    // in the carrier rather than the header.
    AccessKey embeddedCommsKey = kNoAccessKey;
};

// True when `path` starts with kSecureMagic. Cheap; used to route Open between
// the JSON and binary readers, and to stop a .ct3s being parsed as JSON.
bool isSecureFile(const QString &path);

// Read the header only. Enough to know whether to ask for a password before
// disturbing the open document. Returns false only when the file is unreadable
// or is not a .ct3s.
bool peekSecureFile(const QString &path, SecureFileInfo *out, QString *error = nullptr);

// Write `plainBody` (the compact JSON body a .ct3 would carry) as a .ct3s.
bool writeSecureFile(const QString &path, const QByteArray &plainBody,
                     const SecureSaveOptions &options, QString *error = nullptr);

// THE CONTAINER WITHOUT THE FILE. Everything above and below works on a path,
// because a .ct3s IS the container and starts at byte zero. A .ct3 puts the
// same bytes after a readable preamble, so it needs the container as a buffer
// rather than as a file — and the one thing that must not happen is a second
// implementation of the sealing, drifting against this one.
//
// writeSecureFile and readSecureFile are thin wrappers over these two.
bool sealSecureBlob(const QByteArray &plainBody, const SecureSaveOptions &options,
                    QByteArray *blobOut, QString *error = nullptr);
bool openSecureBlob(const QByteArray &blob, QByteArray *plainBody,
                    SecureFileInfo *info, QString *error = nullptr);

// Recover the body. `password` is ignored unless the file requires one, and a
// wrong one fails the payload's integrity check rather than yielding garbage —
// so `error` distinguishes "wrong password" from "damaged file" for the caller
// to say which. `plainBody` is only written on success.
bool readSecureFile(const QString &path, QByteArray *plainBody,
                    SecureFileInfo *info, QString *error = nullptr);

} // namespace ct
