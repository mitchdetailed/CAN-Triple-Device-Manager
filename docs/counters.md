# Up/Down Counters

A counter is a calculation that steps a value up or down on the edges of boolean input channels. Its output is an ordinary generated [channel](channels.md), so it can feed [math channels](math-channels.md), [conditions](conditions.md), transmit messages, or be watched live in [Monitor Channels](monitor.md). Counters are evaluated on the device at 100 Hz.

To edit counters, choose **Calculations → Up / Down Counters…**. The dialog lists every counter with columns **#**, **Active**, **Output**, **Type** and **Inputs**. Use **Add…**, **Change…** (or double-click a row) and **Remove** to manage the list. The device supports at most **50** counters. Changes are written into the configuration only when you close the dialog with **OK**; **Cancel** discards them.

## To add a counter
1. Choose **Calculations → Up / Down Counters…** and click **Add…**. The **Up / Down Counter Settings** editor opens.
2. Under **Output**, click **Select…** and pick or create the output channel. An output channel is required.
3. Under **Input**, choose the **Type :** — **Up / Down**, **Follow Changes** or **Every x Hz** — and select the trigger channels (or, for **Every x Hz**, the rate and direction).
4. Set the limits and step under **Options**, tick **Active**, and click **OK**.

## Counter settings

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td>Channel : (Output)</td><td>The generated channel the counter writes.
The decimal precision of the value fields below follows this channel's decimal
places.</td></tr>
<tr><td>Type :</td><td><b>Up / Down</b> counts edges on separate Up and Down
inputs; <b>Follow Changes</b> counts changes of a single Follow input;
<b>Every x Hz</b> is driven by the device's own clock and reads no input channel
at all. The selection enables the matching rows.</td></tr>
<tr><td>Up :</td><td>Adds <i>Step</i> on each rising edge (Up / Down type).</td></tr>
<tr><td>Down :</td><td>Subtracts <i>Step</i> on each rising edge (Up / Down type).</td></tr>
<tr><td>Follow :</td><td>Adds <i>Step</i> on <em>every change</em> of the input
— rising or falling (Follow Changes type).</td></tr>
<tr><td>Rate :</td><td>How many steps a second, and whether each one
<b>Increment</b>s or <b>Decrement</b>s (Every x Hz type). See below.</td></tr>
<tr><td>Reset :</td><td>On its rising edge the counter is set to <i>Reset
value</i>. Reset always applies, even while the counter is disabled.</td></tr>
<tr><td>Enable :</td><td>The counter counts only while this channel is true
(value &gt; 0). Leave it empty to count always.</td></tr>
<tr><td>Minimum value : / Maximum value :</td><td>The counting range. If the
maximum does not exceed the minimum, no limiting is applied (validation warns
about this).</td></tr>
<tr><td>Reset value :</td><td>The value loaded on a reset edge.</td></tr>
<tr><td>Step :</td><td>How far each edge moves the value. A step of 0 counts as
1.</td></tr>
<tr><td>Roll at limits</td><td>Ticked, the value wraps around within the
Minimum–Maximum range instead of stopping; clear, it holds at the limit.</td></tr>
<tr><td>Preserve value</td><td>Keeps the counter's value across power cycles —
see below.</td></tr>
<tr><td>Active</td><td>Only active counters run on the device.</td></tr>
</table>

> **Note:** All counter inputs are boolean channels: true means the channel's value is greater than 0. A [condition](conditions.md) output is the usual way to build a clean trigger from an analog channel.

> **Note:** The Reset value is always forced into the Minimum–Maximum range, even when the limits are disabled for counting (Maximum ≤ Minimum), so keep it inside the range you configure.

## Every x Hz — counting on the clock

A counter set to **Every x Hz** is driven by the device's own clock instead of by a channel. It moves one *Step* at the chosen rate, in the direction you pick, for as long as it is enabled. Choose from **1, 2, 5, 10, 20, 50** or **100 Hz**, and **Increment** or **Decrement**.

The Up, Down and Follow rows are switched off in this mode — a rate counter reads no input channel, and the configuration is sent without those references. **Reset** and **Enable** still work, and they are most of what makes the mode useful:
- **Enable** gates the counting, and the period is *frozen* while the gate is shut rather than continuing to run. Re-enabling therefore costs no step and banks none: the counter picks the period up where it left it.
- **Reset** loads the Reset value on its rising edge and restarts the period, so the first step after a reset is a full 1/rate away rather than whatever remained on the clock. Reset applies even while disabled.

The limits behave as they do in the other modes: the value stops at Minimum or Maximum, or wraps if **Roll at limits** is ticked. A 1 Hz counter with Step 1 rolling between 0 and 60 is a seconds hand; a decrementing counter with a Reset channel is a countdown.

> **Note:** The device evaluates at 100 Hz, which is why 100 Hz is the fastest rate offered — one step per evaluation. Every rate in the list divides 100 exactly, so each is a whole number of ticks per step and the timing is exact rather than averaged.

> **Note:** This is close to what an [integrator](integrators.md) does, and the difference is worth knowing: an integrator adds the value of a *channel* at a rate, while a rate counter adds a fixed *Step*. Use a counter when you want a clock-driven ramp or countdown, and an integrator when you want to accumulate something the bus is telling you.

## Preserve value

Ticking **Preserve value** keeps the counter's value across power cycles. The value is written to a small flash store about once a minute, so up to a minute of counting can be lost on a sudden power cut, and the store is reset whenever you send a changed configuration — preserved values never reattach to a different counter.

At most **20** values can be preserved, and the store is *shared* between counters and [integrators](integrators.md). The label at the bottom of the dialog shows a running budget of the counters in this list ("Preserved across power cycles: N of 20."); the combined counter + integrator total is enforced by [Check Channels](validation-report.md), which reports an error when the two together exceed 20.

> **Warning:** On a device whose flash has no room for the retained-value store, preserved counters still run but start from their reset value at every power-up. Without **Preserve value**, every counter starts from its reset value at power-up.

## See also

[Timers](timers.md) · [Integrators](integrators.md) · [Conditions](conditions.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md) · [Validation &amp; the Config Summary](validation-report.md)
