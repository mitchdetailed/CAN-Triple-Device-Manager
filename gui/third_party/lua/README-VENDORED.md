# Lua 5.4.8 — vendored verbatim

Source: the official Lua mirror, tag `v5.4.8`
(https://github.com/lua/lua/archive/refs/tags/v5.4.8.tar.gz,
sha256 `d85b70a65f43c5d2254944d58d625e822c8e2e10d9c6a3bd9b5b657e46376a19`).
Every `.c` and `.h` from the tag, unmodified. License: MIT — the full text is
at the end of `lua.h`, and it is reproduced in
`installer/THIRD-PARTY-NOTICES.txt`, which ships with every binary.

## Why vendored, and why this way

The configurator embeds Lua twice over: as the user-facing scripting engine
(Tools → Lua Console) and, later, as the parser front end that compiles device
scripts to bytecode (docs/SCRIPTING-PLAN.md in the firmware repo). A build
must not depend on a network fetch or a system package — this project's builds
are self-contained by policy — and Lua is ANSI C designed for exactly this
kind of embedding.

Only **onelua.c** is compiled (with `MAKE_LIB` defined). It is upstream's own
single-translation-unit amalgamation and `#include`s the individual `.c`
files, which is why they must all be present even though the build lists one
file. `lua.c` (the standalone interpreter), `ltests.c` (debug instrumentation)
and `onelua.c`'s other MAKE_* modes are not used.

## Upgrading

Replace every file with the new tag's, update the sha256 above and the version
in THIRD-PARTY-NOTICES.txt, and re-run the sandbox tests in test_lua — they
pin the API surface the scripting layer depends on (which stdlib tables
exist, that the instruction hook fires, that loading binary chunks is
refused). Do not patch files in here; anything CAN Triple needs different is
done from the embedding side in src/scripting/.
