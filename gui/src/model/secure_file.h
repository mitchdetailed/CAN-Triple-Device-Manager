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
// This mirrors MoTeC's "Hide setup information" / "Require access password for
// use" pair on a locked comms template, for the same reasons.
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

#include "access_keys.h"

namespace ct {

constexpr int kSecureHeaderBytes = 64;
constexpr int kSecureMagicBytes = 8;
constexpr quint16 kSecureFormatVersion = 1;
constexpr quint16 kSecureFlagRequiresPassword = 0x0001;
constexpr int kSecureFileKeyBytes = 32;
constexpr int kSecureChunkBytes = 16;

// The eight bytes that identify a .ct3s. Deliberately not ASCII: a file that
// announces itself as "CT3SECURE" in a hex dump invites the next question.
extern const unsigned char kSecureMagic[kSecureMagicBytes];

// What "Save Secure Config" was asked for.
struct SecureSaveOptions
{
    // Wrap the file key under `password` as well, so the file cannot be opened
    // without it. Off by default: the common case is a config a customer must
    // be able to deploy but not read.
    bool requirePassword = false;
    // The Protected Comms password. Required when requirePassword is set;
    // otherwise unused.
    QString password;
    // The 4-byte Protected Comms key carried in the file, so this app can
    // satisfy a device's protected-comms gate on the customer's behalf without
    // them ever typing the password. kNoAccessKey when there is none.
    AccessKey embeddedCommsKey = kNoAccessKey;
    // Extra noise beyond what the payload needs, as a fraction of payload size.
    // Non-zero so two saves of the same document differ in length as well as
    // content, and a file's size says nothing about how much configuration is
    // in it.
    double noiseRatio = 0.35;
};

// What a .ct3s says about itself before it is opened.
struct SecureFileInfo
{
    bool requiresPassword = false;
    quint16 formatVersion = 0;
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
bool openSecureBlob(const QByteArray &blob, const QString &password, QByteArray *plainBody,
                    SecureFileInfo *info, QString *error = nullptr);

// Recover the body. `password` is ignored unless the file requires one, and a
// wrong one fails the payload's integrity check rather than yielding garbage —
// so `error` distinguishes "wrong password" from "damaged file" for the caller
// to say which. `plainBody` is only written on success.
bool readSecureFile(const QString &path, const QString &password, QByteArray *plainBody,
                    SecureFileInfo *info, QString *error = nullptr);

} // namespace ct
