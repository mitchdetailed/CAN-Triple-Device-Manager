# CAN Triple Device Manager — Design

A Qt 6 (C++/Widgets) desktop application for configuring the CAN Triple gateway
(STM32G473CBT6, 3× CAN) over the ST-Link virtual COM port. Layout and
navigation deliberately mirror **MoTeC C125 Dash Manager** (verified against
the real application, v6.51, on this machine).

## 1. Serial link (as implemented by the firmware in `../src`)

- UART: **USART1**, PB6/PB7, **7,372,800 baud**, 8N1, no flow control
  (ST-Link **V3** VCP required at that rate). Port + baud are user selectable.
- Framing: standard **COBS**, one `0x00` delimiter **after** each frame.
  The GUI also sends a leading `0x00` for resync (firmware ignores empty frames).
- Raw packet (pre-COBS), all little-endian **except the CRC**:
  `[0x55][cmd][len u16 LE][payload…][crc16 hi][crc16 lo]`
  CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over header+payload, appended big-endian.
- **Host→device frames must stay ≤512 wire bytes** (`MAX_TX_WIRE_BYTES`),
  payload ≤**496** (`MAX_TX_PAYLOAD`). The worst-case frame is
  4 header + 496 payload + 2 CRC = 502 raw, +1 COBS code byte per 254 bytes
  and 2 delimiters = **506**, six under the cap; a `static_assert` in
  `wire_structs.h` re-derives that rather than trusting the arithmetic here.
  These numbers used to be 127 and 112, and only because of a v1 RX-DMA fault —
  a half-transfer re-arm bug at 128 B and a permanent lockup at exactly 256 B.
  That fault has been fixed since the DMA went circular (FIRMWARE-NOTES #5);
  the cap now answers to the device's 1 KB `rxBuffer`, which is what a frame
  larger than the buffer would be lapped by. The two moved together, and must
  keep moving together. One command in flight (stop-and-wait), timeout
  250 ms (1500 ms for flash ops), ~5 retries, then a 1.2 s quiet period
  (firmware auto-recovers UART faults on its 1 Hz tick).
- Responses: `0x80` ACK (payload = 1 byte `0x00`), `0x81` NACK (payload =
  1 error byte, does **not** echo the command), data responses **echo the
  request cmd**; range reads echo `start,count`, which the GUI checks to
  reject stale duplicates from retries. Streams `0x82` (monitor, 76 B) and
  `0x83` (value stream) are always on, but **current firmware emits them
  unframed** (FIRMWARE-NOTES #7) — the GUI's framed stream demux activates
  once the firmware routes them through its packet framing.
- The firmware interleaves ASCII printf debug on the same UART **without**
  framing; any inter-delimiter chunk failing COBS/CRC/0x55 checks is silently
  discarded.
- Write chunk sizes at the 496-byte cap: messages 49/frame (4 + 49×10 = 494),
  signals 7 (4 + 7×64 = 452), math 20, conditions 14, timers 24, 8x8
  definitions 6 (4 + 6×73 = 442), 8x8 grid rows 15 (4 + 15×32 = 484). Read
  chunk sizes are bounded by the **unchanged** 2030-byte device→host cap:
  messages ≤200, signals ≤**31** (4 + 31×64 = 1988; 32 would be 2052),
  math ≤84, cond ≤50, timers ≤50, 8x8 defs ≤8, 8x8 rows ≤32 per request.
  Every one of these was recomputed from its record size when the cap rose —
  scaling the old numbers would have been wrong for signals in particular,
  whose record grew 48 → 64 at the same time. (`wire_structs.h` WRITE_CHUNK_* /
  READ_CHUNK_* are the authority if these drift, and `test_firmware_link`
  asserts each write chunk is not merely legal but **maximal**: a chunk one
  record short of the cap is a Send that takes more round trips than the wire
  requires, and nothing else would ever report it.)
- Table capacities, declared in both `protocol.h` and `wire_structs.h` and
  asserted equal in `test_firmware_link`: messages **500** (see the 9-bit
  ceiling in §3), signals **1000**, math 100, conditions **250**, counters 50,
  timers **50**, constants 100, relays 32, 2x16 tables 8, 8x8 tables **8**
  (and therefore 64 grid-row records — table `t` owns rows `t*8 .. t*8+7`),
  integrators 8. Together they are 126,368 B of the 131,072 B (128 KB) config
  region; `CFG_TOTAL` in `flash_store.c` is generated from `FLASH_TABLE_LIST`
  and `_Static_assert`ed against the region size, so that total is derived
  rather than quoted.
- **Conditions went 100 → 250**, and that is where most of the layout's slack
  went: `ConditionConfig` is 35 B, `PAD8(35)` is 40, so the table costs
  10,000 B where it cost 4,000 and only **4,704 B** of the region are left.
  What caps the next raise is not flash, though — it is the shared channel
  pool. Every condition owns an output slot, so 250 of them claim up to 250 of
  the 1000 signal slots, and another 250 signals would cost 16,000 B against
  4,704 spare.

Commands (see `../include/protocol.h`): GET_STATUS 0x01, WRITE/READ
MSG 0x02/0x03, SIG 0x04/0x05, MATH 0x06/0x07, COND 0x08/0x09,
SAVE_TO_FLASH 0x0A (payload **optional**: two bytes carrying the configuration's
version number — see "Fleet identity"), LOAD_FROM_FLASH 0x0B, CLEAR_CONFIG 0x0C,
CONTROL_CAN 0x0D, INJECT_CAN_FRAME 0x0E (71 B payload),
STREAM_VALUES 0x0F, WRITE/READ COUNTER 0x10/0x11, TIMER 0x12/0x13,
CONST 0x14/0x15 (v6), WRITE/READ CONFIG_NAME 0x16/0x17, RESET_DEVICE 0x18
(v7), WRITE/READ RELAY 0x19/0x1A (v11),
WRITE/READ TABLE2X16_DEF 0x1F/0x20 + TABLE2X16_OUT 0x21/0x22 (v13),
WRITE/READ INTEG 0x23/0x24 (v16), GET_DEVICE_ID 0x29 + WRITE_CONFIG_BINDING
0x2A (v18), the access / fleet block: READ/WRITE_ACCESS_KEYS
0x2B/0x2C, ACCESS_CHALLENGE 0x2D, ACCESS_RESPONSE 0x2E, **READ_FLEET_ID 0x2F**,
**FLEET_ID_PROVE 0x31**, and **READ_CAN_SETUP 0x30** (see "Reading the bus setup
back"), WRITE/READ DEVICE_CHANNELS 0x32/0x33, and the 8x8 lookup table's four:
WRITE/READ TABLE8X8_DEF **0x34/0x35** + TABLE8X8_ROW **0x36/0x37**.
**There is no write for the fleet identity** — it is
compiled into the firmware, so `0x30` (briefly WRITE_UPDATE_ID, during the
revision where the identity was runtime state) went back into the pool and was
handed to READ_CAN_SETUP. That reuse is safe in a way the retirements below are
not: this is protocol v1 and nothing is deployed, so no host anywhere holds an
older meaning for it. A command gated behind an access password the
session has not proved NACKs **ERR_LOCKED 0x07**; which password is implied by
what was asked (a read trips Get, a write trips Send, a protected-comms
operation trips Edit Protected Comms), so the host can name the right one in its
prompt without the device having to say which key it tripped over.
**0x1B/0x1C are retired** — they were the v12 2x8 table, and because the v13
definition record is also 70 bytes, reusing them would let a v12 host's record
pass the length check and be misread; a version mismatch now fails cleanly on
ERR_INVALID_CMD. **0x25-0x28 are retired too** — they were the v18 single
configuration password (READ/WRITE_CONFIG_LOCK, LOCK_CHALLENGE, LOCK_RESPONSE),
replaced by the per-function access keys, and are likewise not reused: a v18 host
would otherwise get a plausible-looking answer from a device that no longer means
the same thing by it, and would conclude the device is unprotected.
**0x1D/0x1E are retired as well** — the v12 4x4 lookup table, replaced by the
8x8 at fresh ids rather than in place. Its 105-byte record would probably have
failed the length check against the 8x8's 73-byte definition, but "probably" is
not the standard a replaced table gets, and the two failures do not read the
same to whoever is holding the cable: ERR_INVALID_CMD says the device does not
have this feature, a length NACK says the record is malformed.
Compound (multiplexed) messages are v8
(per-signal mux selector); a per-bus termination-resistor byte in
`ControlCanPayload` is v9; compound transmit (Batch/Sequential via
MSGFLAG_TX_SEQUENTIAL) is v10; the message-relay table is v11; the lookup
tables are v12, the 1-axis one widened to 2x16 in v13 and the 2-axis one
replaced by the 8x8; the three per-function access keys and the fleet identity
block came after those, and the capacity expansion (message, signal and timer
tables widened, the label back to 32 bytes, the payload cap raised, the 8x8
replacing the 4x4) is the most recent.
Known firmware gaps are collected in `FIRMWARE-NOTES.md`.

**The version numbers were reset.** `PROTOCOL_VERSION` and `FLASH_STORE_VERSION`
were both set to **1**, and the dead `PROTOCOL_VERSION_V2`…`_V17` ladder is
deleted — it was never compared against anything, so it recorded a release
history no customer ever saw. Nothing has shipped; a version whose only two
readers are halves of one repository that are always rebuilt together is a
liability, not a compatibility guarantee. v1 is the first protocol anyone
outside this repo will meet. The
`v6`/`v8`/`v13`… tags above and throughout this document therefore read as
**development milestones** — the order features arrived in — and no longer as a
number that crosses the wire. The practical consequence is the same one every
`FLASH_STORE_VERSION` bump has had: any image stored by an earlier build is
rejected rather than misread, so the first boot after flashing comes up on
bring-up defaults and the configuration has to be sent again.

The two numbers have not stayed together since, and that asymmetry is the
point. `PROTOCOL_VERSION` is **still 1** — the length check
(`4 + count*item_size`) is what makes a version mismatch fail cleanly, so the
wire number buys nothing a record size does not. A stored *layout*, on the
other hand, really does move, and `FLASH_STORE_VERSION` is **10**: most
recently for `MAX_CONDITIONS` 100 → 250, which shifts every table below the
conditions and would otherwise have a v9 image misread record-for-record
before its CRC ever ran. The GUI's `EXPECTED_STORE_VERSION` is the same
number, and Send Configuration checks it against the unit **before it writes
anything**, so a mismatch is named up front rather than discovered as a
part-written device.

**The `.ct3` FILE schema was deliberately NOT reset with it.**
`kConfigSchemaVersion` keeps counting — it is **17**, the schema in which a
transmit section can name the User Condition that triggers it
(`transmitCondition` / `resetConditionOnTransmit`) — and files written at 2
through 16 are on people's disks right now. 17 is additive, and the bump is
mandatory for the familiar reason: `"cyclic": false` has been a legal, inert
value in every file since the beginning, so an older Manager reads a Triggered
section as a perfectly ordinary message, finds nothing missing, and sends one
that transmits **continuously** — traffic on a customer's bus nobody asked
for, out of a file that looked like it loaded cleanly. Refusing the file names
the real remedy instead. What each bump was for lives beside the constant in
`configuration.cpp`; the one the migration below is about is **12**, the schema
in which the two-axis lookup table became an 8x8 (the `tables4x4` key became
`tables8x8`). Those keep
loading: a schema-11 `tables4x4` entry is read and placed in the top-left of an
8x8, counts and cells intact, because a 4x4 row always carried variable-length
site lists and an `outputs` grid strided by its own X width, never a fixed 4.
A saved 4x4 already *is* an 8x8 whose sites stop early. `readWrapper()` refuses a file whose `fileVersion` is
ahead of what this build knows, which is the guard against a document saved by a
newer app; renumbering the schema to 1 would make every one of those existing
files look like it came from the future and be refused. A file format's version
answers to the files in the world. A wire version answers to the two programs at
either end of a cable, and here those are always built and shipped together. The
mismatch between the two numbers is the point rather than an oversight.

## 2. UI inventory (mirrors Dash Manager; field lists verified on-screen)

- **Main window** — menu bar `File · Connections · Calculations · Functions ·
  Online · Tools · Help`; toolbar; central splash (logo + device art
  placeholder); connection selector bottom-left; status bar with link state.
  Editing menus disabled until a configuration document is open (offline-first
  document model; explicit Send/Get to move it to/from the device).
## Set Access Passwords

**Online → Set Access Passwords…** (`src/model/access_keys.*`,
`src/ui/access_passwords_dialog.*`) — three independent passwords, laid out like
the same screen in Dash Manager: a list of protected functions, a Set… button,
and a tick against the ones that carry a password.

- **Send a Configuration** — the device refuses a Send without it.
- **Get a Configuration** — the device refuses a Get without it.
- **Edit Protected Comms** — reveals and edits messages marked "Protect
  Communication", both in this app and on the device.

They live **in the device**, not in the file, which is why the dialog sits under
Online and needs a connection; what it shows is read back from the unit every
time it opens rather than cached, because the interesting case is a device
someone else configured. They are also independent of each other: holding one
proves nothing about the others, and a session that has earned the right to Send
has not thereby earned the right to Get. Collapsing them would make the weakest
password the only one that mattered.

**One password, two derivations, and the reason there are two.**

- `deriveAccessKey()` → a **4-byte key**, PBKDF2-HMAC-SHA256 over a **fixed
  application salt**. This is what the hardware stores and compares, and what it
  proves by challenge-response. The salt has to be fixed because the same
  password must produce the same key on every unit in a fleet — that is exactly
  what lets one `.ct3s` update a hundred devices. A per-unit salt would make one
  password mean something different on each of them.
- `AccessVerifier` → a **32-byte verifier**, PBKDF2 over a **random per-file
  salt**, stored in a `.ct3`/`.ct3s` so the app can check a typed password
  offline. It deliberately cannot be turned into the 4-byte key, so a
  configuration file lying around does not hand over the thing that opens
  hardware.

A file therefore leaks nothing usable and the device holds the only copy of what
matters. Both derivations run 210 000 rounds. The verifier stores its own count
alongside itself so raising it later does not orphan existing files; the key
derivation cannot store one — the device has neither the room nor the need — so
`kAccessKeyIterations` is fixed for the life of the protocol, which is why it is
a separate constant rather than a shared one. Zero is reserved to mean "no
password", and a derivation that lands on zero is nudged off it: the alternative
is a password that silently reads as no password at all.

**Proving one is challenge-response.** `CMD_ACCESS_CHALLENGE` (0x2D) returns 16
random bytes and the host answers `CMD_ACCESS_RESPONSE` (0x2E) with the function
index plus HMAC-SHA256(the 4 key bytes, challenge). A serial capture is worth
nothing on the next connection, and a wrong guess costs a full round trip.
`CMD_READ_ACCESS_KEYS` (0x2B) answers only a bitmask of *which* passwords are
set — the keys are write-only and nothing ever reads one back off the wire.
Setting, changing or clearing one (`CMD_WRITE_ACCESS_KEYS`, 0x2C) requires the
current one to have been proved first, otherwise "set a new password" would be
the way past not knowing the old. The same is true document-side:
`Configuration::setCommsPassword()` refuses while comms are still concealed.
`AccessPasswordsDialog::promptAndProve()` is shared with the Send and Get paths,
so the prompt before a Send is the same one the dialog uses.

**What four bytes buys, and what it does not** — stated plainly, because a lock
trusted further than it reaches is worse than no lock:

- Against password **guessing** it is strong. 210 000 PBKDF2 rounds make each
  candidate cost real time, and the device answers one guess per serial round
  trip.
- Against a **flash dump** it is worth nothing: the key is right there, and the
  attacker never needs the password. The backstop is STM32G4 readout protection,
  a device programming decision rather than a code one.
- The key space is **2³²**. Unreachable online; reachable offline by someone who
  captures a challenge/response pair and is willing to spend the compute. Four
  bytes is what the hardware compares, so this is the floor the design sits on,
  not an oversight in it.

## Message protection — the three tiers

`CommsSection::protection` is **one ordered level**, not a set of flags:

```
CommsProtection::None(0) < ReadOnly(1) < Hidden(2) < Protected(3)
```

It replaced two independent booleans (v19 `protectedComms`, v20 `readOnlyComms`)
that described nested behaviours — eight representable states for four real ones,
with the implication `readOnlyComms = readOnly || protect` maintained by hand in
one dialog and walked around by a dozen other paths into the document. An ordered
level makes that invariant unrepresentable, and every consumer asks a comparison:
`isConcealed(revealed)` is `>= Hidden`, `isEditLocked()` is `!= None`. No call
site tests the level directly, so "which tiers hide things" is decided once.

| | View | Edit | Remove | To tick **or** untick |
|---|---|---|---|---|
| **ReadOnly** | allowed | refused | **allowed** | the section's own `messageKey` |
| **Hidden** | refused | refused | **allowed** | the section's own `messageKey` |
| **Protected** | refused | refused | **allowed** | the section's own `messageKey` **and** `AccessFunction::EditProtectedComms` **proved against a connected device** |

`Configuration::proofsRequiredFor(tier)` is the one statement of that last
column, and both dialogs read it rather than switching on the tier themselves. It
returns `{sectionPassword, deviceProof}`; only `Protected` sets both.

**Every marked tier carries a password of its own, `Protected` included.**
`SectionEditorDialog::accept()` refuses while `protection != None` and the
section has no key — that is the *only* place it is refused, so Cancel still
works from any state. Protected used to be the one marking with no per-section
secret: one document password opened every Protected message in a file.

**Moving a marking costs the current password, in either direction.** Raising was
free until 2.3.1, which meant the tier whose whole promise is "this needs my
password to change" could be walked *up* by anyone holding the file.
`SectionEditorDialog::authoriseTierChange()` is the single path for both
directions, and a move across the `Protected` boundary — on or off — takes the
device round trip as well. The key belongs to the marking rather than to the
message, so it does not carry onto a new one: `sectionPasswordSatisfied()` ties
the stored key to `m_openedTier` and demands a fresh password anywhere else.

**A grant stands for the whole of a tier's challenge.** `grantSectionAccess()` is
called once, after the last proof, by both `SectionEditorDialog` (via
`protectionUnlocked()`) and `CommunicationsDialog::unlockConcealedSection()`.
Recording it between Protected's two halves would hand out the device half for
free — for that section, everywhere in the app, for the rest of the session.

**A grant names a section, not a string the viewer picked.** It records the *bus
index*, the lower-cased *name*, and the `messageKey` it was proved against. The
bus, because a bare name was matched across every bus and a name is a value the
person being kept out chooses: a section added on another bus under that name,
unlocked with a password of their own, opened the real one. The key, because a
grant must not outlive the secret it stood for — replacing a section's password
retires every grant proved against the old one automatically, which is the belt
under the chokepoint's refusal below. `sectionAccessGranted(bus, section)` is the
exact question, and the dialogs ask it; the two predicates below are handed
section *values* with no bus (a live view's copy, an incoming candidate) and so
match on name **and** key, the half an impostor cannot reproduce.

**A grant does not outlive the editor while the marking still conceals.** Closing
the section editor — OK or Cancel — on a section that is still `>= Hidden`
re-conceals it in Communications Setup immediately. The revoke itself
(`Configuration::revokeSectionAccess()`) is QUEUED in
`CommunicationsDialog::m_pendingRevoke` and flushed *after* the
`applyBusSections()` loop, because the grant is also what authorises a lowering:
revoking first refuses the very edit that was just authorised, and
`Protected → Hidden` is exactly that shape — lowered, and still concealing.
`CommunicationsDialog::sectionRevealed()` is the single place that lays the
pending set over `Configuration::isSectionRevealed()` for display.

- **Withheld** means the CAN ID, length, alignment, timing and every row's start
  bit / bit length / DBC type / factor / offset, in the sections list, the
  section editor, the Config Summary and Check Channels. A concealed message's
  validation findings collapse into one entry keeping the **severity** (an Error
  still blocks Send) and dropping the detail. Channel *names* stay visible at
  every tier: the point is to protect the protocol, not the outputs.
- **The edit lock is not lifted by revealing**, deliberately. The password buys
  viewing and the right to untick; unticking is what allows editing.
  `isEditLocked()` takes no `revealed` argument so that "revealed, therefore
  editable" cannot be written.
- **The channels of a locked message are locked with it**
  (`Configuration::isChannelEditLocked()`) — data type, base resolution, decimal
  places, range, units. Not cosmetic: change a resolution and the message
  silently decodes to different numbers with nothing on screen to say why.
- `isChannelProtected()` **split in two** because ReadOnly is visible AND locked:
  `isChannelConcealed()` withholds a VALUE, `isChannelEditLocked()` disables a
  CONTROL. Driving a control from the first would leave a ReadOnly message's
  channels editable.

**Authorisation is per section, and the tier picks the password.**
`Configuration::isSectionRevealed()` (viewing) and `Configuration::maySectionLower()`
(unticking) are the two predicates, and both switch on the tier: `None`/`ReadOnly`
are always viewable, `Hidden` takes the section's own `messageKey`, `Protected`
takes a grant — which by `proofsRequiredFor()` means that `messageKey` *and* a
device-confirmed Edit Protected Comms. Unticking `ReadOnly` takes its `messageKey`
too. Both predicates share `protectedSectionProved()` so they cannot answer that
tier differently.

`commsRevealed()` is **not consulted by either predicate any more, at any tier.**
It used to survive as `Protected`'s *keyless* arm, justified as an upgrade path
for the pre-2.3.1 messages that carry no `messageKey`. That arm was the third
face of the 2.3.2 bug: `commsRevealed()` is true for every document with no Edit
Protected Comms verifier, and a Get into a fresh window produces exactly that
document, so the arm published every retrieved `Protected` message to anyone.

**Both predicates fail closed on a keyless marking.** A marked section with no
`messageKey` is concealed from everybody and lowerable by nobody, at all three
tiers, because no password for it exists to be produced. `grantSectionAccess()`
refuses to record a grant against `kNoAccessKey` for the same reason — every
keyless section shares that value, so such a grant would be decided by the
section's NAME alone. What a section in that state can still do: be removed (free
at every tier), be reordered and sent, have its channels used elsewhere, and be
given a FIRST password — which is free, and is what makes it lowerable
afterwards.

There is deliberately **no** document-wide `mayLowerProtection()`. It used to
exist and it was `commsRevealed()`, which is true for every document that carries
no Edit Protected Comms verifier — i.e. every document that uses per-section
passwords instead. So the chokepoint performed no check on exactly those
documents, and `commsRevealed()` passed as `revealed` at each suppression site
un-concealed every Hidden section in them. The Edit Protected Comms password is
**not** a master key over `ReadOnly` and `Hidden`; no code path substitutes it.

**One chokepoint.** `Configuration::applyBusSections()` is the only writer of a
bus's sections, and it refuses **two** changes to a still-present section (matched
by name, case-insensitively) unless `maySectionLower()` says otherwise **for that
section**, asked of the section as it stands in the document rather than of the
proposed replacement: its tier **lowered**, and its `messageKey` **replaced**.
Both hand the message to somebody else, and only the first used to be guarded —
Read Only never conceals, so its editor opened with no challenge and typing into
Message Password made the section yours, with the tier never moving and the
untick rule never firing. Setting a *first* password is free (there was no prior
owner), and removal is free at every tier. Two sections may share a name, so the
"before" side is a **list per name consumed in document order** — a single-slot
map let the unguarded one of a duplicate pair answer for the guarded one. The
guard is *not* on `QCheckBox::toggled`: the whole-bus
commit, the per-section overwrite, DBC import, a Get and the Lua bindings all
reach the document without a checkbox being touched. Anything added later that
can replace sections — an undo stack, a Duplicate command — must route through it.

**A Get does not launder the MARKING — and does not carry the LOCK.**
`mapFromDevice` calls `clearContent()`, not `clear()`, so the access verifiers,
the reveal state and the session's grants survive; and the tier rides the wire in
the top two bits of `CanMessageConfig` and `RelayConfig` flags (`MSGPROT_MASK`),
so a Get→Send round trip cannot turn a Hidden message into an ordinary one. That
is the *token*, and it is all the wire carries: the key field is `reserved[4]`,
so a configuration read off a unit arrives **marked and keyless** — and since
2.3.2 a marked keyless section is **concealed**, not open. Into a fresh document
a Get therefore produces messages nobody can read, which is the product's stated
purpose (the customer installs updates and does not read them) and is what the
reported bug was: they used to come back padlocked *and* legible. What actually
survives a Get is whatever the open document already knew, per the snapshot
below — so a Get in the window the configuration was built in restores every
password and reads normally. Do not read the marking's survival as the
protection's.

`clearContent()` does empty the buses, though, so a Get rebuilds every section
from scratch and the two per-section facts the wire does not carry — `messageKey`
and the user's **name** — would be lost with it. `mapFromDevice` therefore
snapshots both before `clearContent()` and re-applies them to the matching
rebuilt section; a section the device no longer has simply drops. Two indexes
over one set of snapshots, each record handed out once:

1. the section's identity **on the wire** (bus + CAN ID + direction/kind), tried
   first — names are regenerated by the rebuild, so a name-first pass would
   pair against a name that means nothing;
2. failing that, the **bus and name** the rebuilt section is about to carry. Its
   absence was a bug in its own right: renumbering a message meant its identity
   named nothing the document knew, so its password went silently over the side.
   Scoped to the bus, because a cross-bus name match would let a section someone
   else created donate its key to this one. Without that, every `messageKey` came back `kNoAccessKey`, which
means "no password", which means a Get opened every Hidden section in the document
and permitted every untick. A rebuilt section with no prior name and a concealing
tier gets a neutral name (`Hidden message 1`) rather than `Receive 0x640`, which
would print the withheld CAN ID in the sections list.

**What bounds all of it, said here rather than left to be discovered:**

- **The device enforces nothing.** As of 2.3.0 there is no message gate in the
  firmware at all: no write gate, no `CMD_CLEAR_CONFIG` gate, and opcode `0x40`
  (`CMD_MSG_ACCESS_RESPONSE`) is retired and answers `ERR_INVALID_CMD`. The
  `MSGPROT_*` bits are transported for round-trip fidelity **only**. Any other
  serial tool reads a marked message in full and writes over it freely.
- **A plain `.ct3` contains every field in clear JSON and nothing in it is
  signed.** A text editor defeats all three tiers. Only Save Secure Config
  (`.ct3s`) makes the bytes themselves unreadable.
- **ReadOnly is accident prevention, not security.** The viewer sees every field
  and may remove the section, so remove-and-retype reproduces the message without
  the password. Hidden and Protected differ substantively for the one reason that
  matters: a viewer who cannot see the message cannot retype it, so removal
  destroys rather than reveals.
- **With no password set, NOBODY can view or untick — and you can no longer make
  one.** A section with no `messageKey` is concealed at `Hidden` and `Protected`
  and un-lowerable at all three tiers. This inverts the pre-2.3.2 rule ("anyone
  can view and untick", mirroring the firmware's treatment of keyless records),
  which was the reported bug: the wire carries `reserved[4]` and no key, so every
  section a Get produces is keyless and that rule handed back every message the
  author had marked. There is no seam left to explain: the editor will not let you
  *create or keep* a keyless marking (`accept()` refuses), and one that already
  exists conceals rather than opens.

  The cost is deliberate and bounded. A configuration retrieved from a device
  cannot have its marked messages read in this application, by anyone. The person
  who built it holds the original `.ct3`, which holds the passwords; the retrieved
  section can be removed, reordered and re-sent; and giving it a first password is
  free, after which it behaves like any other guarded section. Because of that,
  the file writers ask `anyKeyedSectionConcealed()` rather than
  `anySectionConcealed()` — a keyless concealed section has no secret to launder,
  and refusing it would have made "back up this unit" impossible.
- **Removal is permitted at every tier**, in the dialog, from Lua and on the
  device. Nothing anywhere refuses it.

## The .ct3s container

**File → Save Secure Config…** (`src/model/secure_file.*`). A `.ct3` is indented
JSON: every CAN ID, bit layout and scaling factor is legible in Notepad, and a
message marking only stops *this app* from displaying them. That is fine
for a configuration you own and useless for one you ship to a customer. A `.ct3s`
is the same document as an opaque binary blob. Open… routes on the file's magic
rather than its extension, so a `.ct3s` can never be parsed as JSON.

**Two modes, and the difference between them is the whole story.**

- **Standard** (`requirePassword = false`) — the body is encrypted, but the key
  that decrypts it travels inside the file, obfuscated. Anyone with CAN Triple
  Device Manager can open the file, send it to a device and use its channels;
  nobody can read the protocol detail out of the bytes, and the protected
  messages stay concealed in the UI. This is the shipping format: a customer can
  deploy and update without ever seeing the CAN layout. Be honest about the
  limit — this is **obfuscation over encryption**. It defeats a hex editor, a
  text search, a grep for `0x640`, and any tool that does not implement this
  format. It does **not** defeat someone who reads this source or disassembles
  the app: the key is in the file and a determined reader will find it. If that
  reader is in your threat model, use the second mode.
- **Password-protected** (`requirePassword = true`) — the file key is
  additionally wrapped under the Edit Protected Comms password, so the file
  cannot be opened at all without it. There is no recovery: no reset, no back
  door, no copy held anywhere. A lost password is a lost configuration, and the
  save dialog says so before the fact.

This mirrors MoTeC's "Hide setup information" / "Require access password for use"
pair on a locked comms template, for the same reasons.

**File layout** — all multi-byte integers LITTLE-ENDIAN.

```
Header, 64 bytes, cleartext:
    0   u8[8]  magic          kSecureMagic
    8   u16    formatVersion  kSecureFormatVersion
   10   u16    flags          bit 0 = requires password
   12   u8[16] salt           random; seeds wrapping AND chunk placement
   28   u32    iterations     PBKDF2 rounds for the password wrap
   32   u32    carrierLength  bytes of carrier following the header
   36   u32    payloadLength  bytes of sealed payload hidden in the carrier
   40   u8[24] padding        random; ignored on read

Carrier, carrierLength bytes: CSPRNG noise, divided into 16-byte slots,
into which the secret material is SCATTERED rather than laid down in one
run. In order, the material is:
    chunks 0-1  wrapped file key   (32 bytes)
    chunks 2+   sealed payload     (payloadLength bytes, zero-padded to a
                                    multiple of 16 in the final chunk)
```

The sealed payload's **plaintext** is the four embedded-access-key bytes
followed by the configuration body, so the key is encrypted and covered by the
payload's tag along with everything else.

Slot order comes from a Fisher-Yates shuffle of the slot indices driven by the
placement keystream — `HMAC-SHA256(salt, "ct3s/placement/v1" || u32be(ctr))`,
counter from 0, consumed 4 bytes at a time as a big-endian `u32` — so consecutive
chunks land nowhere near each other and unclaimed slots stay indistinguishable
noise. Everything a reader needs to reverse this is derived from `salt`, which is
in the clear: **the scatter buys unreadability, the encryption buys secrecy**, and
neither is asked to do the other's job.

Wrapping, in the same terms:

- `fileKey` — 32 random bytes, generated per save.
- `wrapMask` — `HMAC-SHA256(salt, "ct3s/wrap/v1")` XOR
  `PBKDF2(password, salt, iterations, 32)`, the second term present only when a
  password is required; `wrapped = fileKey XOR wrapMask`.
- `encKey` / `macKey` — `HMAC-SHA256(fileKey, "ct3s/enc/v1" / "ct3s/mac/v1")`,
  fed to `sealPayload()` / `openPayload()` from `config_lock.h`. The `.ct3s` body
  therefore uses exactly the authenticated encryption (HMAC-SHA256 in counter
  mode, encrypt-then-MAC, PBKDF2 via `QPasswordDigestor` — hence the Qt6::Network
  link) that the rest of the app already relies on; those primitives are all that
  survived of the old configuration lock.
- `accessKey` — the 4 big-endian key bytes, prepended to the plaintext before
  sealing. This is the Edit Protected Comms key, carried so the app can satisfy
  a **device's** protected-comms gate on the customer's behalf without them ever
  typing the password.

  It began life in a chunk of its own, masked with `HMAC(salt, …)`. That was
  wrong twice over and the reasoning is worth keeping: the salt is cleartext at
  offset 12, so the mask hid nothing from anyone holding the source, and nothing
  authenticated the four bytes, so a single flipped bit produced a file that
  opened perfectly and yielded a silently wrong key — surfacing much later as an
  unexplainable refusal from a device. Folding it into the payload costs
  nothing, because the key only ever needs to be reachable in exactly the cases
  the payload is: a standard file opens without a password, and anyone who can
  open a password-protected one can derive the key themselves.

`SecureSaveOptions::noiseRatio` (0.35) adds carrier beyond what the payload needs,
so two saves of the same document differ in length as well as in content and a
file's size says nothing about how much configuration is in it. A truncated,
tampered or wrong-password file fails at the payload's HMAC tag and is rejected
whole — nothing is ever half-parsed — and `readSecureFile()`'s `error`
distinguishes "wrong password" from "damaged file" so the caller can say which.
`peekSecureFile()` reads the header alone, which is what lets Open… ask for a
password *before* disturbing the document that is already open; cancelling leaves
it untouched. Save follows the format the file already has
(`Configuration::isSecureFile()`), so a secure configuration is never silently
downgraded to plain JSON, and `saveSecureToFile()` refuses while comms are
concealed: the body has to be assembled in full to be sealed, and a session that
cannot see the protected messages must not be the one that rewrites the file
carrying them.

## Fleet identity

**Online → Fleet Identity…** (`src/ui/fleet_identity_dialog.*`). The problem it
exists for: you ship a customer a device running a configuration whose CAN
protocol is yours, not theirs. Six months later you need to send them a new one.
They must be able to install it; they must not be able to read it; and it must not
install on anything it was not built for. Reading the device's configuration to
find out is the one thing a locked unit must not do, so the unit carries a small
block that answers the question on its own.

The dialog's two panels are deliberately **not symmetrical**, and the asymmetry is
the design: *This Device* is read-only, *This Configuration* is editable. One is a
statement about a piece of hardware; the other is a statement about which hardware
a file is allowed to reach.

### The device's half is compiled in

A unit's identity lives in `firmware/include/fleet_identity.h` and is fixed when
the firmware is built. **Nothing writes it at runtime and no host command can
change it** — there is no `CMD_WRITE_FLEET_ID` and no `writeFleetIdentity()` in
`device_session`. Re-badging a unit means editing its identity file and flashing
it. That is what makes the identity worth believing: an attacker cannot send a
packet to become somebody else, a flash erase cannot lose it, and it cannot
drift out of step with a configuration that was sent later.

Set it in `firmware/identity.local.ini` — a **gitignored** file that a fresh
checkout does not have. Copy `identity.local.ini.example` to create one:

```ini
CT_VENDOR_ID=Detailed Motorsp
CT_MODEL_ID=CAN Triple
CT_SERIAL_NUMBER=0x00000123
CT_FLEET_KEY=0xDEADBEEF
```

`firmware/scripts/build_flags.py` runs before every build (`extra_scripts =
pre:`), reads that file and appends the matching `-DCT_*` defines. **The tracked
`platformio.ini` carries no identity values at all**, which is the point: real
values checked in would have every developer's bench board claiming to be a
shipping product. No file means an **unprovisioned** build; a file that is
present but malformed is a **hard build error naming the line**, never a
half-badged unit.

**The quoting sharp edge is gone.** The old flow put `-DCT_VENDOR_ID='"Detailed
Motorsp"'` in `platformio.ini`, because the shell and SCons each got a go at the
string and a plain `-DCT_VENDOR_ID="Acme"` arrived at the compiler as a bare
identifier. `identity.local.ini` takes vendor and model as **plain unquoted
text**, spaces and all — a `"` or `\` in the value is now itself the build error
— and the script applies the quoting the define needs.

| Field | Identity key | Wire | Set at |
| --- | --- | --- | --- |
| Vendor ID | `CT_VENDOR_ID` | 16 bytes, NUL-padded | build |
| Model ID | `CT_MODEL_ID` | 16 bytes, NUL-padded | build |
| Serial Number | `CT_SERIAL_NUMBER` | `u32` | build — **per UNIT** |
| Flags | `CT_FLAGS` | `u16` | build (reserved, 0 — **not** an `identity.local.ini` key) |
| Fleet Key | `CT_FLEET_KEY` | 4 bytes, **never read back** | build |
| Config Version | — | `u16` | **runtime**, in the flash header |

`CT_FLAGS` is the one macro the identity file will not accept: nothing reads its
bits yet, and an unknown key is a build error rather than a knob that does
nothing. Every value has a default, so a build that sets nothing still compiles
— it just reports itself **unprovisioned**, which is honest, rather than
silently claiming to be somebody. `fleet_identity_is_set()` answers false and
the host stops checking there; a blank unit must never satisfy a policy that
names a vendor.

The two strings are **NUL-padded, not NUL-terminated**: all 16 bytes are usable
characters, so they are compared as counted fields and never with `strcmp`. They
are 16 *bytes*, not 16 QChars — a name with non-ASCII characters in it runs out of
room sooner than its length suggests, which is why every writer on the PC side
goes through `FleetIdentity::clampToWire()` (truncates on a UTF-8 boundary) rather
than `QString::left()`.

### The per-unit serial, and what it costs

`CT_SERIAL_NUMBER` is the one field that is per-BOARD rather than per-PRODUCT, so
**a build carries exactly one unit's serial and each board needs its own
compile-and-upload**. That is the deliberate price of "set it once in
`identity.local.ini` and forget it": there is no provisioning step, no tool to
write, no database to keep, and nothing a customer can reflash away — in
exchange for not being able to flash fifty boards from one binary.

The alternative is sitting right there and is worth naming rather than pretending
it does not exist: the STM32's 96-bit unique ID is already readable
(`CMD_GET_DEVICE_ID`, the v18 device binding) and needs no provisioning at all.
It is a different thing, though — **silicon chooses that number, not you**, so it
cannot be the serial on an invoice, cannot be allocated before the board exists,
and cannot be made to mean anything. The two coexist: the UID is the chip's
identity and the serial is yours.

### Config version — the one runtime field, and why it rides SAVE

A configuration's version has to move every time a configuration is released.
Compiling it in would mean a firmware build and a reflash per revision, which is
precisely the thing the uploader exists to avoid, so this one field stays runtime
state in the flash header.

It arrives as an **optional 2-byte payload on `CMD_SAVE_TO_FLASH`** (0x0A); an
empty payload leaves the stored version alone. It rides the commit rather than
having a command of its own so the two cannot separate — a version that landed
without its configuration, or a configuration that landed without its version,
would each make the uploader's "is this update newer?" check lie, in a different
direction each time. Firmware side it is `engine_set_config_version()` /
`engine_config_version()`, saved and restored with the image by
`flash_store_commit()` / `flash_store_validate()`.

Unlike the access keys, the version does **not** survive `CLEAR_CONFIG`: the
version describes the configuration, and a device with no configuration is running
revision nothing. Reporting a stale version for tables that are no longer there
would answer the newer-than question about a config that does not exist.

### What crosses the wire

`CMD_READ_FLEET_ID` (0x2F) answers `FleetIdentityPublic`, **41 bytes** —
`vendor_id[16]`, `model_id[16]`, `serial_number` u32,
`config_version` u16, `flags` u16, `key_present` u8. The fleet key is absent by
design; `key_present` says only that there is one. (It was 45 while a fourth
4-byte identifier sat between the serial and the version — see the Upload
Configuration rules for why that field went. The size is worth stating exactly,
because the host validates the reply by length and a stale 45 here would send
someone hunting for four bytes the device never sends.)

`CMD_FLEET_ID_PROVE` (0x31) is the one exchange that runs the other way: the
**host** sends a 16-byte challenge and the **device** answers
HMAC-SHA256(fleet key, challenge). Reading the identity says what a device
*claims* to be; the challenge says it actually holds the fleet secret, which is
what stops a look-alike collecting an update meant for someone else's fleet.

Both are answerable **unconditionally**, by design: a locked-down unit still has
to be able to say "is this update for me?", and the block carries no protocol
detail — two opaque strings, two opaque numbers and a key nobody can read.

### The configuration's half

The document carries a `FleetIdentity` (which fleet this file is *for*) and an
`UploadPolicy` (how strictly a device must match), both in
`src/model/access_keys.h`. `sameFleetAs()` compares vendor and model
**exactly** — they are identifiers, not display names, so case and spacing matter.
Version and serial are deliberately not part of it: matching is the "is this my
fleet" question, version is the "is this a step forward" question and serial is
the "is this the right car" question, and they are asked separately so a customer
cannot be walked backwards onto a configuration whose problems are already known.

`FleetIdentityDialog::copyFromDevice()` fills the document's identity from the
connected unit, so building an update for a device in front of you does not mean
retyping its vendor and model and getting one character wrong.

The fleet key never appears in a plain `.ct3`. `FleetIdentity::toJson()` takes
`includeFleetKey` explicitly at every call site and only the encrypted `.ct3s`
body passes `true`, because putting a fleet secret into legible JSON that people
mail around would give away the attestation itself.

## Upload Configuration

**Online → Upload Configuration…** (`src/ui/upload_dialog.*`). Send Configuration
is the engineer's command: it asks few questions and assumes you know what is on
the bench. This is the one you hand to a dealer, an installer, or a customer. It
opens a `.ct3s`, reads the unit in front of it, and refuses to install anything
the package was not built for.

Every rule's verdict is shown **before anything is written**, because the
failure this exists to prevent is silent: a configuration that installs cleanly on
the wrong product and misbehaves a week later on a track.

| Rule | Compares | If it does not hold |
| --- | --- | --- |
| Vendor ID | exact match against the package | refuses |
| Model ID | exact match against the package | refuses |
| Serial Number | must be in the package's allow-list, when it pins one | refuses |
| Fleet Key | the device must **prove** it, not merely claim the identity | refuses |
| Config Version | whether installing this would move the unit **backwards** | warns |

There was a **Series** rule here once, comparing an opaque 32-bit id that said
which configuration line a unit belonged to *within* a model. It is gone rather
than renamed, along with the series id and series key everywhere else in the
project: Model ID is a sixteen-character string set by the same build flag at the
same moment, so "CAN Triple TD" tells two configuration lines apart better than a
hex number nobody can read off a screen — and it sat one row above Serial Number,
one letter away from it, which is the last thing a rule table wants. The cost,
stated so it is a decision and not a surprise: two lines sharing an identical
Model ID can no longer be distinguished. Give them distinct model names.

Each rule is an `UploadRule` — name, what the package wants, what the device says,
and `Pass` / `Warn` / `Fail` / `NotChecked`. Kept as data rather than rendered on
the spot so the Send path can apply the same rules without drawing the dialog;
`UploadDialog::evaluate()` is static and shared, so the two can never disagree
about what counts as a match. `NotChecked` means there was nothing to compare — an
unprovisioned unit or an unprovisioned package — and is normal on a bench unit.
`UploadVerdict::deviceReadable` is false when there is no connection or the
firmware is too old to answer, and carries the reason in `problem` — which is a
different thing from a rule failing, and is reported as such rather than as a
refusal the operator might try to argue with.

**Only a `Fail` blocks an upload** — that is the whole of
`UploadVerdict::hasBlockingFailure()`, and there is no override for one. A rule
that can be clicked past is a warning wearing a costume, and an installer handed
this dialog has no way to judge which is which.

**The version rule therefore warns and never refuses**, which is the same
conclusion reached from the other direction. Reinstalling the revision a unit
already runs is the single most common thing anyone does with an update package —
a replaced unit, a reload after a clear — and equal counts as fine for exactly
that reason. Deliberately going back to an older revision is also a normal act,
performed by someone who has decided the newer one was worse. Neither is an
error, so neither can be allowed to block; only "you are about to go backwards"
is worth remarking on, the warning says it once, and the upload proceeds. A
package whose `configVersion` is 0 makes no ordering claim at all and the rule
sits out, because a prompt that appears on every unnumbered configuration and is
always waved through only teaches people to click past the one that matters.

The two policy switches — `requireFleetKey` and `warnOnOlderVersion`, both on by
default — are **package-author decisions made in Fleet Identity when the file is
built**, not bench-time overrides. That distinction is the point: the person who
knows whether backdating is worth remarking on, or whether a fleet even has a key,
is the person who built the package, not the installer holding it.
`allowedSerials` empty means any serial in the fleet; a populated list is how a
one-off replacement configuration is stopped from installing on the rest of a
customer's cars.

**None of these rules read a byte of the device's configuration.** That is the
point: a locked-down unit can be checked completely without being opened, which is
what lets a customer take updates for a protected config they cannot read.

**What it is not** — stated here rather than left to be discovered:

- The **fleet key is shared across a fleet**, so one dumped unit compromises
  attestation for every unit built with it, and there is no revocation short of a
  new key and a reflash. It is in the binary: anyone who can read a unit's flash
  — or the `.bin` you ship — has it. The backstop is STM32G4 readout protection, a
  device programming decision rather than a code one.
- A **serial is public**. It narrows *which* unit an update reaches; it is not a
  second secret, and any host that can ask a device for its identity learns it.
- **This is not a signature and it is not DRM.** It stops the wrong file reaching
  the wrong product and stops a look-alike collecting someone else's update.
  Nothing in it prevents someone who already holds the package from doing as they
  like with their own tooling.

The nearest neighbour worth distinguishing is the **v18 UID binding**, which pins
a configuration to one chip by its silicon serial. That is a different mechanism
answering a different question: binding says "only this chip may run this image",
where the fleet identity says "this device is one of ours, and this package was
built for it". Serial pinning in an `UploadPolicy` sits between the two — it
narrows the install by a number *you* chose, and unlike the UID binding it is a
check the host performs, not one the device enforces on itself.

## Send Secure Configuration

**Online → Send Secure Configuration…** (`MainWindow::onSendSecureConfiguration()`),
directly under Send Configuration in the menu because it is the same verb with a
different subject. It reads a `.ct3s` and installs it **without ever putting its
contents on screen**.

The gap it fills is one the other two commands leave open. Send Configuration
sends the document you are looking at. Upload Configuration *loads* a package into
the document and then sends that. Both leave the configuration sitting in the
application afterwards, which is exactly right when the configuration is yours and
exactly wrong when it is not: hand either to a dealer and "install this update"
quietly becomes "and here is the CAN protocol, have a browse". The whole `.ct3s`
arrangement exists so a customer can deploy a configuration they are not entitled
to read, and it would be undone by the app opening the file for them.

**Nothing about the package reaches the document, and nothing reaches the screen.**
The file is decoded into a `Configuration` declared inside the function, used only
to produce the device tables, and destroyed on the way out; `m_config` is never
read and never written, so the operator's own work is still on screen and
unmodified when the install finishes. Nor is any of it displayed: not a message
count, not a channel name, not a bus rate, and deliberately not the per-stage
progress text — `ConfigTransfer` narrates "Sending channels (64/82)", which is
harmless-looking and still counts the package's contents out loud, so the progress
dialog takes the bar and drops the caption. A mapping failure reports **how many**
errors there were and not one word of what they were, because every mapper error
names a row. The line being held is between a file the app declines to show and a
file the app shows in pieces.

**The rules are the uploader's, and so is the answer to a failure.**
`UploadDialog::evaluate()` runs here too, before the Send password is asked for and
before a single record goes out — a package that does not belong on this unit must
never get as far as partially overwriting it. A `Fail` refuses outright, with each
failure on its own line naming its field: *this package was not built for this
device* is true and useless to the person holding the laptop, where *Vendor ID
incorrect — this package is for "Acme", the device reports "Other"* tells them
whether they picked up the wrong file or the wrong unit, which is the only thing
they can act on. Warnings are appended to the confirmation and got on with. This is
a deployment command rather than an engineer's, so unlike Send Configuration there
is no "anyway".

A plain `.ct3` chosen here is refused by name rather than quietly sent.
`Configuration::peekFile()` reads the header first, and a file that is not sealed
has no business being installed by the command whose entire premise is that its
contents stay closed — the message points at Send Configuration or at Save Secure
Config… instead. A package that *is* sealed under "Require access password for
use" prompts for that password before anything else, because there is nothing to
send until the file can be decoded at all.

One case needed deciding rather than inheriting. When the identity cannot be read
at all — firmware older than the fleet commands, or a read that simply failed —
`evaluate()` returns no rules, and "no rule failed" is then trivially true. That is
the right answer for Send Configuration, where the operator owns both ends and an
unaskable bench unit has to stay usable. It is the wrong answer here, so the demand
comes from the **package**, exactly as it does for a blank field: one that names no
fleet and pins no serial asks nothing and installs anywhere, which is what an
unbadged development package should do, while one that names a fleet is refused
rather than installed on faith.

**The honest boundary**, so nobody mistakes this for more than it is: the bytes are
decrypted in this process, because they have to be to be sent at all. This
withholds them from the UI; it does not withhold them from someone instrumenting
the application. What makes a package genuinely unreadable is saving it with
"Require access password for use" — and then it cannot be installed here without
that password either, which is the trade its author made.

- **File**: New, Open…, Save, Save As…, **Save Secure Config…** (the binary
  `.ct3s`), Check Channels (live validation report with unused-channel cleanup),
  Config Summary… (MoTeC-style Channel Summary Report via
  `src/model/config_report.*` — usage analysis incl. incomplete/unused channels,
  print / PDF / text export), Reveal / Conceal Protected Communications,
  recent files, Exit. Document = one config file: `*.ct3` indented JSON by
  default, `*.ct3s` when the protocol detail has to be unreadable (see "The
  .ct3s container"). Open… decides which reader to use from the file's magic.

  **What the three severities mean** (`src/model/validation.cpp`; Send
  Configuration refuses to run while any **Error** exists, so the ladder is a
  policy decision, not decoration):
  - **Error** — the device cannot be given this configuration, or a row was
    left half-finished: a length or CAN ID out of range, a signal that doesn't
    fit its frame, a duplicate receive ID, a calculation with no output, an
    input set to "channel" with no channel chosen, a blank table axis.
  - **Warning** — the configuration is buildable but a specific behaviour is
    surprising. The channel-level one is **two things writing the same
    channel**: the device has one slot per channel, so the writers overwrite
    each other and whichever runs last wins. Every writer counts — receive
    rows, math, conditions, counters, timers, integrators, constants *and*
    table outputs — with an inactive row counting as no writer at all, and one
    warning raised per channel naming all of them.
  - **Info** — worth knowing, never a problem. Notably: a channel referenced
    somewhere that nothing writes *yet*, which simply reads its default value.
    Referencing a channel — transmitting it, using it as a calculation input or
    a table axis — reads the value and never alters it, so it is never an Error
    or a Warning no matter how many sites do it.

  The device mapper agrees, and that is what makes the Info honest: every read
  resolves through `signalFor()`, which returns the channel's canonical value
  slot and **creates a virtual one** (`SIG_MSG_NONE`, factor 1 / offset 0) when
  nothing writes it. A transmit row has always worked this way; math,
  condition, counter, timer, integrator and table-axis inputs now do too, so a
  configuration that references a not-yet-written channel maps and sends
  instead of being refused. The firmware zeroes `g_signal_values[]` at init and
  only ever writes a slot from something that owns it, so an unwritten slot
  reads 0.0 — the "default value" the UI promises. What is *still* a mapping
  error is a **blank** input that is set to "channel", since there is no slot to
  read, and a full signal table (every distinct referenced channel costs one of
  the `MAX_SIGNALS` slots, which the Device usage line reports).
- **Connections → Communications…** — "Communications Setup" dialog:
  tabs **CAN 1 / CAN 2 / CAN 3**; per-tab `Options` row (Mode: CAN/Off,
  Rate: 1M/500k/250k/125k — display-only note that current firmware hardcodes
  bus bring-up); `Sections:` list (columns Section | Name) with button column
  **Select… / Import DBC… / New… / Edit… / Remove / ↓↑ / Remove All**; right
  pane `Channels:` listing channels of the selected section; "N available"
  count under the list; OK/Cancel.
- **CAN Communications Setup** (section editor; from New…/Edit…) — two tabs:
  - *Parameters*: Device (Off / Receive Message / Transmit Message),
    Alignment (Normal = big-endian, the default / Word Swap =
    little-endian), Receive Timeout ms + "Default value on timeout", Diagnostic
    Channel (Select/Clear); group *CAN Settings*: Address Format
    (Standard/Extended) + "CAN FD frame" checkbox (enabled only when the bus
    has an FD Data rate set), Base Address (hex,
    `0x%03X` when Standard / `0x%08X` when Extended), Message Length (plain
    text field, default 8; 0–8 classic or 0–8/12/16/20/24/32/48/64 with FD),
    Transmission (Cyclic/Triggered), Transmit Rate (Hz dropdown), and — shown
    only while **Triggered** is selected — **Transmit Condition** and a
    **Reset User Condition once Triggered** tickbox; *Gateway
    Routing*: bus checkboxes selectable only while "Route this message to"
    is checked. Load…/Save As… template buttons.

    **What Triggered means, and why the combo stores a channel name.** A
    Triggered message goes out only while its User Condition's output is true;
    the rate still caps how often, so the two settings are deliberately
    independent — the condition decides *whether*, the period decides *how
    often*. Transmit Condition is a combo listing the document's User
    Conditions rather than the channel picker, because the requirement is
    literally "only User Conditions": the picker offers the whole catalogue by
    design and has no filter, and adding one would put a **New Channel…**
    button in front of a choice where creating a channel means nothing. What
    it stores is the condition's **output channel**, not its row number,
    because a `ConditionRow` has no name and no stable id — an index would
    silently re-point at a different condition the moment a row above it was
    inserted, deleted or reordered. The output channel is the one handle a
    condition really has, it is unique (two conditions writing one channel is
    already a validation warning), and it comes free with the rename walk that
    repoints every other channel reference. `mapToDevice` resolves the name to
    the index the wire wants and `mapFromDevice` resolves it back.

    A condition the document no longer holds stays in the list marked
    **"(missing)"** rather than being dropped: losing it here would quietly
    rewrite the section to a different trigger the next time anyone opened the
    dialog, and a dangling reference is validation's to report. It reports it
    as an **Error**, both for a missing condition and for Triggered with none
    selected — mapping either to a message that transmits continuously is the
    one outcome the author certainly did not ask for. Leaving the two fields
    filled in on a *Cyclic* section is only an **Info**: the mapper ignores
    them, but they are still on screen reading as though they did something.
  - *Received (or Transmitted) Channels*: Message Type (Single/Compound);
    Single → checkbox list of channel rows + **Add… / Change… / Remove**;
    Compound → `Identifiers` table (Number | Offset | ID | ID Mask,
    Change…/Clear) and per-identifier channel list titled "Channels (ID n)".
    Switching Single→Compound warns it clears the channel list (Yes/No).
  - *Frame Layout* (`src/ui/bit_layout_table.*`), across the full width under
    both panes — a DBC-style bit map of the message: a row per byte, eight
    columns for the bits inside it, 7 (most significant) on the left to 0 on
    the right. **Every cell is labelled with its global bit index** (`byte × 8
    + bit`), which is exactly the number a channel's Start Bit field takes, so
    the map doubles as the index table you read a position off.

    It exists because a signal's bits are *not* a contiguous run of cells.
    Motorola (Normal) fields walk **backwards** a byte at a time, so "start bit
    8, 16 bits" covers byte 1 then byte 0 — a shape two numbers cannot convey.
    Occupied cells are filled with their channel's colour, **the channel list
    above carries the same colour on its rows** (via `ColorItemDelegate`, so it
    survives hover and selection), and clicking a channel redraws its bits in
    that hue at full strength, bold. Clicking a coloured cell selects the
    owning channel — the map drives the list as well as following it.

    Red is reserved: it means **two signals claim one bit**, the thing the map
    is best at revealing, and no channel is ever drawn red (the hue palette
    skips the red band entirely for that reason). A caption under the grid
    spells the selection out — "start bit 24, 12 bits: bits 24–35, byte 3 to
    byte 4" — and warns when a field runs off the frame or past the message
    length.

    **How long the map is.** A transmit message *is* the frame it composes, so
    its rows are its **Message Length**. A receive message takes whatever the
    sender puts on the wire, so it lays out the whole frame the bus can carry —
    **8 bytes classic, 64 CAN FD** — and greys the bytes past this section's
    own Message Length, which is the point beyond which `computeExtraction`
    refuses to read. A compound section shows **one identifier at a time** (the
    selected one, named in the group title): its identifiers are mutually
    exclusive frame variants that may reuse bits on purpose, so merging them
    would invent overlaps. The map follows Message Length as it is typed, and
    redraws on Alignment, CAN FD, and Receive/Transmit changes.

    The bit walk itself is `rowBitPositions()` in `device_mapper.cpp`, shared
    with validation's overlap check — what the user is shown and what they are
    warned about cannot drift apart. Covered by `testBitLayout` in
    test_roundtrip, which pins the Intel and Motorola traversals against
    `computeExtraction` and checks that adjacent-but-not-overlapping fields
    raise nothing.
- **Add Comms Channel** (row editor, DBC-style) — Channel + **Select…** (a
  Receive row *writes* its channel so it opens the **output** picker; a
  Transmit row *reads* one so it opens the **input** picker — see Select
  Channel below. A note line under the preview says one of two things, and is
  coloured accordingly: a transmit row whose channel has no generator *yet* is
  dimmed information (the frame carries the default value until something
  writes it — transmitting is a read, so any channel may be sent, from any
  number of messages), while a receive row naming a channel something else
  already writes is the warning colour. Neither blocks OK);
  Default Value (in the channel's units and **at the channel's own Decimal
  Places** — the same number Edit Custom Channel sets, so a 1-dp channel cannot
  be given a default of 12.345; it rounds to 12.3 on the spot. The decimals are
  shown at **fixed width**: a 2-dp channel reads `0.00`, a 3-dp one `1.500`.
  This is the one numeric field here that does *not* trim trailing zeros —
  `TrimmedDoubleSpinBox::setTrimTrailingZeros(false)` — because its decimal
  count states how precisely the channel holds the value, where Factor's and
  Offset's 8 places are only the widest input they accept. A row naming a
  channel outside the catalogue keeps the trimmed 8-place field: there is no
  precision to state, and narrowing it would round a stored default on nothing
  but a guess. One step is one count of the channel's Base Resolution, floored
  at the smallest digit displayed so an unstated or over-fine resolution can't
  step invisibly or by a whole unit. Disabled until
  a channel is selected); **Start Bit** (0–511), **Bit Length** (1–64), **DBC
  Type** (Unsigned / Signed / IEEE754 — IEEE754 forces a 32-bit length and
  disables the field), **Bit Resolution** and **Offset** (`physical = raw ×
  Resolution + Offset`, live preview — the same two numbers whichever way the
  message goes; the device inverts them when transmitting, so a transmit row is
  NOT entered with the signs reversed), and on a TRANSMIT row **Clamp to Signal
  Limit** (ticked by default — unticked, the field carries the low `bitLength`
  bits and the count rolls over: 256 into 8 bits is 0, not 255).
  These map 1:1 onto the signal record,
  so Get Configuration reads them back exactly. Numeric fields are plain inputs
  without spin arrows. OK/Cancel.
- **Select Channel** — single searchable list of the document's channels
  (all channels are user-created; there is no predefined catalogue or
  Category tree). Search box with *any-order substring* matching — every typed
  word must appear somewhere in the name, so "temp eng oil" finds Engine Oil
  Temp and "set" finds CruiseSetSpeed (a word-prefix match could never find an
  inner word of a run-together name). Text containing regex metacharacters is
  used as a **regular expression** over the whole name instead (`^Cruise`,
  `Speed$`, `set|limit`); an invalid pattern falls back to substring matching so
  a half-typed `(` doesn't blank the list.

  **The picker has two roles, and every call site has to declare which** — but
  the roles describe what the site *does to* the channel, and only one of them
  carries a guard rail. **Referencing a channel is not editing it.** Reading a
  value — transmitting it, feeding it to a calculation, driving a table axis —
  leaves the value exactly as it was, so there is no such thing as too many
  readers and no combination of readers to protect the user from. The single
  real conflict is two things *writing* one channel, because the device has one
  slot per channel.
  - **Output** (a receive comms row, a math / condition / counter / timer /
    integrator / table output) — the site *writes* the channel: **New… /
    Edit…** are offered, and "(allocated)" marks channels a comms row already
    uses. A channel something else already writes is listed in the warning
    colour with the source named ("— already written by CAN 1 · Receive
    0x640"), and choosing it asks for confirmation: two writers overwrite each
    other and whichever runs last wins (Check Channels reports this as a
    Warning, never an Error, so it does not block Send). The confirmation says
    explicitly that *reading* the channel elsewhere is unaffected. Re-picking
    the value the site opened with is never a conflict — that writer *is* this
    site.
  - **Input** (a transmit comms row, math / condition inputs, counter and timer
    triggers, an integrator input, a table axis) — the site *reads* the
    channel. **The entire catalogue is listed, unfiltered**, however many other
    sites already read the same channel. **New…** is offered here too, so a
    configuration can be built out of order (name the channel at the transmit
    row, add the receive row that fills it afterwards). A channel nothing
    writes yet is annotated "— no generator yet, reads its default value" in
    the *neutral* dim colour, not the warning colour, and is fully pickable;
    the note under the list says the same thing and adds that reading it
    changes nothing. Nothing here blocks OK.

  **The live view.** Every grid dialog edits a local copy and writes it back
  only on OK, so mid-session the document is stale in *both* directions: it
  lacks rows just added and still carries rows just deleted or re-pointed. Each
  dialog therefore hands the picker a `ConfigPatch` — a closure that re-states
  its own slice ("my math rows are really these", "this bus's sections are
  really these") — and `Configuration::buildLiveView()` applies it to a scratch
  copy of the document. Everything the picker judges against comes off that
  copy, never the document, so a channel created seconds ago is offered and one
  whose only generator was just deleted stops being flagged. The section editor
  composes: Communications Setup's patch re-states the working buses, and the
  section editor layers its own in-progress section over the top at its index.
  The picker rebuilds the view after **New… / Edit…** too, since a rename there
  rewrites every reference in the document. Document state — file path, dirty
  flag, title — is deliberately *not* copied, so a scratch view can never be
  mistaken for a document. Covered by `testLiveView` in test_roundtrip.
- **Edit Custom Channel** — Channel Name (capped at `MAX_CHANNEL_NAME_BYTES`
  = 31, the device label budget; the field stops typing there and the dialog
  re-checks the UTF-8 byte count, which a non-ASCII name can exceed within that
  character cap); *Channel Details* pane with Channel
  Type (quantity: Unitless, Temperature, Pressure, …), required Data Type
  (boolean / u8 / u16 / u32 / s8 / s16 / s32 / float — blank until chosen),
  Decimal Places (capped per type: boolean locked 0, u8/s8 ≤ 2, u16/s16 ≤ 4,
  u32/s32/float ≤ 8), and Display Units. Base Resolution (10^-decimals) and
  Range Minimum/Maximum (type range × resolution) are derived, read-only,
  shown at the Decimal Places precision.
- **Calculations → Integrators…** — up to `MAX_INTEGRATORS` (8) rate
  accumulators, each counting **up or down**. Every step moves the output
  channel by the input, `rateHz` times a second: `output += input`, or
  `-= input` in **Count down** mode. This is **raw accumulation, not
  `input × dt`** — the rate scales the result, which is what makes it worth
  setting (10 Hz moves ten times as fast as 1 Hz on the same input). To turn a
  rate channel into a true total, pre-scale it with a Math channel so one step
  moves the right slice.

  A **decrementor is a Count down row with a Starting value at its peak** and
  the Minimum as its floor — there is no separate feature, because the device
  runs both directions through one table and one evaluation pass. The Starting
  value is loaded when the *configuration* loads (power-up or Load from Flash),
  not on a reset edge, so a decrementor begins full. Start and Reset are
  separate fields: they usually match, but keeping them apart lets a row boot
  full and reset to a different mark.

  The input is a channel or a fixed value (a fixed value at N Hz is a
  configurable ramp); an optional Enable channel gates accumulation and
  **freezes** the step phase rather than dropping steps; an optional Reset
  channel reloads its value on a rising edge, even while disabled. Min/max clamp
  the value (max ≤ min turns clamping off).

  **Preserve value** retains the running total across power cycles, restoring it
  in place of the Starting value at the next boot. It shares the counters'
  20-entry ring, so validation counts the budget across *both* and errors when
  the combined total exceeds it. It works on **either** flash mode — the ring's
  geometry is derived from the DBANK bit at runtime (FIRMWARE-NOTES #18). The
  thing the UI does say out loud, because it is otherwise invisible: a running
  integrator changes constantly, so unlike an event-driven counter it writes on
  nearly every 60 s flush, making it the dominant consumer of both the ring's
  space and its erase budget — validation reports the resulting erase cadence.
  Without Preserve, validation warns about a row with no reset channel; *with*
  Preserve and no reset channel it warns harder, since nothing can ever clear
  it.
- **Calculations → Math Channels… / User Conditions… / Timers… / Up / Down
  Counters… / Constants…** — grid editors mapping 1:1 onto firmware
  `MathConfig` (op, A, B, C, dest), `ConditionConfig` (A op B → boolean output
  channel), `TimerConfig` (start/stop channel, count up/down, limit + rollover,
  start/stop set), `CounterConfig` (up/down/reset/enable or follow-changes
  input channels, min/max/reset/step, roll/preserve), and `ConstantConfig`
  (dest slot + fixed value, v6). The math op list runs from the original
  add / subtract / multiply / divide / scale / min / max / AND / OR through
  the advanced set: absolute value, negate, square root, floor, ceiling,
  round, modulo, bitwise XOR, logical AND / OR / NOT, the six comparisons
  (equality is an EXACT float compare), and the three-operand multiply-add,
  clamp, interpolate, select and wrap. An op reads one, two or all three of
  A/B/C — `mathOpArity()` in `wire_structs.h` is the one arity table — and
  the row editor, the grid, validation and the Config Summary all show and
  check only the operands the op actually uses. A **condition** is a
  pure boolean logic channel: its output is true (1) while its expression
  holds, false (0) otherwise (v5 — the earlier block/force-routing, set-value
  and mute-bus actions were removed).

  **They are called User Conditions in the interface.** The menu item, both
  dialog titles, every validation location ("User Condition 3") and the Config
  Summary heading all say so; the wire, the firmware and this document's
  structure names still say `conditions`, because the record and the table did
  not change. The menu mnemonic moved `o` → `C` with the rename — "Up / Down
  Counters" already owns U and "Constants" owns n, so C is the letter left.
  The device holds **250** of them (was 100).

  **Every User Condition output is forced to Boolean.**
  `Configuration::forceConditionOutputsBoolean()` rewrites dataType, minimum,
  maximum and decimal places *together* — a "boolean" still carrying a 0–100
  range is not one — and runs on file load, on close of the User Conditions
  editor (whether it was accepted or cancelled, since the picker's **Edit…**
  reaches the full Channel Editor from inside it), and at the end of a Get. It
  is idempotent and needs no schema version to key off; a document already
  holding boolean outputs loads bit-identical. It skips device channels, which
  are the firmware's definition rather than the document's and are the
  mapper's to refuse, not this function's to silently rewrite.

  The Get case is the one that is not cosmetic. `mapToDevice` now stamps a
  condition's destination slot with `typeOutputSignal(destIdx, "boolean", 0)`,
  which it never used to do — which is why a condition output came back from a
  device with no data type at all, or declared float while carrying nothing but
  0 and 1. Constants, lookup-table outputs and device channels each stamp their
  slot from the data type the document declares for them; a condition's type is
  **knowable without being declared anywhere**, and that is precisely the case
  that got missed. Stamping it is not enough on its own: on the wire Boolean
  **is** `SIGNAL_TYPE_UINT8`, the same eight bits as u8 and always was, so
  `inferDataType` reads every condition output back as "u8" however carefully
  it was written. The **condition table** is the one piece of unambiguous
  evidence — a channel a condition writes is a boolean because a condition
  writes it, whatever the signal record's type byte says — so the re-type runs
  from the table after the catalogue is installed. Without it, one Get would
  un-type every condition output in the document.

  **v14: a condition holds up to `COND_MAX_TERMS` (3) comparisons joined by
  AND/OR.** The editor has a *Number of comparisons* dropdown (1/2/3) that shows
  or hides the extra comparison groups and the AND/OR selector between each
  pair, plus a live "Evaluates as:" preview. On the wire, `ConditionConfig` (35
  B) is `ConditionTerm terms[3]` (10 B each: A index, op, B type/index/const)
  plus `dest_signal_idx`, `term_count`, `joiners` and `is_active`; `joiners`
  packs one bit per gap (0 = AND, 1 = OR). Commands 0x08/0x09 are **reused** —
  the record size changed 13 → 35, so a version mismatch fails the length check
  cleanly instead of being misread. The fold is **strictly left to right**,
  `((t0 J0 t1) J1 t2)` — deliberately NOT C's "&& binds tighter than ||" — and
  the editor, the Config Summary and the conditions list all print that exact
  bracketing (shared `joinConditionTerms()`), so `A OR B AND C` reads and
  evaluates as `(A OR B) AND C`. Every term is evaluated (no short-circuit), so
  a pass costs the same each time. A 1-comparison condition is byte-identical in
  behaviour to pre-v14, and pre-v14 `.ct3` files — which store the single
  comparison inline on the condition object — load as a 1-term condition.
  A **constant** is essentially a custom
  channel (Name + Data Type + Decimals, range derived from the type) with no
  Channel Type or Display Units, carrying a fixed Value the firmware writes to
  its generated channel every pass (before math, so math can read it); the
  dialog also registers a catalogue channel so the constant is referenceable.
  Counter and timer inputs are boolean
  channels (true = value > 0, rising-edge triggered); all of these outputs are
  ordinary generated channels, so a condition's output can drive a
  counter/timer, feed math, or be transmitted. A counter's **Preserve value**
  makes its output survive power cycles — the firmware retains up to 20 such
  values (shared with integrators since v17) in a small flash ring and restores
  them at boot. It works on either flash mode, and is reformatted on any config
  change so a value never reattaches to a different counter. See
  FIRMWARE-NOTES #18.
- **Online** — Send Configuration **F5** (clears, writes, verifies, and saves
  to flash in one step so it reloads at power-up), **Send Secure
  Configuration…** (sends a `.ct3s` straight to the device without opening it or
  showing any of it — see that section; it sits immediately under Send because
  the two are the same verb with different subjects, and adjacency is what makes
  the distinction findable), Get Configuration, Verify
  Configuration, Monitor Channels **F3** (live grid from value stream),
  CAN Viewer (raw frame monitor + inject-frame form; buffers up to 10M frames
  and exports them as a Vector ASCII `.asc` log via "Save to File…" — classic
  frames as the MoTeC-style line, CAN FD frames as Vector `CANFD` lines carrying
  real BRS and ESI), Load Device Config from Flash, Clear Device Config, Device
  Status, **Upload Configuration…**, **Set Access Passwords…** and **Fleet
  Identity…**. All three need a connection: the uploader has a device to check
  against, the access keys live in the device rather than in the document, and
  half of the Fleet Identity dialog is read out of the unit in front of it.
  Send and Get each prove the matching access password first via
  `MainWindow::ensureDeviceAccess()`, which for Edit Protected Comms tries the
  key the session already holds — typed earlier, or carried inside a `.ct3s` —
  before asking, so a customer deploying a locked configuration is never prompted
  for a password they were never given.
- **Tools → Channel Editor…** — every channel in the document in one sortable
  table: Channel, Data Type, Dec, Resolution, Minimum, Maximum, Unit, Default on
  Timeout (shown only where a receive section actually enables it, i.e. the
  condition that makes the mapper emit a timeout), and Source (the CAN section
  or the calculation that generates it). Same search box as Select Channel;
  **New… / Edit…** reuse Edit Custom Channel. A channel whose data type cannot
  represent its own range is flagged in red with the type that would fit —
  an integer channel stores a scaled integer, so its reach is
  `rawRange × 10^-decimals`, and the device clamps to the channel's range.
- **Tools → Connection Settings…** — COM port list (ST-Link VCP highlighted),
  baud (default 7,372,800), Connect/Disconnect.

## 3. Channel & scaling model (MoTeC semantics, mapped to firmware)

**Signal record.** `CanSignalConfig` is **64 bytes** and `MAX_SIGNALS` is
**1000**. The history is worth knowing, because the record has been both ways:
it was 72 B, and at 500 entries the signal table was 69% of the whole config
image — the only place meaningful capacity existed. v15 cut it to 48 B by
shrinking `label` 32 → 16 B (the firmware never reads it; it exists only so a
Get can rebuild channel names, so 44% of the record did no runtime work),
hand-bit-packing the eight small fields behind accessors in both `protocol.h`
and `wire_structs.h`, and narrowing `mux_id`/`mux_mask` to 16 bits. The
bit-packing and the narrow mux fields stay. The label does not: the config
region grew to 96 KB, which bought the 16 bytes back and 232 signal slots with
them, and 32 + 20 + 8 + 4 = **64** happens to be exactly `PAD8(64)` — the
record fills its flash slot with no padding waste, which is the arithmetic that
made 32 the right number to go back to rather than 24 or 48.

Because the packing is duplicated in two headers, `test_firmware_link` packs
with the GUI's setters and reads back with the firmware's — the only place both
headers are visible — and pins every field offset to a literal, since sizeof
agreeing proves nothing about where `factor` starts. Two visible consequences:
channel names are capped at **31 bytes** (`MAX_CHANNEL_NAME_BYTES` =
`SIGNAL_LABEL_LEN - 1`, up from 15) — the name editors and DBC import enforce
that budget up front, and the mapper still clips + warns for documents saved
before the cap existed, since a silent truncation merges two channels on
read-back — and a compound multiplexor must fit a **2-byte** selector window
(DBC import rejects wider ones and the mapper errors rather than truncating,
since a truncated mask would read as 0 = "always active" and silently un-gate
the signal). The name budget is a **byte** count, not a character count: the
dialogs stop typing at 31 characters, but one non-ASCII character is 2-4 UTF-8
bytes, so a legal-looking 31-character name can still overrun the label — which
is why the dialogs re-check the encoded length and the clip stops on a
codepoint boundary rather than leaving half of one behind.

`WRITE_CHUNK_SIGNALS` is 7 records per frame at the raised payload cap (it was
2 at 48 B and the 112-byte cap, and 1 before that), so a full Send spends
roughly a third of the round trips on signals that v15 needed. `READ_CHUNK_SIGNALS`
went the other way, 42 → **31**: the read cap did not move and the record grew.

A compound transmit identifier that the author configured but gave no channels
to still reaches the wire. The device has no stored identifier list — it infers a
compound message's variants by walking that message's SIGNALS
(`collectMuxIdentifiers`), which is why an empty identifier used to infer nothing
and never transmit. The mapper emits one **selector-only** signal per such
identifier (`SIG_FLAG_SELECTOR_ONLY`, msg_and_flags bit 12); `composeVariant`
skips it when packing, so the frame carries its selector over a zeroed payload.
Storing the list per message instead would have cost `MAX_MESSAGES ×
MAX_TX_MUX_IDS` records for something the signals already imply. On Get the flag
is what rebuilds the identifier without inventing a channel row for it — the
signal carries no label, and a blank label would otherwise reconstruct as a
phantom "Signal 12" channel.

Comms rows are **DBC-native**: `physical = raw × Bit Resolution + Offset`,
which is exactly what the device stores (`factor`/`offset` as two IEEE-754
binary32 values). The labels lost their `DBC` prefix in v18 — the number says
how much one raw count is worth and reads the same in both directions, which
"Factor" left ambiguous — but the model fields and the `.ct3` keys keep
`dbcFactor`/`dbcOffset`, because renaming a stored key would invalidate every
saved configuration to no purpose.

The row's `clampToRange` is the one field stored INVERTED: the wire bit is
`SIG_FLAG_TX_WRAP` (msg_and_flags bit 11), set to mean *wrap*, so that a record
written before the flag existed — all zeros there — means *clamp*, which is what
it did. `device_mapper` inverts once in each direction and nothing else in the
GUI knows about the polarity. Wrapping skips BOTH clamps in the firmware's
`inverseSignalScaling`, the channel-range one included, because that one runs
first and would otherwise settle the answer before the field width mattered.
Receive ignores the bit entirely.

The mapper writes `sig.factor = float(dbcFactor)`,
`sig.offset = float(dbcOffset)`, `decimal_places = 0`, and uses the channel's
range for min/max (the firmware **always clamps**; defaults would clip to
0…255). Because the row fields map 1:1 onto the signal record, Get
Configuration reconstructs each row verbatim — no scaling is approximated (the
only lossy step is the user's double being rounded to float32 on the first
Send, and that value then round-trips stably).

Because that channel range becomes the device's clamp, **a channel's storage
type is chosen from its physical range and decimals, never from the signal's
raw bit width** (`storageTypeForRange()` in `dbc_import.cpp`, used by both DBC
import and `mapFromDevice`'s read-back). An integer channel holds a scaled
integer, so its reach is `rawRange × 10^-decimals`: a 16-bit field at 0.036
km/h per bit spans 0…2359.26 and needs 3 dp, which `u16` can only carry to
65.535 — sizing it by width would clamp the channel at 65.535 km/h. The same
rule fixes signedness, which follows the *physical* range rather than the DBC
sign flag, so an unsigned field with offset −40 becomes a signed channel
instead of clipping to 0. A span no integer type can hold falls back to
`float`, and that channel's range is then left **intact** — the dialogs' ±1e9
float span is a display convention, not a storage limit (`min_val`/`max_val`
reach the device as float32), so trimming to it would clamp away most of a wide
signal: J1939 High Resolution Total Vehicle Distance (32-bit, 5 m/bit) really
does span 0…2.1e10. For the same reason **Edit Custom Channel preserves a range
wider than the type's displayed span** unless the user actually changes the type
or precision, so opening a channel to fix a typo cannot silently clamp it.
`dbcPhysicalRange()` derives the span from both raw endpoints through
factor/offset; it is meaningless for an IEEE754 signal (whose bits are the
value) and is bypassed for those.

Bit extraction: rows carry **Start Bit + Bit Length** directly, plus a **DBC
Type** (Unsigned → `UINT8/16/32`, Signed → `INT8/16/32` by width, IEEE754 →
`FLOAT`, 32-bit). The section's Alignment picks the byte order for the whole
message:
Bit numbering: the frame lays out as `7,6,5,4,3,2,1,0, 15,14,...,8, 23,22,...`
— bits count 0..7 right-to-left within a byte (bit 0 = LSB), bytes count
left-to-right from 0, and bit `S` sits at `byte S/8, bit S%8`. **The start bit
is the signal's LSB for both byte orders**; the walk ascends the bit within
the byte, then steps to the next byte (Word Swap → firmware Intel,
`byte_order 0`) or the previous byte (Normal → firmware Motorola,
`byte_order 1`) — the device's `dbc_decode`/`dbc_encode` convention.
- Word Swap 16-bit at start 0: `value = data[1]<<8 | data[0]`.
- Normal 16-bit across bytes 2 (MSB) and 3 (LSB): LSB = byte 3 bit 0 →
  `start_bit 24` (`value = data[2]<<8 | data[3]`); the top-of-frame field
  (`data[0]<<8 | data[1]`) is `start_bit 8`.
`computeExtraction` walks this exact traversal to validate the field fits, and
produces the wire fields for Send; on Get the row is rebuilt straight from
`start_bit` / `bit_length` / `value_type` / `factor` / `offset`.
- Compound messages are captured in the document model, but the firmware
  matches messages by CAN ID only — upload validation reports compound
  sections as requiring a firmware update.

**Message table (v3): one unified 500-entry table.** Direction is a per-message
flag (`MSGFLAG_TRANSMIT`), so receive and transmit messages draw from the same
500 — mixed freely across the three buses. **500, not 512, and that is a hard
ceiling rather than a round number:** a signal's parent message index is the
9-bit `SIG_MSG_IDX_MASK` field inside `msg_and_flags`, so 0..510 are addressable
and 511 is the `SIG_MSG_NONE` sentinel that marks a virtual signal. 500 leaves
11 spare; 512 would not fit, and the symptom would not be a rejected
configuration — message 511's signals would read back as virtual and quietly
detach from their message. Widening this axis again means widening the field
first. `#define MAX_MESSAGES` in `protocol.h` says so at the definition. A transmit message carries
`period_ms`; its channel rows are packed (physical → raw via the inverse of the
same scaling) and the frame is composed and sent on schedule. A transmit
signal's value slot is the channel's canonical slot, encoded in the signal's
`unit_type`/`unit_val` as "source index + 1", so a channel received in one
message can be re-transmitted in another.

**Triggered transmit:** a transmit message carries `tx_trigger_cond` (the index
of the condition whose boolean output gates it; `TX_TRIGGER_COND_NONE` = 0xFFFF
when unset, so a Get cannot invent condition 0 out of an empty field) and
`tx_trigger_flags` (`TXTRIG_ENABLED` 0x01, `TXTRIG_RESET_ON_TX` 0x02). Both were
taken **in place** from three of the four bytes of the retired per-message key,
so `CanMessageConfig` is still **14 bytes**, `PAD8(14)` is still 16, no table
offset moved, `CFG_TOTAL` is unchanged, every chunk constant stands, and the
feature cost **zero config flash**. `PROTOCOL_VERSION` stays 1 for the same
reason: the record is the same size, so an older host's write still satisfies
the length check — and it writes zeros there, which decode to "cyclic", exactly
the behaviour it intended. The flags need a byte of their own because
`CanMessageConfig.flags` has none free: 0x01–0x20 are the `MSGFLAG_*` set and
0x40/0x80 are the `MSGPROT_*` level, whose values are pinned.

`engine_service_transmit` reads the condition's **published value slot** at the
5 ms transmit slot rather than re-evaluating the expression. That is what makes
the 200 Hz claim true and cheap: `executeConditions` already runs on every
calculation tick *and* on every matching received frame, so a condition watching
bus data is fresher than 200 Hz already and one watching calculated data cannot
beat the 100 Hz that produced its inputs — re-running it here would cost a table
pass every 5 ms and could not change an answer. While the condition is false the
message's period accumulator is **parked at `period`**, so the first slot after
it goes true transmits immediately; each triggered transmission then **zeroes**
the accumulator instead of subtracting the period, so the run is phased from the
trigger rather than dragged back onto a free-running grid. A 1 Hz message whose
condition becomes true at 1.2 s sends at 1.2, 2.2, 3.2 — not at 2.0, 3.0.

Anything that is not plainly a satisfied, active, in-range condition — the unset
sentinel, an index past the used count, an inactive record, a destination slot
out of range — makes the message **silent, never cyclic**. Treating a broken
reference as "no gate" would put frames on a customer's wire precisely when the
configuration says it does not know whether they belong there; silence is the
recoverable failure.

`TXTRIG_RESET_ON_TX` turns that into one frame per rising edge, and it needed
the engine's **first and only per-condition runtime state**:
`g_cond_consumed[MAX_CONDITIONS]` (250 B, not persisted, cleared by
`resetRuntime`). Conditions were purely combinational, which is what lets
`executeConditions` run from both the tick and the receive path without caring
how often, and the latch does not change that — it is memory of the
*transmission*, not of the expression, and it only ever forces the published
value down. It exists because a plain zero-write into the value slot would be
overwritten within milliseconds by the next `executeConditions`. A consumed
condition publishes 0 even while its expression holds and re-arms the instant
the expression goes false; clearing on `!met` rather than on a timer is what
makes this an edge trigger and not a rate limiter, and nothing has to agree on a
duration. The latch is set only after `composeAndTransmit` **accepted** the
frame, so a full outgoing ring costs one transmission rather than the whole
edge, and a power cycle re-arms everything — the alternative is a unit that
boots refusing to send because of an edge it consumed before it was last
switched off.

**Receive timeout defaults (v4):** a receive message reuses its otherwise-unused
`period_ms` field as a receive timeout in milliseconds ("Receive Timeout" +
"Default value on timeout"); each signal carries a `default_value`. When a
message is not seen within its timeout, `engine_tick()` writes each of its
signals' `default_value` into their value slots before the calculation passes,
so derived channels follow. A fresh frame resets the window. `period_ms == 0`
disables the feature (the checkbox is unchecked). This grew `CanSignalConfig`
59 → 63 bytes and bumped the protocol and flash-image versions to 4.

**Counters & timers (v3):** counter/timer inputs resolve to boolean channel
slots (0xFFFF = unused); outputs are generated channels, pre-allocated
alongside math destinations so any calculation can reference any other's
output (one-pass latency). They evaluate in `engine_tick()` at 100 Hz.

**Integrators (v16; bidirectional + retained in v17):** an 8-entry
`IntegratorConfig` (30 bytes, cmds `0x23`/`0x24`) evaluated in `engine_tick()`
after the timers. Each moves its output slot by its input `rate_hz` times a
second — raw accumulation, so the rate is part of the answer. Several details
are worth knowing:

- **The step phase counts in Hz·ms, not milliseconds.** It gains
  `elapsed_ms × rate_hz` per tick and spends 1000 per step, so a rate that
  doesn't divide 1000 (3 Hz, 7 Hz) still averages *exactly* `rate_hz` steps per
  second. Storing a truncated integer period instead would over-fire: 7 Hz as
  142 ms drifts to 704 steps over 100 s instead of 700. `test_firmware_link`
  runs exactly that case for 100 simulated seconds.
- **The enable gate freezes the phase** rather than discarding steps, so gating
  costs no accumulation, and reset restarts the phase so the first step after a
  reset lands a full period later.
- **`INTEGFLAG_COUNT_DOWN` only flips the sign** of the per-step delta (v17). A
  decrementor is this same loop with `start_value` at the peak and `min_value`
  as the floor, which is why there is no second table — two passes differing by
  a sign would be two places for the same bug.
- **`start_value` is seeded at config load AND on record write.**
  `engine_seed_integrators()` runs inside `engine_load_config` (after
  `resetRuntime()` zeroes the slots), and `engine_table_write` seeds the records
  it just wrote. The second one matters because an upload begins with
  `CLEAR_CONFIG`: without it a freshly-sent decrementor would read 0 — parked on
  its floor, looking broken — until the device was rebooted. Boot order is
  load → seed start → `preserveRestoreAtBoot`, so a **retained value overrides
  the start value**. Reversing those two would make Preserve silently do
  nothing; `test_firmware_link` pins the ordering.
- **Preserve keys namespace both tables** (v17): counters keep
  `0..MAX_COUNTERS-1`, integrators start at `PRESERVE_KEY_INTEGRATOR_BASE`, and
  the boot restore loop walks `PRESERVE_KEY_COUNT`. Without the base, counter 0
  and integrator 0 would collide on key 0 and restore into each other's slots.
  `engine_preserve_enumerate` lists counters first, so if more than
  `PRESERVE_MAX` (20) slots are flagged the integrators are the ones dropped —
  a backstop only, since validation refuses that configuration.

The reset seed is only clamped when `max > min`, matching `clampRoll`'s
"`hi <= lo` means clamping is off" convention — this is deliberately unlike the
counter, which clamps its seed unconditionally and so pins an unclamped
counter's reset to `min`. The input side does **not** go through
`resolveBoolInput` — that helper degrades a bad lookup to "no channel", which is
right for a trigger and wrong for the value being accumulated — and
`INTEGFLAG_CONST_INPUT`, not a sentinel index, selects the fixed-value mode, so
a failed lookup can never masquerade as a constant.

**Constants (v6):** a dedicated 100-entry `ConstantConfig`
(`dest_signal_idx`, `value` float32, `is_active`, 7 bytes) table — separate
from the 100 math slots. Each active constant's output channel is
pre-allocated like any generated channel, and its value slot is stamped with
the constant's `value_type`/`bit_length`/`decimal_places` so Get can rebuild
the channel definition. `executeConstants()` writes the fixed value into the
slot at the start of each evaluation pass — in both `engine_process_can()` and
`engine_tick()`, before math — so downstream calculations and transmit signals
see it. Adding the table bumped the protocol and flash-image versions to 6.

**Flash-resident config (v7, firmware-internal):** the firmware runs its config
tables directly out of flash instead of a RAM copy, reclaiming ~40 KB of RAM
(use fell from ~62% to ~32%). The bank-2 region — 48 KB with a 64-byte header
when this landed, **96 KB with a 256-byte header** today — holds that header
then fixed, 8-byte-padded record slots per table; each `WRITE_*` programs its
records in place, `SAVE` commits the header (per-table counts + bus setup + a
CRC over the header and every live record) which marks the image valid, and
boot validates it and sets the engine's iteration bounds. The engine reads
records through pointers into the memory-mapped image and iterates only the
written/validated prefix, so an un-programmed (0xFF) slot is never mistaken for
an active record; an interrupted upload leaves no valid header and boots to
defaults. Config lives in bank 2 while code runs from bank 1, so programming
never stalls execution, and the cooperative main loop never reads the region
mid-program. The flash header also carries a 32-byte **configuration title**
(`CMD_WRITE_CONFIG_NAME` / `CMD_READ_CONFIG_NAME`): the Send dialog requires a
non-empty title (≤32 bytes, defaulting to the document's Save/SaveAs base name),
stored on the document as `configTitle` and written to the header; Get reads it
back. The Send dialog also offers **Reset device after sending**, and Online →
Reset Device sends the same `CMD_RESET_DEVICE` on demand — the firmware ACKs
then reboots after a ~150 ms delay so the ACK flushes first (the config already
runs from flash, so the reboot just re-initializes cleanly from power-up state). One behavior change: `CLEAR_CONFIG` now erases
the single flash copy, so a clear also drops the persisted config (it is no
longer undone by Load-from-Flash, which existed for the old RAM+flash two-copy
model).

**Compound (multiplexed) messages + DBC import (v8):** `CanSignalConfig` gained
a per-signal mux selector — `mux_byte_offset` (u8), `mux_id` (u32), `mux_mask`
(u32). `mux_mask == 0` = always active; otherwise the engine extracts the signal
from a received frame only while `(selector & mux_mask) == (mux_id & mux_mask)`,
where `selector` is the up-to-4-byte little-endian window of the frame at
`data[mux_byte_offset..]`. A compound section carries channels **only inside
identifiers** — there is no shared always-present set (a signal present in every
frame is defined in each identifier). Each identifier's rows carry that
identifier's `byteOffset`/`id`/`idMask`. On receive, a channel repeated across
identifiers gets a distinct gated signal per identifier (each decodes under its
own selector; the first is the channel's canonical value slot). `mapFromDevice`
regroups signals by selector so Get rebuilds the compound section. (`mux_mask ==
0` still means "always active" and is used by ordinary single-message sections.) The struct grew 63 → 72 bytes (padded flash slot
64 → 72; worst-case image ~47 KB, still inside the 48 KB region; `READ_CHUNK_
SIGNALS` dropped 30 → 28 to stay under the response cap). RAM is unchanged — the
config is flash-resident.

**Compound transmit (v10):** a transmit message whose signals carry mux
selectors is composed one variant frame per identifier. The firmware detects
compound-ness from the presence of mux-gated signals; `MSGFLAG_TX_SEQUENTIAL`
picks the cadence — **Batch** (clear) sends every identifier's frame each
transmit period, **Sequential** (set) sends one per period, round-robin via a
per-message cursor. Each variant packs that identifier's signals (a channel
needed in every variant is defined in each identifier), then writes the
identifier's selector value into the frame so the receiver decodes the matching
variant. The GUI exposes this as a **Transmit Mode** dropdown
(`CommsSection::compoundTxMode`) on compound transmit sections. No wire struct sizes changed (just the new flag bit), so the
flash-image version stays at 9 and existing configs still load. **Import DBC…** (per-bus button in Communications
Setup) parses a `.dbc` file (`src/model/dbc_import.*`) into a checkable tree
(`ImportDbcDialog`); the load-bearing subtlety is that DBC stores a signal's
start bit as its LSB for Intel (`@1`, matches directly) but its MSB for Motorola
(`@0`), so `dbcStartBitToLsb()` walks the Motorola MSB down to the LSB the engine
expects. Multiplexed DBC messages become compound sections (multiplexor value →
identifier selector via `muxSelectorForValue()`); mixed-endianness or
byte-unmappable multiplexors are skipped with a warning.

**Lookup tables (v12, 1-axis widened v13, 2-axis replaced by the 8x8):** a
**Tables** entry under Calculations backs the device's lookup tables — the
1-axis table (`x_count` active of **16** `x_sites` → `outputs`) and the 2-axis
**8x8** (`x_count`/`y_count` active of **8** sites each → a 64-cell grid,
`grid[y*8 + x]`); up to 8 of each. The 8x8 replaced `Table4x4Config`, which held
4 sites per axis and 16 cells; its commands 0x1D/0x1E are retired, and a saved
4x4 loads into the top-left of an 8x8 (see the `.ct3` schema note in §1).

Both are **split across multiple wire records**, for the same reason and by two
different splits:

- The 1-axis table is `Table2x16Def` (70 B — `x_signal_idx`, `dest_signal_idx`,
  `flags`, `x_count`, `x_sites[16]`, commands 0x1F/0x20) plus `Table2x16Out`
  (64 B — `outputs[16]`, commands 0x21/0x22), because a combined 16-site record
  is 134 bytes and the payload cap was 112 when it was designed. They are two
  parallel device tables **paired by index**: `Def[i]` describes the table whose
  values are `Out[i]`.
- The 8x8 is `Table8x8Def` (**73 B** — `x`/`y_signal_idx`, `dest_signal_idx`,
  `flags`, `x_count`, `y_count`, `x_sites[8]`, `y_sites[8]`, commands 0x34/0x35)
  plus **one `Table8x8Row` record per grid row** (**32 B** — eight floats,
  commands 0x36/0x37). A combined record would be 329 bytes: over the payload
  cap even at 496, and over `MAX_PADDED_RECORD` (112), so it could be neither
  sent nor stored. Table `t` owns Def index `t` and Row indices `t*8 .. t*8+7`,
  so the row table holds `MAX_TABLES_8X8 * TABLE_8X8_SITES` = **64** records.

  **The row split is chosen for one property, and it is the whole reason for
  this shape:** `PAD8(32)` is 32, so a table's eight row slots are
  byte-contiguous in the device's flash. The engine takes a single pointer at
  row `t*8` and indexes `grid[y*8 + x]`, exactly as the 4x4 indexed
  `outputs[y*4 + x]` — no reassembly buffer, no cross-record arithmetic, no RAM.
  Any other chunking loses that, and a row record that grew by one byte would
  pad to 40 and turn that pointer into a walk across the wrong memory.

Neither `Out`/`Row` record carries flags or a count of its own; `TABLEFLAG_ACTIVE`
lives on the `Def` alone. Two guards keep a torn upload harmless: the engine
evaluates the 2x16 at `t` only while `t < min(count[Def], count[Out])` and the
8x8 at `t` only while `count[Def] > t && count[Row] >= (t+1)*8` (a slot past the
written prefix reads erased flash, i.e. `0xFF` → NaN, and a NaN in an output
channel propagates through everything downstream of it), and the GUI writes
**values before definitions** so a table cannot go live before its cells are
resident. Note the 8x8's guard is **per table**, not a global `min()` of the two
counts: with one complete table and a second table's Def written, the first must
keep evaluating while the second must not.

Tables are **partial**: only the populated sites are used (the GUI starts a
table empty and requires an output for each filled site; the model stores
variable-length site/output lists, the grid row-major over its **own** X width).
That last detail is where the two strides differ — the document packs at
`xSites.size()`, the wire always at 8 — so the mapper re-lays a partial grid row
by row into the top-left of the fixed-width records rather than copying it, and
leaves the rest zero. The engine runs `executeTables2x16` / `executeTables8x8`
right after constants each pass: it reads the axis channel value(s), resolves
each axis independently via `axisResolve()` — Interpolated (linear) blends the
bracketing sites, Discrete (`TABLEFLAG_*_INTERP` clear) snaps to the nearest site
with the transition at the site midpoint — then a single bilinear form covers
every 8x8 axis-mode mix (a discrete axis collapses to weight 0). Inputs clamp to
the end **used** sites, bounded by `x_count`/`y_count` and not by the array
width. The output is a generated channel typed like a Constant
(`Table2x16Row`/`Table8x8Row` carry `outputChannel`/`dataType`/`decimalPlaces`,
edited in a grid in the Tables dialog); the mapper allocates + types its signal
slot the same way constants do, and axis inputs resolve like math inputs.

A naming note that will otherwise trip a reader: the firmware calls the 32-byte
grid-row record `Table8x8Row`, and the GUI cannot, because `ct::Table8x8Row` is
already the DOCUMENT row — one whole table, one line in the Tables dialog — and
`device_mapper.h` has both headers open at once. The GUI's mirror is
`Table8x8GridRow`. `test_firmware_link` asserts the pair byte for byte, which is
where the two names are reconciled.

Flash: the region is **96 KB** (`FLASH_STORE_CAPACITY` 98304 — kept a multiple
of 4096 so the single-bank 4 KB page-erase arithmetic stays aligned), image
version **4**, `FLASH_NUM_TABLES` **13** (the 4x4 out, the 8x8's Def and Row in),
`MAX_PADDED_RECORD` still 112 (the 8x8 Def pads to 80, a row to 32; the retired
4x4's 112 was the peak and nothing has replaced it). `.ct3` files save the
1-axis table under a `tables2x16` key and still load the older `tables2x8` key,
and save the 2-axis table under `tables8x8` while still loading `tables4x4`.

**Message relay (v11):** a new **Message Type** in Communications Setup
(alongside Receive/Transmit Message) — a masked-ID gateway rule stored in its own
device table (`ENGINE_TABLE_RELAYS`, `RelayConfig` = 11 bytes: address, bitmask,
flags, `src_bus`, `forward_bus_mask`; `MAX_RELAYS` 32; commands
`WRITE/READ_RELAY_CFG` 0x19/0x1A). Unlike a message, a relay carries no channels:
the firmware runs `engine_process_relays()` on **every** received frame,
independent of the message table, and forwards the whole frame to the rule's
target buses when `(can_id & bitmask) == (address & bitmask)`. A rule listens on
its `src_bus` only (the bus tab it was defined on), never forwards back onto the
source bus, matches only frames of its own extended-ness, and `RELAYFLAG_INVERT`
forwards the *non-matching* frames instead. The GUI models this as
`SectionDevice::MessageRelay` with `relayBitmask` + `relayInvert` fields (the
forward set reuses `routeBusMask`); the section editor shows a **Message Relay**
group (Message Bitmask in 0x%03X/0x%08X width matching the address, Invert Result,
and Forward-to checkboxes for the other two buses) and hides the message framing
controls. This grew the flash-image version to 11 (`FLASH_NUM_TABLES` 8).

**Per-bus termination resistor (v9):** `ControlCanPayload` gained a `termination`
byte (10 → 11 bytes; flash-image version → 9). Each bus has a **Termination
Resistor** dropdown in Communications Setup (Mode / Rate / FD Data / Termination
Resistor), stored on `BusConfig::termination`, sent per bus via `CMD_CONTROL_CAN`
on Send, and persisted in the flash header so a terminated bus stays terminated
across power cycles. The firmware's `applyBusConfig` drives the board's
`setCAN_Termination(bus, on)` GPIO regardless of mode (a bus can be terminated
while Off). Like mode/baud, termination comes back on a Get only through
`CMD_READ_CAN_SETUP` — the signal and message tables carry nothing about a bus.

**Reading the bus setup back (`CMD_READ_CAN_SETUP`, 0x30):** answers
`ControlCanPayload[3]`, buses 1..3 in that order, with no request payload.

`CMD_CONTROL_CAN` had always been write-only, and that left a hole in Get
Configuration that nothing else could fill. A Get could recover every message,
every signal, every calculation — and then had to **guess** what the buses were
running, because no table on the wire records a mode or a bitrate. It assumed the
firmware's bring-up rates (CAN1 1M, CAN2 500k + FD 2M, CAN3 500k) and warned the
user to go and check. A configuration that comes back subtly different from the one
that went out is a bad answer to "what is on this device", and this guess was the
only part of a Get that was not simply true. Sending that document straight back
would then re-rate a running bus without anyone having asked for it.

Three details are decisions rather than mechanics:

- **It answers the LIVE setup**, not the stored image — what `CMD_CONTROL_CAN`
  last applied, which the firmware's glue owns because it owns the peripherals
  and `serial_proto.c` only serialises. After a boot the two are the same;
  mid-session, after a Send that has not been saved, they are not, and what the
  buses are actually running is the honest answer to "what is this device
  doing".
- **It is gated on `ACCESS_FN_GET`**, like every other read of configuration
  content. A bus map says as much about a proprietary setup as the message table
  does, so exempting it would have been a hole in the Get password.
- **`bus_idx` is stamped by the protocol layer, not trusted from the glue.** The
  reply is positional, so a host that cross-checks the index must not be able to
  be told something inconsistent with the slot it arrived in.

Host side the read is the last step of a Get and is **optional** in the same way
the relay and table reads are: firmware without the command NACKs
`ERR_INVALID_CMD`, `busSetup` comes back empty, and `mapFromDevice()` falls back to
the bring-up assumption plus its warning — the behaviour every Get had before.
Empty therefore means "unknown" and never "off"
(`ConfigTransfer`'s `busSetup`, `src/protocol/config_transfer.h`). One lossy edge
is documented rather than hidden: mode 2 (listen-only) reads back as *enabled*,
since the document models only enabled/disabled, and recording it as Off would let
a later Send silently stop a bus that is running. `mapFromDevice()` raises a note
whenever that conversion happens, so the difference between what was read and what
the document can express is on screen rather than left to be discovered on the
next Send.

## 4. Code layout

```
CANTripleDeviceManager/
  CMakeLists.txt              Qt 6.7 Widgets+SerialPort, MinGW kit
  src/protocol/               wire_structs.h (packed mirrors + static_asserts),
                              cobs, crc16, framer (packet build/split/demux),
                              device_link (QSerialPort, stop-and-wait, retries,
                              stream signals, transfer procedures),
                              device_session (identity, access keys, fleet
                              identity — short synchronous round trips; there
                              is deliberately no fleet-identity WRITE)
  src/model/                  channel catalog (user-created channels only,
                              any-order search), comms section/row document
                              model, configuration (load/save, dirty),
                              device_mapper (document ⇄ firmware tables,
                              index maps for live streams), validation,
                              access_keys (the three passwords: 4-byte device
                              keys + file verifiers; also FleetIdentity and
                              UploadPolicy, which are keyed material's near
                              neighbours), secure_file (the .ct3s
                              container), config_lock (the sealed-payload
                              primitives both of those build on)
  src/ui/                     main_window + the dialogs listed above,
                              bit_layout_table (the Frame Layout bit map)
  firmware/include/           fleet_identity.h — the build-time identity. What
                              you actually edit per unit is
                              firmware/identity.local.ini (gitignored; copy
                              identity.local.ini.example), which
                              scripts/build_flags.py turns into the -DCT_*
                              defines.
```

Send Configuration: GET_STATUS → CLEAR_CONFIG → chunked writes (msg/sig/math/
cond/counter/timer) → read-back verify → SAVE_TO_FLASH (carrying the document's
Config Version as the command's optional 2-byte payload, so the stored
configuration and the number the uploader compares against land together) →
report, all in one transfer. Get Configuration: chunked reads → the
configuration name → `READ_CAN_SETUP` for the bus modes and rates →
reverse-map into a document (lossy: names come from signal labels).
Monitor/Viewer subscribe to DeviceLink stream signals; the mapper's
signal-index→channel map labels live values.
