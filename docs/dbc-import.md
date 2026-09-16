# DBC Import

To import messages and signals from an industry-standard .dbc file, open **Connections &gt; Communications…**, pick the bus tab the traffic arrives on, and click **Import DBC…**. After choosing a file (the picker filters on "DBC files (\*.dbc)"), the **Import DBC — *filename*** dialog shows every message and signal the parser found. Files are decoded as UTF-8, falling back to Latin-1 when that produces replacement characters.

## The import dialog
- **Import into :** the target bus — preset to the tab you clicked from, changeable here.
- **Import as :** **Receive Messages** (the default) or **Transmit Messages**. Receive creates a channel for every ticked signal; transmit sends channels that already exist, and changes what the columns are for — see [Importing as transmit messages](#transmit). A **Rate :** box appears beside it in transmit mode.
- **Filter :** type to filter messages and signals by name.
- The tree has columns **Message / Signal**, **Channel Type**, **Details** and **Unit**. Tick the signals to import; ticking a message row ticks all of its signals, and a partially ticked message shows a tristate check.
- **Messages are listed by arbitration id, lowest first**, whatever order they appear in the file — a DBC has no required message order, and the tool that wrote it may have used none you would recognise. Sorting them makes the list match a spec sheet and puts related ids together. The sections the import creates are made in that same order. Note that the id sorted on is the arbitration id itself, so an extended-frame message sorts by its 29-bit id and not after every standard one.
- Two columns are editable on signal rows before importing: the name in **Message / Signal** (this becomes the channel name, with underscores already turned into spaces, capped at 31 bytes) and **Channel Type** (the physical quantity, pre-guessed from the DBC unit).
- **Select All** / **Select None** tick or clear everything; the label beside them counts "*N channel(s) in M message(s) selected*".
- **Parser notes :** lists anything the parser had to work around in the file itself.
- **Import** is enabled once at least one signal is ticked.

## What an import creates

As receive messages (the default), each message with at least one ticked signal becomes a **Receive Message** section on the target bus, and each ticked signal becomes a channel row plus a User Channel in the catalogue. The section takes the message's CAN ID, extended flag and DLC; a DLC over 8 bytes marks the section CAN FD. Sections are appended to the bus — import never overwrites existing sections, and channel names are never allowed to clobber existing channels.

If any signal had to be renamed or skipped, a summary box reports "Imported N message(s) with M note(s)" — the full list is under **Show Details**.

### Units

A `.dbc` writes its unit as free text, and every tool spells it differently: `degC`, `°C` and `Celsius` are one unit, and none of them is how this application spells it (**C**). The import translates, so `kph` arrives as **km/h** and `1/min` as **rpm** — an imported channel ends up indistinguishable from one you made by hand, and its unit can be picked from the same lists everywhere else.

**Both columns are yours to change before importing.** **Channel Type** chooses the quantity, and **Unit** offers exactly the units that quantity has. Changing the type resets the unit to that type's default, because a Pressure channel measured in rpm is not something the application can express.

**A unit nothing matches is flagged, not guessed.** If the file says something this application has no equivalent for, the Unit cell reads **(pick one)** in the warning colour and its tooltip quotes what the file actually said. Set the Channel Type, then pick a unit — or leave it, and the channel imports unitless, which the import notes list at the end.

> **Note:** **Nothing converts the numbers.** The unit is a label on the value the scaling already produces. If a signal arrives in psi and you pick kPa, the readings are still psi — change the **Bit Resolution** instead, which is what actually scales them. This matters most where the translation is not exact: `hPa` imports as **mbar** (identical), but `atm` arrives as **bar** and `kV` as **V**, and those are relabels rather than conversions.

### Channel names

**Underscores become spaces.** A DBC signal name has to be a C identifier, so an author who means "Engine Speed" is obliged to write `Engine_Speed` — the underscore is the file format's limitation, not part of the name, and channels in this application are spelled with spaces. Runs of underscores collapse to one space and the ends are trimmed, so `Engine__Speed` imports as "Engine Speed" and `_Rpm` as "Rpm". It is a starting point, not a rule: the name column is editable, so type the underscore back if you want it.

The device stores a 31-byte channel label. A longer DBC signal name is clipped to that budget (never splitting a multi-byte UTF-8 character), and a name already in use gains a numeric suffix (" 2", " 3", …) that fits inside the same budget. Every rename is listed in the import notes.

### Bit positions and byte order

DBC stores a signal's start bit as its **LSB for Intel** (@1) but its **MSB for Motorola** (@0); the importer converts Motorola start bits to the LSB convention the device uses, so the numbers you see in the section editor are Start Bit values (see the frame layout map in [Communications](communications.md)).

> **Warning:** One section carries one byte order. The section's Alignment is taken from the first selected signal; any selected signal with the other byte order is skipped, with a note ("byte order differs from the rest of the message — skipped"). Split mixed-endianness messages by importing twice with different selections if you need both halves.

### Data types, scaling and ranges

Each imported channel keeps the DBC scaling exactly — physical = raw × factor + offset — and its definition is derived as follows:
- **Decimal places** come from the factor (0.1 → 1 dp, 0.05 → 2 dp, 1 → 0 dp).
- **Range**: the DBC's declared [min|max] wins when it is a real span (max &gt; min); otherwise the range is everything the field can physically encode, pushed through factor and offset.
- **Data type** is the smallest storage type that holds that physical range at that precision — chosen from the range and decimals, never from the raw bit width, and falling back to float when no integer type reaches. Signedness follows the physical range, so an unsigned raw field with offset −40 becomes a signed channel. See [Channels](channels.md) for the type table.
- An **IEEE754** signal becomes a 32-bit float row and a float channel; a 1-bit flag with no decimals becomes a boolean.
- Ranges wider than the dialogs' displayed ±1e9 float span are kept intact — that span is a display convention, and the range is the device's clamp.

### Multiplexed messages

A DBC message with a multiplexor imports as a **compound** section: each multiplexor value becomes an identifier (byte offset, ID and ID mask derived from the multiplexor field), and that value's signals become the identifier's rows. Signals that are not multiplexed apply to every variant, so they are replicated into each identifier (a compound section has no shared always-present set). Identifiers are emitted in ascending multiplexor-value order.

**The multiplexor itself is not imported as a channel**, and its row in the list has no tick box. Its bits *are* the identifier's selector: the device writes that selector into the frame after the channels, so a channel on the same bits would not share them, it would be overwritten by them — and the section editor refuses to save a message in that state. The row is still shown, so you can see which signal picks the variant, and the Details column says what becomes of it.

> **Warning:** The device matches selectors through a 16-bit window, so a multiplexor field must be 1–16 bits wide, and a Motorola multiplexor must not span multiple bytes. A multiplexor value that cannot be expressed is skipped with a note and its channels are not imported; a message whose multiplexed signals all fail imports as a plain message carrying only the non-multiplexed channels.

<a id="transmit"></a>

## Importing as transmit messages

A .dbc written from the far side of the wire — what a dash, a cluster or a gearbox controller *expects to receive* — is, from this device's side, a list of messages to send. Set **Import as :** to **Transmit Messages** and the same tree imports the other way round: each message with a ticked signal becomes a **Transmit Message** section, and each ticked signal becomes a row that sends an existing channel.
- **Nothing is created.** A transmit row reads a channel that something else already produces — a receive message, a calculation, a constant — so the import creates no channels, and the name, type and unit columns give way to the one that matters here: **Send Channel**.
- **Send Channel** is prefilled wherever the catalogue already has a channel of the signal's name (underscores read as spaces, case ignored) — the case when the file was written against this document's channels, or when the same file has already been imported as receive messages. Anything else reads **(pick one)** in the warning colour: **double-click the cell** to choose the channel through the ordinary channel picker. Picking a channel ticks the signal.
- A ticked signal with no Send Channel is **skipped**, with a note — a transmit field has no way to carry "nothing". The count beside Select All says how many ticked signals are in that state, and Import stays disabled until at least one ticked signal has a channel.
- **DBC Unit** shows what the file said, for matching by eye. Nothing converts the numbers: the channel is sent through the signal's scaling as it stands.

### Scaling: the offset changes sign

A DBC scales one way — physical = raw × factor + offset, so a sender computes raw = (physical − offset) ÷ factor — while a transmit row in this application **adds** its Offset before dividing (see [Communications](communications.md)). The import therefore negates the file's offset. A coolant signal written `(0.1,-40)` imports with Bit Resolution 0.1 and Offset **+40**, so 25 °C goes out as (25 + 40) ÷ 0.1 = 650 — the count a receiver decoding with the same DBC turns back into 25. The factor is unchanged, and so is everything else about the row: start bit, length, signedness and byte order import exactly as they do for receive.

### Rate

Every imported transmit message is **Cyclic**. A message whose file states a cycle time (a `BA_ "GenMsgCycleTime"` line, or the file's `BA_DEF_DEF_` default) keeps that period exactly, with the nearest whole rate shown in the section editor; every other message takes the **Rate :** chosen beside the mode. The device transmits no faster than every 5 ms, so a cycle time under that is set to 200 Hz with a note. Triggered transmission is a choice about this document's User Conditions, which a .dbc knows nothing about, so it is left to the section editor.

Multiplexed messages import as compound transmit sections the same way as for receive: each multiplexor value becomes an identifier the device writes into the frame, and the multiplexor itself is never a row.

## After the import

Communications Setup switches to the target bus tab with the new sections appended and selected. The "N of M device messages used" count spans all three buses, so a large import on one bus consumes budget visible on the others. Review the result in the section editor — the frame layout map makes overlaps obvious — and run [validation](validation-report.md) before sending.

See also: [Communications: Messages &amp; Sections](communications.md) · [Channels](channels.md) · [Online: Send, Get &amp; Flash](online.md)
