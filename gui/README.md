# CAN Triple Device Manager

A Windows desktop configurator for the [CAN Triple](https://github.com/mitchdetailed/CAN_Triple)
gateway (STM32G473CBT6, 3× CAN), styled after **MoTeC C125 Dash Manager** —
same layout and navigation for communications, messages, and channels.

Talks to the device over the ST-Link virtual COM port (USART1, PB6/PB7,
7,372,800 baud — ST-Link **V3** required for that rate). Built and released
together with the **firmware in [`firmware/`](firmware/README.md)**: one
repository, one wire format, one release.

**Installing rather than building?** The Windows installer — application,
device drivers, firmware and the recovery tool in one Setup — is published on
the repository's **Releases** page. The manual is browsable under `docs/` in
the published repository, and ships inside the program as Help → Contents (F1).

Capacities: **500 messages** (receive or transmit, mixed freely), **1000
channels** (a shared pool), 100 math channels, 100 conditions, 50 up/down
counters, **50 timers**, 100 constants, 32 message relays, 20 Transmit CRC8
rules, 8 integrators, and
8 lookup tables of each kind — 2x16 (1 axis, 16 sites) and **8x8** (2 axes,
64 cells). Channel names reach the device as up to **31 UTF-8 bytes**.

## Highlights

- **Connections → Communications…** — per-bus tabs (CAN 1–3) with Mode, Rate,
  FD Data and a **Termination Resistor** toggle (drives the board's 120Ω
  resistor, persisted across reboots), a sections list, and
  New/Edit/Remove, exactly like Dash Manager's Communications Setup.
- **Section editor** — Parameters (device, alignment, timeout, standard/
  extended base address with hex↔dec readout, message length) and
  Received/Transmitted Channels tabs incl. compound (multiplexed) messages —
  every channel lives under a multiplexor identifier (no shared "always" set; a
  channel needed in every variant is defined in each identifier). **Compound
  transmit** adds a **Transmit Mode** (Batch = all IDs each period, Sequential =
  one ID per period round-robin), plus a CAN-Triple-specific
  gateway-routing group. A third Message Type, **Message Relay**,
  is a masked-ID gateway rule: it forwards whole frames whose ID
  matches `address & bitmask` (or, with **Invert Result**, the non-matching
  frames) from its bus to the other two selected buses — no channels involved.
  A fourth, **Transmit CRC8**, is a transmit message that stamps a configurable
  CRC-8 checksum (polynomial / init / final XOR / reflection, computed over up
  to 15 elements — identifier bytes, frame bytes after packing, or literal
  bytes) into one byte of each frame it sends, channels packed first and the
  stamp last, and publishes the value to a channel; the device runs up to 20
  such rules across the buses.
- **Add Comms Channel** — DBC-style start bit / bit length / DBC type
  (unsigned, signed, IEEE754) with DBC factor + offset scaling
  (`physical = raw × factor + offset`), live-validated against what the firmware
  can actually extract and read back exactly on Get.
- **Import DBC…** (per-bus button in Communications Setup) — opens a `.dbc`
  file into a checkable tree of messages and signals. Pick what to receive,
  rename channels and set their Channel Type inline, then import. Motorola
  start bits are converted to the app's LSB convention automatically, and
  multiplexed messages import as **compound sections** (one per multiplexor
  value).
- **Select Channel** — searchable list of the document's user-created
  channels with any-order word-prefix matching ("temp eng oil" finds
  Engine Oil Temp); create and edit channels via New…/Edit….
- **Calculations** — Math Channels, Conditions, **Timers**, **Up / Down
  Counters**, **Constants**, and **Tables** grid editors, each mapping onto the
  matching firmware table. A constant is a custom channel carrying a fixed value
  the firmware writes every evaluation pass. **Tables** are
  MoTeC-style lookups — 8× 2x16 (one axis, up to 16 sites) and 8× **8x8**
  (X + Y axes, up to 8 sites each, 64 cells) — with each axis Interpolated or
  Discrete (centered); the looked-up value drives a generated output channel.
- **Online** — Send Configuration (F5, chunked + read-back verified, applies
  bus rates via CONTROL_CAN), **Send Secure Configuration…**, **Upload
  Configuration…**, Get / Verify Configuration,
  Monitor Channels (F3, live value stream), CAN Viewer (raw
  frame monitor + frame injection), flash save/load, device status, **Set Access
  Passwords…** and **Fleet Identity…**. Get also reads the buses' modes, rates
  and termination back off the device via `CMD_READ_CAN_SETUP`, so the document
  reflects what the buses are actually running, not an assumption.
- **Online → Set Access Passwords…** — three independent, MoTeC-style function
  passwords held **in the device**: Send a Configuration, Get a Configuration,
  and Edit Protected Comms. Each folds into a 4-byte key the device stores
  write-only and proves by challenge-response, so a serial capture is worth
  nothing next time. Send and Get are real device gates; **Edit Protected Comms
  is prove-only** — the device confirms it and gates nothing on it. It is one of
  the two things this application requires before it will apply or remove a
  **Protect Communication** marking, and it insists on a *connected device*
  confirming it. The other is that message's own **Message Password**; this is
  not a master key over it.
- **Message markings** (section editor: **Read Only**, **Hidden**, **Protect
  Communication**) are one ordered level. Read Only is visible and not editable;
  Hidden and Protect Communication also withhold the CAN detail; the channels a
  marked message carries are read-only at every level, with their names still
  visible so a customer can use the values. **Every marked message carries a
  Message Password of its own**, and moving a marking costs the current
  password — raising as well as lowering, since the password guards the marking
  rather than the message. Opening a hidden message does not leave it open:
  closing the editor with the box still ticked re-locks it. **Removal is
  permitted at every level** — in the dialog, from a script and on the device —
  because protecting a message protects its protocol, not its place in a
  configuration. All three are conventions of this application: the device
  enforces none of them, and a plain `.ct3` is unsigned JSON, so only a `.ct3s`
  makes the bytes unreadable.
- **Online → Fleet Identity…** — who a unit is (Vendor / Model / Serial Number /
  plus a fleet key it proves it holds) and which fleet a configuration
  is *for*. The device half is **read-only here on purpose: a unit's identity is
  compiled into its firmware**, set in `firmware/identity.local.ini` at build
  time — plain `CT_VENDOR_ID=Acme`, `CT_SERIAL_NUMBER=0x00000123` and the rest of
  the `CT_*` keys documented in `firmware/include/fleet_identity.h`. That file is
  gitignored and does not exist in a fresh checkout: copy
  `identity.local.ini.example` to create it, and `firmware/scripts/build_flags.py`
  turns it into the compiler's `-D` defines before every build (strings go in
  unquoted — the old `'"…"'` shell-quoting dance is the script's problem now).
  Nothing on the wire can change it, so re-badging a unit means building and
  flashing it, and a flash erase cannot lose it. The serial number is per unit,
  so each board gets its own build. The one runtime field is Config Version,
  which travels with each save to flash.
- **Online → Upload Configuration…** — the command you hand a dealer or a
  customer. It opens a `.ct3s`, reads the unit in front of it and refuses to
  install a package the device does not match: vendor, model, an optional
  serial allow-list, and a fleet key the device must **prove**. Config Version
  only **warns**, and only when the install would move the unit backwards —
  rolling back to a revision you know is good is a deliberate act, not a mistake.
  Every rule's verdict is shown before a byte is written, and none of them read
  the device's configuration — which is exactly what lets a
  customer take updates for a protected config they cannot read. There is no
  override for a failing rule: one that can be clicked past is a warning wearing
  a costume, and an installer has no way to tell which is which.
- **Online → Send Secure Configuration…** — installs a `.ct3s` on the connected
  device **without ever opening it**. Send Configuration sends the document on
  screen and Upload Configuration loads the package into it first; both leave the
  configuration in the app afterwards, so handing either to a dealer turns
  "install this update" into "and here is the CAN protocol, have a browse". This
  one decodes the package into a throwaway Configuration, maps it, sends it, and
  discards it — your own open document is untouched, and nothing about the
  package is displayed, down to a mapping failure reporting how many errors there
  were rather than what they were. It applies the uploader's rules and refuses on
  a failure, since a deployment command has no "anyway".
- **File → Check Channels** — full validation report on the current in-memory
  configuration (no save required — the header states exactly what was checked
  and when); the firmware validates almost nothing, so the GUI enforces the
  rules (see `FIRMWARE-NOTES.md`). Also lists **unused channels** (orphans left
  after removing messages) and offers **Remove Unused Channels** cleanup.
- **File → Config Summary…** — MoTeC-style Channel Summary Report: summary
  info, comments, bus setup, used channels, channels by function (per-message
  Generates/From and Uses/For tables with the DBC extraction detail, compound
  Id[n] groups, calculations), incomplete channels, unused channels — with
  Print, Save PDF, and Save Text.
- **File → Save Secure Config…** — writes the document as a binary `.ct3s`
  instead of legible JSON, so a customer can deploy and update a configuration
  without ever reading its CAN layout. Optionally wrapped under the Edit
  Protected Comms password, in which case the file will not open without it and
  there is no recovery. See `DESIGN.md` for the container format and an honest
  account of what each mode does and does not defeat.

## Build

**The firmware sources are required.** The application embeds the device's own
image validator and script VM by compiling a handful of C files straight out of
the firmware tree — that is what makes the configurator run the *device's* code
rather than a re-implementation. CMake finds that tree in either place:

- `../firmware` — the layout of the published repository
  ([CAN-Triple-Device-Manager](https://github.com/mitchdetailed/CAN-Triple-Device-Manager)),
  where `gui/` and `firmware/` sit side by side. **A full clone builds with no
  extra step.**
- `./firmware` — a junction/symlink or checkout inside `gui/`, for split
  working copies (`mklink /J firmware ..\firmware`).

With Qt 6.7.2 (MinGW 64-bit), CMake ≥ 3.21, Ninja, from `gui/`:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.7.2/mingw_64 `
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build build
build\test_roundtrip.exe       # protocol/mapper self-tests
build\test_firmware_link.exe   # GUI stack against the real firmware core
build\CANTripleDeviceManager.exe
```

(CMake stops with a clear message, not a cryptic "cannot find source file", if
the firmware tree is missing from both places.)

Distribution comes off the same build tree:

```powershell
cmake --build build --target deploy      # deploy/ — standalone payload: windeployqt,
                                         # MinGW runtime DLLs, help files, licences
cmake --build build --target package     # zips deploy/ into CANTripleDeviceManager-windows-x64.zip
cmake --build build --target installer   # dist/CANTripleDeviceManager-<version>-Setup.exe
                                         # (needs Inno Setup 6; ISCC found automatically)
```

> The test suite compiles the firmware's engine/serial/flash/identity sources
> out of the same firmware tree the application uses, so everything above —
> application and tests alike — needs it present. In the published repository
> it always is.

Firmware: `pio run` (and `pio run -t upload`) inside `firmware/` — see
[firmware/README.md](firmware/README.md).

## Documents

- `DESIGN.md` — UI inventory, protocol spec, MoTeC→firmware mapping rules.
- `FIRMWARE-NOTES.md` — firmware findings the GUI works around (flash sizing,
  unimplemented commands, Motorola extraction, UART burst limits…), with
  suggested fixes.
- Configurations are saved as JSON (`*.ct3`), or as the binary **`*.ct3s`**
  (File → Save Secure Config…) when the CAN protocol inside them is not for the
  reader to see — same document, opaque bytes, optionally unopenable without the
  Edit Protected Comms password. Open… picks the reader from the file's contents,
  not its extension. With the firmware in `firmware/` the device reloads its
  saved config from flash at every power-up; the PC file remains the editable
  master.

## Licence

MIT — the full text is [COPYING](COPYING) at the repository root. The device
firmware and bootloader are NOT open source: the app links their desktop-side
portion (the image validator and script VM) as the `ct_device_core` library —
built from source in the private tree, prebuilt in the public one — and those
objects are © Minton Performance, all rights reserved. The shipped binaries
bundle Qt 6 and the MinGW runtime, so the third-party paperwork lives under
[`installer/`](installer/): `THIRD-PARTY-NOTICES.txt` (what is bundled, under
which licence, and where its sources are), `LICENSE.LGPLv3.txt` and
`GCC-RUNTIME-EXCEPTION.txt`. The `deploy` target stages all of it into the
payload, so the zip and the installer cannot ship without it.
