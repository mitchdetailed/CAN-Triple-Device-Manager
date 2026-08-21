# Channels

A channel is a named value the device holds — "Engine RPM", "Coolant Temp" — with a data type, resolution, range and units. Channels are *written* by receive message rows and by calculations ([math](math-channels.md), [User Conditions](conditions.md), [constants](constants.md), [tables](tables.md), [counters](counters.md), [timers](timers.md), [integrators](integrators.md)), and *read* by transmit message rows, calculation inputs and the [channel monitor](monitor.md). A channel received in one message can be re-transmitted in another.

Almost every channel is one you create. The exception is the **device channels** below, which the CAN Triple produces about itself and which are always available.

## Device channels

These come from the firmware rather than from your configuration. They need no setup, appear in every document under the **Device Channels** category, and are used exactly like any other channel — as a maths input, a condition input, or a channel in a transmit message.

<table>
<tr><th>Channel</th><th>Type</th><th>Meaning</th></tr>
<tr><td>Device OnTime</td><td>u32, 2 dp, seconds</td>
<td>How long the device has been powered up and running, in seconds, resolving
to 0.01 s.</td></tr>
</table>

### CAN diagnostics

Ten more channels exist for each bus, named **Device CAN1 …**, **Device CAN2 …** and **Device CAN3 …**. They report what the CAN controller itself sees, which is how you tell a wiring or termination fault from a configuration mistake — a bus that is mis-wired shows climbing error counters while a bus that is merely mis-configured does not.

<table>
<tr><th>Channel</th><th>Type</th><th>Meaning</th></tr>
<tr><td>Device CAN<i>n</i> Rx Errors</td><td>u8, 0–127</td>
<td>The controller's receive error counter. Climbs on reception errors and falls
on good frames, so a steady non-zero reading means faults are still arriving.</td></tr>
<tr><td>Device CAN<i>n</i> Tx Errors</td><td>u8, 0–255</td>
<td>The transmit error counter. The usual sign of a bus with no other node
listening, no termination, or the wrong bit rate.</td></tr>
<tr><td>Device CAN<i>n</i> Warning</td><td>Boolean, Ok/Error</td>
<td>Set once either counter reaches 96 — the standard's early warning that a bus
is degrading while still working.</td></tr>
<tr><td>Device CAN<i>n</i> Error Passive</td><td>Boolean, Ok/Error</td>
<td>Set while the node has backed off to error-passive: it still communicates,
but it can no longer flag other nodes' errors.</td></tr>
<tr><td>Device CAN<i>n</i> Bus Off</td><td>Boolean, Ok/Error</td>
<td>Set while the node has taken itself off the bus entirely after the transmit
counter passed 255. Nothing is sent or received on that bus.</td></tr>
<tr><td>Device CAN<i>n</i> Error Frames</td><td>u32</td>
<td>How many protocol errors the controller has logged since power-up. A total,
so it only ever climbs — watch the <i>rate</i> of change, not the number.</td></tr>
<tr><td>Device CAN<i>n</i> Rx Count</td><td>u32</td>
<td>Frames received on the bus since power-up.</td></tr>
<tr><td>Device CAN<i>n</i> Tx Count</td><td>u32</td>
<td>Frames transmitted on the bus since power-up.</td></tr>
<tr><td>Device CAN<i>n</i> Bus Load</td><td>u16, 1 dp, %</td>
<td>Estimated bus utilisation over the last second. See the note below.</td></tr>
<tr><td>Device CAN<i>n</i> Bus Off Recoveries</td><td>u32</td>
<td>How many times the device has restarted this bus after a bus-off since
power-up. See <b>Recovery from bus-off</b> below.</td></tr>
</table>

The three state channels are separate booleans rather than one number because they are not exclusive: a bus-off node is also error-passive and also in warning, and all three read true at once. Test whichever one you care about.

### Recovery from bus-off

Bus-off is the CAN controller taking itself off the bus after its transmit error counter passes 255 — the protocol's way of stopping a faulty node from disrupting everyone else. The controller will not rejoin on its own.

The device brings it back automatically. It shuts the bus down, waits one second, and starts it again with the settings your configuration asked for. If the fault is still there the bus drops straight back into bus-off, and the device repeats — down, one second, up — for as long as it takes. Once the fault clears, the next attempt sticks and the bus carries on.

A bus you deliberately switched **Off** in the configuration is never restarted, and switching a bus Off while it is retrying stops the retries.

