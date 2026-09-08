# Firmware Licensing &amp; Access Keys

These features exist for one arrangement: you ship a customer a device running a configuration whose CAN protocol is yours, not theirs. They must be able to install updates; they must not be able to read them; and an update must not install on anything it was not built for.

Three things answer those, and they are deliberately separate:

<table>
<tr><th>What</th><th>Where it lives</th><th>Answers</th></tr>
<tr><td><b>Firmware licence</b></td><td>Its own flash page on the device</td>
<td>Who this unit's firmware is licensed to, and what proves it.</td></tr>
<tr><td><b>Access passwords</b></td><td>The device's configuration store</td>
<td>What a connected host may do to this unit.</td></tr>
<tr><td><b>Secure package</b></td><td>A <code>.ct3s</code> file</td>
<td>Which devices this configuration may be installed on.</td></tr>
</table>

> **Note:** This replaces the **fleet identity**, which was compiled into the firmware and could only be changed by a rebuild and a reflash. That made it unforgeable and useless for anything issued after manufacture — and a licence is something you grant, revise and re-issue. The hardware record that *cannot* change now lives in the chip's OTP area; see **Online → Get Device Info** in [Online: Send, Get &amp; Flash](online.md).

## Firmware License Manager

**Online → Firmware License Manager…** writes four values into the connected unit. They live in the device, not in the configuration file, and they survive a Send, a Clear and a firmware update.

<table>
<tr><th>Field</th><th>Budget</th></tr>
<tr><td>Firmware Manufacturer</td><td>32 bytes</td></tr>
<tr><td>Firmware Model</td><td>32 bytes</td></tr>
<tr><td>Firmware Version</td><td>8 bytes</td></tr>
<tr><td>Firmware Key</td><td>passphrase, up to 32 characters</td></tr>
<tr><td>FW Updater Password</td><td>passphrase, up to 32 characters</td></tr>
</table>

The three text fields are byte budgets rather than character counts: one non-ASCII character costs two to four bytes, so the boxes stop where the device does.

### Two secrets, and they are not alike

**The FW Updater Password is the gate.** Blank means anyone who connects can rewrite these details. Set means the device demands it first, and the dialog prompts for it before Apply will go through. This is the one that protects the record.

**The Firmware Key is the claim.** It authorises nothing about the licence itself. It is the value a unit *proves* in order to show which licence it holds, and it is what a secure package checks against. A unit can carry a key and still be freely rewritable, which is the right state for a board that has been given an identity but not yet locked down.

> **Warning:** **The Firmware Key is also a master key over the access passwords.** A host that proves it may overwrite the Send, Get and Protected Comms passwords without knowing any of them — that is what lets a secure package re-provision a unit whose customer has locked it and moved on. The cost is real and worth stating: anyone who recovers a Firmware Key owns every unit built under it, to the same depth you do.

**Neither secret is ever read back.** Not the passphrase, not the derivation, not a hash. Both fields show blank with a placeholder saying so, and blank means *keep* — which is what makes fixing a typo in a model name bearable. Removing the FW Updater Password is a separate tick box, offered only while the field is empty, because "set it to this" and "take it away" are contradictory instructions.

Apply connects if you are not connected: composing a licence is desk work, and the dialog opens without hardware. Setting the first password asks for confirmation, because there is no recovery — a lost FW Updater Password means the licence on that unit can no longer be changed by anyone except a holder of the Firmware Key.

## Set Access Passwords…

**Online → Set Access Passwords…** manages the passwords that live in the device, which is why the dialog needs a connected CAN Triple. There are three functions, and Protected Comms holds four slots, so the list shows six rows.

<table>
<tr><th>Function</th><th>Without the password</th></tr>
<tr><td>Send a Configuration</td><td>The device refuses to have its configuration replaced.</td></tr>
<tr><td>Get a Configuration</td><td>The device refuses to have its configuration read back.</td></tr>
<tr><td>Protected Comms (Slots 1–4)</td><td>The send gate has nothing to confirm. A configuration containing Protect Communication messages goes only to a unit that confirms the matching password, and a unit with none set cannot confirm anything. Confirming any of the four slots satisfies the gate, which lets one unit accept sealed configurations from several suppliers without any of them sharing a password.</td></tr>
</table>

