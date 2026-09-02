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

**File → Secure Configuration Builder…** turns a `.ct3` into a `.ct3s` package that only installs where it is meant to. It takes a configuration as an input — the open document's path is offered as the default — attaches a policy, and writes the package.

A **Package version** (0–65535) is stamped on the unit when the package installs and shown afterwards by Device Status as the configuration version it is running. This is where a release gets its number: a plain Send from the editor leaves the unit's version exactly as it was, so bench work never renumbers a fleet. 0 means unversioned.

### Install only on devices matching

<table>
<tr><th>Option</th><th>Checked against</th></tr>
<tr><td>Match FW Manufacturer</td><td>The device's licence. Optional.</td></tr>
<tr><td>Match FW Model</td><td>The device's licence. Optional.</td></tr>
<tr><td>Match FW Version</td><td>The device's licence. Optional.</td></tr>
<tr><td><b>Match FW Key</b></td><td><b>Always.</b> No tick box, because it is not optional.</td></tr>
</table>

The three string matches are exact. The key is not a string compare at all: the host picks a nonce and the device answers under the key it holds, so this proves the unit really carries the licence rather than merely reporting one. A look-alike echoing the right manufacturer and model still fails.

> **Warning:** **An unlicensed unit takes no packages.** Every package names a key and every target must prove it, so a board that has never been given a licence cannot match anything. The provisioning order follows from that: flash the firmware, issue a licence with the Firmware License Manager — which needs only a serial connection — and only then can packages be installed. There is no deadlock in it, because issuing a licence needs no package.

### Set device passwords on install

Six more options, one per access password: Send, Get, and the four Protected Comms slots. A ticked box with a value sets that password as the package installs; **a ticked box with an empty field removes it**; unticked leaves it alone. Those are three different instructions and the file records them separately.

The device accepts these because the package proves the Firmware Key, which is the master key described above. Without that, a package could only ever provision a blank unit.

### What the package protects, and what it does not

The body is encrypted, but **the key that decrypts it travels inside the file**, obfuscated. That defeats a hex editor, a text search, and any tool that does not implement the format. It does not defeat someone who reads this application's source. The policy itself is sealed with the configuration, so a package lying on a disk does not announce which fleet it is for — and, more to the point, its demands cannot be edited out.

> **Note:** What stops a package being *used* where it should not be is the licence match at install. That is a different guarantee from being unreadable, and it must not be mistaken for it.

## Send Secure Configuration…

**Online → Send Secure Configuration…** is the installer's command. It opens a package, checks it against the unit in front of it, and installs it without ever displaying its contents — not a message count, not a channel name, not a bus rate. A mapping failure reports how many errors there were and not one word of what they were.

The checks run before the device is touched, before the Send password is asked for, and before a single record goes out — a package that does not belong on this unit must never get as far as partially overwriting it. Each failure names the field and both values, so the person holding the laptop can tell whether they picked up the wrong file or the wrong unit.

A `.ct3s` with no policy is refused. "This package makes no demands" is not something a package is allowed to be.

> **Note:** The honest boundary: the bytes are decrypted in this process in order to be sent at all. This withholds them from the interface; it does not withhold them from someone instrumenting the application.

## What this is — and is not
- A Firmware Key is **shared across everything built under it**. One recovered key compromises every unit that holds it, and revocation means re-issuing licences.
- None of this is a signature and none of it is DRM. It stops the wrong file reaching the wrong product, and a look-alike collecting someone else's update.
- The message protection tiers — Read Only, Hidden, Protected — are conventions of this application. Any other serial tool talking to the device defeats them, and the device enforces nothing about them. See [Communications](communications.md).
- None of it stops an ST-Link reading a device's flash directly. The backstop for that is STM32G4 readout protection, a device programming decision rather than a code one.
