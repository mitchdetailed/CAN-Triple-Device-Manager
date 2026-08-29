# Communications: Messages &amp; Sections

Everything the device sends and receives on its three CAN buses is defined in **Connections &gt; Communications…**, which opens the **Communications Setup** dialog. The dialog has one tab per bus — **CAN 1**, **CAN 2** and **CAN 3** — plus a **Passwords** tab holding the configuration's four [Message Passwords](#passwords). Each bus tab holds that bus's options and its list of *sections*. A section is one entry on the bus: a receive message, a transmit message — plain or [Transmit CRC8](#crc8), which stamps a checksum into the frames it sends — a [message relay](relays.md), or Off.

The device stores receive and transmit messages in one shared table of **500 messages**, drawn freely across the three buses. Relay sections do not use that table — they have their own table of 32 rules — and an Off section uses nothing at all. A Transmit CRC8 message sits in the message table like any transmit message and additionally takes one rule from the device's table of **20 CRC8 rules**, shared across all three buses. The "*N of M device messages used*" label under the sections list counts every section on every bus, so it reads high when a bus carries relays or Off entries. Signals (channel rows) share a separate budget of 1000 across the whole configuration.

## Bus options

The **Options** row at the top of each tab configures the bus itself:

<table>
<tr><th>Control</th><th>Choices</th><th>Meaning</th></tr>
<tr><td><b>Mode :</b></td><td>CAN / Off</td><td>Whether the bus runs at all. The
combo is coloured — green for CAN, red for Off — so a disabled bus is obvious at
a glance.</td></tr>
<tr><td><b>Rate :</b></td><td>1M / 800k / 500k / 250k / 200k / 125k / 100k /
83.3k / 50k</td><td>Arbitration bitrate. 83.3k is GMLAN's 83.333 kbit/s
(1 Mbit / 12) — the device is programmed with 83,333 bit/s,
not 83,000.</td></tr>
<tr><td><b>FD Data :</b></td><td>Off (classic) / 1M / 2M</td><td>CAN FD data
phase rate. Setting a rate here is what enables the <b>CAN FD frame</b> checkbox
in the section editor for this bus. The data rate must <em>exceed</em> the base
rate — that is the device's own test for bringing the bus up in FD — so choices
at or below the current Rate are disabled (1M is available while the base rate
is below 1M).</td></tr>
<tr><td><b>Termination Resistor :</b></td><td>Off / On</td><td>Enables the bus's
built-in 120Ω termination resistor. Applied regardless of Mode — a bus can be
terminated while Off.</td></tr>
</table>

> **Note:** Bus options take effect on the device when you Send the configuration (see [Online: Send, Get &amp; Flash](online.md)), and they persist across power cycles once saved to flash. A Get Configuration reads the live bus setup back from the device.

## The sections list

The list is labelled **Sections :  (list order = transmit order)** — on each cycle the firmware composes and enqueues the bus's transmit messages in list order, top first, so moving a message up sends it earlier. The **Section** column shows the kind (**CAN Rx**, **CAN Tx**, **CAN Tx CRC8**, **Relay** or **Off**, with a lock-and-pen 🔏 beside it for a **Read Only** message, or a padlock 🔒 on its own replacing the kind for a **Hidden** or **Protect Communication** one), and the **Name** column shows the name, CAN ID, an "x" marker for extended IDs, a "(compound)" tag, or a relay's forwarding targets — or, for a concealed message, the name alone followed by "(hidden)" or "(protected)".

The buttons beside the list:

<table>
<tr><th>Button</th><th>Action</th></tr>
<tr><td><b>Select…</b></td><td>Predefined device templates — planned; currently
disabled. <b>Load…</b> below does the same job from a template file you or a
supplier saved.</td></tr>
<tr><td><b>Import DBC…</b></td><td>Import messages and signals from a .dbc file —
see <a href="dbc-import.md">DBC Import</a>.</td></tr>
<tr><td><b>New…</b></td><td>Create a section and open the section
editor.</td></tr>
<tr><td><b>Edit…</b></td><td>Open the selected section in the section editor.
Double-clicking a row does the same. It needs a <i>single</i> row selected —
there is no one section for it to open out of several.</td></tr>
<tr><td><b>Save…</b></td><td>Write the selected messages to a communications
template (*.ct3t) — see below.</td></tr>
<tr><td><b>Load…</b></td><td>Add a template's messages to this bus, creating the
channels they need — see below.</td></tr>
<tr><td><b>↑ Move Up</b> / <b>↓ Move Down</b></td><td>Reorder the
list, and with it the transmit order.</td></tr>
<tr><td><b>Remove</b></td><td>Delete the selected sections. Removing more than
one asks first.</td></tr>
<tr><td><b>Remove All</b></td><td>Delete every section on this bus, after
confirmation.</td></tr>
</table>

### Selecting more than one message

The list takes ordinary multiple selection: **shift-click** a second row to take everything from the first to it, and **ctrl-click** to add or drop a single row. **Remove**, **Move Up** and **Move Down** then act on the whole selection — which is what makes tidying up after a DBC import bearable.

Moving works the way you would expect from a file manager. The selection moves one place as a unit, keeping its own internal order, and stays selected afterwards so you can press the button again to keep going. A scattered selection is fine: each selected message steps one place and the unselected ones between them are pushed the other way. The two buttons grey out when the selection already reaches the end it would travel towards — the group has nowhere to go, and moving only part of it would change the order *within* your selection, which is the transmit order.

The **Channels :** pane on the right lists the channels carried by the selected section, grouped by identifier for a compound message. A relay shows "(relay — forwards whole frames, no channels)"; a **Hidden** or **Protect Communication** message shows "(Channel information locked)". A **Read Only** message lists its channels normally — it conceals nothing.

<a id="templates"></a>

## Communications templates

A template is a group of messages lifted out of one configuration so it can be dropped into another — the supplier's ECU, the dash that goes in every car, the lambda controller whose protocol never changes. **Save…** writes one, **Load…** reads one back, and what travels is everything the section editor's two tabs hold: every field on **Parameters**, every row on **Received Channels** or **Transmitted Channels**, the **CRC8** recipe where there is one, and the definitions of the channels those rows name.

Templates are files, so they can be emailed, kept in version control, or handed to a customer.

### Saving

**Save…** writes *the messages you have selected*, not the whole bus — so it saves one message or thirty, and **Ctrl+A** in the list is how you save the lot. The button's tooltip names what it is about to write. The bus settings (rate, FD data rate and termination) are recorded alongside them, because a template for a 500 k device is not much use if it arrives on a 1 M bus and says nothing about it.

> **Note:** **Any message can go into a template, locked or not.** A **Hidden** or **Protect Communication** message keeps its marking through the file, and the template carries the configuration's [Message Passwords](#passwords) (as derived keys) alongside it, so loading it elsewhere produces a padlocked row rather than an open one — which is why saving one does not ask you to unlock it first. The message arrives on the other side guarded by exactly the password that guarded it here, and whoever saved it never saw inside it either. The confirmation tells you when a file carries locked messages, so you know what you are handing on.

> **Note:** **Loading fits the file's passwords into your four slots first.** A password the destination already holds costs nothing; one it lacks takes a free slot, whichever is free — the slot *number* a password occupied at the source means nothing at the destination. If the passwords the file's marked messages need cannot all fit, the load is refused before anything is applied: free a slot on the Passwords tab, or remove the markings from the template. Passwords already in your slots are never overwritten by a load.

> **Note:** Protection is enforced where it bites: **opening a concealed message for editing**. That takes the **Message Password** guarding it, and for **Protect Communication** the **Protected Comms** password confirmed by a connected device as well. Save and Load move a locked message around; neither one opens it.

<a id="protectedsend"></a>

### Sending a configuration that contains protected messages

**It goes only to a unit that holds the same Protected Comms password.** Before sending, the Device Manager asks the connected unit to confirm the password the configuration carries. If the unit agrees, the send proceeds normally. If it does not, nothing is sent and the reason is named:
- **The unit has no Protected Comms password set.** No password is not the same password. Set one on the unit to match the configuration's, under **Online → Set Access Passwords…** — any of the unit's four Protected Comms slots will do.
- **The configuration carries no Protected Comms password of its own** — there is nothing to match against. Give it one with **File → Save Secure Config…**, or remove the markings.
- **The two disagree.** This is the case the rule exists for.

> **Note:** The unit holds up to **four** Protected Comms passwords — [Set Access Passwords](fleet-identity.md) shows them as Slot 1 to Slot 4 — and confirming *any* of them satisfies this check. Four slots are what let one device accept sealed configurations from more than one supplier without any of them sharing a password.

You are not asked to type the password at this point, and that is deliberate: being able to supply a unit's password would let anyone who knows it send a configuration that was never sealed with it, which is the substitution the marking is there to prevent. The password has to be *in* the configuration, which is what saving it as a secure file does.

> **Note:** This applies only to **Protect Communication**. Read Only and Hidden are conventions of this application — the device enforces nothing about them — so neither one holds up a send. It also applies only to messages that are actually being sent: a section switched **Off** is not uploaded, so a marking on one does not stand in the way.

### The file is not readable

A .ct3t is binary and encrypted. Nothing in it can be read with a text editor, found by searching the file for a channel name or a CAN ID, or recovered by any tool that does not implement the format — which is the point, since the file exists to be given away. Two saves of the same messages produce different bytes, so the files cannot be compared against each other either.

> **Warning:** Be clear about the limit, because it is the same one a [secure configuration](files.md) (.ct3s) has in its standard mode: the key that decrypts the file travels inside it. This stops a customer reading your protocol out of a file you sent them. It does not stop someone who sets out to reverse-engineer the program itself. If that is your concern, the protection you want is per-message **Hidden** or **Protect Communication** marking, which survives inside the template.

### Loading

**Load…** adds a template's messages to the end of the current bus's list and selects them, so they are ready to reorder or think better of.

<a id="folders"></a>

### Where the files are kept

The program keeps two folders of its own, beside the program itself:

<table>
<tr><th>Folder</th><th>Holds</th></tr>
<tr><td><code>Communications Templates</code></td><td>Templates (*.ct3t). Both
<b>Save…</b> and <b>Load…</b> start here.</td></tr>
<tr><td><code>Configurations</code></td><td>Configurations (*.ct3, *.ct3s).
<b>File → Open</b>, <b>Save As</b> and <b>Save Secure Config</b>
start here when the document has no file of its own yet.</td></tr>
</table>

On a standard install that means:

`C:\Program Files\Minton Performance\CAN Triple Device Manager\Communications Templates`

Both are ordinary folders — put files in them, take them out, keep sub-folders of your own — and both are only a *starting point*. Every one of those dialogs lets you browse anywhere, and once a configuration has been saved somewhere, its own folder is where it opens and saves from next.

Because they sit with the program rather than in one person's Documents, the whole machine shares them: every Windows account on that PC sees the same templates and the same configurations. On a workshop bench with several technicians that is usually what you want. It also means anyone using that PC can overwrite or delete what is in them, so they are shared storage, not private storage.

> **Note:** The installer is what grants permission to write to these two folders — nothing else inside the program directory is writable. A copy put there by an older installer, or on a machine whose IT policy has since removed that permission, will refuse to save and say so, naming the folder. Re-running the current installer repairs it.

> **Note:** Uninstalling does *not* delete what you put in them. The uninstaller removes only the files it installed, and both folders ship empty, so everything in them is yours and stays — which does mean that after an uninstall they are left behind in the program directory with your files still in them.

Two other folders are unchanged and still live under your Documents, because that is where earlier releases put them and moving the folder would not move the files already in it: **Documents → CAN Triple Device Manager → Firmware Update Backups** and **… → Scripts**.

The messages arrive naming channels, and those channels have to exist in *this* configuration for the rows to decode into anything. Loading resolves them one at a time:
- a channel this configuration does not have is **created**, with the units, data type, base resolution, decimal places and range it was saved with;
- a channel it already has, meaning the same thing, is **reused** — so loading the same template twice does not fill the catalogue with copies;
- a channel it already has that means something *different* — a different resolution, different units — is left alone, and the incoming one is **renamed** ("Coolant Temp 2"). Overwriting yours would silently change what every other message using that channel decodes to, with nothing on screen to explain the new numbers.

Anything that could not be resolved cleanly is listed under **Show Details** in the box that appears afterwards: every rename, a relay whose forwarding target was the bus you loaded it onto (that target is dropped — a bus cannot relay to itself), and any channel the template refers to that this configuration does not have. The one that always appears is a **Triggered** transmit message: it names a [User Condition](#triggered), and a template cannot carry conditions, so you have to build that condition before the message will send.

If the template's bus settings differ from the bus you are loading onto, you are asked whether to apply them. Answering no leaves the bus exactly as it was.

> **Note:** The channels a load creates go into the configuration immediately, and stay there even if you then press **Cancel** — only the messages are discarded. **Import DBC…** has always behaved this way, and the two match on purpose. **Calculations → Remove Unused Channels** clears up anything left behind.

<a id="marking"></a>

## Marking a message: the three levels

**Protecting a message protects its protocol, not its place in your configuration.** That one sentence governs everything below, and it is the reason **Remove**, **Move Up** and **Move Down** work at every level and a Get is never refused.

<a id="passwords"></a>

### The Passwords tab: four Message Passwords

Markings are guarded by the configuration's **Message Passwords** — up to **four**, held on the **Passwords** tab of this dialog. They belong to the configuration rather than to any one bus or message, and a marked message *names* the one that guards it. Only the derived key is stored, never the password itself, so a slot can be cleared and set again but never read back.

The tab shows each slot's **State** — "not set", "set", or "set, guarding *N* messages" — with one button per slot: **Set** while the slot is empty, **Clear** once it holds a password.
- **Set** asks for the new password (at least 4 characters). Each slot must hold a *different* password: a message names a slot, so two slots holding one password would make the second unreachable.
- **Clear** asks for the current password first — the password is what authorises removing it. A slot that is guarding messages cannot be cleared: release those markings first.

### Ticking a box

The section editor offers three tick boxes, and **a message wears one marking at a time** — exactly one box, or none. Ticking a box asks, there and then, for the password that will guard the message, and the answer must be **one of the configuration's four Message Passwords**: with none set at all you are sent to the Passwords tab first, and a password that matches no slot is refused with the same advice. Cancel the prompt and the box goes back up, because a marked message with nothing guarding it is not a state worth creating.

> **Note:** **To change a message from one marking to another, release the current one first.** Ticking a second box while a marking is already set is refused — "Object is currently Read Only, release Current Message Protection before enabling another protection model" — and the way through is exactly what the popup says: untick the current box, which asks for the password guarding it, then tick the box you want and choose the password for that. Two decisions rather than one, deliberately: a marking is chosen, not slid into.

<table>
<tr><th></th><th>View</th><th>Edit</th><th>Remove</th><th>To tick or untick the box</th></tr>
<tr><td><b>Read Only</b></td><td>allowed</td><td>refused</td><td><b>allowed</b></td>
<td>the <b>Message Password</b> guarding it — one of the configuration's
four</td></tr>
<tr><td><b>Hidden</b></td><td>refused</td><td>refused</td><td><b>allowed</b></td>
<td>the <b>Message Password</b> guarding it</td></tr>
<tr><td><b>Protect Communication</b></td><td>refused</td><td>refused</td><td><b>allowed</b></td>
<td>the <b>Message Password</b> guarding it. The tick itself works offline —
what this level adds happens elsewhere: a configuration containing such a
message can only be <a href="#protectedsend"><em>sent</em></a> to a unit that
confirms the <b>Protected Comms</b> password, and <em>opening</em> an existing
Protect Communication message needs a connected device confirming it
too</td></tr>
</table>

Unticking a box removes the marking altogether, after asking for the Message Password guarding it — the prompt's "this section's own Message Password" means the one of the four this message names.

> **Note:** Opening a hidden message does not leave it open. When you close the section editor — with OK or with Cancel — and the message is still marked Hidden or Protect Communication, it goes straight back to padlocked in the sections list and its Channels pane returns to "(Channel information locked)". The password you gave opened it once; it is not a standing licence to leave it on screen while the box is still ticked.

**View** means the CAN ID, length, byte order, timing and every channel's bit layout and scaling — in the sections list, the Channels pane, the section editor, Check Channels and the reports. Channel *names* stay visible at every level, so the channels a marked message produces can still be used everywhere else. A concealed message shows its name and "(hidden)" or "(protected)" and nothing more.

**Edit** is refused at every level, *including* Read Only, and it stays refused even after you have supplied a password. Supplying the password buys viewing and the right to untick the box; unticking is what allows editing. The channels a locked message produces are locked with it in the [Channel Editor](channels.md) — data type, resolution, decimal places, range and units — because changing a channel's resolution silently changes what the message decodes to.

**Remove** is allowed at every level, everywhere: in this dialog, from a script, and on the device. Nothing anywhere refuses it.

**File &gt; Reveal Protected Comms…** takes the document's **Protected Comms** password. It is *not* a master password over any of the three levels. A **Hidden** message is opened by the Message Password guarding it and by nothing else, and so is unticking **Read Only**; a **Protect Communication** message needs its Message Password too, so revealing the document is only one of the things that message asks for. Revealing does not untick anything by itself and does not make anything editable.

A marked message with *no* Message Password behind it can still reach you — in a file that carries markings without the passwords that guard them. Such a message **cannot have its marking unticked by anybody**: unticking is authorised by the password guarding it, and there isn't one. At **Hidden** and **Protect Communication** it is also **concealed from everybody** — no password for it exists, so there is nothing that could open it and nothing to go looking for. A **Read Only** message is not concealed by this or by anything else; it opens and shows every field, as Read Only always does. The sections list says which you have rather than leaving you hunting: a concealed one reads **(hidden — no password)** instead of plain **(hidden)**.

What you can still do with such a message, at *every* level: **remove** it, which is always allowed; reorder it and send it as it stands; and use the channels it produces anywhere else in the configuration.

**Whether you can repair it depends on the level.** A **Read Only** message opens — Read Only conceals nothing — and unticking the box on one finds nothing to prove, so the editor offers the repair itself: give the message a Message Password now (one of the four), keep it with OK, and the marking can be released with that password the next time the message is opened. Two visits, deliberately: the untick is checked against the password the document is holding, not against one typed in the same sitting. A concealed **Hidden** or **Protect Communication** message is not repairable — the editor will not open for a message nobody can open, and pressing **Edit…** explains this instead of prompting. Remove the message and enter it again, or go back to the configuration file it was built in, which still holds its passwords.

> **Note:** A device round trip never creates this state: the device stores the configuration's four Message Passwords (as derived keys, in its configuration store) and **Get Configuration brings them home**, re-guarding every marked message with the password it names — see [Online: Send, Get &amp; Flash](online.md).

### What these levels actually are

> **Warning:** **All three are conventions of this application.** Nothing on the device enforces any of them. A CAN Triple hands its full message table to anything that speaks its protocol and accepts a write over any record, so any other serial tool reads a marked message in full and overwrites it freely. A plain `.ct3` is scrambled rather than legible, which stops a text editor reading a marking straight out of it — but the key to a .ct3 travels inside the .ct3, and opening one in this program gives back every marked message in full. The marking that survives being handed to somebody else is **File &gt; Save Secure Config…** (`.ct3s`). If a protocol must stay secret from a determined reader, do not ship it to them.

> **Warning:** **Read Only is accident prevention, not security.** The viewer sees every field of the message and may remove it, so removing it and retyping what they read reproduces the message without the password. Treat it as a guard rail against an accidental edit and nothing more.

Hidden and Protect Communication are substantively different, for one reason: a viewer who cannot *see* the message cannot retype it, so removing it destroys it rather than revealing it. That is a real property and it is the whole of what concealment buys.

Protect Communication adds exactly one thing on top of Hidden: the [send gate](#protectedsend). A configuration containing such a message goes only to a unit that confirms the Protected Comms password, and opening an existing one here needs a connected device confirming that password as well as the Message Password guarding it. Holding the file is not enough. That is the whole difference between the two levels — what is withheld on screen, and from whom, is identical.

> **Note:** Earlier releases said the device refused to let a marked message be changed or erased without its password. That is no longer true and the claim has been removed rather than softened. The rule made a unit whose password had been lost impossible to clear or reconfigure, and it was walked past on the bench by clearing the device and writing a fresh message over the top. Files written by those releases still open: a message marked "Protect Communication" arrives as **Protect Communication**, and one marked "Read-only" arrives as **Hidden** — because that older marking concealed the message, and the new Read Only does not.

## The section editor

New… and Edit… open **CAN Communications Setup — CAN *n***, which has three tabs: **Parameters**; **Received Channels** or **Transmitted Channels** depending on the message type; and **CRC8**, which applies to exactly one message type and is enabled only while the type is [Transmit CRC8](#crc8).

The channels tab is **grayed out while the Message Type is Off or Message Relay**. Neither carries channels: an Off message is not built into a frame at all, and a relay forwards whole frames without looking inside them. It is grayed rather than hidden, and for **Off** the rows are **kept** — switch a configured message off and back on and its channels are all still there. A relay is the exception: its rows are dropped when you press OK, because a relay has no frame of its own to put them in.

### Parameters tab

<table>
<tr><th>Field</th><th>Meaning</th></tr>
<tr><td><b>Name :</b></td><td>The section's display name. Left empty
("(automatic)"), it defaults to "Receive 0x640", "Transmit 0x…" or "Relay 0x…"
from the base address.</td></tr>
<tr><td><b>Message Type :</b></td><td><b>Off</b>, <b>Receive Message</b>,
<b>Transmit Message</b>, <b>Transmit CRC8</b> (a transmit message that stamps a
checksum into its frames — see <a href="#crc8">below</a>) or
<b>Message Relay</b>. The type decides which of the remaining controls
apply. <b>Off</b> keeps the message and everything defined in it but sends and
decodes nothing, so its channels tab is grayed — the rows are still there and
come back the moment you set a type again.</td></tr>
<tr><td><b>Alignment :</b></td><td><b>Normal (big-endian)</b> (Motorola) or
<b>Word Swap (little-endian)</b> (Intel). One byte order for the whole
message.</td></tr>
<tr><td><b>Read Only</b> / <b>Hidden</b> /
<b>Protect Communication</b></td><td>The three marking levels — see
<a href="#marking">above</a>. <b>One at a time:</b> exactly one box, or none.
Ticking one asks, there and then, for one of the configuration's four
<a href="#passwords">Message Passwords</a> to guard the message. Ticking one
while another is set is refused: untick the current box first, which asks for
the password guarding it. Everything runs through those popups — there is no
password field on this tab.</td></tr>
<tr><td><b>Receive Timeout :</b></td><td>0–60000 ms. Receive messages
only.</td></tr>
<tr><td><b>Default value on timeout</b></td><td>When ticked and the timeout is
non-zero, each channel row's Default Value is written into its channel whenever
the message has not been seen within the timeout; derived calculations follow. A
fresh frame resets the window. A timeout of 0 disables the feature.</td></tr>
</table>

Under **CAN Settings**:

<table>
<tr><th>Field</th><th>Meaning</th></tr>
<tr><td><b>Address Format :</b></td><td><b>Standard</b> (11-bit, up to 0x7FF) or
<b>Extended</b> (29-bit, up to 0x1FFFFFFF).</td></tr>
<tr><td><b>CAN FD frame</b></td><td>Allows message lengths 12, 16, 20, 24, 32, 48
and 64 bytes. Enabled only when the bus has an FD Data rate set — or when the
section is already FD, so it can still be opened and, if you choose, converted
back to classic.</td></tr>
<tr><td><b>Base Address :</b></td><td>The CAN ID, in hex (e.g. 0x640). The label
beside it shows the decimal value and flags an ID that exceeds the 11-bit or
29-bit range.</td></tr>
<tr><td><b>Message Length :</b></td><td>The DLC in bytes. Editable for transmit
messages only — a transmit message is the frame it composes, while a receive
message's length comes from the sender. Classic frames are 0–8 bytes; FD frames
also allow 12, 16, 20, 24, 32, 48 and 64.</td></tr>
<tr><td><b>Transmission :</b></td><td><b>Cyclic</b> or <b>Triggered</b>. Transmit
messages only. <b>Cyclic</b> transmits every period, as it always has;
<b>Triggered</b> transmits only while a chosen
<a href="conditions.md">User Condition</a> is true — see
<a href="#triggered">below</a>.</td></tr>
<tr><td><b>Transmit Condition :</b></td><td>Appears only while
<b>Triggered</b> is selected. It lists the document's active
<a href="conditions.md">User Conditions</a>, and only a User Condition can be
chosen — no other channel is offered, however boolean it happens to
be.</td></tr>
<tr><td><b>Transmit Rate :</b></td><td>1, 2, 5, 10, 20, 50, 100 or 200 Hz. A
rate read back from the device that falls outside the presets is inserted into
the list rather than snapped to the nearest. When each transmission actually
happens — phase spreading, ordering, what an oversubscribed bus does, and what
a 200 Hz message's payload actually refreshes — is on
<a href="engine.md">Order &amp; Timing of Operations</a>.</td></tr>
<tr><td><b>Transmit Mode :</b></td><td>Compound transmit messages only:
<b>Batch (all IDs each period)</b> sends every identifier's frame each transmit
period; <b>Sequential (one ID per period, round-robin)</b> sends one.</td></tr>
</table>

The **Gateway Routing (CAN Triple)** group — **Route this message to :** with a checkbox per bus — forwards this one message's frames to other buses. The source bus is never routed back and stays disabled. For forwarding whole ranges of IDs by mask, use a [Message Relay](relays.md) instead; in relay mode this group is replaced by the **Message Relay** group.

<a id="triggered"></a>

### Triggered transmit

**Cyclic** transmits every period, which is what a transmit message has always done. **Triggered** transmits only while the chosen [User Condition](conditions.md) is true — the rest of the time the message is silent, and nothing else about it changes.

**Transmit Condition :** appears only while Triggered is selected, and it lists the document's active [User Conditions](conditions.md). Only a User Condition can be chosen; the ordinary channel picker is not offered here, so no other channel can be named however boolean its values happen to be. The condition is identified by its output channel rather than by its row number, which is what keeps the binding pointing at the same condition when rows above it are added, removed or reordered — and renaming that channel carries the binding along with it.

The control is hidden outright on a Cyclic message rather than greyed out: it has no meaning without a trigger, and a disabled control only invites you to wonder what reaching it would do.

**Sending once per event, rather than once per period, is done in the condition.** Triggered transmits for as long as its condition holds, so a condition that comes up and stays up produces a frame every period until it drops. When what you want is *one* frame — a reply to a request, a one-shot announcement — the place to say so is the [User Condition](conditions.md) itself, which has two shapes that do it:
- A **Set / Reset** condition **set on Message Received** and **reset on Message Transmitted**. The condition comes up when the request arrives, this message answers it, and the transmission itself puts the condition away again. One reply per request — see [the message operators](conditions.md#messages). The reset lands on the next calculation pass rather than the instant the frame leaves, so at 100 Hz or 200 Hz the message can come due again before it arrives and send a second frame; at every slower rate the reset is there first.
- A **Momentary** condition, whose output pulses for one period of its Latch Frequency on each rising edge and then drops on its own.

Writing it in the condition rather than beside the message is the better answer for three reasons: it reads where you can see it, next to the thing it resets; the reset can name a *different* message than the one it gates, which a per-message tick box could never do; and nothing reaches sideways from a transmit message to rewrite a calculation's output.

> **Note:** A tick box on this tab, **Reset User Condition once Triggered**, used to do the first of those by force — it set the named condition's output channel back to 0 after each frame. It is gone, and it never shipped, so no configuration in the field carries it. With it gone, two messages triggered on one User Condition simply both transmit while it holds, which is what sharing a condition ought to mean and needs no warning of its own.

**When the frame actually goes out** is the part worth reading twice. The device tests the condition in its **5 ms transmit slot** (200 Hz), whatever the message's own Transmit Rate is, so a condition that becomes true is acted on within 5 ms even for a 1 Hz message. A slow message is not a slow trigger.

The message then transmits at its configured **Transmit Rate**, and the interval is measured **from the moment the condition became true** — not from a free-running clock the message was already keeping. A 1 Hz message whose condition becomes true at 1.2 s transmits at 1.2 s, 2.2 s, 3.2 s and so on for as long as the condition holds. It does *not* transmit at 2.0 s and 3.0 s, and it does not sit out the remaining 0.8 s waiting for a tick that was already on its way.

Dropping the condition stops transmission immediately, mid-period if that is where it falls — there is no closing frame to round the period off. Arming it again starts a fresh interval with an immediate frame, exactly as the first arming did.

> **Note:** A Triggered message with **no User Condition selected**, or naming one the document no longer has — deleted since, or since made inactive — is an **error**: Check Channels reports it and Send is blocked until it is fixed. It is deliberately not treated as Cyclic. Falling back that way would turn a message configured to speak only on a condition into one that never stops — frames on somebody's bus that nobody asked for, from a configuration that looked like it mapped cleanly. A condition that has gone missing is kept as a selectable entry in the combo and marked "(missing)", so what the section names stays visible rather than being quietly rewritten to some other trigger the next time the message is opened.

The Config Summary prints such a message as "triggered on *channel*". A cyclic message says nothing there, because cyclic is what a transmit message has always been and every line would otherwise carry a word meaning "normal". See [Validation &amp; the Config Summary](validation-report.md).

> **Note:** Earlier releases stored **Transmission** in the file and then ignored it: a message set to Triggered transmitted cyclically like any other. That is why a file using a transmit condition is refused by an older build of the program rather than opened — the older build would read the section as a perfectly ordinary message, find nothing missing, and send one that transmits continuously where you had configured it to transmit only on a condition.

### Channels tab

The **Message Type** box selects **Single** or **Compound** (multiplexed). Switching between them removes all channels and identifiers, after a confirmation.

**Single** messages carry one flat list of channel rows. **Add…**, **Change…** and **Remove** manage rows; each row shows its channel name, start bit, width, type and scaling, and is tinted with the colour its bits carry in the frame map. Adding or changing a row opens the Comms Channel dialog — see [Channels](channels.md) for its fields.

**The list is always in frame order:** Start Bit, then Bit Length, then name. So it reads the way the frame map below it is drawn, and a channel is where its bits are rather than where it happened to be added. Two rows may share a start bit — a receive message can decode the same bits twice — and then the narrower field comes first, then the name. Changing a row's Start Bit moves it in the list, and the selection moves with it, so the frame map keeps shading the channel you were looking at. The order is saved with the message, and the **Channels** pane in Communications Setup shows the same one.

**Compound** messages carry channels only inside *identifiers* — there is no shared always-present set; a signal present in every variant is defined in each identifier. The **Identifiers :** table lists 16 numbered slots with **Offset**, **ID** and **ID Mask** columns; **Change…** opens the **Compound Message Identifier** dialog (Offset / Identifier / Identifier Mask) and **Clear** empties a slot. A received frame decodes an identifier's channels only while the frame's selector window at that byte offset matches (selector &amp; mask) == (ID &amp; mask). The channel list and frame map show one identifier at a time — variants may legitimately reuse the same bits.

**An identifier with no channels is still transmitted.** Give a slot an Offset, ID and ID Mask and leave it empty, and the device sends that variant each period carrying its selector over an all-zero payload — which is the whole message for a request or a ping frame, where the ID byte IS the content. Both cadences honour it: Batch sends it alongside the others, and Sequential gives it its turn in the rotation. Only slots you have actually set count; an untouched slot sends nothing.

### The frame layout map

The **Frame Layout** grid at the bottom draws the frame one byte per row, bits 7…0 left to right. The number in each cell is the global bit index — the value a channel's Start Bit field takes. Each channel's bits are filled with the same colour as its row in the list; clicking a coloured cell selects the channel that owns it, and the caption states the selected channel's exact extent.
- **Red cells** mean the bit is claimed twice — either by two signals, or by a signal and something the device writes over it. Either way the frame will not carry what the list says it carries.
- **Slate cells** are **reserved**: spoken for before any channel is placed, by a compound identifier's selector or by a Transmit CRC8's stamped byte. Neither a channel nor a fault — place your channels elsewhere. Put one there anyway and the cell turns red.
- **Dimmed bytes** lie beyond the message's own length: for a receive message the map draws the whole frame the bus can carry (8 bytes classic, 64 CAN FD) and greys the bytes past this message's length, where nothing is extracted.

<a id="overlaps"></a>

### Overlaps: what is warned about and what is refused

Two things can want the same bits, and they are not equally serious.

**Two channels overlapping each other** is only a problem on a message the device *sends*. On a **receive** message it is legitimate — the same byte decoded twice at different scalings, or a raw copy kept beside a cooked one — because reading a bit does not consume it. So a receive overlap is reported and allowed. On a **transmit** message the same overlap is two values packed into one place: whichever is packed last wins, and the other never leaves the device. That is refused.

**A channel under something the device writes itself** is always refused, on receive and transmit alike. There are two such things:
- a **compound identifier's selector** — the bits that say which sub-message this frame is. They are written after the channels on the way out and read over them on the way in, so a channel there carries the identifier value rather than its own;
- a **Transmit CRC8's stamped byte** — the checksum is stamped last, over whatever the channel put there.

In both cases the channel is not sharing those bits, it is *replaced* by them, which is why neither is something you can choose to accept.

<table>
<tr><th>Message</th><th>What collides</th><th>Result</th></tr>
<tr><td>Receive</td><td>two channels</td><td>Warned, allowed</td></tr>
<tr><td>Receive</td><td>channel under an identifier selector</td><td><b>Refused</b></td></tr>
<tr><td>Transmit</td><td>two channels</td><td><b>Refused</b></td></tr>
<tr><td>Transmit</td><td>channel under an identifier selector</td><td><b>Refused</b></td></tr>
<tr><td>Transmit CRC8</td><td>channel in the stamped byte</td><td><b>Refused</b></td></tr>
</table>

You meet a refusal twice, on purpose. **Add…** and **Change…** will not let a channel be *placed* on reserved bits at all — OK there stays greyed out and the reason is written under the fields — so the usual case never becomes a mistake you have to undo. Pressing **OK** on the message itself checks the whole layout again and names anything left, because a channel can also become trapped by something moving *underneath* it: change an identifier's offset or the CRC's byte location and the reserved bits move onto channels that were placed perfectly well.

> **Note:** A compound message is checked one identifier at a time, never across them. Two identifiers using the same bits is the whole point of multiplexing — they are alternative frames, never on the wire together — so that is not an overlap and is not reported. Only the identifier you are looking at reserves anything against its own channels.

Bit numbering: bits count 0–7 right-to-left within a byte (bit 0 is the LSB), bytes count left-to-right from 0, so bit *S* sits at byte *S*/8, bit *S*%8. The Start Bit is always the signal's **least** significant bit; a Word Swap (little-endian) field continues into higher-numbered bytes, a Normal (big-endian) field into lower-numbered bytes. For example, a 16-bit Word Swap field at start bit 0 reads bytes 0 (LSB) and 1 (MSB); a 16-bit Normal field across bytes 2 (MSB) and 3 (LSB) has start bit 24.

<a id="crc8"></a>

## Transmit CRC8

A **Transmit CRC8** message is a transmit message that stamps a CRC-8 checksum into one byte of every frame it sends. In every other respect it is an ordinary transmit message — same Parameters fields, same Transmitted Channels tab and frame map, same rates, routing, markings, CAN FD and compound behaviour — and the third tab, **CRC8**, holds the checksum's recipe.

The order of operations is the fact to hold on to: on each transmission the firmware packs every transmitted channel (and a compound message's identifier selector) into the frame *first*, computes the CRC over the configured elements, stamps it into the CRC byte *last*, and publishes the value to the configured channel. The stamp always wins its byte, and the published value is always the byte that went onto the wire. Where composition sits in the evaluation pass is on [Order &amp; Timing of Operations](engine.md).

### The CRC8 tab

<table>
<tr><th>Field</th><th>Meaning</th></tr>
<tr><td><b>Channel :</b></td><td>The channel the computed checksum is published
to, picked with <b>Select…</b>. Every transmission writes it, so the value on
the wire is visible in <a href="monitor.md">Monitor Channels</a> and usable
anywhere a generated channel is — a math input, a condition, another message.
OK refuses to close without one.</td></tr>
<tr><td><b>CRC8 Byte Location :</b></td><td><b>Byte 0</b>–<b>Byte 7</b>: the
byte of the frame the checksum is stamped into. It must lie inside the Message
Length — OK refuses a location past the end — and the frame layout map shades
it (see below).</td></tr>
<tr><td><b>CRC8 Polynomial :</b></td><td>The generator polynomial, a hex byte
0x00–0xFF with the x⁸ term implicit. Defaults to 0x00.</td></tr>
<tr><td><b>Init Value :</b></td><td>The register's starting value, a hex byte.
Defaults to 0x00.</td></tr>
<tr><td><b>Final XOR :</b></td><td>XORed onto the register after the last
element, a hex byte. Defaults to 0x00.</td></tr>
<tr><td><b>Ref In</b> / <b>Ref Out</b></td><td>The reflection flags: Ref In
bit-reverses each input byte before it enters the register, Ref Out reflects
the register before the Final XOR.</td></tr>
<tr><td><b>Element Count :</b></td><td>0–15, typed directly: how many element
rows feed the checksum. The tab shows exactly that many rows; rows removed by
lowering the count keep their settings, so raising it again hands your
elements back. A count of 0 is legal — nothing feeds the register, so the
stamp is the constant Init Value/Final XOR transform — but the validator
flags it, since it usually means a recipe someone stopped halfway
through.</td></tr>
</table>

The hex fields take "0x1D" and bare "1D" alike and reformat to the canonical 0x form when you leave the field.

Each element row is a type and a value, and the CRC is computed over the elements in row order, each contributing one byte:

<table>
<tr><th>Type</th><th>Value</th><th>The byte fed to the CRC</th></tr>
<tr><td><b>ID</b></td><td><b>ID</b> / <b>ID &gt;&gt; 8</b> /
<b>ID &gt;&gt; 16</b> / <b>ID &gt;&gt; 24</b></td><td>A byte of this message's
own CAN identifier, spelt as the shift the firmware performs — which is how OEM
checksum specifications quote it.</td></tr>
<tr><td><b>Data</b></td><td><b>Byte 0</b>–<b>Byte 7</b></td><td>A byte of the
frame <em>after</em> every channel is packed. A byte at or past the Message
Length feeds 0; the CRC byte itself feeds its pre-stamp value, because the
stamp runs last.</td></tr>
<tr><td><b>Raw Value</b></td><td>a hex byte (e.g. 0x5A)</td><td>A literal byte,
fed to the CRC as-is.</td></tr>
</table>

Recipes you may recognise:

<table>
<tr><th>Checksum</th><th>Polynomial</th><th>Init Value</th><th>Final XOR</th>
<th>Ref In / Ref Out</th></tr>
<tr><td>SAE J1850</td><td>0x1D</td><td>0xFF</td><td>0xFF</td><td>off / off</td></tr>
<tr><td>SMBus CRC-8</td><td>0x07</td><td>0x00</td><td>0x00</td><td>off / off</td></tr>
<tr><td>CRC-8/ROHC</td><td>0x07</td><td>0xFF</td><td>0x00</td><td>on / on</td></tr>
</table>

### The CRC byte in the frame map

The Transmitted Channels tab's frame layout map shades the CRC byte slate — deliberately neither a channel colour nor the clash red, because the byte is neither a channel nor a fault: it is spoken for, place your channels elsewhere. The caption carries the same fact in words ("· CRC8 stamped into Byte *N*"), and the shading follows the Byte Location combo live.

A channel whose bits land in that byte turns **red**, like any other double-claimed bit, and **OK is refused** until it is moved — see [Overlaps](#overlaps). This used to be a warning that let the message be saved, on the grounds that it still mapped to the device. It does map, and then transmits a channel the stamp has already overwritten, so the frame does not carry what the configuration says it carries.

### Capacity and validation

The device runs at most **20** CRC8 rules, counted across all three buses — one table for the whole configuration, like the relays' 32, and separate from the 500-entry message table the message itself occupies. The Config Summary counts them under Device usage ("x/20 CRC8 rules"); see [Validation &amp; the Config Summary](validation-report.md).

Check Channels rules on the recipe:
- **Errors** — more than 20 Transmit CRC8 messages in the configuration; no CRC channel selected; a CRC byte location outside the message. The last two are also refused by the section editor's OK.
- **Warnings** — a channel's bits packed into the byte the CRC is stamped into (the stamp runs last, so it overwrites them in every frame); a Data element reading at or past the Message Length (it feeds 0); a Data element reading the CRC's own byte (it feeds the pre-stamp value). Warnings rather than errors because some protocols genuinely do these — but each is far more often a mis-typed index than a choice.

> **Note:** The stamp writes its channel on every transmission, so the CRC channel counts as a writer in the two-writers check: pointing a calculation's output at the same channel earns the shared-slot warning, and the calculation's value would never be seen — the composer re-stamps on every transmit.

## Scaling

Channel rows are DBC-native: **physical = raw × Bit Resolution + Offset**, stored on the device as two float32 values, so a Get Configuration reconstructs each row verbatim. The channel's range becomes the device's clamp — every reading is clamped to the channel's Range Minimum…Maximum. See [Channels](channels.md) for how storage types and ranges are chosen.

**Bit Resolution reads the same whichever way the message goes.** 0.1 is a tenth per count in both directions: a received count of 7 is 0.7, and transmitting 0.7 puts 7 on the wire.

**The Offset is always ADDED**, in both directions — it is a bias, not a correction, and it never quietly changes sign. Where it lands differs, because the two directions are describing different things:

<table>
<tr><th>Direction</th><th>Arithmetic</th><th>Offset is in</th></tr>
<tr><td><b>Receive</b></td><td>physical = raw × Bit Resolution + Offset</td>
<td>channel units</td></tr>
<tr><td><b>Transmit</b></td><td>raw = physical ÷ Bit Resolution + Offset</td>
<td>raw counts</td></tr>
</table>

So a transmit row with Bit Resolution 1 and Offset 64, sending a channel that reads 1, puts **65** on the wire. At Bit Resolution 0.1 the same row sends 1 ÷ 0.1 + 64 = **74** — the offset is applied after the resolution, so it counts in raw counts rather than channel units.

> **Note:** These two are deliberately **not** inverses of one another. If you are transmitting to another CAN Triple that receives the same signal with the same Offset, the offset is applied twice — once going out and once coming in. Negate the Offset on one of the two rows to get the value back unchanged.

A transmit row also chooses what happens when the value will not fit: **Clamp to Signal Limit**, ticked by default, sends the nearest value the signal's bits can carry, while unticking it sends the low bits and lets the count roll over. 256 into an 8-bit field is 255 ticked and 0 unticked. Unticking also skips the channel's own range clamp, which would otherwise decide the answer before the field width came into it. See [Channels](channels.md) for the full note. The Config Summary marks such a row **rolls over**, and so does the channel list in the section editor.

> **Note:** If a transmitted value looks pinned at one end, check the preview under the Offset field: it names the range of channel values the signal's bits can actually carry, with the Offset already accounted for. A negative Offset large enough to drive the raw count below zero has nowhere to go in an unsigned field, and pins at 0.

> **Note:** OK validates the section before closing: the base address must be valid hex within its 11/29-bit range, the message length must fit the frame kind, a relay must forward to at least one bus, and a Transmit CRC8 message must have a CRC channel selected and its byte location inside the message length. Deeper cross-checks — overlapping signals, channels written twice, FD frames on a classic bus — appear in the validation report; see [Validation &amp; the Config Summary](validation-report.md).

See also: [Channels](channels.md) · [DBC Import](dbc-import.md) · [Message Relays](relays.md) · [User Conditions](conditions.md) · [Monitoring Live Values](monitor.md) · [Online: Send, Get &amp; Flash](online.md)
