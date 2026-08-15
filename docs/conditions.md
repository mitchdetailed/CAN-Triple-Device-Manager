# Conditions

A condition is a boolean logic channel: it evaluates up to three comparisons and drives its output channel **true (1)** while the expression holds, **false (0)** otherwise. The device supports up to **100** conditions. The output is an ordinary generated channel, so it can trigger a [counter](counters.md) or [timer](timers.md), gate an [integrator](integrators.md), feed a [math channel](math-channels.md) (for example as the A input of Select), be transmitted, or be watched in [Monitor Channels](monitor.md).

## The Conditions dialog

To add a condition, choose **Calculations → Conditions…**. The dialog lists every row with the columns **#**, **Active**, **Condition** and **Output channel**, with **Add…**, **Change…** and **Remove** buttons at the right. The **Condition** column shows the full expression exactly as the device evaluates it, brackets included. Double-clicking a row opens it for change. OK commits the changes; Cancel discards them.

> **Note:** Adding beyond the limit reports "The device supports at most 100 conditions."

## Adding or changing a condition

The **Condition** editor holds:
- **Number of comparisons:** — 1, 2 or 3. The extra comparison groups and the AND/OR selector between each pair appear as the count is raised.
- **Comparison 1** … **Comparison 3** — each compares **Input A:** (a channel, picked with **Select…**) against an operator (=, ≠, &lt;, ≤, &gt;, ≥) and a right-hand side that is either a **Channel:** (picked with **Select…**) or a **Constant:** (a fixed value, up to 6 decimal places).
- **AND** / **OR** — a selector between each pair of comparisons chooses how they join.
- **Evaluates as:** — a live preview of the whole expression, bracketed exactly the way the device will evaluate it. It updates as you edit.
- **Output channel:** — the boolean output, picked with **Select…**. Because the condition *writes* this channel, the picker warns when something else already writes it.
- **Active** — an inactive condition is kept but not evaluated.

OK refuses a comparison whose Input A is empty ("Select a channel for Input A for comparison 2."), a right-hand side set to Channel with no channel chosen, and an empty output channel.

## How multiple comparisons combine

Comparisons are folded **strictly left to right**: with three comparisons the result is **((first JOIN₁ second) JOIN₂ third)** — the first two are evaluated together, and that result is combined with the third. This is deliberately *not* C-style precedence where AND binds tighter than OR:

<table>
<tr><th>You configure</th><th>Evaluates as</th><th>Not as</th></tr>
<tr><td>A OR B AND C</td><td>(A OR B) AND C</td><td>A OR (B AND C)</td></tr>
<tr><td>A AND B OR C</td><td>(A AND B) OR C</td><td>—</td></tr>
</table>

The editor's **Evaluates as:** preview, the Conditions list and the Config Summary all print this exact bracketing, so what you read is always what the device computes. Every comparison is evaluated on every pass (there is no short-circuiting), so a condition costs the same each time regardless of the outcome.

> **Note:** The boolean convention is shared across the whole device: true = value &gt; 0. Anything that produces 1/0 — another condition, a [math](math-channels.md) comparison, a boolean channel decoded from a message — can be an input, and a comparison like `Launch Armed = 1` tests it directly.

## Evaluation

Conditions are evaluated after [constants](constants.md), [lookup tables](tables.md) and [math channels](math-channels.md) in each pass (100 Hz, and again on message receive), so a condition comparing a math result sees the value computed in the same pass. A math row that reads a condition's output sees the value from the previous pass — the usual one-pass lag for a reference that points later in the evaluation order. The full order, and every case of the lag, is on [Order &amp; Timing of Operations](engine.md).

## See also

[Math Channels](math-channels.md) · [Constants](constants.md) · [Lookup Tables](tables.md) · [Up/Down Counters](counters.md) · [Timers](timers.md) · [Monitoring Live Values](monitor.md)
