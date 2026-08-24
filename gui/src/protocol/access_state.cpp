#include "access_state.h"

namespace ct {
namespace device_session {

ProtectedSendVerdict protectedSendVerdict(bool accessRead, const AccessState &state,
                                          AccessKey documentKey, bool proved, bool wrongKey)
{
    // "I could not tell" first, and never folded into a refusal that names a
    // password: a unit that did not answer has not disagreed with anything.
    if (!accessRead)
        return ProtectedSendVerdict::NoDeviceAnswer;
    // No password on the unit is not the same password. Ahead of the document
    // check because it is the end a user can fix on the bench in front of them,
    // and because a prove against a unit holding nothing tells them nothing.
    if (!state.supported || !state.isSet(AccessFunction::EditProtectedComms))
        return ProtectedSendVerdict::DeviceHasNoPassword;
    // Nothing to match with. Reached only once the unit is known to HAVE a
    // password, so the message can say plainly which side is missing one.
    if (documentKey == kNoAccessKey)
        return ProtectedSendVerdict::DocumentHasNoKey;
    if (!proved)
        return wrongKey ? ProtectedSendVerdict::Mismatch : ProtectedSendVerdict::ProofFailed;
    return ProtectedSendVerdict::Allowed;
}

} // namespace device_session
} // namespace ct
