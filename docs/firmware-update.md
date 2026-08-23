# Updating Firmware

**Online → Update Firmware…** installs new firmware on a connected device over the serial link. No debugger, no dismantling the installation.

## What happens, and why it is safe

The image is not written over the running firmware. It is sent to a spare area of the device's flash — the *staging* area — while the device carries on running normally, still handling CAN traffic throughout. Only when the whole image has arrived and the device has checked it does it restart and install.

That ordering is what makes an interrupted update harmless. If the cable is pulled, the program is closed, or the image turns out to be corrupt, the device is still running exactly the firmware it started with. The worst outcome is a wasted few seconds.

Even a power cut during the install itself is survivable: the staging copy is kept intact until the new firmware is verified in place, so the device simply finishes the job the next time it powers up. It will retry the install up to three times; a unit interrupted that many times in a row needs an ST-Link.

## Doing it
1. Connect to the device as usual (**Tools → Connection Settings…**).
2. Open **Online → Update Firmware…**. The **Device** panel shows what is currently running.
3. **Browse…** and pick the `.ctf` file you were supplied. The program checks it immediately, using the same test the device itself applies — a damaged or wrong-product file is refused here, before anything is sent.
4. Read the warnings, if any (see below).
5. Leave **Save a copy of the device's configuration** ticked.
6. **Update Firmware**.

A 55 KB image takes roughly six seconds to send, and the device is back about two and a half seconds after it restarts. The progress bar sits still for a moment at the start while the staging area is prepared — that is erasing, not a hang.

## The configuration backup

Some firmware updates change the format the configuration is stored in. When that happens the device's saved configuration cannot be read by the new firmware and the unit starts up with nothing configured. **The configuration cannot be recovered from the device afterwards** — reading it out beforehand is the only opportunity.

So the update reads the configuration out first and saves it as a dated `.ct3` file in:

`Documents\CAN Triple Device Manager\Firmware Update Backups\`

If the device does come back empty, the program offers to send the backup straight back. On a password-protected device you will be asked for the Send Configuration password again at that point — the restart clears the device's memory of it, which is expected.

If the backup *fails*, the update does not start. Continuing would destroy the very thing the backup was protecting.

## Warnings you may see

<table>
<tr><th>Warning</th><th>What it means</th></tr>

<tr><td>This update changes the stored-configuration format</td>
<td>The device will start up with no configuration. Keep the backup ticked; the
program will offer to restore it afterwards.</td></tr>

<tr><td>This image is OLDER than the firmware the device is running</td>
<td>Going backwards is allowed, but rarely intended. Check you picked the right
file.</td></tr>

<tr><td>The device is already running version …</td>
<td>Harmless, but it will not change anything.</td></tr>

<tr><td>This device has no bootloader</td>
<td>The unit is running firmware from before over-the-wire updates existed. It
needs a one-time update using an ST-Link debugger; after that it can be updated
from here like any other. <b>Update Firmware</b> stays disabled until then.</td></tr>

<tr><td>This image needs bootloader version …</td>
<td>The file is newer than this unit's bootloader can install. You need either
an older image or a bootloader update over ST-Link.</td></tr>
</table>

## If something goes wrong

Reopen **Online → Update Firmware…** — the **Device** panel reports what happened on the last attempt, including anything that failed while nothing was connected to hear about it.

<table>
<tr><th>Message</th><th>What to do</th></tr>

<tr><td>The device refused the image</td>
<td>The file did not arrive intact. Try again; if it keeps happening, the file
itself is probably damaged — get a fresh copy.</td></tr>

<tr><td>Last install attempt failed: checksum does not match</td>
<td>Same cause, caught after the restart. The previous firmware is still running
and unharmed.</td></tr>

<tr><td>The device did not come back within 25 seconds</td>
<td>Wait a little longer, then reconnect and reopen this dialog. The install
either finished or was never started; either way the device is running valid
firmware.</td></tr>

<tr><td>The device is password protected</td>
<td>The update needs the Send Configuration password. On a unit that also guards
"Get a Configuration", prove that password first from any Online command — the
dialog cannot read the device's firmware status or take a configuration backup
without it, and the Update button stays disabled.</td></tr>
</table>

A device whose front LED is flashing rapidly and which does not answer at all has no valid firmware to run and needs an ST-Link. This should not happen from a failed update — the running firmware is never touched until a replacement has been fully verified — but it is what a physically damaged unit looks like.

## See also
- [Troubleshooting](troubleshooting.md)
- [Configuration files](files.md)
