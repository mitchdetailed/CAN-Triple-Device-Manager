# Online: Send, Get &amp; Flash

The Online menu moves configurations between the editor and a connected CAN Triple. The document model is offline-first: nothing you edit reaches the device until you send it, and nothing on the device changes the document until you get it. If no serial connection is open, commands that talk to the device first open Connection Settings so you can connect. Monitor Channels and the CAN Viewer open without prompting — they simply show nothing until a device is connected — and Fleet Identity… is deliberately usable offline.

<table>
<tr><th>Command</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>Connect</td><td></td><td>Opens the serial link. Reuses this session's
last port; the first connect of a session (or a port that has gone away)
opens Connection Settings. The status bar's right side shows the result.</td></tr>
<tr><td>Disconnect</td><td></td><td>Closes the serial link. The device keeps
running its configuration; the live windows simply stop updating.</td></tr>
<tr><td>Send Configuration</td><td>F5</td><td>Programs the open document onto the device and saves it to flash.</td></tr>
<tr><td>Send Secure Configuration…</td><td></td><td>Installs a sealed <code>.ct3s</code> package without opening or displaying it.</td></tr>
<tr><td>Get Configuration</td><td></td><td>Reads the device's configuration back into the editor.</td></tr>
<tr><td>Verify Configuration</td><td></td><td>Reads the device's tables and compares them with the document.</td></tr>
<tr><td>Monitor Channels…</td><td>F3</td><td>Live channel values — see <a href="monitor.md">Monitoring Live Values</a>.</td></tr>
<tr><td>CAN Viewer…</td><td>F4</td><td>Raw frame monitor and inject-frame form, with Vector <code>.asc</code> export.</td></tr>
<tr><td>Reset Device</td><td></td><td>Reboots the device; it reloads its saved configuration.</td></tr>
<tr><td>Device Status…</td><td></td><td>Uptime, bus counters, identity, passwords and fleet information.</td></tr>
<tr><td>Set Access Passwords…</td><td></td><td>Device-held function passwords — see <a href="fleet-identity.md">Fleet Identity &amp; Access Keys</a>.</td></tr>
<tr><td>Upload Configuration…</td><td></td><td>Dealer-facing installer that refuses packages not built for the unit — see <a href="fleet-identity.md">Fleet Identity &amp; Access Keys</a>.</td></tr>
<tr><td>Fleet Identity…</td><td></td><td>Reads the unit's identity and edits the document's fleet block — see <a href="fleet-identity.md">Fleet Identity &amp; Access Keys</a>.</td></tr>
</table>

## Send Configuration (F5)

To program the device with the open document, choose Online → Send Configuration. The command runs these steps in order:
1. **Access password.** If the device protects "Send a Configuration", you are asked for that password first — there is no point mapping a send the device will refuse.
2. **Fleet identity check.** The same rules the uploader applies are run against the connected unit. Here a failing rule is *advisory*: it is shown with its reason and a Yes/No question defaulting to No, because Send is the engineer's command and a bench unit is routinely re-loaded. Upload Configuration… is the path that refuses outright.
3. **Validation.** If the configuration has any Error, the send stops and Check Channels opens — see [Validation &amp; the Config Summary](validation-report.md).
4. **The Protect Communication gate.** If any message being sent is marked **Protect Communication**, the connected unit must confirm the **Protected Comms** password the configuration carries — any of the unit's four slots will do. A unit that cannot is **refused**, with the reason named: it has no Protected Comms password set, the configuration carries none of its own, the two do not match, or the unit could not even be asked. See [Sending a configuration that contains protected messages](communications.md#protectedsend). When the only Protect Communication messages are switched **Off** — not sent, so the gate does not run — a unit with no Protected Comms password gets a Yes/No warning instead: it cannot guard what those markings promise, and the fix is Online → Set Access Passwords…
5. **Confirmation.** A dialog summarises what will be sent (messages, channels, math, User Conditions, counters, timers), the bus settings that will be applied, and any mapper warnings. It asks for a required **Configuration Title :** (stored on the device, up to 32 bytes) and offers two checkboxes:
    - **Lock this configuration to this device** — stores the device's unique chip ID with the configuration, so the device refuses to run it if it is copied to a different CAN Triple. Sending to another device re-binds it. Offered only when the firmware can report a device ID.
    - **Reset device after sending** — reboots the unit once the transfer completes.

