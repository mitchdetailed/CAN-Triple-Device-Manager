# Device Scripts

A **device script** is a small program that runs *on the CAN Triple*, 100 times a second, alongside everything else the configuration does. It reads channels, does arithmetic, remembers values between ticks, and writes channels back. Where a [math channel](math-channels.md) is one expression and a [User Condition](conditions.md) is one comparison, a script is the place for logic that needs several steps and a memory — a state machine, a hysteresis band, a debounce, a staged fan curve.

> **Note:** A device script is **not** the [Lua Console](scripting.md). The console runs on your PC and edits the document in front of you; nothing it does reaches a unit. A device script is part of the configuration and runs on the hardware. They share a language and nothing else.

## Writing one

Choose **Calculations → Device Script…**. The left half is the editor; the right half is the simulator. Press **Compile** (or Ctrl+Enter) to check the script, then **Step** or **Run** to watch it work.
```
-- Persistent values, declared at file scope. They survive across ticks.
local hold = state(0)

function on_tick()
    local temp = sig("Coolant Temp")

    -- Turn the fan on at 96, off at 90 — a 6-degree band, so it does not
    -- chatter around a single threshold.
    if temp > 96 then
        hold = 1
    elseif temp < 90 then
        hold = 0
    end

    setSig("Fan Request", hold)
end
```
`sig("name")` reads a channel and `setSig("name", value)` writes one. Channels are addressed by **name**, exactly as they appear everywhere else in the program — naming a channel that does not exist is a compile error, not a silent read of nothing. `on_tick()` is the hook the device calls; a script without one is refused, since it could never run.

## The simulator

The panel on the right is not a model of the device — it *is* the device's script engine, the same code compiled into this program. A tick here and a tick on the unit run the same instructions over the same numbers, so a script that behaves correctly in the simulator behaves correctly on the hardware.
- **Channels** — type a number into any channel to feed it to the script. Channels the script *writes* are highlighted after a tick, so cause and effect are visible at a glance.
- **Persistent state** — the values declared with `state()`, as they stand after the last tick.
- **Step** runs one tick. **Run** runs the number of ticks in the box beside it — 100 ticks is one second of device time — and reports the peak cost and any fault.
- **Reset** puts it back to a fresh power-on: state cleared, initialisers re-run.

## The budget

The device gives every tick a fixed **budget**, and each instruction costs against it — more for the expensive operations (division, square root, `%`, `floor`, `clamp` and `wrap`). A script that exceeds its budget in a single tick is **suspended**: it stops running and does not run again until the configuration is reloaded, the rest of the configuration carries on untouched, and its persistent `state()` values are restored to what they were at the start of the tick. Channels the script had already written that tick keep the values it wrote — treat a suspended script's outputs as stale. The engine and the CAN buses never stall waiting for a script.

That is a safety net, not a design target. After **Run**, the figure under the state table shows the highest cost any of those ticks reached. After **Compile** it shows the straight-line cost with every loop taken zero times — a floor, not a bound. **Step** does not update it. A script sitting near the limit is one input change away from being suspended. If the simulator reports a fault, look for a loop whose end condition depends on something that can grow without bound.

For scale, measured on a device: the fan example above costs **23 of 2000 units — 37 microseconds, or 0.4% of a tick**. A script deliberately written to run away and be killed is stopped inside about a millisecond — a tenth of a tick — either by exhausting the budget or by hitting the device's cycle ceiling, whichever comes first. Ordinary logic does not come close to the limit; if yours does, it is looping.

Not every instruction costs the same, because on this processor floating-point work is done in software. A divide costs about three times an add, and `sqrt`, `floor`, `ceil`, `round`, `%`, `clamp` and `wrap` cost more still — so the budget charges them more. That is why replacing a divide with a multiply, or hoisting a `sqrt` out of a loop, buys more than it looks like it should.

## The two ways a script can be stopped

There are two limits, and they answer different questions. The simulator can show you the first; only a device can hit the second.

<table>
<tr><th>Reported as</th><th>What it means</th><th>What to do</th></tr>
<tr>
<td><b>Budget</b></td>
<td>The script did too much <em>work</em> in one tick. This is the limit the
simulator enforces too, so a script that trips it on the device trips it on your
desk as well.</td>
<td>Look for a loop whose end condition can grow without bound.</td>
</tr>
<tr>
<td><b>Overrun</b></td>
<td>The script took too long in real <em>time</em>, whatever the budget thought
it was spending. In practice this means <code>wrap</code>, <code>clamp</code> or
<code>%</code> applied across a very wide range of magnitudes — those are far
slower when the two numbers differ by many powers of ten.</td>
<td>Bring the operands closer in magnitude, or do the range reduction with
arithmetic you control.</td>
</tr>
</table>

