# Monitoring Live Values

Two windows show what a connected device is doing: **Monitor Channels** (F3) shows decoded channel values, and the **CAN Viewer** (F4) shows raw CAN frames. Both stay open alongside the editors while you work.

## Monitor Channels (F3)

Choose **Online → Monitor Channels…** or press **F3**. The window lists every mapped channel in a live grid with columns **Channel**, **Value** and **Units**, sorted by channel name. Values are fed by the device's always-on value stream and are formatted with the channel's decimal places; a value that has never arrived shows as "—". An enumerated channel — **Device Last Reset Reason** is the one (see [Channels](channels.md)) — shows its label with the number, "Power On (1)"; a value its enumeration does not name shows as the bare number. The stream refreshes every channel within 70 ms even at the device's channel limit — see [Order &amp; Timing of Operations](engine.md) for the exact rates.

> **Note:** A value that has not updated for about 2 seconds turns gray — the channel's message has stopped arriving, or the device is not sending the stream. It returns to normal color with the next update.

The label above the grid states what is being shown: the channel mapping of the *current document*, not necessarily what the device is running. If you have edited the configuration since the last Send, the rows and the device's stream can disagree — [send the configuration](online.md) to bring the device in line with the document. The grid re-maps itself automatically when the document changes.

If the document has mapping errors, the label reports how many and shows the first one; the device mapping may then be incomplete. Run **File → Check Channels** for the full list — see [Validation &amp; the Config Summary](validation-report.md). For a **concealed** message — one marked Hidden or Protect Communication — the error detail is withheld: the label names the message and tells you to open it in **Connections → Communications** with its own **Message Password**, since what is wrong with it cannot be shown without that. It does *not* point at File → Reveal Protected Comms: that is the document-wide Edit Protected Comms password, which opens no Hidden message at all and is only one of the two things a Protect Communication message asks for. A **Read Only** message reports its errors in full; it conceals nothing (see [Marking a message](communications.md#marking)).

## CAN Viewer (F4)

Choose **Online → CAN Viewer…** or press **F4**. The viewer shows raw frames from all three buses as they arrive, with columns **Time (s)**, **Bus**, **Dir** (Rx or Tx), **ID**, **Len** and **Data**. Standard IDs are shown as three hex digits (0x123), extended IDs as eight (0x18FEF100).

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td>Pause</td><td>Stops capturing new frames; frames arriving while paused
are discarded, not queued.</td></tr>
<tr><td>Auto scroll</td><td>Keeps the newest frame in view.</td></tr>
<tr><td>Show: CAN 1 / CAN 2 / CAN 3</td><td>Which buses appear in the list. All
three are ticked by default — see below.</td></tr>
<tr><td>Save to File…</td><td>Writes every buffered frame as a Vector ASCII
log (*.asc), readable by common CAN tools. Timestamps in the file are relative
to the first buffered frame.</td></tr>
<tr><td>Clear</td><td>Empties the table and the capture buffer.</td></tr>
<tr><td>Inject Frame</td><td>Sends a frame from the device — see below.</td></tr>
</table>

> **Note:** The capture buffer holds up to 10 million frames for export; the on-screen table is a bounded window of the most recent 5,000 rows. "N frames buffered" above the table counts the full capture buffer, not the visible rows.

### Filtering by bus

The **Show:** checkboxes — **CAN 1**, **CAN 2**, **CAN 3** — pick which buses appear in the list. Unticking a busy bus is the quickest way to read a quiet one: on a gateway where one network runs at a few thousand frames a second, the traffic you care about otherwise scrolls past before you can see it.

These filter the **display only**. Every bus is captured regardless, so:
- **Save to File…** always writes the whole trace, including buses that were hidden while it was recording.
- The "N frames buffered" count is the whole capture, not the visible rows.
- Re-ticking a bus brings its history back rather than starting from that moment — the list is rebuilt from the buffer.

> **Note:** Hiding a bus is not the same as **Pause**. Pause stops capture altogether and discards what arrives; a bus filter keeps recording everything and only changes what is on screen.

### Inject Frame

The **Inject Frame** panel at the bottom transmits a single frame through the device: choose the **Bus:** (CAN 1–3), enter the **ID:** in hex (for example 0x7E0), tick **Extended** for a 29-bit ID, enter the **Data:** as hex bytes separated by spaces (for example `00 11 22 33 44 55 66 77`), and click **Send**. Standard IDs reach 0x7FF, extended IDs 0x1FFFFFFF; up to 64 data bytes are accepted. An injected frame is also treated as if it had arrived on that bus: it is parsed into channels, and any relay or routing rule that matches it fires. That is why it appears twice in the viewer — once as Rx and once as Tx.

> **Warning:** A payload of 9 bytes or more is sent as a CAN FD frame; 8 or fewer goes out as a classic frame, because the format is inferred from the length. Injected frames go onto a live bus — be sure the ID you send cannot be mistaken for real traffic by other nodes.

## See also

[Online: Send, Get &amp; Flash](online.md) · [Channels](channels.md) · [Communications: Messages &amp; Sections](communications.md) · [Validation &amp; the Config Summary](validation-report.md) · [Troubleshooting](troubleshooting.md)