> **Note:** Nothing on the device asks for a password because of a message marking — the Protect Communication check above runs before the transfer starts, and acting on it is this program's doing. Earlier releases asked for one during the transfer — the device refused to erase a marked message, so a send to a unit somebody else had protected stopped at the first step. That rule is gone: **removal is permitted at every protection tier**, so a send always clears what the unit is holding. See [Communications Setup](communications.md).

The transfer itself opens with a **configuration clear**: the device's running configuration is erased before the new tables are written. The tables are then sent in chunks, the per-bus CAN setup is applied (v2 firmware; v1 NACKs the step and it is reported as skipped), the sent tables are **verified by reading every record back and comparing the echoed bytes** with what was written, and finally the image is **saved to flash**, which is what makes it boot-persistent — it reloads automatically at every power-up. The configuration's own revision number rides the save step, so the version the device reports afterwards matches the tables it just committed.

> **Warning:** Cancelling or losing the link mid-send leaves the device cleared but not reprogrammed — the clear has already happened and the flash save has not. Send again before relying on the unit.

> **Note:** On firmware too old to save to flash, the result message says so: the configuration lives in device RAM and is lost at power-off. If the device guards "Get a Configuration" and that password is not proved, the verify pass is skipped and listed under "Skipped" in the result — the send itself still completes.

## Send Secure Configuration…

Installs a `.ct3s` package on the connected device **without opening it**: nothing about the package reaches the document or the screen — not a message count, not a channel name, not the per-stage progress text. The configuration you have open stays untouched. This is the command to use when the person at the laptop is entitled to install a configuration but not to read it.
- A plain `.ct3` is refused by name — open it and use Online → Send Configuration, or seal it with File → Save Secure Config… first. See [Configuration Files (.ct3)](files.md).
- A package saved with "Require access password for use" asks for its Protected Comms password before anything is sent — without it the file cannot be decoded at all.
- The fleet identity rules are checked **before the device is touched**, and a failure refuses with no override. If the package names a fleet and the connected device cannot be asked for its identity (older firmware, or a failed read), the install is refused rather than performed on faith. A package that names no fleet and pins no serial installs anywhere.
- The install verifies and saves to flash under the same rules as Send Configuration: firmware too old to save reports that the configuration is running but will be lost at power-off, and a device that guards Get a Configuration skips the verify pass.

## Get Configuration

Reads the device's configuration into the editor, replacing the current document (you are asked first if it has unsaved changes). If the device protects "Get a Configuration", that password is proved first.

The read recovers every message, channel, math channel, User Condition, counter, timer, constant, table and integrator, the stored configuration title, and — on current firmware — the live bus setup (mode, bit rates, termination), so what comes back can be re-sent without guessing. A [device script](device-scripts.md) comes back as well, as the **compiled bytecode** the device stores — sending that document back puts the same script on a unit byte for byte. What a Get cannot recover is a script's *Lua source*, because bytecode does not turn back into source: the Script Editor shows a retrieved script as a [read-only disassembly](device-scripts.md#retrieved), and replaces it only if you deliberately ask it to.

On firmware that cannot report its bus setup, the assumed bring-up rates are used and a note tells you to check Connections → Communications before sending. A bus running listen-only reads back as enabled, and a note is raised whenever that conversion happens.

> **Note:** A device holding a configuration that was locked to a *different* CAN Triple reads back as empty, because its engine refused to load the image. Get Configuration says so instead of handing you a blank document: send a configuration to that device to replace it.

### Reading a device that holds marked messages

It just works. A configuration containing [Read Only, Hidden or Protect Communication](communications.md) messages reads back in full — no password is needed to retrieve one, and Get is never refused because of a marking.

The markings survive the round trip, which is the only thing the device does with them: it carries the level on the wire so that a Get followed by a Send cannot quietly turn a Hidden message into an ordinary one. It never reads the level to decide anything.

> **Note:** **The passwords survive too.** The device stores the configuration's four [Message Passwords](communications.md#passwords) — as derived keys, in its configuration store — and each marked message records which of the four guards it, so a Get re-guards every marked message with the password it names. Where the open document already holds a password in a slot, the document's own value wins.

A **Read Only** message shows every field and cannot be edited. A **Hidden** or **Protect Communication** message is **concealed**: its CAN ID, frame layout, timing and every channel's bit positions are withheld from a viewer without the password, in the sections list, the section editor, Check Channels and the reports.

