# Constants

A constant creates a channel that always carries a fixed value. The device writes the value into the channel at the start of every evaluation pass — before [tables](tables.md), [math](math-channels.md) and [User Conditions](conditions.md) run — so anything downstream can read it: a calibration factor for a math channel, a threshold for a condition, a fixed field in a transmit message. The device supports up to **100** constants.

A constant is essentially a custom channel (name, data type, decimal places, with the range derived from the type) plus the value itself; it has no Channel Type or Display Units. Saving the dialog registers the channel in the document, so the constant is immediately selectable wherever a channel can be picked.

## The Constants dialog

To add a constant, choose **Calculations → Constants…**. The dialog lists every row with the columns **#**, **Active**, **Name**, **Data Type** and **Value**, with **Add…**, **Change…** and **Remove** buttons at the right. Double-clicking a row opens it for change. OK commits the changes; Cancel discards them.

> **Note:** Adding beyond the limit reports "The device supports at most 100 constants."

## Adding or changing a constant

The **Constant** editor holds two groups plus the Active tick:
- **Channel Name** — **Channel Name:**, up to 31 characters (the device stores a 32-byte label). A new constant starts as "New Constant" with the name selected for typing over.
- **Constant Details**:
    - **Data Type:** — boolean, u8, u16, u32, s8, s16, s32 or float. The field starts blank and a choice is required.
    - **Decimal Places:** — 0–8, capped by the type: boolean is locked at 0, u8/s8 allow up to 2, u16/s16 up to 4, u32/s32/float up to 8.
    - **Base Resolution:**, **Range Minimum:**, **Range Maximum:** — derived and read-only. The resolution is 10^−decimals; for integer types the range is the raw type range scaled by that resolution (a u16 with 2 decimal places spans 0…655.35), while boolean is 0…1 and float is shown as ±10⁹.
    - **Value:** — the fixed value, limited to the derived range and entered at the chosen number of decimal places.
- **Active** — an inactive constant is kept in the configuration but its value is not written on the device.

## Name rules

OK enforces the same rules as the other channel editors:
- The name must not be empty, and must fit the device's 31-byte label budget — a name with non-ASCII characters can exceed the byte limit within the character cap and is refused with "Names are limited to 31 bytes on the device."
- No two constants may share a name (case-insensitive): "A constant named "X" already exists."
- A constant may not take the name of an unrelated existing channel — it would overwrite that channel's definition: "A channel named "X" already exists. Choose a different name."

> **Note:** Renaming a constant carries its references along: math inputs, User Condition comparisons, transmit rows and table axes that used the old name are rewritten to the new one when the dialog is closed with OK.

> **Warning:** Deleting a constant removes its channel from the document. Any calculation still referencing the old name will be flagged by File → Check Channels, so run it after a clean-up.

## See also

[Math Channels](math-channels.md) · [User Conditions](conditions.md) · [Lookup Tables](tables.md) · [Channels](channels.md) · [Monitoring Live Values](monitor.md)
