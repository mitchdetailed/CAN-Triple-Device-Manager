// Talking to a device about its identity and its configuration password.
//
// Everything here is synchronous (DeviceLink::requestSync, a nested event
// loop). That is deliberate: every caller is a menu action with a modal dialog
// in front of it, and a handful of short round trips before Send or Get is not
// worth the state machine an async version would need.
//
// Every call degrades cleanly on older firmware: the device NACKs with
// ERR_INVALID_CMD, and these report `supported = false` rather than an error.
// An old device simply has no access passwords, no fleet identity and no
// binding, which is the truth about it.
#pragma once

#include <QByteArray>
#include <QDate>
#include <QString>

#include "../model/access_keys.h"
#include "access_state.h"
#include "device_link.h"

namespace ct {
namespace device_session {

// Who the device is, and why its stored configuration is or is not running.
struct Identity {
    QByteArray uid;                            // CONFIG_UID_LEN bytes; empty if unsupported
    quint8 configStatus = CONFIG_STATUS_NONE;
    bool supported = false;                    // false on pre-v18 firmware

    bool boundElsewhere() const { return supported && configStatus == CONFIG_STATUS_WRONG_DEVICE; }
    // Printable form of the 96-bit ID, most significant byte first, for the
    // Device Status dialog and for telling two units apart.
    QString uidText() const;
};

// The configuration version the unit is running — the revision the package
// that installed it stamped into the flash header. Once part of the fleet
// identity reply; the one field of it that anybody still asks for.
struct ConfigVersionState {
    quint16 version = 0;
    bool supported = false; // false on firmware without CMD_READ_CONFIG_VERSION
};

// The firmware licence, as much of it as a host is ever allowed to see.
//
// THERE IS NO KEY MEMBER, AND THAT IS THE DESIGN. The device does not disclose
// the key in any form — not the passphrase, not the derivation, not a hash. It
// can only be written, and afterwards proved by challenge. `keySet` is the
// single bit a host learns about it: whether there is one.
//
// Contrast DeviceInfo below, which is the HARDWARE record and is entirely
// readable. The two answer different questions on purpose: what this board IS
// (burned once, immutable, in OTP) versus who this FIRMWARE is licensed to
// (writable, revisable, in its own flash page).
struct LicenseState {
    QString manufacturer;
    QString model;
    QString firmwareVersion;
    // The two secrets, as the two bits a host is allowed to know about them.
    //
    //   keySet     — the Firmware Key: this unit can PROVE which licence it
    //                holds. It authorises nothing.
    //   updaterSet — the FW Updater Password: writing this record REQUIRES it.
    //                This is the one an operator gets prompted for.
    //
    // Kept apart because they answer different questions, and a dialog that
    // collapsed them would have to lie about one of them.
    bool keySet = false;
    bool updaterSet = false;
    bool supported = false; // false on firmware without CMD_READ_LICENSE

    // Nothing has been issued. Distinct from !supported, which is "this
    // firmware cannot be asked" — a caller has to tell an unlicensed unit from
    // one that predates licensing, because only one of them can be fixed here.
    bool blank() const
    {
        return manufacturer.isEmpty() && model.isEmpty() && firmwareVersion.isEmpty() && !keySet
               && !updaterSet;
    }
};

// One write. A struct rather than six parameters because four of them are
// optional-with-a-default and a call site reading writeLicense(link, a, b, {},
// false, {}, true, &err) is one nobody can check by eye.
//
// An empty secret means KEEP what the device holds — the ordinary case, since
// editing a model name must not disturb either password. Clearing has to be
// asked for explicitly.
struct LicenseWrite {
    QString manufacturer;
    QString model;
    QString firmwareVersion;
    QByteArray key;        // empty = keep the Firmware Key
    bool clearKey = false;
    QByteArray updaterKey; // empty = keep the FW Updater Password
    bool clearUpdater = false;
};

// The manufacturing record burned into the device's OTP area — who built this
// board, what it is, and when. See the layout note beside CMD_GET_DEVICE_INFO in
// protocol.h; the short version is 88 bytes of ASCII and one big-endian u64,
// burned once at manufacture and physically unable to change afterwards.
//
// WHY EVERY FIELD IS OPTIONAL, AND SEPARATELY SO. Each field occupies a whole
// number of double-words, and OTP is programmed a double-word at a time, so a
// part with a manufacturer and no serial number is a normal thing to meet — it
// is what a board looks like between two stages of provisioning. A struct that
// could only say "burned" or "not burned" would have to call that part one or
// the other, and both would be wrong.
//
// An empty QString and serialKnown == false mean UNKNOWN, never "blank". The
// parser maps both 0x00 and 0xFF to unknown: 0xFF is virgin OTP on a part nobody
// has burned, 0x00 is a field a programmer zeroed instead of filling. Neither is
// a value, and rendering them as "" or "0" would present a non-answer as one.
struct DeviceInfo {
    QString manufacturer;    // empty = unknown
    QString product;         // empty = unknown
    QString hardwareVersion; // empty = unknown
    quint64 serialNumber = 0;
    bool serialKnown = false;
    // The date field twice over, because the two say different things. `dateText`
    // is the eight characters as burned and is what a support call should quote;
    // `date` is those characters read as DDMMYYYY and is invalid when they are
    // not a real date. Keeping both means a mis-burned date is shown as what it
    // actually says rather than silently corrected to a plausible day, and a
    // reader can tell the two cases apart.
    QString dateText; // empty = unknown
    QDate date;       // invalid = present but not a readable DDMMYYYY

