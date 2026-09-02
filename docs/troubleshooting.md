# Troubleshooting

## Serial connection problems

The device talks over the ST-Link's virtual COM port at **7,372,800 baud** — an ST-Link **V3** is required at that rate. In **Tools → Connection Settings…**:
- Click **Refresh** after plugging in; the ST-Link port is preselected when its description contains "STLink".
- "Failed to open COMx" usually means another program holds the port — close other terminals, loggers or a second copy of this program.
- After connecting, click **Test**. If the device answers, the link is fine and any remaining problem is elsewhere; if it times out, check the cable and that the CAN Triple firmware (not a bootloader) is running.

Each command waits 250 ms for a reply (4 s for flash operations) and is retried up to 5 times before the program reports a failure, so occasional line noise is absorbed silently.

## After a firmware update the device reports an empty configuration

This happens when an update changes the stored-configuration format, and it is expected rather than a fault. The configuration in the device's flash carries the flash-store version and a CRC of the image, and firmware loads only an image whose version matches its own exactly — in either direction — rather than risk misreading its records. When the versions differ the first boot after flashing comes up on bring-up defaults, Device Status… shows 0 active messages, and Get Configuration returns nothing. Any update that changes the store format asks the one re-Send below — and because the device's access passwords live beside the configuration in the same store, they need setting again too.

**Fix:** open your saved .ct3 and run **Online → Send Configuration** (F5). The send verifies the transfer and saves the configuration to flash as part of the same operation, so it reloads at every power-up. Your .ct3 files on disk are never affected by a firmware update. For the update procedure itself, see [Updating Firmware](firmware-update.md).

## Device error (NACK) meanings

When the device refuses a command, the program reports "Device error:" followed by one of these:

<table>
<tr><th>Reported error</th><th>Meaning and what to do</th></tr>
<tr><td>invalid command (0x01)</td>
<td>The firmware predates the command. Harmless in most flows — the program
detects this and reports the step as skipped or the feature as unsupported
("Skipped (not accepted by this firmware)"). Update the firmware to get the
feature.</td></tr>
<tr><td>invalid length (0x02)</td>
<td>The GUI and the firmware disagree about the size of a table record —
in other words, <b>mismatched GUI and firmware versions</b>. Record layouts
change together on both sides (for example the math record grew when the
advanced operations were added), and the length check is what turns a
mismatch into a clean refusal instead of a misread configuration. Update
both to builds from the same source tree.</td></tr>
<tr><td>invalid CRC (0x03)</td>
<td>The frame arrived corrupted — at 7.37 Mbaud this simply happens now
and then on a long transfer. The program retransmits automatically, so a
single glitch no longer aborts a Send. If it persists, suspect the USB
cable or the serial line.</td></tr>
<tr><td>out of bounds (0x04)</td>
<td>An index outside the device's table — normally prevented by validation
before a send.</td></tr>
<tr><td>flash write failed (0x05)</td>
<td>The device could not write its flash during Save to Flash. Retry; if it
persists the unit needs attention.</td></tr>
<tr><td>bus busy (0x06)</td>
<td>The device could not act on a bus command at that moment. Retry.</td></tr>
<tr><td>the device configuration is password protected (0x07)</td>
<td>The device guards this operation with an access password that this
session has not proved. The program normally prompts for the right one — Send a
Configuration for anything that writes, clears or commits, Get a Configuration
for anything that reads. Protected Comms never causes this error code: the
device only confirms that password — it is this program that acts on the
answer, refusing a send before anything is written. See
<a href="licensing.md">Firmware Licensing &amp; Access Keys</a>.</td></tr>
<tr><td>the device refused the image (0x08)</td>
<td>Raised only by Update Firmware: the unit has no bootloader, the image was
built for a different product, or the staged image failed its checks. The
Firmware Update status names the specific reason. See
<a href="firmware-update.md">Updating Firmware</a>.</td></tr>
</table>

## "Two writers" warning in Check Channels

The warning that two (or more) things write the same channel means exactly that: the device has one value slot per channel, so the writers overwrite each other and whichever runs last wins. Every writer counts — receive message rows, math channels, User Conditions, counters, timers, integrators, constants and table outputs — and one warning is raised per channel, naming all of them. An inactive row counts as no writer.

It is a **Warning**, never an Error, so it does not block Send: there are configurations where last-writer-wins is intended. If it is not what you meant, re-point one of the named writers at its own channel. *Reading* a channel in many places is always fine — transmitting it, using it in math or as a table axis never alters the value. See [Validation &amp; the Config Summary](validation-report.md).

## Get Configuration returns nothing / device looks inert
- **The stored configuration is locked to a different CAN Triple.** A configuration sent with "Lock this configuration to this device" refuses to run on any other unit, and such a device reads back as empty. Both Get Configuration and **Online → Device Status…** say so explicitly. Send a configuration to the device to replace it.
- **No stored configuration** — the device is running bus defaults (see Device Status…). Send one.
- **Firmware update** — see the empty-configuration section above.

## An access password disappeared after a power cycle

On current firmware a password set in **Online → Set Access Passwords…** is in force immediately but reaches the device's flash only with the next flash commit — in practice, the next Send Configuration. Set the password, then send a configuration before powering the unit off; the dialog warns about this at the moment the password is set.

## Other messages worth knowing
- **"The configuration has errors. Fix them first — see File → Check Channels."** — Send refuses while any validation Error exists. The Check Channels report opens automatically; fix the red entries.
- **"This firmware is too old to save to flash"** after a send — the configuration was sent and verified but lives in device RAM only, and is lost at power-off. Update the firmware to keep it.
- **"Hidden comms — concealed"** in the status bar — the document carries a Protected Comms password, this session has not entered it, and at least one message marked **Hidden** or **Protect Communication** is therefore showing no detail. **File → Reveal Protected Comms…** is not by itself the answer: each of those messages also has a **Message Password** of its own, and it is opened by selecting it in Communications Setup and pressing **Edit…**, which asks for whatever that message's level requires. A document whose only markings are **Read Only** reads "revealed", because Read Only withholds nothing from anyone — see [Marking a message](communications.md#marking).
- **A message you just opened is padlocked again** — that is deliberate. Closing the section editor on a message still marked **Hidden** or **Protect Communication** re-locks it immediately, whether you pressed OK or Cancel: the password opened it once and the box is still ticked. Press **Edit…** again to reopen it.
- **Send Secure Configuration refuses a package** — the package was built for a different vendor, model, serial number or fleet. That check has no override by design; see [Online: Send, Get &amp; Flash](online.md).

> **Note:** For a quick overall picture — firmware protocol version, active table counts, device ID, which access passwords are set, and the firmware licence — use **Online → Device Status…** before digging further.
