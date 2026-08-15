# Lookup Tables

A lookup table maps one or two input channels onto an output channel through a set of calibration points ("sites"). The device offers two kinds, up to **8 of each**:

<table>
<tr><th>Type</th><th>Axes</th><th>Sites</th><th>Typical use</th></tr>
<tr><td>2x16</td><td>1 input axis</td><td>up to 16</td><td>Sensor linearisation,
a 1-D calibration curve.</td></tr>
<tr><td>8x8</td><td>X and Y inputs</td><td>up to 8 × 8</td><td>A 2-D map — output
as a function of two channels, 64 cells.</td></tr>
</table>

> **Note:** The 8x8 replaced the earlier **4x4** table, which held 4 sites per axis and 16 cells. Configurations saved with 4x4 tables still open: each one loads into the top-left of an 8x8, keeping its sites, its cells and its interpolation modes, and simply leaves the extra sites unused. Nothing needs re-entering, and re-saving writes the file in the new form.

Tables are evaluated right after [constants](constants.md) in every pass, before [math](math-channels.md) and [conditions](conditions.md), so a table output can feed a math channel or a condition in the same pass. The output is a generated channel typed like a [constant](constants.md) (name, data type, decimal places) and registered in the document, so it can be picked anywhere a channel can.

## The Tables dialog

To add a table, choose **Calculations → Tables…**. The dialog lists every table with the columns **#**, **Type**, **Output**, **Axes** and **Active**, with **Add 2x16…**, **Add 8x8…**, **Change…** and **Remove** buttons at the right. Double-clicking a row opens it for change. OK commits the changes; Cancel discards them. A new table's output channel is pre-named "Table N"; adding beyond the limit reports "The device supports at most 8 2x16 tables" (or 8x8 tables).

## Editing a 2x16 table

The **2x16 Table** editor holds:
- **Output Channel** — **Channel Name:** (up to 31 characters — the device stores a 32-byte label), **Data Type:** and **Decimal Places:**, exactly as for a [constant](constants.md). The type and decimals set the range and precision the output cells accept.
- **Input Axis** — **Input:** plus **Select…** picks the channel the table reads, and **Interpolated** / **Discrete (centered)** chooses how values between sites are resolved (see below). The axis cells take the picked channel's range and decimal places.
- **Sites and Output Values** — a two-row grid, **Axis** above **Output**, with 16 numbered columns. As the dialog itself says: "Fill only the sites you need (left to right, up to 16) — they auto-sort ascending. Each site needs an output; Delete clears a cell. Values are limited to their channel's range/decimals."

Editing an axis cell re-sorts the sites ascending automatically, carrying each site's output value with it, so you can type calibration points in any order. Typing an axis value that already exists on the axis clears the cell — duplicate sites are not allowed. Delete or Backspace clears the selected cells.

OK checks that the sites are filled contiguously from the left, that an axis channel is selected, that every filled site has an output value, and that the axis strictly ascends.

## Editing an 8x8 table

The **8x8 Table** editor has the same **Output Channel** group, then an **X Axis** and a **Y Axis** group — each with its own **Input:** channel and its own **Interpolated** / **Discrete (centered)** choice. Each group's title shows the axis and its chosen channel (for example "X Axis — Manifold Pressure"), so which axis is which is readable at a glance. The **Sites and Output Grid** is a 9×9 grid: the "Y ↓ \ X →" corner label, the X sites across the top row, the Y sites down the left column, and the inner 8×8 cells holding the output for each X/Y combination. As the dialog says: "Fill only the X/Y sites you need (up to 8 per axis) — they auto-sort ascending. Every cell in the filled grid needs an output; Delete clears a cell. Values are limited to their channel's range/decimals."

The site band — the top row, the left column and the corner — is **shaded** so the axis inputs read apart from the output cells they index. (The 2x16 grid shades its **Axis** row the same way.) The shade is derived from your desktop's colours, so it works on a light or a dark theme, and the selection highlight still shows through it on whichever cell is current.

