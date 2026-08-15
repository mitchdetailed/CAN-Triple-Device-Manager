// The authenticated-encryption primitives the .ct3s container is built from.
//
// Deliberately small and deliberately ignorant: a key derivation, a seal, an
// open, and a constant-time compare. Nothing here knows what a configuration
// is, what a password is for, or where the bytes came from. secure_file.cpp is
// the caller that matters — it derives a per-file key by its own route, hands it
// over as a ConfigKeys, and gets back an opaque blob to hide in a carrier.
//
// ---------------------------------------------------------------------------
// One primitive, and why
//
// Everything here is SHA-256: PBKDF2-HMAC-SHA256 (key derivation), HMAC-SHA256
// (integrity, and the keystream), and a constant-time compare. That is
// deliberate — the firmware has to verify an access key too, and giving it ONE
// primitive to implement is the difference between ~150 lines of well-tested C
// and a block cipher it does not otherwise need. The device never decrypts
// anything; it only ever answers "is this the right key", so it never needs the
// cipher side at all. The price on this side is speed, and it is a price worth
// paying — see sealPayload.
//
// ---------------------------------------------------------------------------
// What it does NOT do — worth stating, because a lock that is trusted further
// than it reaches is worse than no lock
//
//   * It protects BYTES AT REST and nothing else. A sealed payload is opaque on
//     disk; the moment it is opened it is an ordinary QByteArray in an ordinary
//     process, and anything that can read this app's memory can read it.
//   * It says nothing about WHO wrote a file. The tag proves the bytes have not
//     changed since someone holding macKey sealed them. It does not prove who
//     that someone was. There are no signatures here and no identities.
//   * It cannot help a container that gives the key away. A standard .ct3s
//     carries its own file key, obfuscated (see secure_file.h) — against a
//     reader of this source that is obfuscation, not secrecy, and the header
//     says so plainly. Only the password-protected mode withholds the key, and a
//     password forgotten there is unrecoverable. That is the point of it, and
//     the save dialog says so before the fact.
//   * None of this stops an ST-Link reading a device's flash directly. The
//     backstop for that is STM32G4 readout protection, a device programming
//     decision rather than a code one.
#pragma once

#include <QByteArray>
#include <QString>

namespace ct {

// PBKDF2 rounds. High enough that guessing a weak password offline costs real
// time, low enough that one open is not a visible pause. Stored in every file
// that uses it, so raising it later does not orphan existing files.
constexpr int kLockDefaultIterations = 210000;
constexpr int kLockSaltBytes = 16;
constexpr int kLockKeyBytes = 32; // each of the three derived keys
constexpr int kLockNonceBytes = 16;

// The keys one payload is sealed under. Lives only as long as the operation
// that needs it — never written to disk, never sent to the device — and clear()
// overwrites rather than merely releasing, so call it instead of letting one
// fall out of scope.
//
// `verifier` is the odd one out: it takes no part in sealing or opening. It
// exists so that a password can be CHECKED against something that could not
// decrypt anything even if it leaked. isValid() insists on it anyway, which
// means a caller deriving keys by some other route (secure_file.cpp does) must
// fill it with something of the right size — on purpose, so "I only need
// encKey" cannot quietly turn into a half-populated key set.
struct ConfigKeys {
    QByteArray encKey;   // keystream
    QByteArray macKey;   // integrity tag over the ciphertext
    QByteArray verifier; // proves a password; never decrypts anything

    bool isValid() const;
    void clear();
};

// PBKDF2-HMAC-SHA256, then split into three independent keys. Deliberately
// slow: stretching a password is the one operation here that is meant to cost
// real time.
ConfigKeys deriveKeys(const QString &password, const QByteArray &salt, int iterations);

// Authenticated encryption, encrypt-then-MAC:
//   sealed = nonce || (plain XOR keystream) || HMAC(macKey, nonce || cipher)
// The keystream is HMAC(encKey, nonce || counter) — HMAC-SHA256 used as a PRF
// in counter mode. Config files are tens of kilobytes, so the speed a block
// cipher would buy is worth less than not vendoring one.
QByteArray sealPayload(const QByteArray &plain, const ConfigKeys &keys);

// Undoes sealPayload. Verifies the tag BEFORE decrypting, so a tampered or
// truncated file is rejected rather than half-parsed. `error` is filled on
// failure and is safe to show the user.
bool openPayload(const QByteArray &sealed, const ConfigKeys &keys, QByteArray *plain,
                 QString *error);

// Length-independent equality. Used wherever a secret is compared, so a timing
// measurement cannot walk a value out byte by byte.
bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

} // namespace ct
