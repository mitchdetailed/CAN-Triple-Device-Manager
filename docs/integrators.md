# Integrators

An integrator is a *rate accumulator*: at its configured rate it adds its input to its output channel — `out += input`, *Rate* times a second. It is deliberately *not* a time integral (`input × dt`): the rate scales the result, which is what makes it worth configuring. A steady input of 1 at 10 Hz moves the output by 10 per second; the same input at 1 Hz moves it by 1 per second.

> **Note:** To integrate a rate channel into a total — litres per second into litres, for example — pre-scale the input with a [math channel](math-channels.md) first, so that one step adds the right slice (input ÷ rate). The editor's summary line shows the resulting per-second movement as you type.

To edit integrators, choose **Calculations → Integrators…**. The dialog lists every integrator with columns **#**, **Active**, **Output**, **Applies** (the channel or fixed value it accumulates), **Rate**, **Direction**, **Starts at** (with "(preserved)" appended when Preserve is on) and **Reset**. Use **Add…**, **Change…** (or double-click a row) and **Remove**. The device supports at most **8** integrators. Changes are written into the configuration only when you close the dialog with **OK**; **Cancel** discards them.

## To add an integrator
1. Choose **Calculations → Integrators…** and click **Add…**. The **Integrator** editor opens.
2. Under **Output**, click **Select…** next to **Output Channel :** and pick or create the output channel (required).
3. Under **Accumulate**, choose **Channel :** or **Fixed value :**, set the **Rate :** and the **Direction :**, and check the summary line below — it spells out how fast the output will move.
4. Under **Control**, set the **Starting value :** and, if needed, the enable and reset channels.
5. Tick **Active** and click **OK**.

## Integrator settings

<table>
<tr><th>Group</th><th>Control</th><th>Meaning</th></tr>
<tr><td>Output</td><td>Output Channel :</td><td>The generated channel the
running total is written to.</td></tr>
<tr><td>Accumulate</td><td>Channel : / Fixed value :</td><td>What is added each
step — a channel's current value, or a fixed number. The input channel cannot
be the output channel: the value would double every step instead of
accumulating, and the editor refuses the combination.</td></tr>
<tr><td>Accumulate</td><td>Rate :</td><td>How many times a second the input is
applied, 1–100 Hz. The engine evaluates at 100 Hz, which is the
ceiling. A rate that does not divide evenly into the tick still averages
exactly the configured steps per second — nothing drifts.</td></tr>
<tr><td>Accumulate</td><td>Direction : (Count up / Count down)</td><td>
<b>Count down</b> subtracts each step instead of adding — same input, same
rate. Use it with a <b>Starting value :</b> at the peak and <b>Minimum :</b>
as the floor to build a decrementor (a countdown of fuel remaining, service
life, and the like).</td></tr>
<tr><td>Control</td><td>Starting value :</td><td>The value the device loads at
power-up (and when a configuration is sent). For a count-down integrator this
is the peak it counts down from. A restored preserved value overrides
it.</td></tr>
<tr><td>Control</td><td>Enable while channel :</td><td>Accumulation runs only
while this channel is true (value &gt; 0). Leave it empty —
"(always accumulating)" — to run always. The gate pauses accumulation cleanly
rather than losing steps.</td></tr>
<tr><td>Control</td><td>Reset on channel :</td><td>On this channel's rising
edge the output is set to the reset value. Reset applies even while disabled.
Leave it empty for "(never reset)".</td></tr>
<tr><td>Control</td><td>When reset, set value to :</td><td>The value loaded on
a reset edge.</td></tr>
<tr><td>Control</td><td>Preserve value across power cycles</td><td>Retains the
running total in flash and restores it at the next power-up instead of the
starting value — see below.</td></tr>
<tr><td>Limits</td><td>Minimum : / Maximum :</td><td>The value holds at these
limits — for a count-down integrator the minimum is the floor it stops at. A
maximum that does not exceed the minimum turns clamping off entirely, and the
value can then run away; validation warns about it.</td></tr>
<tr><td></td><td>Active</td><td>Only active integrators run on the
device.</td></tr>
</table>

> **Warning:** An integrator with no **Reset on channel :** can only be cleared by power-cycling the device — and with **Preserve value across power cycles** also ticked, nothing can ever clear the total. [Check Channels](validation-report.md) warns about both.

## Preserve and flash wear

Preserved values live in a small flash store that is flushed about once a minute and holds at most **20** entries, shared with preserved [counters](counters.md). The two are not equal customers of that store: a counter is event-driven and often unchanged for a whole minute, so its flush costs nothing, but a running integrator changes on every step and writes a record at essentially every 60 s flush.

> **Warning:** With N preserved values in use, the store's active page (about 254 records) fills in roughly 254/N flushes — expect a flash erase about every **254/N minutes** while an integrator is running. Check Channels reports this estimate whenever a preserved integrator is configured. Prefer **Preserve value across power cycles** only on totals that genuinely must survive a power cycle.

The store is reset whenever you send a changed configuration, so a preserved total never reattaches to a different calculation.

## See also

[Math Channels](math-channels.md) · [Up/Down Counters](counters.md) · [Timers](timers.md) · [User Conditions](conditions.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md) · [Validation &amp; the Config Summary](validation-report.md)
