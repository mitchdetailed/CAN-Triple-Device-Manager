# Validation &amp; the Config Summary

The firmware validates almost nothing, so the application is the safety layer. Two File-menu commands report on the open document: **Check Channels** runs the validation rules and lists every finding with a severity, and **Config Summary…** produces a printable Channel Summary Report of everything the configuration defines.

## Check Channels

To validate the configuration, choose File → Check Channels. The report lists one row per finding with columns **Severity**, **Location** and **Message**, sorted errors first, then warnings, then info. A line above the list states which document state was checked — the check always runs on the *current in-memory* configuration, saved or not, and shows the time it ran. The summary underneath reads "No problems found." or "*N* errors, *M* warnings."
- **Check Again** — re-runs the validation (useful after editing in another window).
- **Remove Unused Channels…** — offers to delete every catalogue channel that nothing generates, uses or references (typically left behind after removing a message or clearing out a DBC import). The confirmation shows the full list under Show Details; nothing else in the configuration changes.

The same validation runs automatically when you send: Online → Send Configuration refuses to run while any Error exists, and opens Check Channels to show why. See [Online: Send, Get &amp; Flash](online.md).

## The three severities

The ladder is a policy decision, not decoration — Send is gated on the top rung only.

<table>
<tr><th>Severity</th><th>Meaning</th><th>Blocks Send</th></tr>
<tr><td>Error</td><td>The device cannot be given this configuration, or a row
was left half-finished.</td><td>Yes</td></tr>
<tr><td>Warning</td><td>The configuration is buildable but a specific
behaviour is surprising.</td><td>No</td></tr>
<tr><td>Info</td><td>Worth knowing, never a problem.</td><td>No</td></tr>
</table>

### Errors — examples
- A CAN ID beyond the standard (11-bit) or extended (29-bit) range.
- A message length outside 0–8 bytes (classic CAN) or outside 0–8, 12, 16, 20, 24, 32, 48, 64 bytes (CAN FD).
- A duplicate receive CAN ID on one bus — only the first message would match.
- A signal that does not fit its frame, or a row with no channel selected.
- A message relay that forwards to no bus.
- A Transmit CRC8 message with no CRC channel selected, or a CRC byte location outside the message; more than 20 Transmit CRC8 messages across the buses — the device runs at most 20 CRC8 rules.
- A calculation with no output channel, or an input set to "channel" with no channel chosen.
- A transmit rate outside 1–200 Hz; an integrator rate beyond what the engine evaluates.
- A lookup table whose outputs do not match its sites, or a blank table axis.
- More than 20 counters and integrators with Preserve value enabled — the device retains at most 20 across power cycles and would silently drop the excess.

### Warnings — examples

The channel-level warning that matters most is **two things writing the same channel**. The device has one value slot per channel, so two writers overwrite each other and whichever runs last wins. Every writer counts — receive rows, math, User Conditions, counters, timers, integrators, constants, table outputs and the channel a Transmit CRC8 message publishes its checksum to — an inactive row counts as no writer at all, and one warning is raised per channel naming all of its writers. See [Math Channels](math-channels.md), [User Conditions](conditions.md), [Constants](constants.md) and [Lookup Tables](tables.md).

Other warnings include: messages on a bus whose Mode is Off; routing or relaying to a bus whose mode is Off; a route or relay mask that includes its own bus (ignored by the device); a DBC factor of zero; two channels overlapping in a frame; a channel occupying the byte a Transmit CRC8 message stamps its checksum into (the stamp runs last and overwrites it); a CRC element reading at or past the message length, or reading the CRC's own byte; a counter whose maximum does not exceed its minimum; a timer with neither a start nor a stop channel; an integrator with no reset channel; table axis sites that are not strictly ascending.

### Info — examples

Referencing a channel — transmitting it, feeding it to a calculation, driving a table axis — *reads* the value and never alters it, so it is never an Error or a Warning no matter how many sites do it. The one thing worth saying is that a channel nothing writes *yet* reads its default value until a receive row or a calculation writes it — an Info, because building a configuration out of order is normal. Other Info entries: unused catalogue channels (removable via Remove Unused Channels…), the flash-wear note for preserved integrators (see [Integrators](integrators.md)), and the "Device usage" capacity line, which reports how much of each device table the configuration occupies.

## Marked messages in the report

Only the two *concealing* levels change what the report says. A message marked **Read Only** reports in full — every finding, its CAN ID, its bit positions — because Read Only conceals nothing from anybody; see [Marking a message](communications.md#marking).

A message marked **Hidden** or **Protect Communication** withholds its detail from a viewer who has not supplied the password it needs. Its validation findings are collapsed into a single entry that keeps the **severity** — an Error on a concealed message still blocks Send, because you must know the configuration cannot be used even when you cannot be told why — but drops the detail. The entry says how many problems the message reports and tells you to open it in Connections → Communications with its own **Message Password** — not File → Reveal Protected Comms, which is the document-wide Protected Comms password, opens no Hidden message at all, and is only one of the two things a Protect Communication message asks for. Likewise, a CAN ID clash with a concealed message is reported without naming it or printing its ID; a clash with a Read Only message names it and prints the ID like any other.

## Config Summary…

To produce a report of the whole configuration, choose File → Config Summary…. The window shows a column-aligned Channel Summary Report with these sections:
- **Summary Information** — file name, title, date, application version, and whether the document has unsaved changes.
- **Configuration Comments** — the document's comment text.
- **CAN Bus Setup** — per-bus mode, bit rate, FD data rate and termination.
- **Used Channels** — every channel the configuration touches, in first-use order.
- **Channels By Function** — Communications first (each section with its type, ID, length, alignment, timing, routing and its per-row bit/length/ type/factor/offset detail), then Calculations (math, User Conditions, counters, timers, integrators, constants and tables, each with what it uses and what it generates).
- **Incomplete Channels** — channels something consumes but nothing generates, with the list of their users.
- **Unused Channels** — catalogue channels nothing references.

Buttons: **Print…**, **Save PDF…** and **Save Text…** (the suggested file name is "*&lt;configuration name&gt;* Channel Summary", next to the saved file when there is one).

> **Note:** The report obeys the same rules as the rest of the application: a **concealed** message contributes its name and its channel names — the outputs a customer needs — and marks every row "(hidden)" or "(protected)" instead of printing bit positions, IDs, lengths or multiplexor detail. A **Read Only** message prints in full, like any unmarked one. The report is the easiest way to walk off with a protocol, so it withholds exactly what the dialogs withhold and nothing more.

## Related pages
- [Online: Send, Get &amp; Flash](online.md) — where Errors block and Warnings are shown.
- [Communications: Messages &amp; Sections](communications.md) — the message and channel rules being checked.
- [Monitoring Live Values](monitor.md) — checking behaviour on the device once it passes.
- [Troubleshooting](troubleshooting.md) — what the two-writers warning and other findings look like in practice.