> **Note:** The simulator cannot reproduce an **Overrun**, and that is the one place its answer is not the device's. It runs on a PC, where the same instructions take a different amount of time — so it tells you exactly what your script will *compute*, and the device tells you what it costs. Every value the simulator shows is the value the unit produces, bit for bit; only the timing is the device's business.

Either way the outcome is the same and nothing else is affected: the script stops and stays stopped until the configuration is reloaded, its `state()` values are put back, any channels it wrote that tick keep what it wrote, the rest of the configuration keeps running, and the reason is readable from the device.

## What the language has

Device scripts are a **subset of Lua**. The syntax is Lua's, checked by a real Lua parser, so an ordinary typo produces an ordinary Lua error message with a line number.

<table>
<tr><th>Available</th><th>Notes</th></tr>
<tr><td>Local variables, assignment</td><td><code>local x = 1</code></td></tr>
<tr><td>Arithmetic <code>+ - * / %</code></td><td>All values are numbers</td></tr>
<tr><td>Comparison <code>&lt; &lt;= &gt; &gt;= == ~=</code></td><td>Yield 1 or 0</td></tr>
<tr><td><code>and</code> <code>or</code> <code>not</code></td><td>Yield 1 or 0; they do <em>not</em> short-circuit</td></tr>
<tr><td><code>if</code> / <code>elseif</code> / <code>else</code></td><td></td></tr>
<tr><td><code>while</code>, numeric <code>for</code>, <code>break</code></td><td>A <code>for</code> step must be constant</td></tr>
<tr><td><code>abs min max floor ceil round sqrt</code></td><td></td></tr>
<tr><td><code>clamp(x,lo,hi)</code> <code>lerp(a,b,t)</code></td><td></td></tr>
<tr><td><code>select(c,a,b)</code> <code>wrap(x,lo,hi)</code></td><td><code>select</code> returns <code>a</code> when <code>c</code> is greater than zero — not merely non-zero</td></tr>
<tr><td><code>sig(name)</code> <code>setSig(name, v)</code></td><td>Channel read and write</td></tr>
<tr><td><code>state(initial)</code></td><td>File scope only; a value that survives ticks</td></tr>
</table>

Truth is numeric, and the two forms test it differently: `and`, `or` and `not` treat a value as true only when it is greater than zero, while `if` and `while` take any non-zero value as true. For `x = -1`, `if x then` takes the branch but `x and 1` yields 0.

Arithmetic never faults; an out-of-domain case produces a defined value instead: `a / 0` and `a % 0` yield 0, `sqrt` of a negative yields 0, `clamp(x, lo, hi)` returns `x` unchanged when `hi <= lo`, and `wrap(x, lo, hi)` returns a value in `[lo, hi)` — the high bound itself is never produced.

The language also accepts `neg(x)` (unary minus as a function), a bare `return`, `do`…`end` blocks, the literals `true`, `false` and `nil`, hexadecimal number literals, and a negative `for` step.

Fixed limits: at most **64** local variables, at most **64** `state()` values, and a compiled script of at most **1024 instructions** (8 KB of bytecode). Exceeding any of them is a compile error that names the limit.

Deliberately absent: tables, strings as values, closures, functions other than the hooks, varargs, `goto`, `repeat`, `#`, string concatenation, and the trigonometric and exponential functions (`sin`, `cos`, `exp`, `log`, `^`). Most produce a compile error that names the exclusion and says why; a few — tables, `goto`, `repeat`, and a function used as a value — are simply not part of the grammar, so the message is a plain parse error such as "expected a value" or "'repeat' is not declared".

> **Note:** The trigonometric functions are excluded for a specific reason. The IEEE-754 standard does not require them to be rounded identically on every machine, so the same script could differ by the smallest representable amount between your PC and the device — and a comparison sitting on that difference would branch the other way. Leaving them out is what lets the simulator's answer be trusted as the device's answer rather than an approximation of it.

## How a script is stored and sent

