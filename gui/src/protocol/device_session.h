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
#include <QString>

#include "../model/access_keys.h"
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

// v19: which of the three access passwords the device has set. Never carries
// the keys — the device does not hand those out, which is the point of them.
struct AccessState {
    bool set[kAccessFunctionCount] = {false, false, false};
    bool supported = false; // false on pre-v19 firmware
    // v17: which of the four Protected Comms slots hold a password (bit i =
    // slot i+1). 0 on firmware that predates the slots even when the single
    // password is set, so display code should fall back to isSet().
    quint8 protSlots = 0;

    bool isSet(AccessFunction fn) const { return set[int(fn)]; }
    bool any() const { return set[0] || set[1] || set[2]; }
};

// What the device says it is. The fleet key is absent by design — `keyPresent`
// says whether the device holds one, and proveFleetIdentity() is the only way to
// learn that it really does.
struct FleetIdentityState {
    FleetIdentity identity; // identity.fleetKey is always kNoAccessKey here
    bool keyPresent = false;
    bool supported = false;
};

// Parsing, deliberately separated from the I/O. Both wire formats are defined
// by the firmware, and the only way to prove this side reads them correctly is
// to feed it the bytes the firmware actually emitted — which test_firmware_link
// does, because it has the real device modules compiled in. A parser buried
// inside a function that also needs a serial port could not be tested at all.
bool parseIdentity(const QByteArray &payload, Identity *out);
bool parseAccessState(const QByteArray &payload, AccessState *out);
bool parseFleetIdentity(const QByteArray &payload, FleetIdentityState *out);

bool readIdentity(DeviceLink *link, Identity *out, QString *error);
bool readAccessState(DeviceLink *link, AccessState *out, QString *error);
bool readFleetIdentity(DeviceLink *link, FleetIdentityState *out, QString *error);

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

// There is deliberately no writeFleetIdentity(). A device's identity is
// compiled into its firmware, so the only way to change it is to build and
// flash — which is what makes it worth believing.

// Ask the device to prove it holds `expectedKey`: send a random challenge and
// check its HMAC. This is the one exchange that runs device -> host, and it is
// what turns "the device claims to be fleet X" into something worth acting on.
// `mismatch` separates a wrong answer from a link failure.
bool proveFleetIdentity(DeviceLink *link, AccessKey expectedKey, QString *error,
                        bool *mismatch = nullptr);

// Bind the configuration to a chip. An empty or all-zero uid clears the
// binding, so the configuration runs anywhere again.
bool writeBinding(DeviceLink *link, const QByteArray &uid, QString *error);

} // namespace device_session
} // namespace ct
