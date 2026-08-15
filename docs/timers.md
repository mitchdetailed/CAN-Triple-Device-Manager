# Timers

A timer accumulates elapsed time, in seconds, into a generated [channel](channels.md) while it is running. It is started and stopped by the rising edges of boolean input channels, so a [condition](conditions.md) output is the natural trigger. Timers are evaluated on the device at 100 Hz, and their outputs can feed [math channels](math-channels.md), other calculations, transmit messages, or be watched in [Monitor Channels](monitor.md).

To edit timers, choose **Calculations → Timers…**. The dialog lists every timer with columns **#**, **Active**, **Output**, **Start**, **Stop** and **Mode** (**Count up** or **Count down**). Use **Add…**, **Change…** (or double-click a row) and **Remove**. The device supports at most **50** timers. Changes are written into the configuration only when you close the dialog with **OK**; **Cancel** discards them.

## To add a timer
1. Choose **Calculations → Timers…** and click **Add…**. The **Timer** editor opens with two tabs.
2. On the **Start / Stop** tab, click **Select…** next to **Output Channel :** and pick or create the output channel (required), then select the **Start timer on channel :** and **Stop timer on channel :** triggers.
3. On the **Settings** tab, choose the counting direction and limit behaviour.
4. Tick **Active** and click **OK**.

> **Note:** As the editor itself says: the timer runs while started; each input triggers on its rising edge (value &gt; 0). A timer with neither a start nor a stop channel never runs — validation warns about this.

## Start / Stop tab

<table>
<tr><th>Control</th><th>Meaning</th></tr>
<tr><td>Output Channel :</td><td>The generated channel that receives the
running time, in seconds.</td></tr>
<tr><td>Start timer on channel :</td><td>A rising edge starts the timer. If
<b>Enable start setting</b> is ticked, the output is also set to the start
value at that moment.</td></tr>
<tr><td>Stop timer on channel :</td><td>A rising edge stops the timer. If
<b>Enable stop setting</b> is ticked, the output is also set to the stop value
at that moment.</td></tr>
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

[Up/Down Counters](counters.md) · [Integrators](integrators.md) · [Conditions](conditions.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md) · [Validation &amp; the Config Summary](validation-report.md)
