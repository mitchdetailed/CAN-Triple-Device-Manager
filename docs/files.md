# Configuration Files (.ct3)

One document — one configuration file. The document holds everything: buses, messages, channels, calculations, the fleet identity and the upload policy. There are two on-disk formats, and the difference between them is who can read the bytes.

<table>
<tr><th>Format</th><th>Written by</th><th>What it is</th></tr>
<tr><td><b>.ct3</b></td><td>File → Save / Save As…</td>
<td>Indented JSON. Every CAN ID, bit layout and scaling factor is legible in
any text editor.</td></tr>
<tr><td><b>.ct3s</b></td><td>File → Save Secure Config…</td>
<td>An opaque, encrypted binary container of the same document, for shipping
a configuration whose protocol detail must stay unreadable.</td></tr>
</table>

> **Warning:** A plain .ct3 contains every field in clear JSON, and nothing in it is signed. Marking a message **Read Only**, **Hidden** or **Protect Communication** only stops *this application* from displaying or editing it — none of the three survives a text editor, which reads the protocol straight out of the file and can delete the marking outright. None of them survives a different serial tool talking to the device either, because the device enforces none of them. Only Save Secure Config… makes the bytes themselves unreadable. See [Marking a message](communications.md#marking).

## Opening files

**File → Open…** offers both formats in one filter ("CAN Triple Configurations (\*.ct3 \*.ct3s)"). The program decides which reader to use from the file's magic bytes, not its extension, so a renamed file still opens correctly and a .ct3s can never be misparsed as JSON.

Two kinds of file ask for a password before they open:
- A **.ct3s saved with "Require access password for use"** prompts for its Edit Protected Comms password.
- A **plain .ct3 written by an old version** of the program under the retired Configuration Password prompts for that password, and the prompt names it as such.

In both cases the prompt appears *before* the current document is touched — cancelling leaves whatever you had open exactly as it was.

> **Note:** Files saved by earlier versions open normally. Where a feature has changed shape the file is migrated as it loads: a **4x4 lookup table** from an older configuration becomes an [8x8](tables.md) with its sites, cells and interpolation modes intact, occupying the top-left of the wider grid, and a **User Condition** written before conditions had modes becomes a [Set/Reset](conditions.md#migration) whose Reset expression is generated as the logical inverse of its Set — which behaves identically to the plain level it replaces, though it does mean opening one and finding a Reset expression you did not write. Nothing needs re-entering, and the next Save writes the file in the current form. The reverse does not hold — a file saved by a *newer* version of the program is refused rather than half-read, because a field this build does not know about is one it would silently drop.

**File → Recent Files** keeps the last 8 files you opened or saved, most recent first, with the full path shown as a tooltip. Opening one behaves exactly like Open…, including the password handling. The submenu is disabled while the list is empty.

## Saving
- **Save** writes back to the file's own path *in the format the file already has*. A secure .ct3s is never silently rewritten as legible JSON just because Save was the quick path.
- **Save As…** always writes a plain .ct3, whatever the document came from. This is deliberate: Save As… is how you deliberately produce a legible copy, and Save Secure Config… is its counterpart. A name typed without an extension gets ".ct3" appended.
- **Save Secure Config…** always writes a .ct3s (its file dialog offers only "CAN Triple Secure Configurations (\*.ct3s)"), and appends ".ct3s" to an extensionless name.

Closing the program, File → New and File → Open… all ask about unsaved changes first ("The configuration has unsaved changes. Do you want to save them?" — Save / Discard / Cancel).

## Save Secure Config… — the two modes

The Save Secure Config dialog has one decision in it, the **Require access password for use** checkbox:
- **Unchecked (standard)** — the body is encrypted, but the key that decrypts it travels inside the file, obfuscated. Anyone with CAN Triple Device Manager can open the file, send it to a device and use its channels; nobody can read the protocol detail out of the bytes with a hex editor or text search, and the protected messages stay concealed in the UI. This is the shipping format for customers.
- **Checked** — the file key is additionally wrapped under the document's Edit Protected Comms password, so the file cannot be opened at all without it.

> **Warning:** With "Require access password for use" there is no recovery — no reset, no back door, no copy held anywhere. A lost password is a lost configuration. The dialog says so before you save; take it literally.

A document that has no Edit Protected Comms password yet can set one directly in this dialog (Password / Confirm fields). A document that already carries one is instead asked to confirm the existing password, since the saved file will require it.

## Version compatibility

Every .ct3 records the schema version it was written at (currently **18**). Older files keep loading — fields a newer schema added simply take their defaults. A file whose version is *ahead* of the program is refused rather than half-read, with a message saying it was saved by a newer version of CAN Triple Device Manager; update the program to open it.

Refusing is deliberate, and the reason is worth knowing: a setting an older program does not recognise is not always a setting it can safely ignore. A message set to transmit only on a [User Condition](communications.md#triggered) looks like an ordinary message to a program written before that existed, and sending it from there would put a continuously transmitting message on the bus. A [User Condition](conditions.md#modes) with a mode is the same hazard in its sharpest form: a program written before the modes finds none of the keys it expects, reads every condition as a single empty comparison — because a missing key has always been a legal way to say "default" — and would send a configuration whose logic is simply absent. Refusing the file names the real remedy instead.

> **Note:** The file schema version is independent of the firmware protocol version. Updating firmware never invalidates your .ct3 files — but it can invalidate the configuration stored *in the device's flash*, in which case the device comes up empty and the configuration has to be sent again. See [Troubleshooting](troubleshooting.md).

## What the file carries besides the configuration
- The **fleet identity and upload policy** — which fleet the file is for and how strictly a device must match before Upload Configuration will install it. See [Fleet Identity &amp; Access Keys](fleet-identity.md).
- An **access verifier** for the Edit Protected Comms password, so the program can check a typed password offline. It cannot be turned back into the key that opens hardware, so a file lying around leaks nothing usable.
- In a .ct3s only: the embedded comms key that lets a customer's copy satisfy a device's protected-comms gate without them ever typing the password. The fleet key is written only into the encrypted .ct3s body, never into plain JSON.

To install a .ct3s on a device without ever displaying its contents, use **Online → Send Secure Configuration…** or **Online → Upload Configuration…** — see [Online: Send, Get &amp; Flash](online.md).