What the device cannot give back is the **name** you gave a section — message names are not stored on a CAN Triple at all. So a Get matches each message the device returns against the section already open in this document — first by bus, CAN ID and direction, and where that finds nothing, by bus and name — and puts the password and the name back on it. The second match matters because the first is made of exactly the field you are most likely to have just edited: renumber a message, Get, and without it the password went quietly over the side. A message the device no longer has drops from the document, as it should: a Get reports what the hardware holds.

Two consequences worth knowing. A message the open document has *never* seen arrives under a neutral name such as "Hidden message 1" rather than one built from its CAN ID — concealment extends to the name — and the placeholder is replaced with an ordinary automatic name the moment the marking is released. Such a message still arrives **guarded**, by the Message Password it names. What a device never delivers, a file still can: a marking with no password behind it is **concealed from everybody**, and reads **(hidden — no password)** so that nobody goes hunting for a password that was never written down — see [Marking a message](communications.md#marking).

> **Note:** Concealment, not visibility, is the deliberate choice here. The tempting alternative — showing such a message in full, on the reasoning that a marking nobody can supply a password for would otherwise be unopenable — has it backwards: it puts a padlock and the word "hidden" over a row that lists its channels and opens its whole layout on a double-click. A message nobody can open is exactly what a marking with no password *is*; the answer is to say so plainly, not to publish the message.

> **Warning:** Be clear about what this is and is not. **All three markings are conventions of this program.** The device enforces nothing about them: it holds those fields and hands them to anything that speaks its protocol, so any other serial tool reads a marked message in full and writes over it freely. A plain `.ct3` is scrambled rather than legible, so a text editor no longer defeats them there — but this program does, because a .ct3 carries its own key and opening one reveals everything in it. The one promise that is actually kept is a **.ct3s** package, which keeps the markings closed on somebody else's machine. If the protocol must stay secret from a determined reader, the answer is not to ship it to them.

> **Note:** The device could have been made to refuse overwriting or erasing a marked message without its password, and deliberately was not. Such a rule makes a unit whose password has been lost impossible to clear or reconfigure at all, and it is defeated on the bench anyway by clearing the device and writing a fresh message over the top. Enforcement that cannot hold is worse than an honest convention, and the help is written to match what the product does.

## Verify Configuration

Maps the open document to device tables, reads the connected device's tables back (gated by the "Get a Configuration" password, since it is a read), and compares them record by record — every table kind: messages, channels, math channels, User Conditions, counters, timers, constants, relays, lookup tables, integrators and the compiled device script. Extra active entries on the device beyond what the document defines — leftovers from an older configuration — count as differences too. The result is either "Device configuration matches the document." or a count of differing table entries with the advice to use Send Configuration to update the device.

## Reset Device

Reboots the unit after proving the "Send a Configuration" password. It re-initializes and reloads its saved configuration from flash; live streams pause briefly.

> **Note:** There is deliberately no separate "save to flash", "load from flash" or "clear" command. Send Configuration already saves; the device runs its configuration directly out of flash, so what it is running and what it has stored cannot differ; and **wiping a unit is File → New followed by Send** — every Send begins with the same flash erase a dedicated clear performed, so an empty document empties the device. The empty-Send route also keeps the device's access passwords stored, where the old dedicated command erased their flash copy along with the configuration and a unit power-cycled after it came back with no passwords at all.

## Device Status…

One read-only report of what the unit is and what it is doing:
- Uptime and per-bus receive/transmit frame counters.
- Active table counts (messages, channels, math, User Conditions).
- Firmware protocol version.
- Device ID and whether the stored configuration is locked to a different unit (which is why an apparently inert device shows 0 active messages).
- Which access passwords are set — never their values. This tells you in advance that a Send or Get will ask for a password.
- The fleet identity (vendor, model, serial), the configuration version on the unit, and whether a fleet key is present — see [Fleet Identity &amp; Access Keys](fleet-identity.md).

## After a firmware update

> **Note:** Some firmware updates change the format the configuration is stored in. When that happens the device validates its stored image, rejects it, and comes up with an empty configuration that has to be sent again — the Update Firmware dialog warns you in advance when an image will do this. Most updates leave the stored configuration intact. See [Updating Firmware](firmware-update.md).
