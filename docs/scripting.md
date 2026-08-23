# Lua Scripting

**Tools → Lua Console…** runs Lua 5.4 scripts against the open document. Anything you would do four hundred times by hand — generating a block of channels, renaming across a whole configuration, building a message from a spreadsheet-shaped table — is a short loop here.

> **Note:** This page is about scripting the *document*, on your PC. For a script that runs *on the unit* — reading and writing channels 100 times a second — see [Device Scripts](device-scripts.md). The two share a language and nothing else: nothing the Lua Console does reaches a device, and a device script cannot edit a configuration.

## The safety rules
- **All or nothing.** If a script fails — an error, a typo three loops in, the 10-second time limit — every change it made is rolled back. There is no "it died halfway and now 400 of 900 channels are renamed".
- **Scripts cannot touch your machine.** No files, no processes, no environment, no network. A script shared by someone else can waste ten seconds of your time and nothing else. (`os.time`, `os.clock`, `os.date` and `os.difftime` work; the rest of `os` and all of `io` do not exist.)
- **Message markings are honoured.** A concealed (**Hidden** or **Protect Communication**) message lists as its name and level and nothing more, and reading its detail is an error until it is revealed. *Changing* one — or a channel it carries — is an error at every level, **Read Only** included, because the edit lock is not lifted by revealing. *Removing* a section or a channel is allowed at every level, exactly as it is everywhere else in the program.
- **The document is only marked changed when a script changes it.** A read-only report script leaves the file exactly as it was.

## First script
```
for i = 1, 16 do
    ct.addChannel{ name = ('Cell Volt %02d'):format(i),
                   quantity = 'Voltage', unit = 'V',
                   dataType = 'u16', baseResolution = 0.001,
                   decimalPlaces = 3, minValue = 0, maxValue = 5 }
end
print(ct.channelCount() .. ' channels now')
```
Run with **Ctrl+Return**. `print()` goes to the output pane. Scripts save as `.lua` files under `Documents\CAN Triple Device Manager\Scripts`.

## The ct API

### Document

<table>
<tr><th>call</th><th>meaning</th></tr>
<tr><td><code>ct.title()</code> / <code>ct.setTitle(s)</code></td>
<td>The configuration title (32 bytes max).</td></tr>
<tr><td><code>ct.comments()</code> / <code>ct.setComments(s)</code></td>
<td>The document's free-text comments.</td></tr>
<tr><td><code>ct.validate()</code></td>
<td>Runs Check Channels; returns an array of
<code>{severity, location, message}</code> with severity
<code>"error"</code>, <code>"warning"</code> or <code>"info"</code>.</td></tr>
</table>

### Channels

<table>
<tr><th>call</th><th>meaning</th></tr>
<tr><td><code>ct.channels()</code></td><td>Array of channel tables (snapshots).</td></tr>
<tr><td><code>ct.channel(name)</code></td><td>One channel, or <code>nil</code>.</td></tr>
<tr><td><code>ct.channelCount()</code></td><td>How many user channels exist.</td></tr>
<tr><td><code>ct.addChannel{...}</code></td>
<td>Creates a channel. <code>name</code> required (31 bytes max); optional
<code>quantity</code>, <code>unit</code>, <code>dataType</code>
(u8/u16/u32/s8/s16/s32/float/boolean), <code>baseResolution</code>,
<code>decimalPlaces</code>, <code>minValue</code>, <code>maxValue</code>,
<code>category</code>. Errors if the name exists.</td></tr>
<tr><td><code>ct.setChannel{...}</code></td><td>Add-or-update by <code>name</code>.</td></tr>
<tr><td><code>ct.removeChannel(name)</code></td><td>Removes it.</td></tr>
<tr><td><code>ct.renameChannel(old, new)</code></td>
<td>Renames AND updates every reference — comms rows, math, User Conditions.
Returns the number of references updated.</td></tr>
<tr><td><code>ct.allocatedChannels()</code> /
<code>ct.generatedChannels()</code> / <code>ct.protectedChannels()</code></td>
<td>Name lists, as the Channel Editor classifies them.
<code>ct.protectedChannels()</code> lists the channels that are <b>not
editable</b> — every marked message's channels, <b>Read Only</b> included. That
is the list a script needs before it tries to change one. The channels whose
detail is currently <em>withheld</em> is a different and smaller set; read
<code>concealed</code> on the sections instead.</td></tr>
</table>

### Buses and messages