    QByteArray raw;         // the OTP_INFO_LEN bytes exactly as they came back
    bool supported = false; // false on firmware without CMD_GET_DEVICE_INFO

    // Nothing at all was burned. Worth its own question because the answer is a
    // sentence rather than five "Unknown" rows: this is an unprovisioned part,
    // not a part whose record failed to read.
    bool blank() const
    {
        return manufacturer.isEmpty() && product.isEmpty() && hardwareVersion.isEmpty()
               && !serialKnown && dateText.isEmpty();
    }
};

// Parsing, deliberately separated from the I/O. Both wire formats are defined
// by the firmware, and the only way to prove this side reads them correctly is
// to feed it the bytes the firmware actually emitted — which test_firmware_link
// does, because it has the real device modules compiled in. A parser buried
// inside a function that also needs a serial port could not be tested at all.
bool parseIdentity(const QByteArray &payload, Identity *out);
bool parseAccessState(const QByteArray &payload, AccessState *out);
bool parseConfigVersion(const QByteArray &payload, ConfigVersionState *out);
bool parseDeviceInfo(const QByteArray &payload, DeviceInfo *out);
bool parseLicense(const QByteArray &payload, LicenseState *out);

bool readIdentity(DeviceLink *link, Identity *out, QString *error);
bool readAccessState(DeviceLink *link, AccessState *out, QString *error);
bool readConfigVersion(DeviceLink *link, ConfigVersionState *out, QString *error);
bool readDeviceInfo(DeviceLink *link, DeviceInfo *out, QString *error);
bool readLicense(DeviceLink *link, LicenseState *out, QString *error);

// Prove a licence secret TO the device: ask for a nonce, answer
// HMAC(secret, auth-label || nonce). The device tries the answer against BOTH
// its secrets and grants by which one matched — the FW Updater Password buys
// licence writes, the Firmware Key buys those plus the master grant over the
// access passwords. The caller does not learn which; it finds out by trying.
//
// Named for what it does rather than for one of its two inputs: an earlier
// name said "updater", and the Send Secure Configuration path passing it the
// Firmware Key read as a mistake when it was the whole point.
//
// Must precede writeLicense() on a unit with an FW Updater Password — the
// device refuses otherwise. `wrongSecret` separates "the device said no" from a
// link failure, so a caller can re-prompt instead of giving up.
bool proveLicenseSecret(DeviceLink *link, const QByteArray &secret, QString *error,
                        bool *wrongSecret = nullptr);

// Ask the device to prove its FIRMWARE KEY. The reverse direction: the host
// picks the nonce, the device answers under the key it stores, and the caller
// checks that answer against its own expectation. Nothing in this application
// consumes it yet — it exists because a secret with no way to check it is a
// secret nobody can trust, and because the upload policy will want it.
//
// `mismatch` separates a wrong answer from a link failure.
bool proveLicenseKey(DeviceLink *link, const QByteArray &expectedKey, QString *error,
                     bool *mismatch = nullptr);

// Write the licence.
//
// SEND THIS WITH THE LONG FLASH TIMEOUT. The device erases and reprograms a page
// in the bank it executes from, so it stalls for tens of milliseconds and
// answers late — the same reason CMD_FW_UPDATE_BEGIN needs it.
bool writeLicense(DeviceLink *link, const LicenseWrite &write, QString *error);

// Prove one function's password: ask for a challenge, answer
// HMAC(key, challenge). `wrongPassword` distinguishes "the device said no" from
// a link failure, so a caller can re-prompt instead of giving up.
//
// Proving is per function on purpose. A session that has shown it may Send has
// shown nothing about whether it may Get, and collapsing the two would make the
// weakest password the only one that mattered.
bool proveAccess(DeviceLink *link, AccessFunction fn, AccessKey key, QString *error,
                 bool *wrongPassword = nullptr);

// There is deliberately no proveMessageAccess(). The v20 per-message key it
// proved does not exist any more: 2.3.0 retired the field (it is `reserved[4]`
// now) and deleted the CMD_MSG_ACCESS_RESPONSE handler with it, so the opcode
// NACKs ERR_INVALID_CMD. proveAccess(EditProtectedComms) is the whole proof
// mechanism message protection needs, and the only thing it authorises is
// LOWERING a section's tier in the document — the device itself no longer
// enforces anything about protected messages.

// Set or clear one function's password on the device. Clearing and changing
// both require the current one to have been proved first — otherwise "set a new
// password" would be the way past not knowing the old.
// v17: `slot` is the Protected Comms slot (1..4) and is ignored for Send and
// Get, which have exactly one. Existing callers mean slot 1 and say nothing.
bool writeAccessKey(DeviceLink *link, AccessFunction fn, AccessKey key, QString *error,
                    int slot = 1);
bool clearAccessKey(DeviceLink *link, AccessFunction fn, QString *error, int slot = 1);

// Bind the configuration to a chip. An empty or all-zero uid clears the
// binding, so the configuration runs anywhere again.
bool writeBinding(DeviceLink *link, const QByteArray &uid, QString *error);

// There is deliberately no writeDeviceInfo(). OTP has no erase: a double-word
// spent is spent for the life of the part, so a wire command able to write one
// would be a way to destroy a field permanently by accident — over a serial
// cable, on a customer's unit, with no way back. Burning the record is a
// manufacturing step performed once by a separate tool.

} // namespace device_session
} // namespace ct