> **Warning:** Restarting a bus resets the CAN controller, and that clears **Rx Errors** and **Tx Errors** back to zero. On a bus that is retrying, read those two as "how bad is the current attempt", not as a history of the fault. The two counts that survive a restart are **Error Frames** and **Bus Off Recoveries** — those are kept by the device itself, and only a reset or a power cycle clears them.

**Device CAN*n* Bus Off Recoveries** counts how many times the device has *restarted* the bus after a bus-off. It counts the attempt, not the outcome — on a bus that is still broken the restart is undone in a few milliseconds, far too fast to see, and a counter that only recorded lasting successes would sit at zero on exactly the bus you are trying to diagnose. Whether a restart lasted is what **Bus Off** beside it tells you:

<table>
<tr><th>Bus Off</th><th>Recoveries</th><th>What it means</th></tr>
<tr><td>0</td><td>0</td><td>Healthy — this bus has never gone bus-off.</td></tr>
<tr><td>0</td><td>steady, non-zero</td><td>Healthy <i>now</i>. It went down that
many times earlier — worth knowing if you are chasing something
intermittent.</td></tr>
<tr><td>1</td><td>climbing about once a second</td><td><b>The fault is still
there.</b> Every restart fails immediately, so the bus never reads healthy —
this counter is the only sign the device is still trying. Check wiring,
termination and bit rate.</td></tr>
<tr><td>0</td><td>climbing occasionally</td><td><b>Intermittent.</b> The bus
comes back and works for a while before failing again. <b>Bus Off</b> alone will
not show this — it reads 0 whenever you happen to look — so this counter is the
only sign.</td></tr>
<tr><td>1</td><td>0</td><td>Just went down and the first restart has not
happened yet. If it stays like this for more than a second or two, the bus is
failing to start at all — which is a different fault from a bus that keeps
dropping.</td></tr>
</table>

The count is a total since power-up. Sending or clearing a configuration does not reset it.

> **Warning:** **Bus Load is an estimate.** The CAN controller counts frames, not bits, so the firmware works out how long each frame occupied the bus from its ID width and length, and adds an average allowance for the stuff bits the protocol inserts. The real number depends on the data being carried and cannot be measured exactly. Expect agreement with a dedicated bus analyser to within a few percent — good enough to see a bus filling up, not a figure to design a message schedule against. It also reads 0 on a bus that is switched off, which is the same reading an idle bus gives: use the three state channels, not this one, to tell whether a bus is alive.

### MCU health

Five more channels report what the processor sees about itself. The temperature and voltage come from the microcontroller's internal sensors, sampled ten times a second with the factory calibration applied.

<table>
<tr><th>Channel</th><th>Type</th><th>Meaning</th></tr>
<tr><td>Device MCU Temperature</td><td>s16, 1 dp, °C</td>
<td>The processor die temperature right now. See the accuracy note
below.</td></tr>
<tr><td>Device MCU VDDA</td><td>s32, 3 dp, V</td>
<td>The analogue supply voltage right now, measured against the
factory-calibrated internal reference. Nominally 3.3 V.</td></tr>
<tr><td>Device MCU VDDA Minimum</td><td>s32, 3 dp, V</td>
<td>The lowest supply voltage seen since power-up — a sag detector for flaky
wiring and failing regulators, which dip far too briefly to catch by
watching.</td></tr>
<tr><td>Device MCU Temperature Maximum</td><td>s16, 1 dp, °C</td>
<td>The highest die temperature seen since power-up.</td></tr>
<tr><td>Device Last Reset Reason</td><td>u8, 0–7</td>
<td>Why the device last reset, read once at boot and latched — see the table
below.</td></tr>
</table>

The Minimum and Maximum are excursions **since power-up**, and they survive sending or clearing a configuration — like the error and frame totals, they answer "what has this unit been through", which is exactly the question a mid-diagnosis reconfigure must not erase. Only a reset or a power cycle starts them over.

> **Warning:** **The temperature's tenth of a degree is resolution, not accuracy.** The die sensor is a ±2 °C class device even with its factory calibration, so read the decimal as "which way is it moving", not as a calibrated thermometer. It also measures the *die*, which runs warmer than the board around it.

