# Math Channels

A math channel computes one operation over up to three inputs (A, B and C) and writes the result into an output channel on every evaluation pass. The device supports up to **100** math channels. Inputs can be channels or fixed values; the output is an ordinary generated channel, so it can feed another math channel, a [condition](conditions.md), a [table](tables.md) axis, a transmit message, or be watched in [Monitor Channels](monitor.md).

## The Math Channels dialog

To add a math channel, choose **Calculations → Math Channels…**. The dialog lists every row with the columns **#**, **Active**, **Operation**, **Input A**, **Input B**, **Input C** and **Output Channel**, with **Add…**, **Change…** and **Remove** buttons at the right. Double-clicking a row also opens it for change. Operands an operation does not read are shown blank, so each row reads like the expression it computes. Changes are committed when the dialog is closed with OK; Cancel discards them.

> **Note:** Adding beyond the limit reports "The device supports at most 100 math channels."

## Adding or changing a row

The **Math Channel** editor holds:
- **Operation:** — the operation, from the table below.
- **Input A**, **Input B**, **Input C** — one group per operand, each a **Channel** (picked with **Select…**) or a **Constant** (a fixed value, up to 4 decimal places). Only the operands the operation reads are shown: a single-input operation shows **Input A** alone, a two-input operation adds **Input B**, and a three-operand operation adds **Input C**. The groups appear and disappear as the operation changes.
- **Output Channel:** — the channel the result is written to, picked with **Select…**. Because this *writes* the channel, the picker warns when something else already writes it (two writers overwrite each other).
- **Active** — an inactive row is kept in the configuration but not evaluated.

OK refuses an input set to Channel with no channel chosen ("Please select a channel for Input A.") and an empty output ("Please select an output channel."). Operands the operation does not read are stored as defaults, so the row reads back from the device exactly as it was sent.

## Operations

The operation list, in the editor's order. The number is the operation's index on the wire; *Inputs* is how many operands it reads.

<table>
<tr><th>#</th><th>Operation</th><th>Inputs</th><th>Result</th></tr>
<tr><td>0</td><td>A + B</td><td>2</td><td>Sum.</td></tr>
<tr><td>1</td><td>A − B</td><td>2</td><td>Difference.</td></tr>
<tr><td>2</td><td>A × B</td><td>2</td><td>Product.</td></tr>
<tr><td>3</td><td>A ÷ B</td><td>2</td><td>Quotient; B = 0 gives 0.</td></tr>
<tr><td>4</td><td>Scale (A × B)</td><td>2</td><td>Identical to A × B — a
compatibility alias kept from earlier firmware. For scale <i>with</i> offset,
use Multiply-add (op 26).</td></tr>
<tr><td>5</td><td>Min(A, B)</td><td>2</td><td>The smaller of A and B.</td></tr>
<tr><td>6</td><td>Max(A, B)</td><td>2</td><td>The larger of A and B.</td></tr>
<tr><td>7</td><td>A AND B</td><td>2</td><td>Bitwise AND of A and B as integers
(see the bitwise note below).</td></tr>
<tr><td>8</td><td>A OR B</td><td>2</td><td>Bitwise OR of A and B as integers.</td></tr>
<tr><td>9</td><td>Absolute (|A|)</td><td>1</td><td>Magnitude of A.</td></tr>
<tr><td>10</td><td>Negate (−A)</td><td>1</td><td>Sign-flipped A.</td></tr>
<tr><td>11</td><td>Square root (√A)</td><td>1</td><td>√A; negative A gives 0.</td></tr>
<tr><td>12</td><td>Floor (A)</td><td>1</td><td>Largest integer ≤ A.</td></tr>
<tr><td>13</td><td>Ceiling (A)</td><td>1</td><td>Smallest integer ≥ A.</td></tr>
<tr><td>14</td><td>Round (A)</td><td>1</td><td>A rounded to the nearest integer.</td></tr>
<tr><td>15</td><td>Modulo (A mod B)</td><td>2</td><td>Floating-point remainder of
A ÷ B; B = 0 gives 0.</td></tr>
<tr><td>16</td><td>XOR (bitwise)</td><td>2</td><td>Bitwise exclusive-OR of A and B
as integers.</td></tr>
<tr><td>17</td><td>Logical AND (A and B)</td><td>2</td><td>1 when A &gt; 0 and
B &gt; 0, else 0.</td></tr>
<tr><td>18</td><td>Logical OR (A or B)</td><td>2</td><td>1 when A &gt; 0 or
B &gt; 0, else 0.</td></tr>
<tr><td>19</td><td>Logical NOT (A)</td><td>1</td><td>1 when A ≤ 0, else 0.</td></tr>
<tr><td>20</td><td>A &gt; B</td><td>2</td><td>1 when true, else 0.</td></tr>
<tr><td>21</td><td>A ≥ B</td><td>2</td><td>1 when true, else 0.</td></tr>
<tr><td>22</td><td>A &lt; B</td><td>2</td><td>1 when true, else 0.</td></tr>
<tr><td>23</td><td>A ≤ B</td><td>2</td><td>1 when true, else 0.</td></tr>
<tr><td>24</td><td>A = B</td><td>2</td><td>1 when exactly equal, else 0 (exact
float compare — see the warning below).</td></tr>
<tr><td>25</td><td>A ≠ B</td><td>2</td><td>1 when not exactly equal, else 0.</td></tr>
<tr><td>26</td><td>Multiply-add (A × B + C)</td><td>3</td><td>A × B + C — scale
with offset in one row.</td></tr>
<tr><td>27</td><td>Clamp (A between B and C)</td><td>3</td><td>A limited to the
range B (low) … C (high). If C ≤ B, clamping is disabled and A passes through
unchanged.</td></tr>
<tr><td>28</td><td>Interpolate (A to B by C)</td><td>3</td><td>A + (B − A) × C —
C = 0 gives A, C = 1 gives B, values between blend linearly.</td></tr>
<tr><td>29</td><td>Select (A ? B : C)</td><td>3</td><td>B while A &gt; 0,
else C.</td></tr>
<tr><td>30</td><td>Wrap (A into B..C)</td><td>3</td><td>A wrapped into the range
[B, C) — the result is always ≥ B and &lt; C (an angle wrapped into 0..360, for
example). If C ≤ B, wrapping is disabled.</td></tr>
</table>

