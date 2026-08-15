# Fleet Identity &amp; Access Keys

These features exist for one arrangement: you ship a customer a device running a configuration whose CAN protocol is yours, not theirs. They must be able to install updates; they must not be able to read them; and an update must not install on anything it was not built for. The fleet identity answers "is this update for this unit?", the upload policy says how strictly a package checks, and the access passwords control what a connected host may do to the device at all.

## What a fleet identity is

A unit's identity is **compiled into its firmware** — no command on the wire can change it, a flash erase cannot lose it, and a packet cannot forge it. Re-badging a unit means setting its values (`CT_VENDOR_ID`, `CT_MODEL_ID`, `CT_SERIAL_NUMBER`, `CT_FLEET_KEY`) in `firmware/identity.local.ini` and reflashing. The one exception is **Config Version**, which the device records when a configuration is saved to its flash — it is rewritten with every Send, from the number you set here.

That identity file is gitignored and is **not** in a fresh firmware checkout: create it by copying `identity.local.ini.example` beside it. It is one `KEY=VALUE` line per value, and the vendor and model are **plain unquoted text** — spaces and all, 16 bytes each. (Earlier versions of this manual had you edit four `-D` lines in `firmware/platformio.ini` and wrap each string as `'"…"'` to get it past the shell. The tracked `platformio.ini` holds no identity values any more, and that quoting trap went with them: `scripts/build_flags.py` reads the file and emits the defines.) A missing file is not an error — the build is simply **unprovisioned** — but a malformed one stops the build and names the offending line, because a half-read file must never produce a half-badged unit.

<table>
<tr><th>Field</th><th>Set at</th><th>Notes</th></tr>
<tr><td>Vendor ID</td><td>firmware build</td><td>16 bytes on the wire, compared exactly — case, spacing and punctuation count.</td></tr>
<tr><td>Model ID</td><td>firmware build</td><td>16 bytes, compared exactly, like the vendor.</td></tr>
<tr><td>Serial Number</td><td>firmware build</td><td>32-bit, per unit — each board needs its own build.</td></tr>
<tr><td>Fleet Key</td><td>firmware build</td><td>4 bytes, never read back; the device proves it by answering a challenge.</td></tr>
<tr><td>Config Version</td><td>runtime</td><td>Recorded when a configuration is saved to flash.</td></tr>
</table>

A unit built without these values reports itself unprovisioned — the normal state of a bench device. It stays fully usable; it simply cannot be matched against, so the uploader has nothing to refuse on.

## The Fleet Identity dialog

Online → Fleet Identity… does **not** require a connection: building the package a customer's car will be given six months from now is desk work. A connected unit only adds the ability to copy its vendor and model instead of retyping them. The dialog's two panels are deliberately not symmetrical.

### This Device (read-only)

What the connected unit was built with: Vendor ID, Model ID, Serial Number (shown in hex and decimal), Config Version, and whether a fleet key is programmed. It is read once when the dialog opens. Nothing here — and no command on the wire — can change a device's identity.

### This Configuration
- **Vendor ID :** and **Model ID :** — who and what this configuration is for. Each field shows a byte counter because the wire limit is **16 bytes, not 16 characters**: a character outside plain ASCII takes two bytes or more.
- **Config Version :** — which revision of this configuration it is (0–65535). Number it higher for every release; a package whose version is 0 makes no ordering claim and the version rule sits out.
- **Fleet Key :** — a passphrase, never four raw bytes. It is folded (210,000 rounds of PBKDF2) into the same four-byte key the firmware was built with, so the same passphrase produces the same key on every machine. Leave it blank to keep the key the configuration already holds — a passphrase can never be shown back. Clearing the vendor and model is what removes a key.
- **Identity line :** — the ready-to-paste `CT_FLEET_KEY=0x…` line for `firmware/identity.local.ini`, derived from the passphrase typed above and from nothing else. Paste it in as it stands — no `-D`, no quotes — and reflash the unit. It is read-only, and it stays blank until a passphrase is typed in this session.

> **Warning:** The fleet key is the fleet's secret and is stored only in a secure configuration (File → Save Secure Config…). A plain `.ct3` keeps the vendor, model and version and **drops the key** — the loss shows up much later as an upload that cannot challenge the device. A mistyped passphrase locks nothing: it produces a different key, which shows up as a device that cannot prove it belongs to the fleet. See [Configuration Files (.ct3)](files.md).

### Upload Policy
- **Allowed serial numbers :** — one serial number per line. A number starting `0x` is read as hexadecimal and anything else as decimal, so `0x100` and `100` are different devices. Leave the list empty to let any device in the fleet install this configuration. Lines that cannot be read as a number are counted and ignored — and because a shorter list is a *more permissive* policy, applying with unreadable lines asks first.
- **Require the device to prove the fleet key** — without proof, the fleet block is four values a look-alike can echo back. Turn this off only for a fleet whose firmware was built without a fleet key.
- **Warn if the device already runs a newer version** — installing an older configuration is allowed either way; this only decides whether the uploader remarks on it. Reinstalling the *same* version is never remarked on.

**Copy from Device** fills the Vendor ID and Model ID from the connected unit. Config Version is deliberately not copied — the device's version is what it is already running, and a package numbered the same is by definition not newer. If the unit reports a serial, you are asked separately whether to restrict the configuration to it; the question defaults to No, because pinning a package to one unit locks out every other unit in the fleet. **Apply to Configuration** writes the fields and the policy into the open document; neither button closes the dialog. Save the configuration afterwards for the change to reach the file.

