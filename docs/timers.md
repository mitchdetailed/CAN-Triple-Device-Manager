# Timers

> **Note:** **Configurations saved before this version** open normally. A timer that started on a channel becomes the comparison the device was already making — "that channel ≠ 0" — so it runs exactly as it did. Nothing needs changing by hand.

A timer accumulates elapsed time, in seconds, into a generated [channel](channels.md) while it is running. It is started and stopped by **comparisons** — "Engine RPM &gt; 4000", or "this message was received" — each firing on the **rising edge** of that comparison becoming true. A [User Condition](conditions.md) output still works as a trigger, and is what you want when the test needs more than one comparison joined with AND/OR; a single comparison no longer needs one. Timers are evaluated on the device at 100 Hz, and their outputs can feed [math channels](math-channels.md), other calculations, transmit messages, or be watched in [Monitor Channels](monitor.md).

To edit timers, choose **Calculations → Timers…**. The dialog lists every timer with columns **#**, **Active**, **Output**, **Start**, **Stop** and **Mode** (**Count up** or **Count down**). Use **Add…**, **Change…** (or double-click a row) and **Remove**. The device supports at most **50** timers. Changes are written into the configuration only when you close the dialog with **OK**; **Cancel** discards them.

## To add a timer
1. Choose **Calculations → Timers…** and click **Add…**. The **Timer** editor opens with two tabs.
2. On the **Start / Stop** tab, click **Select…** next to **Output Channel :** and pick or create the output channel (required), then fill in the **Start Timer** and **Stop Timer** boxes — choose what kind of comparison each one is, then fill in what it needs. The two boxes are laid out like a [User Condition](conditions.md)'s Set and Reset, one control per line.
3. On the **Settings** tab, choose the counting direction and limit behaviour.
4. Tick **Active** and click **OK**.

> **Note:** Each trigger fires on the **rising edge** of its comparison becoming true — not for as long as it stays true. "Start when RPM &gt; 4000" starts the timer at the moment RPM crosses 4000 and does nothing further while it stays above; it takes a stop trigger, or a drop back below and another crossing, to change anything. A timer with neither a start nor a stop trigger never runs, and one with no start never starts; validation warns about both.

> **Note:** Leaving a trigger's channel empty means that half never fires. A timer with a start and no stop runs from its first trigger until the device restarts, which is usually what you want for an hours counter.

## Start / Stop tab

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td>Output Channel :</td><td>The generated channel that receives the
running time, in seconds.</td></tr>
<tr><td>Start Timer</td><td>The timer starts on the rising edge of this
comparison becoming true. If <b>Enable start setting</b> is ticked, the output
is also set to the start value at that moment.</td></tr>
<tr><td>Stop Timer</td><td>The same, stopping the timer. If
<b>Enable stop setting</b> is ticked, the output is also set to the stop value
at that moment.</td></tr>
<tr><td><b>Compare:</b></td><td>The first control in each box says what kind of
comparison it is, and the rest of the box follows from it:
<ul>
<li><b>Channel Value</b> — a comparison. Pick a channel, an operator
(<b>=</b>, <b>≠</b>, <b>&lt;</b>, <b>≤</b>, <b>&gt;</b>, <b>≥</b>) and either a
value or another channel to compare against.</li>
<li><b>Message Received</b> — pick a message. True on the pass in which a frame
of it actually arrived.</li>
<li><b>Message Transmitted</b> — the same, for frames the device sent.</li>
</ul>
The two message kinds are the same ones a
<a href="conditions.md">User Condition</a> and a
<a href="counters.md">counter</a> offer, evaluated by the same code on the
device.<br><br>
A message is named by its <b>bus and its name</b>, so <b>renaming</b> one in
Communications Setup carries the trigger with it, and <b>deleting</b> one is
reported by <b>File → Check Channels</b> against the timer that names
it.</td></tr>
<tr><td>No <b>for</b> duration</td><td>A User Condition can require its
comparison to hold for a set time; a timer cannot, and that is not an
omission. A timer fires on the <em>rising edge</em> of its comparison, and an
edge is an instant — "has been true for five seconds" describes a level, and
there is no level here to hold. If you want a timer that starts only after a
condition has held, put the duration on a
<a href="conditions.md">User Condition</a> and start the timer from
that.</td></tr>
</table>

## Settings tab

<table>
<tr><th>Group</th><th>Control</th><th>Meaning</th></tr>
<tr><td>Sequence</td><td>Count up / Count down</td><td>Whether elapsed time is
added to or subtracted from the output while running.</td></tr>
<tr><td>Limit</td><td>Value :</td><td>The limit the timer runs against. For a
count-up timer without roll-over, the value holds at this limit (a limit of 0
means no limit). A count-down timer without roll-over stops at 0.</td></tr>
<tr><td>Limit</td><td>Roll over when limit exceeded</td><td>Ticked, the value
wraps around within 0…Limit instead of holding. Roll-over needs a positive
limit value; validation warns otherwise.</td></tr>
<tr><td>Start Setting</td><td>Enable start setting / When started, set value
to :</td><td>Load a fixed value into the output on each start edge — for
example 0 to restart a lap timer, or the period for a count-down.</td></tr>
<tr><td>Stop Setting</td><td>Enable stop setting / When stopped, set value
to :</td><td>Load a fixed value into the output on each stop edge.</td></tr>
</table>

> **Note:** To build a count-down timer, choose **Count down**, tick **Enable start setting** and set **When started, set value to :** to the period you want; the output then runs down and holds at 0. For a repeating saw-tooth, tick **Roll over when limit exceeded** instead.

> **Warning:** Without a start setting, the timer resumes from whatever value its output already holds — starting is not the same as restarting from zero.

## See also

[Up/Down Counters](counters.md) · [Integrators](integrators.md) · [User Conditions](conditions.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md) · [Validation &amp; the Config Summary](validation-report.md)