The grid's columns are a fixed width and scroll rather than stretching, and the editor body scrolls as a whole, so the OK and Cancel buttons stay reachable on a small or scaled display.

## Defining an axis in the Axis Setup window

Each axis group has an **Edit Axis…** button that opens a dedicated **Axis Setup** window for building that axis's breakpoints — handier than typing them across the grid's header band when there are several to lay out. It holds the axis's **Input** channel (with **Select…**), its **Axis Behaviour** (Continuous, i.e. interpolated, or Discrete, i.e. centered), and a row of value cells with the channel's unit and the maximum count shown above them. Four tools build the breakpoints quickly:
- **Insert** — add one breakpoint after the last, spaced by the gap between the previous two.
- **Delete** — remove the selected breakpoints.
- **Linearise** — space every breakpoint evenly between the current first and last, keeping the ends fixed.
- **Generate…** — fill the axis with a chosen count of evenly spaced values between a **From** and a **To**.

Breakpoints ascend left to right — they auto-sort, and a duplicate is dropped. **OK** writes the axis back into the grid (shrinking an axis clears the cells that fall outside the new site band); **Cancel** leaves the grid untouched. Typing the sites directly into the grid's header row/column still works exactly as before — the window is an addition, not a replacement.

## Copying and pasting cells

Both table grids (and the Axis Setup value row) support spreadsheet-style clipboard editing over a cell or a block of cells:
- **Ctrl+C** copies the selection, **Ctrl+X** cuts it, **Ctrl+V** pastes at the top-left of the selection (or the current cell).
- The exchange format is tab-between-columns, newline-between-rows text — the same shape Excel and Google Sheets use — so a block of values round-trips to and from a spreadsheet.
- A single copied value pasted onto a multi-cell selection fills the whole selection.
- Pasted values are clamped to each destination cell's range and decimals; non-editable cells (the corner label) are skipped. **Delete** or **Backspace** clears the selection.

Each axis sorts independently: editing an X site re-orders the columns (carrying their output cells), editing a Y site re-orders the rows. A duplicate value on the same axis is cleared. OK checks that X sites are filled contiguously from the left and Y sites from the top, that both axis channels are selected, that every cell in the filled rectangle has an output, and that both axes strictly ascend.

## Interpolated vs. Discrete (centered)

Each axis resolves the live input independently, in one of two modes:
- **Interpolated** — linear interpolation between the two sites that bracket the input. Use this for continuous calibrations (sensor curves, compensation maps).
- **Discrete (centered)** — the output snaps to the *nearest* site, with the transition at the midpoint between adjacent sites. Use this to map an input onto a set of steps — a rotary switch position, a gear number.

In an 8x8 table the two axes may mix modes freely; the output blends bilinearly over the interpolated axes while a discrete axis contributes its nearest row or column. On both table types the input is **clamped to the end sites**: below the first site the first output applies, above the last site the last output applies — a table never extrapolates. On a partly filled table "the end sites" means the last site you filled, not the eighth column: an 8x8 using three X sites clamps at the third, and the unused cells are never read.

## Output channel name rules
- The name must not be empty and must fit the 31-byte device label budget.
- "The output channel must differ from the axis input." — a table cannot write the channel it reads.
- No two tables may output to the same channel: "A table already outputs to "X"."
- The name may not collide with an unrelated existing channel: "A channel named "X" already exists. Choose a different name."

> **Note:** Renaming a table's output channel carries references along — anything reading the old name is rewritten when the Tables dialog is closed with OK. A table axis may even be another table's output; the one-pass evaluation rule from [Math Channels](math-channels.md) applies to such chains.

> **Warning:** Tables may be partial — only the populated sites are sent to the device — but a site without an output value, or a gap in the middle of an axis, is refused when the editor is closed. If a value seems to vanish as you type it, check that it does not duplicate an existing site on that axis.

## See also

[Math Channels](math-channels.md) · [Conditions](conditions.md) · [Constants](constants.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md)
