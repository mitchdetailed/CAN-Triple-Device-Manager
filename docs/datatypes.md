# Data Types &amp; Decimal Places

Every channel carries three settings that decide what values it can hold and how precisely it holds them. They do different jobs and are easy to conflate:

<table>
<tr><th>Setting</th><th>What it governs</th></tr>
<tr><td><b>Data Type</b></td><td>The storage span — the raw integer (or float)
the device keeps for the channel, and therefore the widest range it can
represent.</td></tr>
<tr><td><b>Decimal Places</b></td><td>The precision — how far below 1.0 the
channel resolves. The base resolution is always
10<sup>−Decimal Places</sup>.</td></tr>
<tr><td><b>Factor / Offset</b> (comms rows)</td><td>The conversion between a
frame's raw field and the physical value:
Physical = raw × Factor + Offset. This — not
Decimal Places — is what scales values arriving on or leaving the bus. See
<a href="communications.md">Communications</a>.</td></tr>
</table>

## The eight storage types

<table>
<tr><th>Type</th><th>Raw range</th><th>Max decimals</th><th>Span at max
decimals</th><th>Typical use</th></tr>
<tr><td>boolean</td><td>0 … 1</td><td>0</td><td>0 … 1</td>
<td>Flags, User Condition outputs, enables.</td></tr>
<tr><td>u8</td><td>0 … 255</td><td>2</td><td>0 … 2.55</td>
<td>Small counts, gear number, status codes.</td></tr>
<tr><td>u16</td><td>0 … 65535</td><td>4</td><td>0 … 6.5535</td>
<td>Most unsigned sensors — speeds, pressures, RPM.</td></tr>
<tr><td>u32</td><td>0 … 4294967295</td><td>8</td><td>0 … 42.94967295</td>
<td>Totals, odometers, wide unsigned spans.</td></tr>
<tr><td>s8</td><td>−128 … 127</td><td>2</td><td>−1.28 … 1.27</td>
<td>Small signed values — trim steps, small offsets.</td></tr>
<tr><td>s16</td><td>−32768 … 32767</td><td>4</td>
<td>−3.2768 … 3.2767</td>
<td>Signed sensors — temperatures below zero, torque, slip.</td></tr>
<tr><td>s32</td><td>−2147483648 … 2147483647</td><td>8</td>
<td>−21.47… … 21.47…</td>
<td>Wide signed spans, signed totals.</td></tr>
<tr><td>float</td><td>±1e9 shown</td><td>8</td><td>display convention</td>
<td>Ranges no integer type can hold, or genuinely fractional data.</td></tr>
</table>

The six integer types hold their physical value as a **scaled integer**: the stored count is the physical value shifted up by 10^Decimal Places. That is why the same type reaches less far the more precision you give it — the span is the raw range × 10^−decimals.

## Decimal places are the channel's resolution

Decimal Places sets the smallest step the channel can represent — the **Base Resolution** field in Edit Custom Channel shows it, derived and read-only, as 10^−Decimal Places. Raising precision costs span, one decade per decimal place. For a u16 channel:

<table>
<tr><th>Decimal places</th><th>Resolution</th><th>Storable span</th></tr>
<tr><td>0</td><td>1</td><td>0 … 65535</td></tr>
<tr><td>1</td><td>0.1</td><td>0 … 6553.5</td></tr>
<tr><td>2</td><td>0.01</td><td>0 … 655.35</td></tr>
<tr><td>3</td><td>0.001</td><td>0 … 65.535</td></tr>
<tr><td>4</td><td>0.0001</td><td>0 … 6.5535</td></tr>
</table>

The per-type decimals caps (2 for 8-bit, 4 for 16-bit, 8 for 32-bit and float) simply stop you asking for more digits than the raw span can fill.

The **Range Minimum / Range Maximum** fields are derived the same way — what the chosen type spans at the chosen precision — and that range is also the **device's clamp**: the firmware limits every value crossing the bus to it, on receive and again on transmit. (Calculation results are written to their value slot unclamped — a math channel can hold a value outside its nominal range until it is transmitted.) A channel whose type is too small for the values arriving does not overflow or wrap; it sits pinned at its ceiling. The [Channel Editor](channels.md) paints such rows in the warning colour and suggests the next type up (for example "u16 ⚠ → u32").

## Choosing a type and precision

Work from the **physical range and the precision you need** — never from a signal's raw bit width on the bus (Factor/Offset already bridges those two worlds). The stored count is physical × 10^decimals, and that count must fit the raw range:
- **Coolant temperature, −40 … 215 °C at 0.1 °C:** counts −400 … 2150. Negative values rule out the unsigned types; s16 (−32768 … 32767) fits easily. Note the signedness rule: an unsigned bus field with a −40 offset still needs a *signed channel*, because the physical value goes below zero.
- **Wheel speed, 0 … 300.00 km/h at 0.01:** counts 0 … 30000 — u16 at 2 decimal places (span 0 … 655.35) holds it.
- **Odometer, 0 … 1 000 000.0 km at 0.1:** counts 0 … 10 000 000 — beyond u16 entirely; u32 at 1 decimal place spans 0 … 429 496 729.5.

> **Note:** [DBC Import](dbc-import.md) applies exactly these rules automatically: it takes each signal's declared min/max from the DBC when the file provides one (and only otherwise computes the span from factor, offset and bit width), then picks the narrowest type and signedness that holds it — falling back to float when no integer type can.

## float and boolean

A **float** channel stores IEEE-754 binary32, so Decimal Places changes nothing about what it can hold — it only sets how many digits the program displays and lets you type. The ±1e9 span shown in the editor is a display convention, not a storage limit: a channel whose real range is wider (J1939 High Resolution Total Vehicle Distance spans 0 … 2.1e10) keeps that range, and Edit Custom Channel preserves it unless you actually change the Data Type or Decimal Places.

**boolean** is locked to 0 decimal places and the range 0 … 1. Anything that writes it follows the boolean convention used everywhere else: true is any value &gt; 0.

## Where decimal places act in the program
- **Entry precision** — spin boxes for values of that channel follow its Decimal Places: a comms row's Default Value, a Constant's value, table sites and outputs.
- **Display** — [Monitor Channels](monitor.md) formats each value to the channel's Decimal Places. (The [Config Summary](validation-report.md) prints values at full precision and shows type and decimals as metadata instead, e.g. "(u16, 2 dp)".)
- **Round-tripping** — constants and table outputs carry their type and decimals to the device, so Get a Configuration rebuilds those channel definitions exactly. A math destination does not: its type is inferred from its range on read-back, with decimals 0.

## Decimal places and frame scaling — under the hood

> **Note:** On the wire, the program expresses **all** frame conversion in Factor and Offset and always sends comms rows with a decimal-places field of zero — Decimal Places never changes the value a received frame produces or a transmitted frame carries. (The device protocol itself also supports a fixed-point interpretation — integer fields shifted down by 10^decimals, float fields rounded to the given digits — but a configuration built by this program never uses it; you would only meet it in a configuration produced by other tooling.) If a monitored value looks "wrong by a factor of 10", look at the row's Factor, not at Decimal Places.

See also: [Channels](channels.md) · [Communications](communications.md) · [Constants](constants.md) · [Monitoring Live Values](monitor.md)
