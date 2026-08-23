// The `ct` table: what a Lua script can see and change in the open document.
//
// Design rules, applied uniformly:
//
//   - Everything is BY NAME, never by index. Channels, sections and constants
//     are addressed the way the user thinks of them; indices shift when rows
//     are removed and a script that held one would corrupt something else.
//     (Math rows are the exception — they have no name — and are 1-based
//     indexed with that caveat documented.)
//   - Reads return plain Lua tables, snapshots by value. There are no live
//     handles: a handle invalidated by a later removal is a crash, a snapshot
//     is merely stale, and stale is the better failure for a batch tool.
//   - Writes go through add/set/remove functions that validate before they
//     touch the model and raise Lua errors with the reason. A script never
//     half-applies a single call.
//   - Protected communications behave exactly as they do in the UI: a
//     concealed section lists as {name, protected=true, concealed=true} and
//     nothing more, its detail is an error to request, and every write to it
//     or its channels is refused. The rules live in ONE place (guardSection /
//     guardChannel here) for the same reason CommsSection::displayDetail does.
//
// The surface (phase 1):
//   title/setTitle, comments/setComments
//   channels/channel/addChannel/setChannel/removeChannel/renameChannel
//   allocatedChannels/generatedChannels/protectedChannels
//   bus/setBus
//   sections/section/addSection/setSection/removeSection
//   constants/addConstant/removeConstant
//   mathRows/addMath/removeMath, ops (name -> op number)
//   conditions/counters/timers/integrators/tables2x16/tables8x8  (read-only)
//   validate
#pragma once

struct lua_State;

namespace ct {

class Configuration;

struct BindingContext {
    Configuration *config = nullptr;
    // Set by every successful write. The runner uses it to decide whether the
    // document is dirty and whether an error needs a rollback.
    bool mutated = false;
};

// Install the `ct` global into `L`. `context` must outlive the state.
void installConfigBindings(lua_State *L, BindingContext *context);

} // namespace ct