The functions are independent: holding one proves nothing about the others. The device never reveals a password; it only reports which slots have one, and proving one is a challenge-response exchange, so nothing useful crosses the wire.

> **Note:** **A password set here is in force immediately but does not reach flash by itself.** The keys share the flash header with the configuration, and that header can only be written once each time the region is erased — which is what sending a configuration does. So a password set on an already-configured unit is real for the session only: power-cycle before sending a configuration and it comes back without it. **Set the password, then send a configuration.**

A secure package built by the Secure Configuration Builder does not have this problem: an install erases and re-commits the header anyway, so passwords applied by a package land atomically with the configuration.

## Secure Configuration Builder

**File → Secure Configuration Builder…** turns a `.ct3` into a `.ct3s` package that only installs where it is meant to, and that the Manager itself cannot read. It takes a configuration as an input — the open document's path is offered as the default — maps it to device records here and now, seals every frame of the install under the Firmware Key, attaches a policy, and writes the package. The passphrase you type is used to derive the key and goes no further: neither it nor the key is written into the file.

A **Package version** (0–65535) is stamped on the unit when the package installs and shown afterwards by Device Status as the configuration version it is running. This is where a release gets its number: a plain Send from the editor leaves the unit's version exactly as it was, so bench work never renumbers a fleet. 0 means unversioned.

### Install only on devices matching

<table>
<tr><th>Option</th><th>Checked against</th></tr>
<tr><td>Match FW Manufacturer</td><td>The device's licence. Optional.</td></tr>
<tr><td>Match FW Model</td><td>The device's licence. Optional.</td></tr>
<tr><td>Match FW Version</td><td>The device's licence. Optional.</td></tr>
<tr><td>Match MCU ID</td><td>The unit itself: the STM32's 96-bit unique ID, the 24 hex
digits Device Status shows and copies. Optional.</td></tr>
<tr><td>Match HW Serial</td><td>The unit itself: the serial number burned into the OTP
manufacturing record, as Get Device Info shows it (decimal, or hex with 0x). Optional.</td></tr>
<tr><td><b>Match FW Key</b></td><td><b>Always.</b> No tick box, because it is not optional.</td></tr>
</table>

The three licence matches are exact. The two hardware matches name one unit rather than a fleet, and a unit that cannot report an ID (older firmware) or a serial (an unburned record) fails them by name rather than being waved through. The key is not a string compare at all: the host picks a nonce and the device answers under the key it holds, so this proves the unit really carries the licence rather than merely reporting one. A look-alike echoing the right manufacturer and model still fails.

> **Warning:** **An unlicensed unit takes no packages.** Every package names a key and every target must prove it, so a board that has never been given a licence cannot match anything. The provisioning order follows from that: flash the firmware, issue a licence with the Firmware License Manager — which needs only a serial connection — and only then can packages be installed. There is no deadlock in it, because issuing a licence needs no package.

### Set device passwords on install

Six more options, one per access password: Send, Get, and the four Protected Comms slots. A ticked box with a value sets that password as the package installs; **a ticked box with an empty field removes it**; unticked leaves it alone. Those are three different instructions and the file records them separately.

The password writes ride inside the sealed install stream, ahead of the configuration, so the device applies them because the stream is sealed under its own Firmware Key — the master key described above. Without that, a package could only ever provision a blank unit. The policy records *which* passwords the package sets; the keys themselves are in the stream, where only the device can reach them.

### Package contents

By default a package is **install-only**: it carries the sealed install stream and the policy, and nothing the Manager can open. Tick **Include an editable copy, opened only with this package password** to add a copy of the configuration wrapped under a password of your choosing (the same rules as a device password, typed twice). Opening that package in the Manager then asks for the package password and refuses anything else. The install stream is sealed either way; the password guards only the editable copy.

> **Note:** Install-only is the default because the `.ct3` the package was built from is the editable master, and a copy inside the package is one more place the configuration exists. Include one when the person receiving the package is entitled to read the configuration as well as install it.

### What the package protects, and what it does not

The configuration is sealed under keys derived from the Firmware Key, and **the Firmware Key does not travel in the file**. The device derives the same keys from the licence it holds and decrypts each frame inside its own memory; the Manager relays the frames and never sees a byte of plaintext. In place of the key, the package carries a *proof* — a challenge and the answer a unit holding the key must give — which lets the installer check a unit before touching it without being able to decrypt anything itself. The policy is sealed in the container with the rest, so a package lying on a disk does not announce which fleet it is for, and its demands cannot be edited out.