**Device Last Reset Reason** is an enumerated value. Wherever live values are shown it displays as the name with the number — "Power On (1)"; a number outside this table (a newer firmware's new reason) displays bare:

<table>
<tr><th>Value</th><th>Reason</th><th>Meaning</th></tr>
<tr><td>0</td><td>Unknown</td><td>The reset flags matched nothing the firmware
recognises.</td></tr>
<tr><td>1</td><td>Power On</td><td>A true cold start — power was
applied.</td></tr>
<tr><td>2</td><td>Brownout</td><td>The supply dipped low enough to reset the
processor without fully draining it. A unit that "reboots by itself" with this
reason has a power problem, not a firmware problem — read <b>Device MCU VDDA
Minimum</b> next.</td></tr>
<tr><td>3</td><td>External NRST</td><td>The hardware reset pin was
pulled.</td></tr>
<tr><td>4</td><td>Software Reset</td><td>The firmware restarted itself —
<b>Online → Reset Device</b>, the reboot after a firmware update, or a fault
handler.</td></tr>
<tr><td>5</td><td>Independent Watchdog</td><td>The firmware stopped answering
the independent watchdog and was restarted by it.</td></tr>
<tr><td>6</td><td>Window Watchdog</td><td>The window watchdog fired.</td></tr>
<tr><td>7</td><td>Low Power Reset</td><td>Reset on leaving a low-power
state.</td></tr>
</table>

A device channel cannot be edited or deleted: its type, resolution and range are fixed by the firmware, so the Channel Editor opens it read-only.

You do not have to do anything to use one. Every device channel is sent to the device with every configuration, whether or not anything in your document reads it, so all of them appear in **Monitor Channels** as soon as you send a configuration and connect. That is the point: when a bus starts misbehaving you want the error counters in front of you, not a rebuild-and-resend cycle first.

Because the device writes it, nothing else should. Pointing a calculation's output at a device channel raises the usual two-writers warning, and the device wins: it republishes the value on every 100 Hz evaluation, so the calculation would appear to do nothing.

**Device OnTime** counts the *device* being on, not the configuration being loaded. Sending a new configuration, or clearing one, does not restart it; only a reset or a power cycle does. The same is true of the running totals — **Error Frames**, **Rx Count**, **Tx Count** and **Bus Off Recoveries** — so a counter you are watching climb is not zeroed by sending a configuration while you watch it.

> **Warning:** The device carries every channel value as a 32-bit float, and the seconds it holds stop resolving 0.01 above **131,072** — roughly 36 hours of uptime. Past that the hundredths coarsen, and later the tenths do too. The clock behind the reading is exact and does not drift; the loss is in the value slot. For a long-running unit, treat the seconds as reliable and the fraction as decorative. The same limit applies to the frame counts, which coarsen past about 16.7 million frames. It does *not* apply to the error counters, the state flags, Bus Load or the MCU health channels: those stay small enough that a 32-bit float holds them exactly, however long the unit runs.

## The Channel Editor

**Tools &gt; Channel Editor…** lists every channel in the document in one sortable table with columns **Channel**, **Data Type**, **Dec**, **Resolution**, **Minimum**, **Maximum**, **Unit**, **Default on Timeout** and **Source**.
- **Search :** filters as you type — any part of the name, or a regular expression (^Cruise, Speed$, set|limit).
- **Default on Timeout** shows the value a channel reverts to when its receive message times out — filled in only when that message declares a Receive Timeout *and* has "Default value on timeout" enabled (see [Communications](communications.md)).
- **Source** names what generates the channel: the comms section that carries it ("CAN1 · Dash") or publishes its checksum to it ("CAN1 · Dash CRC8" — see [Transmit CRC8](communications.md#crc8)), or Math, User Condition, Counter, Timer, Integrator, Constant, Table 2x16, Table 8x8 — or "unused".
- **New…** creates a channel; **Edit…** opens the selected one. Double-clicking a row also opens it.

> **Warning:** A row drawn in the warning colour has a data type too small for its own range — the device clamps every reading to what the type can represent, so such a channel reads as stuck at its ceiling. The Data Type column shows the suggested replacement (e.g. "u16  ⚠ → u32") and the summary line counts the affected channels. Fix it by editing the channel's type.

A channel carried by a marked message shows a padlock 🔒 and is greyed. That is *every* level — **Read Only**, **Hidden** and **Protect Communication** alike — because the lock is about editing, not about secrecy: changing a channel's data type, resolution, decimal places, range or units silently changes what its message decodes to. Its definition is read-only, the Edit… button becomes **View…**, and the dialog opens with every value visible and nothing writable. Supplying the message's password does not unlock it: that buys the right to untick the marking, and unticking is what allows editing. See [Marking a message](communications.md#marking).

Channel names stay visible at every level on purpose: the protocol is the secret, not the outputs, so you can still feed the channel into your own math, conditions and transmit messages.

## Edit Custom Channel

The **Edit Custom Channel** dialog defines a channel:

<table>
<tr><th>Field</th><th>Meaning</th></tr>
<tr><td><b>Channel Name:</b></td><td>Up to 31 characters — the device stores a
32-byte label, and the dialog checks the UTF-8 byte count, which a non-ASCII name
can exceed within the character cap (one accented or CJK character is 2-4 bytes,
so 31 characters is not always 31 bytes). Names must be unique (case-insensitive).
Renaming an existing channel rewrites every reference to it — comms rows, math,
conditions.</td></tr>
<tr><td><b>Data Type:</b></td><td>The storage type — see the table below. Blank
for channels created before data types existed; a type must be chosen before OK
accepts.</td></tr>
<tr><td><b>Decimal Places:</b></td><td>0–8, capped per data type.</td></tr>
<tr><td><b>Base Resolution:</b></td><td>Derived, read-only: 10<sup>−Decimal
Places</sup>.</td></tr>
<tr><td><b>Units Type:</b></td><td>The physical quantity (Temperature, Speed,
Voltage, …), which decides the units on offer.</td></tr>
<tr><td><b>Display Units:</b></td><td>The unit shown wherever the channel
appears.</td></tr>
<tr><td><b>Range Minimum:</b> / <b>Range Maximum:</b></td><td>Derived, read-only:
what the chosen type spans at the chosen precision. This range is also the
<b>device's clamp</b> — every reading is limited to it.</td></tr>
</table>

## Storage types

An integer channel holds its physical value as a scaled integer, so its reach is the raw range × 10^−decimals. This table is the quick reference — for the full treatment (worked type-selection examples, the precision/span trade, and what decimal places do and don't affect), see [Data Types &amp; Decimal Places](datatypes.md):

<table>
<tr><th>Type</th><th>Raw range</th><th>Max decimals</th><th>Span at max
decimals</th></tr>
<tr><td>boolean</td><td>0 … 1</td><td>0</td><td>0 … 1</td></tr>
<tr><td>u8</td><td>0 … 255</td><td>2</td><td>0 … 2.55</td></tr>
<tr><td>u16</td><td>0 … 65535</td><td>4</td><td>0 … 6.5535</td></tr>
<tr><td>u32</td><td>0 … 4294967295</td><td>8</td><td>0 … 42.94967295</td></tr>
<tr><td>s8</td><td>−128 … 127</td><td>2</td><td>−1.28 … 1.27</td></tr>
<tr><td>s16</td><td>−32768 … 32767</td><td>4</td><td>−3.2768 …
3.2767</td></tr>
<tr><td>s32</td><td>−2147483648 … 2147483647</td><td>8</td><td>−21.47…
… 21.47…</td></tr>
<tr><td>float</td><td>±1e9 shown</td><td>8</td><td>display convention — see
note</td></tr>
</table>

> **Note:** Pick the type from the channel's **physical range and decimals**, never from a signal's raw bit width. A 16-bit field at 0.036 km/h per bit spans 0…2359.26 at 3 decimal places — u16 only reaches 65.535, so the right type is u32. Signedness follows the physical range too: an unsigned field with offset −40 needs a signed channel. [DBC Import](dbc-import.md) applies these rules automatically.

> **Note:** A float channel's ±1e9 span is a display convention, not a storage limit — the range reaches the device as float32. A channel imported with a wider range (J1939 High Resolution Total Vehicle Distance really spans 0…2.1e10) keeps it: Edit Custom Channel preserves a range wider than the type's displayed span unless you actually change the Data Type or Decimal Places, so opening a channel to fix a typo cannot silently clamp it.

## Comms channel rows

Within the section editor's Channels tab (see [Communications](communications.md)), **Add…** and **Change…** open the **Add Comms Channel** / **Change Comms Channel** dialog, which places one channel in the frame:

<table>
<tr><th>Field</th><th>Meaning</th></tr>
<tr><td><b>Channel :</b></td><td>The channel this row reads (transmit) or writes
(receive). Set via <b>Select…</b>.</td></tr>
<tr><td><b>Default Value :</b></td><td>The physical value written to the channel
when the message's receive timeout fires. Enabled once a channel is selected; its
precision follows the channel's own Decimal Places.</td></tr>
<tr><td><b>Start Bit :</b></td><td>0–511. The bit index of the signal's
<b>least</b> significant bit: bit S = byte S/8, bit S%8; bits count 0–7
right-to-left within a byte, bytes left-to-right from 0. Word Swap fields
continue into higher bytes; Normal (big-endian) fields continue into lower
bytes.</td></tr>
<tr><td><b>Bit Length :</b></td><td>1–64. Locked to 32 for IEEE754.</td></tr>
<tr><td colspan="2"><i>On a new row both bit fields start empty and OK stays
off until real numbers are entered — a prefilled bit position looks chosen and
decodes the wrong bits without anything to object to. Changing an existing row
keeps its values.</i></td></tr>
<tr><td><b>DBC Type :</b></td><td><b>Unsigned</b>, <b>Signed</b> or
<b>IEEE754</b> (32-bit float).</td></tr>
<tr><td><b>Bit Resolution :</b></td><td>What one raw count is worth, in the
channel's units. 0.1 means each count is a tenth. The same number reads both
ways: a received count of 7 is 0.7, and transmitting 0.7 puts 7 on the
wire.</td></tr>
<tr><td><b>Offset :</b></td><td><b>Always added, never subtracted</b>, in
whichever direction the row faces.
<br><br>
On a <b>receive</b> row it is added after scaling, to reach the channel's units:
<b>-40</b> on a temperature whose counts start at -40 makes a raw 0 read as -40.
<br><br>
On a <b>transmit</b> row it is added to the raw count on its way out, after the
resolution has been divided out — a bias in raw counts. Offset <b>64</b> puts 64
more on the wire: a value of 1 at resolution 1 sends 65. The range the field can
carry is previewed live under the fields.</td></tr>
<tr><td><b>Clamp to Signal Limit :</b></td><td><b>Transmit rows
only</b>, ticked by default. Ticked, a value too big for the field is sent as
the biggest the field can hold — 256 into 8 bits sends <b>255</b>. Unticked,
only the low bits are sent, so the count rolls over and 256 sends <b>0</b>.
Under the fields the preview names the range the bits actually hold, in the
channel's own units, so the choice is made against a number.</td></tr>
</table>

> **Note:** Unticked also stops the CHANNEL's range from clamping first. That is not a side effect, it is the point: the channel's Range Minimum…Maximum is applied before the field width is ever consulted, so a channel ranged 0…255 would present 255 and an 8-bit field would have nothing left to roll over. Rolling over means the value goes out as it is and the field keeps its low bits.

Roll-over is what a free-running counter, a wrapping angle, or a value feeding someone else's checksum wants — the count is *supposed* to return to zero. Clamping is what a measurement wants: a coolant temperature that briefly reads past the top of its field should report the top of its field, not suddenly report the bottom. Receive rows have no such choice and always clamp: a received field is as wide as it is, so nothing can overflow it, and the clamp on that side is the channel's declared range.

The dialog validates the row as you type — a field that does not fit the frame blocks OK with the reason — and shows one informational line:
- A **transmit** row naming a channel nothing writes yet transmits its default value until a receive row or a calculation writes it — information, not an error.
- A **receive** row naming a channel that is already written elsewhere is a genuine conflict: both writers overwrite each other's values.

## Selecting channels

**Select…** opens **Select Input Channel** or **Select Output Channel**, depending on whether the field reads or writes. The picker searches by any part of the name or a regular expression, marks channels that have no generator yet or that are already written elsewhere, and its **New…** and **Edit…** buttons open Edit Custom Channel without leaving the picker. The same picker is used for math, condition, counter, timer, integrator and table inputs and outputs.

**New… picks the channel it creates.** Defining a channel from inside the picker *is* the choice — you opened the picker to pick one and there was none to pick — so the picker closes and hands the new channel back to the field you started from. **Edit…** does not: editing a channel is not choosing it, so the picker stays open with the edited channel highlighted, renamed or not.

See also: [Communications: Messages &amp; Sections](communications.md) · [DBC Import](dbc-import.md) · [Math Channels](math-channels.md) · [Monitoring Live Values](monitor.md) · [Validation &amp; the Config Summary](validation-report.md)