<table>
<tr><th>call</th><th>meaning</th></tr>
<tr><td><code>ct.bus(n)</code> / <code>ct.setBus(n, {...})</code></td>
<td>Bus 1..3: <code>enabled</code>, <code>rateKbps</code>,
<code>dataRateKbps</code>, <code>termination</code>.</td></tr>
<tr><td><code>ct.sections(n)</code></td>
<td>The bus's message sections. Every entry carries
<code>protection</code> = <code>"none"|"readOnly"|"hidden"|"protected"</code>,
<code>concealed</code> (may its detail be read?) and <code>editLocked</code>
(may it be changed?) — two separate questions, and they differ for
<code>"readOnly"</code>. A concealed section appears as
<code>{name, protection, concealed=true, editLocked=true}</code> and nothing
else; a Read Only one returns its full scalars, <code>id</code>
included.</td></tr>
<tr><td><code>ct.section(n, name)</code></td>
<td>Full detail including <code>rows</code> (or <code>identifiers</code> for
compound sections). Errors if concealed; a Read Only section returns
normally.</td></tr>
<tr><td><code>ct.addSection(n, {...})</code></td>
<td><code>name</code> required; <code>device</code> =
<code>"receive"|"transmit"|"relay"|"off"</code>, <code>id</code>,
<code>extended</code>, <code>fd</code>, <code>lengthBytes</code>,
<code>alignment</code> = <code>"normal"|"wordswap"</code>,
<code>rateHz</code>/<code>cyclic</code> (transmit),
<code>timeoutMs</code> (receive), and <code>rows</code> — an array of
<code>{channel, startBit, bitLength, dbcType, factor, offset, default,
clampToRange}</code> with <code>dbcType</code> =
<code>"unsigned"|"signed"|"float"</code>. <code>clampToRange</code> defaults to
<code>true</code>; <code>false</code> makes a transmit row send the low
<code>bitLength</code> bits and roll over.</td></tr>
<tr><td><code>ct.setSection(n, name, {...})</code></td>
<td>Updates fields; include <code>name</code> to rename; include
<code>rows</code> to replace the rows.</td></tr>
<tr><td><code>ct.removeSection(n, name)</code></td><td>Removes it. Allowed at
every protection level, concealed or not — removal is permitted everywhere in
the program, and this is no exception.</td></tr>
</table>

A section's `protection` is deliberately *not* settable from a script, in either direction. That is mechanism rather than policy: changing it is authorised by a password prompt, and a batch run has nowhere to put one. Raising is refused along with lowering, so a script cannot conceal a message from the operator running it. Passing the value a section already has is fine, so an ordinary read-modify-write does not trip over a field it never meant to touch.

### Calculations

<table>
<tr><th>call</th><th>meaning</th></tr>
<tr><td><code>ct.constants()</code> / <code>ct.addConstant{...}</code> /
<code>ct.removeConstant(name)</code></td>
<td><code>{name, dataType, decimalPlaces, value, active}</code>.</td></tr>
<tr><td><code>ct.mathRows()</code> / <code>ct.addMath{...}</code> /
<code>ct.removeMath(i)</code></td>
<td>Operands <code>a</code>, <code>b</code>, <code>c</code> are a channel name
(string) or a constant (number); <code>dest</code> is the output channel;
<code>op</code> is <code>ct.ops.NAME</code> or the name string. Math rows have
no name, so removal is by 1-based index.</td></tr>
<tr><td><code>ct.ops</code></td>
<td>Every math operation by name: <code>ADD SUB MUL DIV SCALE MIN MAX AND OR
ABS NEG SQRT FLOOR CEIL ROUND MOD XOR LAND LOR LNOT GT GE LT LE EQ NE MULADD
CLAMP LERP SELECT WRAP</code>.</td></tr>
<tr><td><code>ct.conditions()</code>, <code>ct.counters()</code>,
<code>ct.timers()</code>, <code>ct.integrators()</code>,
<code>ct.tables2x16()</code>, <code>ct.tables8x8()</code></td>
<td>Read-only summaries (phase 1). Create these in their dialogs.</td></tr>
</table>

## A worked example
```
-- A receive message and the maths to go with it, in one script.
ct.addChannel{ name = 'Raw Pressure', dataType = 'u16' }
ct.addChannel{ name = 'Pressure kPa', dataType = 'float', decimalPlaces = 1 }

ct.addSection(1, {
    name = 'Sensor Node', device = 'receive', id = 0x310, lengthBytes = 8,
    rows = { { channel = 'Raw Pressure', startBit = 0, bitLength = 16,
               dbcType = 'unsigned' } },
})

-- kPa = raw * 0.75 + 12.5
ct.addMath{ op = ct.ops.MULADD, a = 'Raw Pressure', b = 0.75, c = 12.5,
            dest = 'Pressure kPa' }

-- And prove the document still checks out before anyone sends it.
for _, issue in ipairs(ct.validate()) do
    print(issue.severity, issue.location, issue.message)
end
```
## Limits worth knowing

These are limits on **this program, on your PC**. A console script never reaches a device, so none of them describes the unit or its memory — for what a script running *on the device* is allowed, see [Device Scripts](device-scripts.md).
- A console script runs for at most **10 seconds** and may allocate at most **64 MB of Lua memory**. Both are generous for what this is for: generating hundreds of channels takes milliseconds and a fraction of a megabyte. They exist to stop a runaway loop hanging the program, not to ration anything.
- The interface freezes while a script runs — by design, so nothing can interleave with a script that is halfway through changing the document.
- Scripts see the open document only. They cannot open, save or send configurations, and they cannot talk to a device.
- Reads are snapshots: a table from `ct.channels()` does not change when the document does. Re-read after writing.

> **Note:** The device is far smaller than these numbers might suggest — it has 128 KB of RAM in total — which is exactly why a device script is a different thing with different limits, measured in instructions and microseconds rather than seconds and megabytes.

## See also
- [Device Scripts](device-scripts.md) — the other kind: code that runs *on the unit*, 100 times a second. Shares a language with this page and nothing else.
- [Channels](channels.md)
- [Communications: Messages &amp; Sections](communications.md)
- [Math Channels](math-channels.md)