## Conventions and guards
- **Boolean convention: true = value &gt; 0.** The logical operations and Select test their inputs against 0, and every comparison and logical operation produces exactly 1 or 0. This is the same convention [conditions](conditions.md), [counters](counters.md) and [timers](timers.md) use, so their outputs interoperate freely.
- **Division guards:** A ÷ B and Modulo (A mod B) give 0 when B is 0, and Square root (√A) gives 0 for negative A — a row never produces infinity or an error from these.
- **Bitwise operations** (A AND B, A OR B, XOR) convert both inputs to 32-bit signed integers first, saturating at the integer range, apply the bit operation, and convert the result back.
- **NaN in comparisons:** a comparison with a NaN operand takes the false branch (IEEE behaviour), so Logical NOT (A) of NaN is 1.

> **Warning:** A = B and A ≠ B are **exact** float comparisons with no tolerance. They are dependable against integers and against a value that was stored and read back unchanged, but two independently *computed* values rarely compare exactly equal. To test "close to", compare against a band instead — for example A ≥ B one row, A ≤ C the next, combined with Logical AND, or use a [condition](conditions.md) with two comparisons.

## Evaluation order and chaining

The device evaluates the configuration in a fixed pass: [constants](constants.md) first, then [lookup tables](tables.md), then the math rows in list order, then [conditions](conditions.md). The pass runs at 100 Hz and again when a received frame updates channels.

Within the math list, rows run top to bottom in a single pass. A row that reads the output of a row **above** it sees that row's fresh result in the same pass, so a chain of calculations laid out in order settles immediately. A row that reads the output of a row **below** it sees the value from the *previous* pass — a one-pass lag. Order dependent chains accordingly: put the producer above the consumer.

> **Note:** The same one-pass rule applies across features: a math row reading a condition, counter, timer or integrator output sees the value those produced on their last evaluation.

## See also

[Order &amp; Timing of Operations](engine.md) · [Conditions](conditions.md) · [Constants](constants.md) · [Lookup Tables](tables.md) · [Up/Down Counters](counters.md) · [Integrators](integrators.md) · [Device Scripts](device-scripts.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md)