The script is part of the configuration. A script you wrote saves in the `.ct3` file as **source** and is compiled to bytecode when you send it; a script [read back off a device](#retrieved) has no source, so the file carries the compiled image instead. A document describes its script one way or the other, never both.
- **Send Configuration** compiles the script and writes it with everything else. A script that does not compile blocks the send, with the reason — the device is never left running an old script against new tables. A retained compiled image is sent exactly as it came back, and is refused before anything is erased if it has been damaged since.
- **Get Configuration** reads the device's stored script back like any other table.
- Sending a configuration with *no* script **removes** any script already on the device.

<a id="retrieved"></a>

## Reading a script back off a device

**Get Configuration** brings the script back with everything else — as **compiled bytecode**, which is what the device stores. Sending that document back puts the same script on a unit **byte for byte**. Nothing is re-compiled, re-numbered or re-encoded on the way through, so a script survives being read out of one unit and written into another unchanged.

What a Get cannot bring back is the **Lua**. Bytecode does not turn back into source — the names, the structure and the comments were gone the moment it was compiled. So a document read off a device holds a working script and no text for it, and the Script Editor says so rather than opening on an empty box:
- A banner at the top states what the document is carrying and that there is no source for it.
- The **Disassembly** tab lists the script as instructions — index, operation, operands, and the budget cost of each — with the instruction count measured against the device's ceiling of 1024. The line `on_tick` starts at is marked. It is a *view*: there is no way to edit it, and no assembler behind it.
- The editor itself is read-only, and **Compile**, **Load Script…** and **Save Script…** are off, because there is no source for any of them to work on.
- To write Lua instead, press **Replace With a Lua Script…**. It asks first, and it is the only thing in the dialog that discards the compiled image. Pressing OK on a document you have not deliberately taken over changes nothing.

> **Warning:** **Keep your source anyway.** A retrieved script can be sent back, listed and copied to another unit, but it can never be read as Lua or edited a line at a time — *only replaced whole*. The `.ct3` it was written in, or a `.lua` saved with **Save Script…**, is still the only copy you can change.

> **Note:** If a device answers with something that is not a valid script image — a mangled transfer, a truncated file — nothing is kept, and the Get says which check failed. That is deliberate: an unusable image stored in the document would be refused at the next Send, which happens *after* the device has been erased.

## Keeping scripts as files

The script lives in the configuration, but two buttons at the right of the editor's control row move it to and from a plain `.lua` file:
- **Save Script…** writes what is in the editor to a file. Use it to keep a library of scripts, to share one, or simply to have a copy that does not depend on the `.ct3`.
- **Load Script…** replaces what is in the editor with a file's contents, and compiles it straight away so any error shows up now rather than at Send. It asks first if that would discard unsaved changes.

Loading a script does *not* tie the document to that file — the script is copied in, and from then on it belongs to the configuration like any other setting. Saving again after editing is up to you.

## The worked examples

The program installs ten complete, working scripts in the **examples** folder beside it. Open any of them with **Load Script…**. Each one starts with a comment block saying what it does, which channels it expects, and which constants are meant to be tuned; the channel names are ordinary ones like "Coolant Temp", so adapt them to whatever your configuration actually calls things.

<table>
<tr><th>File</th><th>What it shows</th></tr>
<tr><td><code>fan-hysteresis.lua</code></td><td>A two-stage fan with separate on
and off thresholds, so a temperature sitting between them cannot chatter the
relay.</td></tr>
<tr><td><code>rolling-counter.lua</code></td><td>An alive counter that wraps
0-15, plus detecting a <em>received</em> counter that has stopped
changing.</td></tr>
<tr><td><code>crc8-can-message.lua</code></td><td>A CRC8 over an eight-byte
message. The interesting part is how it works without bitwise operators, and how
it splits the work across two ticks to stay inside the budget.</td></tr>
<tr><td><code>gear-from-ratio.lua</code></td><td>Gear from engine speed over road
speed, including guarding the division at a standstill.</td></tr>
<tr><td><code>smoothing-filter.lua</code></td><td>An exponential moving average
for a noisy sensor, seeded from the first reading so it does not ramp up from
zero at power-on.</td></tr>
<tr><td><code>slew-rate-limiter.lua</code></td><td>Limiting how fast a commanded
output may change, in units per second.</td></tr>
<tr><td><code>debounce-input.lua</code></td><td>Ignoring a switch until it has
held the same value for several ticks.</td></tr>
<tr><td><code>peak-hold-decay.lua</code></td><td>A peak that jumps instantly and
decays slowly, alongside a session peak that never decays.</td></tr>
<tr><td><code>latching-fault.lua</code></td><td>A warning that latches after a
condition persists, and clears only on a deliberate reset.</td></tr>
<tr><td><code>run-timer.lua</code></td><td>Accumulating running hours, and why
the total lives only in <code>state()</code>.</td></tr>
</table>

All ten compile and run inside the tick budget — that is checked automatically whenever the program is built, against the same compiler and the same virtual machine the device uses.

## See also
- [Math Channels](math-channels.md) — for a single expression
- [User Conditions](conditions.md) — for a single comparison
- [Tables](tables.md) — for a curve or a map
- [Lua Console](scripting.md) — scripting the *document*, on your PC
