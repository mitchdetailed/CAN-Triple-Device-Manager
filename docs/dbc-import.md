# DBC Import

To import messages and signals from an industry-standard .dbc file, open **Connections &gt; Communications…**, pick the bus tab the traffic arrives on, and click **Import DBC…**. After choosing a file (the picker filters on "DBC files (\*.dbc)"), the **Import DBC — *filename*** dialog shows every message and signal the parser found. Files are decoded as UTF-8, falling back to Latin-1 when that produces replacement characters.

## The import dialog
- **Import into :** the target bus — preset to the tab you clicked from, changeable here.
- **Filter :** type to filter messages and signals by name.
- The tree has columns **Message / Signal**, **Channel Type**, **Details** and **Unit**. Tick the signals to import; ticking a message row ticks all of its signals, and a partially ticked message shows a tristate check.
- **Messages are listed by arbitration id, lowest first**, whatever order they appear in the file — a DBC has no required message order, and the tool that wrote it may have used none you would recognise. Sorting them makes the list match a spec sheet and puts related ids together. The sections the import creates are made in that same order. Note that the id sorted on is the arbitration id itself, so an extended-frame message sorts by its 29-bit id and not after every standard one.
- Two columns are editable on signal rows before importing: the name in **Message / Signal** (this becomes the channel name, capped at 31 bytes) and **Channel Type** (the physical quantity, pre-guessed from the DBC unit).
- **Select All** / **Select None** tick or clear everything; the label beside them counts "*N channel(s) in M message(s) selected*".
- **Parser notes :** lists anything the parser had to work around in the file itself.
- **Import** is enabled once at least one signal is ticked.

## What an import creates

Each message with at least one ticked signal becomes a **Receive Message** section on the target bus, and each ticked signal becomes a channel row plus a User Channel in the catalogue. The section takes the message's CAN ID, extended flag and DLC; a DLC over 8 bytes marks the section CAN FD. Sections are appended to the bus — import never overwrites existing sections, and channel names are never allowed to clobber existing channels.

If any signal had to be renamed or skipped, a summary box reports "Imported N message(s) with M note(s)" — the full list is under **Show Details**.

### Channel names

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

A DBC message with a multiplexor imports as a **compound** section: each multiplexor value becomes an identifier (byte offset, ID and ID mask derived from the multiplexor field), and that value's signals become the identifier's rows. Signals that are not multiplexed — including the multiplexor itself, if ticked — apply to every variant, so they are replicated into each identifier (a compound section has no shared always-present set). Identifiers are emitted in ascending multiplexor-value order.

> **Warning:** The device matches selectors through a 16-bit window, so a multiplexor field must be 1–16 bits wide, and a Motorola multiplexor must not span multiple bytes. A multiplexor value that cannot be expressed is skipped with a note and its channels are not imported; a message whose multiplexed signals all fail imports as a plain message carrying only the non-multiplexed channels.

## After the import

Communications Setup switches to the target bus tab with the new sections appended and selected. The "N of M device messages used" count spans all three buses, so a large import on one bus consumes budget visible on the others. Review the result in the section editor — the frame layout map makes overlaps obvious — and run [validation](validation-report.md) before sending.

See also: [Communications: Messages &amp; Sections](communications.md) · [Channels](channels.md) · [Online: Send, Get &amp; Flash](online.md)