## Upload Configuration… — the rules

Online → Upload Configuration… is the installer's command: it opens a package (**Open Package…** — whatever happens to be open in the application is deliberately not enough), reads the unit in front of it, and shows every rule pass or fail *before anything is written*. The table's columns are **Rule**, **Package wants**, **Device says** and **Result**; each result is ✓ Pass, ⚠ Warning, ✗ Fail or – Not checked.

<table>
<tr><th>Rule</th><th>Compares</th><th>If it does not hold</th></tr>
<tr><td>Vendor ID</td><td>Exact match against the package.</td><td>Refuses.</td></tr>
<tr><td>Model ID</td><td>Exact match against the package.</td><td>Refuses.</td></tr>
<tr><td>Serial Number</td><td>Must be in the package's allow-list, when it pins one.</td><td>Refuses.</td></tr>
<tr><td>Fleet Key</td><td>The device must <b>prove</b> the key by challenge, not merely claim the identity.</td><td>Refuses.</td></tr>
<tr><td>Config Version</td><td>Whether installing would move the unit backwards.</td><td>Warns; the upload proceeds.</td></tr>
</table>

Only a ✗ Fail blocks the upload, and there is no override for one. "Not checked" means there was nothing to compare — an unprovisioned unit or an unpinned package — which is normal on a bench and is different from a pass: a verdict made entirely of unchecked rules is reported as exactly that. None of these rules needs the Get password or reads a record out of the device's configuration, which is what lets a locked-down unit be checked without being opened.

> **Note:** The **Upload** button re-runs every rule immediately before writing, in case the unit on the cable has been swapped since the table was drawn. An upload always saves to flash and never binds the configuration to the chip — that is the package author's decision, not the installer's. It verifies the write when it can; on a unit that guards "Get a Configuration" the read-back is refused, the verify pass is skipped, and the result lists it under "Skipped". Send Configuration applies the same rules from the same code, but asks instead of refusing — see [Online: Send, Get &amp; Flash](online.md).

## Set Access Passwords…

Online → Set Access Passwords… manages three independent passwords that live **in the device**, not in the file — which is why the dialog needs a connected CAN Triple. The list under **Function Passwords** shows each function with ✓ = Password set; select one and click **Set…**.

<table>
<tr><th>Function</th><th>Without the password</th></tr>
<tr><td>Send a Configuration</td><td>The device refuses to have its configuration replaced.</td></tr>
<tr><td>Get a Configuration</td><td>The device refuses to have its configuration read back.</td></tr>
<tr><td>Edit Protected Comms</td><td><b>Prove-only: the device confirms this password and gates nothing on it.</b> It is one of the two things <i>this application</i> requires before it will let a <a href="communications.md#marking">Protect Communication</a> marking be applied or removed, and it insists on a connected device confirming it — which is the only thing that makes that level stronger than Hidden. The other is that message's own <b>Message Password</b>; this one is not a master key over it. Without both, such a message stays concealed here and its marking cannot be moved here. It does <em>not</em> stop the message being read off the device: a Get returns every record in full, to anyone, at every level.</td></tr>
</table>

The three are independent: holding one proves nothing about the others. The Set… flow asks one question at a time — the old password when one is set ("Enter Old Password"), then the new password twice ("Enter New Password", "Confirm New Password"). Leaving the new password **blank clears it**, after a confirmation that spells out what anyone will then be able to do. The device never reveals a password; it only reports which functions have one, and proving one is a challenge-response exchange, so nothing useful crosses the wire.

> **Note:** The device keeps its passwords alongside the configuration, so a password change is in force immediately but does not reach flash by itself. The dialog attempts to commit it for you, and asks for the Send password if the device wants one for that commit.

> **Warning:** Be clear about how far that gets, because it is less far than it sounds and it was measured rather than assumed. The keys share the flash header with the configuration, and that header can only be written once each time the region is erased — which is what **sending a configuration** does. So the commit succeeds only on a unit that has just been cleared. On a unit that is already configured — the ordinary case — it is refused, the dialog says so, and the password is real **for this session only**: power-cycle before sending a configuration and the device comes back without it, while this dialog goes on showing it as set.

**So: set the password, then send a configuration.** That mattered little when nothing depended on these keys. It matters now, because Edit Protected Comms is what a device confirms before a Protect Communication marking can be applied or removed — a unit that lost it cannot answer for that half at all, so those markings can no longer be moved against it in either direction. What a lost device password does *not* do is throw the markings open: each of those messages still has a **Message Password** of its own, and it is required whether the device answers or not.

> **Note:** Setting Edit Protected Comms also writes a matching verifier into the open document, so the file and the hardware agree about which password answers for that half of a Protect Communication marking. It is only that half — opening such a message here also takes the message's own Message Password, and no Hidden or Read Only message answers to this password at any time. The document's protected messages must be revealed first (File → Reveal Protected Comms), and the configuration must be saved afterwards.

## What this is — and is not
- The fleet key is **shared across the fleet**: one dumped unit compromises attestation for every unit built with it, and revocation means a new key and a reflash.
- A **serial number is public**. Pinning narrows which unit an update reaches; it is not a second secret.
- This is **not a signature and not DRM**. It stops the wrong file reaching the wrong product and a look-alike collecting someone else's update.
- It is distinct from the per-chip **device binding** ("Lock this configuration to this device" in the Send dialog): binding says "only this chip may run this image", the fleet identity says "this device is one of ours and this package was built for it".
