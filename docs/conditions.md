# User Conditions

A User Condition is a boolean logic channel: it evaluates up to three comparisons and drives its output channel to **true (1)** or **false (0)**. The device supports up to **250** User Conditions. The output is an ordinary generated channel, so it can trigger a [counter](counters.md) or [timer](timers.md), gate an [integrator](integrators.md), feed a [math channel](math-channels.md) (for example as the A input of Select), decide when a [Triggered transmit message](communications.md#triggered) goes out, be transmitted, or be watched in [Monitor Channels](monitor.md).

**What brings the output back down is the condition's [Mode](#modes).** There is no plain level any more — a condition no longer just follows its comparisons pass by pass with no memory of the last one. It is either **Momentary**, where each rising edge produces a pulse of a length you set, or **Set / Reset**, where one expression raises the output and another lowers it. Configurations written before the modes are [migrated](#migration) as they load and behave exactly as they always did.

> **Note:** Each active User Condition also takes one of the device's **1,000** channel slots for its output, and that — not room in the condition table — is the real reason the number is not higher: 250 User Conditions can claim a quarter of the channel table on their own.

## The User Conditions dialog

To add a User Condition, choose **Calculations → User Conditions…**. The dialog lists every row with the columns **#**, **Active**, **Condition** and **Output channel**, with **Add…**, **Change…** and **Remove** buttons at the right. The **Condition** column shows the expression exactly as the device evaluates it, brackets included. Double-clicking a row opens it for change. OK commits the changes; Cancel discards them.

> **Note:** Adding beyond the limit reports "The device supports at most 250 User Conditions."

<a id="modes"></a>

## The two modes

Every User Condition is one of two shapes, and the mode is the whole of the difference between them. Both build their expressions the same way, out of the same comparisons; what they disagree about is when the output goes back to 0.

<table>
<tr><th>Mode</th><th>Output goes to 1</th><th>Output goes to 0</th>
<th>What you configure</th></tr>
<tr><td><b>Momentary</b></td><td>on each rising edge of the Set
expression</td><td>one period of the Latch Frequency later, on its
own</td><td>Set comparisons, Latch Frequency</td></tr>
<tr><td><b>Set / Reset</b></td><td>while the Set expression is
true</td><td>while the Reset expression is true — Reset wins</td>
<td>Set comparisons, Reset comparisons</td></tr>
</table>

### Momentary

You give the **Set** comparisons and a **Latch Frequency :**. On the **rising edge** of the Set expression the output goes to 1 and holds there for **one period of that frequency** — 10 Hz is 100 ms, 1 Hz is one second — and then drops on its own, whether or not the comparisons are still true. The Reset comparisons are not used.

It is **retriggerable**: a fresh rising edge while the output is still high *restarts* the hold rather than extending it, so the pulse is always one whole period measured from the most recent edge. This is the rule a [counter](counters.md)'s reset edge already applies to its phase, and it is the one that makes a Momentary predictable — a pulse is a fixed length or it is not a pulse.

> **Note:** The Latch Frequency's ceiling is **100 Hz**, and the reason is worth stating: the device spends the hold on its 10 ms calculation pass, so one tick is the shortest hold that exists and 100 Hz is the frequency whose period *is* one tick. It is the same ceiling, for the same reason, that the [rate counter](counters.md) and the [integrator](integrators.md) already have. The hold is also spent in 10 ms lumps, so it ends on the first pass at or after the period rather than exactly on it: a 30 Hz pulse asks for 33 ms and lasts 40.

### Set / Reset

You give a **Set** expression and a **Reset** expression. It is a latch: Set drives the output to 1, Reset drives it to 0, and between the two it **holds** whatever it was last driven to. Neither expression has to keep being true for the output to stay where it was put, and that is what makes this a latch rather than a level — the output outlives the thing that caused it.

**Reset is dominant.** With both expressions true on the same pass the output is **0**. This is deliberate, and it is the safe bias: a Reset that means "stop" must not be defeatable by a Set that is stuck on. The [timer](timers.md) stage already behaves this way — a start edge and a stop edge on the same pass leave the timer stopped — so the two agree rather than each having its own convention to remember.

> **Note:** A Set/Reset with *no* Reset comparisons never clears once it has set; validation reports one. That is the honest outcome rather than a helpful guess, and it is why a Reset the device cannot evaluate — a comparison pointing at a channel that is no longer there — reads as "reset now" instead of being skipped. A latch stuck on with nothing able to clear it is the one outcome a reset-dominant design must not produce.

## Adding or changing a User Condition

The **User Condition** editor holds:
- **Mode :** — **Momentary** or **Set / Reset**. It decides which of the fields below have any effect. What the mode does not use is kept rather than cleared, so switching back and forth does not destroy what you typed.
- **Number of comparisons:** — 1, 2 or 3, counted for each expression separately. The extra comparison groups and the AND/OR selector between each pair appear as the count is raised.
- **Set** — the comparisons that drive the output to 1. Both modes use them.
- **Reset** — the comparisons that drive it back to 0. Set/Reset only.
- **Comparison 1** … **Comparison 3** — each compares **Input A:** (a channel, picked with **Select…**) against an operator (=, ≠, &lt;, ≤, &gt;, ≥) and a right-hand side that is either a **Channel:** (picked with **Select…**) or a **Constant:** (a fixed value, up to 6 decimal places). The two message operators — **was received** and **was transmitted** — name a message instead and take no right-hand side at all; see [below](#messages).
- **AND** / **OR** — a selector between each pair of comparisons chooses how they join.
- **Latch Frequency :** — Momentary only: 1–100 Hz, the hold being one period of it.
- **Evaluates as:** — a live preview of the expression, bracketed exactly the way the device will evaluate it. It updates as you edit.
- **Output channel:** — the boolean output, picked with **Select…**. Because the User Condition *writes* this channel, the picker warns when something else already writes it. The channel is forced to Boolean — see [below](#boolean).
- **Active** — an inactive User Condition is kept but not evaluated.

OK refuses a comparison whose Input A is empty ("Select a channel for Input A for comparison 2."), a right-hand side set to Channel with no channel chosen, a message operator naming no message, and an empty output channel.

<a id="messages"></a>

## Comparing a channel, or naming a message

Two of the operators do not compare anything. **was received** and **was transmitted** take a **message** where the others take Input A, and have no right-hand side. Each is true only on the evaluation pass in which a frame for that message **actually happened** — arrived, or went out — and false on every other pass.

This is what makes request/response work, and it is what the modes were built for. **Set on Message Received, reset on Message Transmitted** is a Set/Reset condition that comes up the moment a request arrives, gates a [Triggered transmit message](communications.md#triggered) that sends the reply, and puts itself away again when that reply reaches the wire. One reply per request, written where you can read it.

These are *events*, not levels. "Is this message alive?" is a different question, and a receive message's **Receive Timeout** already answers it.

> **Warning:** **Receive and transmit are not symmetric, and the difference is visible.** A received frame is evaluated in the *same* pass that records it, so **every received frame is seen** and none can be missed. A transmitted frame is recorded by the 200 Hz transmit service, which does not evaluate conditions, so it is seen by the **next** pass — up to 10 ms later — and any transmissions of one message inside that window **collapse into a single event**. A 5 ms cyclic message manages two per window; a compound message in **Batch** mode emits every identifier in one service. A condition asking "did this go out" does not need a count, but a user counting frames with one would be surprised.

A message is named by its **bus and its name**, not by its position in the sections list. Renaming a message therefore **keeps the reference** — the rename carries the condition along with it — and reordering the list, removing a section or switching one to Off cannot silently re-point a condition at some other message. The cost is the other half of that bargain: two messages on one bus sharing a name are **ambiguous**, and that is reported rather than resolved by picking one.

<a id="migration"></a>

## Configurations written before the modes

Every User Condition in an older configuration is **migrated to Set/Reset** as the file loads, with its **Reset expression generated as the logical inverse of its Set**: every comparison flipped (= becomes ≠, &lt; becomes ≥, and so on) and every AND/OR flipped with it. Nothing needs re-entering and nothing stops working. A latch that sets whenever the expression holds and resets whenever it does not *is* a level, with extra steps — so the migrated condition behaves identically to the one you had.

It is written down here because you will see it. Open a migrated condition and there is a Reset expression you did not write, reading like the opposite of your Set, because that is exactly what it is. Left alone it keeps the condition behaving as it always did. Edit the Set without editing the Reset to match and the two stop being opposites — which is a real change, and usually a welcome one, but the output then *holds* in the gap between them instead of following.

> **Note:** The migration is keyed on the condition itself rather than on the file's version number: a condition with no mode recorded is a pre-modes condition. That makes it self-describing and impossible to apply twice. The next Save writes the file in the current form — see [Configuration Files (.ct3)](files.md).

<a id="boolean"></a>

## The output channel is Boolean

Every User Condition's output channel is **Boolean**. Its data type, its range (0–1) and its decimal places are set for you, and they are not yours to change in the [Channel Editor](channels.md)'s usual way: the Manager rewrites all three whenever conditions are edited, whenever a file is opened, and whenever a configuration is read back from a device.

This is not a restriction on what a User Condition can do. A condition has only ever written 1 or 0 — the engine assigns the literals and no setting anywhere changed that — so declaring the channel Boolean is the document finally saying what was already true. What it ends is the other half of that arrangement: an output declared float, or s32, or (very commonly, in files written before channels carried types at all) nothing at all, carrying a value that was never anything but 0 or 1, and handing the device the float ±1e9 span as its clamp.

> **Note:** On the wire, Boolean and u8 are the same eight bits, and always were. That is why a channel read back from a device is re-typed from the condition table rather than from the signal record: the record reads back as u8 for every condition output — or, from an older unit, as nothing at all — while the condition table says without ambiguity which channels a condition writes. A Get would otherwise un-type every condition output in the document.

## How multiple comparisons combine

Each expression folds its own comparisons **strictly left to right**: with three comparisons the result is **((first JOIN₁ second) JOIN₂ third)** — the first two are evaluated together, and that result is combined with the third. Set and Reset fold independently and share nothing but the output. This is deliberately *not* C-style precedence where AND binds tighter than OR:

<table>
<tr><th>You configure</th><th>Evaluates as</th><th>Not as</th></tr>
<tr><td>A OR B AND C</td><td>(A OR B) AND C</td><td>A OR (B AND C)</td></tr>
<tr><td>A AND B OR C</td><td>(A AND B) OR C</td><td>—</td></tr>
</table>

The editor's **Evaluates as:** preview, the User Conditions list and the Config Summary all print this exact bracketing, so what you read is always what the device computes. Every comparison is evaluated on every pass (there is no short-circuiting), so a condition costs the same each time regardless of the outcome.

> **Note:** The boolean convention is shared across the whole device: true = value &gt; 0. Anything that produces 1/0 — another User Condition, a [math](math-channels.md) comparison, a boolean channel decoded from a message — can be an input, and a comparison like `Launch Armed = 1` tests it directly.

## Evaluation

User Conditions are evaluated after [constants](constants.md), [lookup tables](tables.md) and [math channels](math-channels.md) in each pass (100 Hz, and again on message receive), so a condition comparing a math result sees the value computed in the same pass. A math row that reads a condition's output sees the value from the previous pass — the usual one-pass lag for a reference that points later in the evaluation order. The full order, and every case of the lag, is on [Order &amp; Timing of Operations](engine.md).

**Conditions carry memory.** Each one holds its latched output, the previous pass's Set expression (that is what an edge is detected against) and, in Momentary, the milliseconds of hold still to run. This is new: a condition used to be a pure function of its inputs, so two passes with identical channel values produced identical outputs. They no longer do — a Set/Reset is holding what it was last driven to, and a Momentary is counting down. Three consequences are worth knowing:
- **Nothing is persisted.** A power cycle, and a configuration being sent, re-arm every condition: outputs start at 0, no edge is remembered and no hold is running. Unlike a [counter](counters.md) or an [integrator](integrators.md), a condition has no "preserve" option — there is no accumulated value to keep, only a latch that the inputs will set again if they still say so.
- **A received frame is an instant, not a duration.** The extra evaluation a frame triggers can change what the expressions say and can set or reset a latch, but it does not advance a Momentary hold — the hold is spent on the 10 ms pass only, so a busy bus cannot shorten a pulse.
- **An edge is the transition, not the level.** A Momentary whose Set expression simply stays true does not retrigger; it has to go false and true again. This is the same convention counters and timers use for their inputs.

## See also

[Math Channels](math-channels.md) · [Constants](constants.md) · [Lookup Tables](tables.md) · [Up/Down Counters](counters.md) · [Timers](timers.md) · [Triggered transmit](communications.md#triggered) · [Order &amp; Timing of Operations](engine.md) · [Monitoring Live Values](monitor.md)
