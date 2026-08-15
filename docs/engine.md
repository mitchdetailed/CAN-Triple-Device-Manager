# Order &amp; Timing of Operations

Everything the device computes runs from one loop, on two clocks: an **evaluation pass** every 10 ms (100 Hz), and **frame arrival**, which triggers extra work the moment a configured message is received. This page discloses the exact order of both, and how often everything else on the unit runs — so when one channel reads another you can say precisely how old the value is, and when a message transmits you can say precisely when.

Nothing here is configurable; it is how the firmware is built. The numbers apply to the whole configuration regardless of size — the pass runs the full table of every calculation type, active rows in row order, on every pass.

## The evaluation pass — every 10 ms

Each pass executes these stages, always in this order:

<table>
<tr><th>#</th><th>Stage</th><th>What happens</th></tr>
<tr><td>1</td><td>Device channels</td><td>Device OnTime, the per-bus
<a href="channels.md">CAN diagnostics</a>, Bus Load and the MCU health block
are written first, so everything downstream reads this pass's
values.</td></tr>
<tr><td>2</td><td>Receive timeouts</td><td>Any receive message past its timeout
has its channels set to their default values (where the section asks for
it).</td></tr>
<tr><td>3</td><td><a href="constants.md">Constants</a></td><td>Fixed values
are written to their channels.</td></tr>
<tr><td>4</td><td><a href="tables.md">Lookup tables</a></td><td>2×16 tables,
then 8×8 tables.</td></tr>
<tr><td>5</td><td><a href="math-channels.md">Math channels</a></td><td>All
rows, in row order.</td></tr>
<tr><td>6</td><td><a href="counters.md">Up/Down counters</a></td><td>Edge
detection and rate steps.</td></tr>
<tr><td>7</td><td><a href="timers.md">Timers</a></td><td>Advance by the real
elapsed time.</td></tr>
<tr><td>8</td><td><a href="integrators.md">Integrators</a></td><td>Accumulate
by the real elapsed time.</td></tr>
<tr><td>9</td><td><a href="conditions.md">Conditions</a></td><td>All
comparisons, in row order — after everything above, so a condition on a math or
table result sees the value computed this pass.</td></tr>
<tr><td>10</td><td><a href="device-scripts.md">Device script</a></td><td>The
script's <code>on_tick</code> runs last of the calculation stages, within its
per-pass budget, so it sees everything the pass produced and its outputs still
reach the transmit stage below.</td></tr>
<tr><td>11</td><td>Transmit</td><td>Every transmit message whose period has
elapsed is composed from the channel values as they stand now — the freshest
the pass can offer — and queued to its bus. Composition packs every transmitted
channel (and a compound message's identifier selector) into the frame first;
for a <a href="communications.md#crc8">Transmit CRC8</a> message the checksum
is then computed over its configured elements, stamped into its byte last —
after every other byte of the frame is final — and published to its CRC
channel.</td></tr>
</table>

Around the pass, on the same 100 Hz beat: the CAN error state of all three buses is sampled just *before* it (feeding the diagnostics the pass publishes in stage 1), and after it the [Monitor Channels](monitor.md) value stream sends its slice of the channel table (see the rate table below).

## When a frame arrives

Received frames are not held for the next pass — each one is processed on arrival, in this order:
1. **[Message relays](relays.md)** — every relay rule is checked against every received frame, matched or not, and forwards immediately.
2. **Message match** — the first active receive message on that bus with that CAN ID (and matching standard/extended type) wins; the message's timeout window restarts.
3. **Channel decode** — the matched section's channels are extracted and scaled into their values.
4. **Recalculation** — constants, lookup tables, math and conditions re-run (the same stages 3–5 and 9 above, in the same order), so anything derived from the received channels updates immediately rather than up to 10 ms later.
5. **Routing** — if the section routes to other buses, the frame is forwarded last, after the recalculation.

Counters, timers and integrators do *not* run here — they advance on the 10 ms clock only, because they measure time. The device script's `on_tick` does not run here either: it is a per-pass hook with a per-pass budget, not a per-frame one. Both pick up the received values on the next pass, at most 10 ms later.

## Reading across the order: the one-pass lag

Within a pass, a reference to something computed at an *earlier* stage (or an earlier row of the same stage) reads the value from *this* pass. A reference to something computed *later* reads the value from the *previous* pass — 10 ms old. The order above is what "earlier" and "later" mean. The cases that matter in practice:

<table>
<tr><th>Reference</th><th>What it reads</th></tr>
<tr><td>Math ← constant, table</td><td>This pass</td></tr>
<tr><td>Condition ← math, table, counter, timer, integrator</td><td>This
pass</td></tr>
<tr><td>Table axis ← math channel</td><td>Previous pass (tables run before
math)</td></tr>
<tr><td>Math ← condition</td><td>Previous pass</td></tr>
<tr><td>Counter / timer / integrator input ← condition</td><td>Previous
pass</td></tr>
<tr><td>Anything ← script-written channel</td><td>Previous pass (except the
transmit stage, which runs after the script)</td></tr>
<tr><td>Transmit message ← anything</td><td>This pass</td></tr>
</table>

> **Note:** A chain that spans several "backwards" references accumulates one pass of lag per hop. A condition gating a counter whose output feeds a math row read by a table is correct — it just settles over a few passes rather than one. At 100 Hz that is tens of milliseconds, which rarely matters; it only surprises when you expect a multi-stage chain to react within a single pass.

## Transmit timing

Each transmit message runs on its own period — the section's Transmit Rate, 1 to 200 Hz, so periods from 1000 ms down to 5 ms. The transmit scheduler runs on its own 5 ms slots, twice as fast as the evaluation pass, and that is the scheduling resolution: a period is always a whole number of slots. Transmit CRC8 messages schedule identically — the checksum is computed for every frame, whatever the rate.

> **Note:** What a 5 ms message carries: **received** channels are updated the moment their frame arrives, so a fast gateway message forwards fresh data on every transmission. **Calculated** channels (math, counters, timers, tables, scripts) update on the 10 ms evaluation pass, so a 200 Hz message repeats each calculated value at most once — the calculation chain deliberately does not speed up with the scheduler.
- **Messages are phase-spread per bus.** When a configuration is loaded, the transmit messages of each bus are offset within their periods so they come due spread across the period rather than all on the same pass. Ten 10 Hz messages on one bus transmit one per 10 ms across their 100 ms period, not ten back-to-back every 100 ms.
- **Section order is the tie-break.** Messages sharing a bus and a rate are offset in list order, so they go out in the order the [Communications](communications.md) list shows — list order = transmit order.
- **An oversubscribed bus degrades fairly.** If more comes due than the bus can carry, the scan resumes each pass where the queue filled, so the shortfall is shared across the whole table instead of silencing the messages at the end of the list.
- **A missed cycle is dropped, not banked.** When a transmission cannot be sent (bus off, queue full), the message does not accumulate a backlog; the next cycle sends the current channel values. Cyclic data carries the freshest value or nothing.

## How often everything runs

<table>
<tr><th>Operation</th><th>Rate / latency</th></tr>
<tr><td>Evaluation pass (stages above)</td><td>Every 10 ms
(100 Hz)</td></tr>
<tr><td>Constants, tables, math, conditions</td><td>Every pass, <em>plus</em>
immediately on every matched received frame</td></tr>
<tr><td>Counters, timers, integrators</td><td>Every pass only</td></tr>
<tr><td>Device script <code>on_tick</code></td><td>Every pass, budgeted (see
<a href="device-scripts.md">Device Scripts</a>)</td></tr>
<tr><td>Message relays, routing</td><td>Immediately, per received
frame</td></tr>
<tr><td>Transmit scheduling</td><td>Checked every 5 ms (its own
200 Hz clock); each message at its configured 1–200 Hz</td></tr>
<tr><td>Transmit CRC8 stamp &amp; publish</td><td>With every transmission of
its message, during composition — channels packed first, checksum stamped
last</td></tr>
<tr><td>Receive timeout check</td><td>Every pass</td></tr>
<tr><td>Device channels (OnTime, diagnostics, MCU health)</td><td>Republished
every pass</td></tr>
<tr><td>CAN error-state sampling</td><td>Every pass, just before it</td></tr>
<tr><td>Bus Load</td><td>Recomputed over a 1 s window</td></tr>
<tr><td>MCU health sampling (temperature, VDDA)</td><td>Sampled at
10 Hz; each pass republishes the latest sample</td></tr>
<tr><td>Reset reason</td><td>Read once at boot and latched</td></tr>
<tr><td>Bus-off recovery</td><td>Bus stopped on bus-off, restart attempted
every 1 s until it stays up</td></tr>
<tr><td>Monitor Channels (value stream)</td><td>Up to 160 channels per pass,
round-robin — a full refresh of every channel takes one pass for up to 160
channels, and at most 70 ms at the device's 1000-channel limit</td></tr>
<tr><td>CAN Viewer (monitor stream)</td><td>Per frame, as sent or
received</td></tr>
<tr><td>Preserved values written to flash</td><td>Once a minute, only when
changed (see <a href="integrators.md">Integrators</a>)</td></tr>
</table>

> **Note:** The pass is driven by real elapsed time, not by counting visits. If something stalls the loop — a flash erase during Save to Flash is the realistic case — the next pass is told how much time actually passed: timers and integrators advance by the true amount, and transmit periods stretch rather than burst-transmitting a backlog afterwards.

## See also

[Math Channels](math-channels.md) · [Conditions](conditions.md) · [Messages &amp; Sections](communications.md) · [Message Relays](relays.md) · [Device Scripts](device-scripts.md) · [Monitoring Live Values](monitor.md) · [Channels](channels.md)
