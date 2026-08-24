// What a device says about its access passwords, and the Protect Communication
// send gate's decision — the parts of device_session that involve no I/O.
//
// Split out of device_session.h for one reason: that header includes
// device_link.h and therefore QSerialPort, which a test binary that only wants
// to reason about a verdict has no business linking. The gate's decision is
// pure logic over values, so it belongs where a test can reach it cheaply.
#pragma once

#include "../model/access_keys.h"

namespace ct {
namespace device_session {

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

// THE PROTECT COMMUNICATION SEND GATE, as a decision rather than as dialogs.
//
// A configuration carrying Protect Communication messages goes only to a unit
// that confirms the matching Protected Comms password. That rule is enforced in
// exactly one place — MainWindow::onSendConfiguration, before anything is
// written — which made it the one protection rule with no test: the branches
// only existed inside a slot that needs a main window, a device and a user.
//
// So the ORDER and the OUTCOME live here, where a test can reach them, and the
// window keeps only the wording. The order is load-bearing and is what the test
// pins: "the device could not answer" must never be reported as "the passwords
// disagree", and a document with no key of its own must be named as such rather
// than sent to a prove that would fail for a second reason.
enum class ProtectedSendVerdict {
    Allowed,             // the unit confirmed the configuration's password
    NoDeviceAnswer,      // readAccessState failed — "I could not tell", not "it is wrong"
    DeviceHasNoPassword, // no Protected Comms password set (or firmware too old)
    DocumentHasNoKey,    // the configuration carries none of its own
    Mismatch,            // the unit holds a DIFFERENT password
    ProofFailed,         // the prove round trip itself failed
};

// `accessRead`/`state` are readAccessState's results, `documentKey` is
// Configuration::commsKey(), and `proved`/`wrongKey` are proveAccess's — passed
// in rather than fetched so the decision stays free of I/O. The caller performs
// the prove only when this would otherwise reach it: ask once with proved=false
// to clear the cheaper refusals, then again with the result.
ProtectedSendVerdict protectedSendVerdict(bool accessRead, const AccessState &state,
                                          AccessKey documentKey, bool proved, bool wrongKey);

} // namespace device_session
} // namespace ct