What it does not protect against, stated plainly:
- Whoever holds the **Firmware Key passphrase** can decrypt any package built under it. The Builder's screen is that person's, and the passphrase must not be shared with the people the package is meant to be closed to.
- The device holds the key, so **reading the device's flash** with an ST-Link recovers both the key and the installed configuration. STM32G4 readout protection (RDP level 1) at manufacture is the backstop, and it is a programming decision rather than a Manager one.
- An older package (format 2, built by Manager 1.1.x) still installs, and still carries its key inside the file the way it always did. Rebuild it with this Builder to get a sealed one.

> **Note:** Two guarantees, and they are different: the licence and hardware matches decide which *devices* will accept the package; the seal decides who can *read* it. A package built by this Builder offers both.

## Send Secure Configuration…

**Online → Send Secure Configuration…** is the installer's command. It reads a package's policy and its sealed install stream — neither needs a password — checks the policy against the unit in front of it, and relays the stream frame by frame. The device decrypts each frame and answers it with the ordinary ACK or NACK; the Manager displays nothing about the package because it holds nothing about it — not a message count, not a channel name, not a bus rate. It needs device firmware 1.0.8 or newer; an older unit refuses the first frame and the message says so.

The checks run before the device is touched and before a single frame goes out — a package that does not belong on this unit must never get as far as partially overwriting it. Each failure names the field and both values, so the person holding the laptop can tell whether they picked up the wrong file or the wrong unit. No Send password is asked for: the sealed frames carry their own authority, and a package that sets passwords sets them from inside the stream, ahead of the configuration, so they commit to flash with it.

A refused frame ends the sealed session on the device, and the install starts again from the beginning rather than resuming — there is no partial state worth keeping. A package built for another fleet's key, or a stream that got out of step after a lost reply, both end the same way: one refusal, and "try again".

A `.ct3s` with no policy is refused. "This package makes no demands" is not something a package is allowed to be. A package with no install stream — one saved from the editor rather than built — is refused too, by name; a package built by an older Manager (format 2) installs through the path it always used.

> **Note:** The honest boundary: for a package built by this Builder, nothing is decrypted in this process. For a format-2 package it still is, because that format has no other way to be sent.

<a id="readout"></a>

## Readout protection

Everything above lives in the unit's flash: the configuration store, the access keys, and the licence page with the Firmware Key that decrypts every package built for the fleet. The STM32's on-board ST-LINK can read all of it back unless the chip's **readout protection** is set. From device firmware 1.0.9, **a unit that holds a Firmware Key sets readout protection level 1 itself** at boot, once, and resets; a unit with no licence — a bare development board — is left open. Device Status reports the level.

What level 1 means, stated plainly:
- The flash cannot be read over the debug port. Firmware updates through the Manager keep working, because they run on the chip itself.
- It is undone only by a **mass erase**: bootloader, firmware, configuration, passwords and licence all go, and the unit comes back blank. That is the protection, and it applies to you as much as to anyone else.
- The initial programming tool cannot program a locked unit until it is unlocked. Run it with `--unlock`, which performs that erase first and then programs the bootloader and firmware as usual; the unit then needs its licence issued again before it will take packages.
- Level 2 is permanent and is never set by anything here.

> **Note:** The order of provisioning therefore matters: program the firmware, then issue the licence. The unit locks at its next power-up after the licence is written, so there is nothing else to do — and nothing to forget.

## What this is — and is not
- A Firmware Key is **shared across everything built under it**. One recovered key compromises every unit that holds it, and revocation means re-issuing licences.
- None of this is a signature and none of it is DRM. It stops the wrong file reaching the wrong product, and a look-alike collecting someone else's update.
- The message protection tiers — Read Only, Hidden, Protected — are conventions of this application. Any other serial tool talking to the device defeats them, and the device enforces nothing about them. See [Communications](communications.md).
- An ST-Link reading a device's flash directly is stopped only by readout protection — and the flash holds the Firmware Key as well as the configuration. A licensed unit sets it itself; see [Readout protection](#readout).
