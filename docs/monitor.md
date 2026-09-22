# Monitoring Live Values

Two windows show what a connected device is doing: **Monitor Channels** (F3) shows decoded channel values, and the **CAN Viewer** (F4) shows raw CAN frames. Both stay open alongside the editors while you work.

## Monitor Channels (F3)

Choose **Online → Monitor Channels…** or press **F3**. The window lists every mapped channel in a live grid with columns **Channel**, **Value** and **Units**, sorted by channel name. Values are fed by the device's always-on value stream and are formatted with the channel's decimal places; a value that has never arrived shows as "—". An enumerated channel — **Device Last Reset Reason** is the one (see [Channels](channels.md)) — shows its label with the number, "Power On (1)"; a value its enumeration does not name shows as the bare number. The stream refreshes every channel within 70 ms even at the device's channel limit — see [Order &amp; Timing of Operations](engine.md) for the exact rates.

> **Note:** Monitor Channels needs a configuration to name the channels. If none is open — you have just started the program and connected — it offers to read the configuration from the device first, and opens over it once the read has finished. It asks to connect first if no link is open.

> **Note:** A value that has not updated for about 2 seconds turns gray — the channel's message has stopped arriving, or the device is not sending the stream. It returns to normal color with the next update.

**Select Channels…** above the grid narrows it to the channels you are watching, and puts them in the order you want. The picker lists every mapped channel on the left and the chosen ones on the right: highlight channels on the left — **Ctrl**-click to pick several, **Shift**-click for a block — and press **&gt;&gt;** (or double-click) to add them; **&lt;&lt;**, a double-click or **Delete** takes them back. The **Find** box above the left list narrows it by name while you look. Drag rows up and down the right-hand list to set their order; the grid follows it, top to bottom. An empty right-hand list means every channel. **Show All** beside the button clears the selection, and the label says how many channels are showing. The selection is remembered the next time the window opens. It hides and reorders rows only: every channel is still mapped and still updated, so Show All shows current values. A channel you have pinned with [Override](#override) always stays in the list, so a selection cannot hide a value that is driving the device.

**Save List…** in the picker writes the selected channels, in their order, as a **channel list** file (`*.ct3l`) in the program's `Channel Lists` folder (see [Where the files are kept](communications.md#folders)), so a set of channels outlives the configuration it was picked from — a "cooling" list, a "boost" list. **Load List…** replaces the selection with a saved list. A channel the list names that this configuration does not have is reported by name as **not found** and left out; the rest load in the list's order. The file is plain, readable JSON of channel names and nothing else, so a list can be fixed in Notepad.

The label above the grid states what is being shown: the channel mapping of the *current document*, not necessarily what the device is running. If you have edited the configuration since the last Send, the rows and the device's stream can disagree — [send the configuration](online.md) to bring the device in line with the document. The grid re-maps itself automatically when the document changes.

If the document has mapping errors, the label reports how many and shows the first one; the device mapping may then be incomplete. Run **File → Check Channels** for the full list — see [Validation &amp; the Config Summary](validation-report.md). For a **concealed** message — one marked Hidden or Protect Communication — the error detail is withheld: the label names the message and tells you to open it in **Connections → Communications** with its own **Message Password**, since what is wrong with it cannot be shown without that. It does *not* point at File → Reveal Protected Comms: that is the document-wide Protected Comms password, which opens no Hidden message at all and is only one of the two things a Protect Communication message asks for. A **Read Only** message reports its errors in full; it conceals nothing (see [Marking a message](communications.md#marking)).

<a id="override"></a>

### Overriding a channel

With device firmware 1.0.12 or newer, two more columns appear: **Override** and **Override Value**. Tick a channel's Override box and the device pins that channel at the value in the box beside it, whatever its own message, calculation or script would have made it — transmit messages, conditions, maths and tables all see the pinned value, so a cluster can be driven with a made-up engine speed, or a condition tripped, without a sensor or a second tool on the bus. Untick to release. The **Value** column keeps showing what the device actually holds, so a pinned channel reads back its override.
- The editor follows the channel: a boolean or enumerated channel offers its choices; a numeric channel takes a number with the channel's decimal places, stepping by its base resolution and limited to what its data type can hold and to the channel's own range. The device applies the channel's range too.
- **Device channels** — MCU temperature, bus states and the like — are written by the hardware and cannot be overridden; their cells are blank.
- Overrides live only while this window is open: closing it releases every one, and **Clear All Overrides** does the same at any time. Nothing is saved to the device's flash, and a reset or Clear Device Config releases them.
- **The lease.** The device drops every override on its own about three seconds after it last heard from this window, which refreshes them each second while anything is pinned. A pulled cable or a crashed Manager therefore cannot leave a pinned value driving a transmit message. If the refresh fails, the window unticks everything and says so.
- On older firmware the two columns are absent and the label above the grid says what is needed.

## CAN Viewer (F4)

Choose **Online → CAN Viewer…** or press **F4**. The viewer shows raw frames from all three buses as they arrive, with columns **Time (s)**, **Bus**, **Dir** (Rx or Tx), **ID**, **Len** and **Data**, plus a **Count** column in [Overwrite Mode](#overwrite). Standard IDs are shown as three hex digits (0x123), extended IDs as eight (0x18FEF100).

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td>Pause</td><td>Stops capturing new frames; frames arriving while paused
are discarded, not queued.</td></tr>
<tr><td>Auto scroll</td><td>Keeps the newest frame in view. Not used in
Overwrite Mode, where the rows hold still.</td></tr>
<tr><td>Show: CAN 1 / CAN 2 / CAN 3</td><td>Which buses appear in the list. All
three are ticked by default — see below.</td></tr>
<tr><td>Show: Tx Msgs</td><td>Whether the frames the device itself transmitted
appear in the list. Ticked by default — see below.</td></tr>
<tr><td>ID filter</td><td>Which arbitration IDs appear in the list — hex IDs,
ranges and <code>!</code> exclusions; blank shows every ID. See
<a href="#filtering">Filtering the list</a>.</td></tr>
<tr><td>Overwrite Mode</td><td>One row per message carrying its most recent
data, instead of a scrolling trace — see below.</td></tr>
<tr><td>Save to File…</td><td>Writes every buffered frame to a file, in the
format chosen by the file type in the save dialog: a <b>Vector ASCII log
(*.asc)</b>, a <b>SocketCAN candump log (*.log)</b> or a <b>PEAK trace 3.0
(*.trc)</b> — see <a href="#export">Export formats</a>. Timestamps in the file
are relative to the first buffered frame in every format.</td></tr>
<tr><td>Clear</td><td>Empties the table and the capture buffer.</td></tr>
<tr><td>Inject Frames</td><td>Eight slots, each sending a frame once or repeating it at a rate — see below.</td></tr>
</table>

> **Note:** The capture buffer holds up to 10 million frames for export; the on-screen table is a bounded window of the most recent 5,000 rows. "N frames buffered" above the table counts the full capture buffer, not the visible rows.

### When frames are dropped

The monitor stream shares the USB link with everything else, so a bus busier than the link can describe will overrun it. It is allowed to drop frames; it is not allowed to drop them silently, because a trace missing frames without saying so invites you to conclude a message was never sent when it was only never reported.

In the scrolling view the loss appears as its own row, in orange, at the point in the trace where it happened. In [Overwrite Mode](#overwrite) there is no chronological place to put such a row, so the notice moves to the frame count above the table, which reads "**… — frames were dropped**" from the first loss until the next **Clear**. Treat any **Count** on screen as a lower bound once you see it.

<a id="filtering"></a>

### Filtering the list

The **Show:** checkboxes — **CAN 1**, **CAN 2**, **CAN 3** — pick which buses appear in the list. Unticking a busy bus is the quickest way to read a quiet one: on a gateway where one network runs at a few thousand frames a second, the traffic you care about otherwise scrolls past before you can see it.

**Tx Msgs** is the same idea applied to direction rather than to a bus: it picks whether the frames the *device* sent — the **Tx** rows — appear. That covers every transmit message the engine sends on its own schedule, every frame a relay forwards, and the echo of anything you send from **Inject Frame**. It is ticked by default. Untick it to leave only what the buses carried in, which is the quickest way to tell an incoming message apart from one of the device's own on a bus where both share an ID.

The **ID filter** field, beside Overwrite Mode, picks messages by arbitration ID. Type the IDs to keep, in hex with or without `0x`, separated by spaces or commas: `7E8, 100`. A range is written with a dash, `100-1FF`. A leading `!` hides an ID or range instead of keeping it, so `!3C0` on its own shows everything but the one message that floods the bus, and `100-1FF !150` keeps a block minus one. An entry that is not a hex ID is named in red beside the field, and the rest of the entry still applies while you fix it. Standard and extended IDs match by their number.

The three filters compose — a frame has to pass all of them: a Tx frame on a hidden bus stays hidden when **Tx Msgs** is re-ticked, because its bus is still unticked, and an ID the filter keeps shows only on the buses that are ticked.

All of these filter the **display only**. Every frame is captured regardless, so:
- **Save to File…** always writes the whole trace, including buses and transmitted frames that were hidden while it was recording.
- The "N frames buffered" count is the whole capture, not the visible rows.
- Re-ticking a box brings its history back rather than starting from that moment — the list is rebuilt from the buffer.

> **Note:** Hiding a bus or the Tx rows is not the same as **Pause**. Pause stops capture altogether and discards what arrives; a filter keeps recording everything and only changes what is on screen.

<a id="overwrite"></a>

### Overwrite Mode

**Overwrite Mode**, on the row below Pause, changes the list from a history into a snapshot. Instead of a new row per frame, each message keeps a single row that is rewritten in place every time it arrives — so what you read is the current value of every message on the bus, in the shape of PCAN-View's Receive/Transmit tab.

Rows are ordered by **bus**, then by **arbitration ID**, and they stay where they are. That is the point: a value you are watching does not move, so you can look at one row while the bus runs.

A **Count** column appears in this mode, showing how many frames that message has contributed since the last **Clear**. It is what tells a message that has stopped arriving from one that is still running — the data of a message that died holds its last value indefinitely, and only the count gives that away.

A row is one *identifier*, which means all four of:
- the **bus** it arrived on;
- the **arbitration ID**;
- whether that ID is **standard or extended** — 0x100 and 0x00000100 are different frames on the wire and get separate rows;
- the **direction** — a message the device transmits with an ID it also receives is not the same traffic, so Rx and Tx keep separate rows. The **Dir** column says which is which.

Everything else keeps working as it does in the scrolling view. The bus, **Tx Msgs** and **ID** filters still apply; **Pause** still stops capture; and capture itself is untouched, so **Save to File…** writes the complete trace frame by frame no matter which mode you were watching in. Switching the mode off gives the full history back — it was recorded the whole time.

<a id="export"></a>

## Export formats

**Save to File…** offers three file types. Pick one in the save dialog's file-type list, or type the extension yourself — an extension that names a format wins over the list. The suggested name is **Logged Msgs *yyMMdd* - *HHmm*** with the chosen extension, so a day's captures sort in the order they were taken. Every format carries one line per frame, with the timestamp relative to the first buffered frame, because the device reports its uptime rather than the time of day.
- **Vector ASCII log (\*.asc)** — the original export, readable by CANalyzer, CANoe and most viewers. Classic frames as standard lines, CAN FD frames as `CANFD` lines carrying the real BRS and ESI flags, the direction as Rx or Tx.
- **SocketCAN candump log (\*.log)** — the line format `candump -l` writes and `canplayer` replays, which python-can, SavvyCAN, cantools and the Linux CAN tools all read: `(0000000012.345000) can2 0F3#7E0D0000F0C400F0`. The bus is written as `can1`, `can2` or `can3`, the device's own numbering; a CAN FD frame reads `id##F` followed by its bytes, F being the flags digit (1 = BRS, 2 = ESI). The format has no column for the direction, so transmitted and received frames look alike in it.
- **PEAK trace 3.0 (\*.trc)** — PCAN-View and PCAN-Explorer's trace file, per PEAK's file format document: a comment header with the start time, then one record per frame with the index, the offset in milliseconds, the type (`DT` for classic, `FD`/`FB`/`FE`/ `BI` for CAN FD without and with BRS and ESI), the bus, the id, Rx or Tx, the DLC and the bytes.

Line endings follow each format's tools: CR/LF for .asc and .trc, LF for the candump log.

> **Note:** Ticking Overwrite Mode shows the bus as it already stands rather than starting empty: the current value of every message seen since the last Clear is there immediately, with its count, instead of filling in over the next few seconds as each message comes round again.

> **Note:** This view is bounded by the number of distinct identifiers, not by the frame rate, and it tracks up to 10,000 of them — far more than any real network carries. Past that, the identifiers already listed keep counting and new ones are ignored, and the frame count reads "**… — identifier limit reached**". Seeing that at all means something is sending on IDs that keep changing, which is worth looking into on its own; **Clear** starts over.

### Inject Frames

The **Inject Frames** panel at the bottom holds **eight independent slots**, each able to send one frame or to repeat one at a rate. Fill in a slot the way you would expect: choose the **Bus** (CAN 1–3), enter the **ID** in hex (for example 0x7E0), tick **Extended** for a 29-bit ID, and enter the **Data** as hex bytes separated by spaces (for example `00 11 22 33 44 55 66 77`). Standard IDs reach 0x7FF, extended IDs 0x1FFFFFFF; up to 64 data bytes are accepted.

The **Hz** dropdown beside the data decides what the button does:
- **Once** — the default, and what earlier versions did. The button reads **Send** and one frame goes per press.
- **1, 2, 5, 10, 20, 50** or **100** — the button reads **Start**, and the slot sends that frame repeatedly at the chosen rate until you press **Stop**. A running slot holds its Bus, ID and Data fields fixed, so what is going out cannot change under you mid-run. Changing the rate while it runs re-times it rather than stopping it.

Slots run independently, so several can be going at once at different rates. If the device refuses a frame, that slot stops and says so; the others carry on.

> **Note:** The rate is produced by this program, not by the device — each repeat is a separate command sent over USB and acknowledged. The low rates are exact for all practical purposes; **50 and 100 Hz are best-effort** and will fall short if the link or the machine is busy, because a repeat is skipped rather than queued when the previous one has not been acknowledged yet. If you need a frame on the bus at an exact period, configure it as a cyclic transmit message in [Communications](communications.md), which the device times itself.

An injected frame is also treated as if it had arrived on that bus: it is parsed into channels, and any relay or routing rule that matches it fires. That is why it appears twice in the viewer — once as Rx and once as Tx.

> **Warning:** A payload of 9 bytes or more is sent as a CAN FD frame; 8 or fewer goes out as a classic frame, because the format is inferred from the length. Injected frames go onto a live bus — be sure the ID you send cannot be mistaken for real traffic by other nodes, and remember that a slot left running keeps putting it there until you stop it.

## See also

[Online: Send, Get &amp; Flash](online.md) · [Channels](channels.md) · [Communications: Messages &amp; Sections](communications.md) · [Validation &amp; the Config Summary](validation-report.md) · [Troubleshooting](troubleshooting.md)
