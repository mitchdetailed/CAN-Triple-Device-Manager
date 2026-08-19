# Getting Started

This page takes you from a bare device to live values on screen. The program is offline-first: steps 1–3 need no hardware at all, and only step 4 touches the device.

## 1. Connect to the device

To open the serial link, choose **Tools → Connection Settings…**:
- **Port:** lists every serial port with its Windows description. The ST-Link virtual COM port is preselected automatically when its description contains "STLink". Click **Refresh** after plugging the cable in.
- **Baud rate:** defaults to **7372800 (CAN Triple default — ST-Link V3)**. The field is editable, and slower standard rates down to 115200 are listed, but the device firmware runs its UART at 7,372,800 baud — an ST-Link **V3** is required at that rate.
- Click **Connect**. The label below the buttons changes to "Connected to COM5 @ 7372800", and the main window's status bar shows "Connected: COM5 @ 7372800" on the right. The button becomes **Disconnect**.
- Click **Test** to confirm the device answers: it requests the device status and shows uptime, per-bus receive/transmit counters, and how many messages, signals, math channels and User Conditions are active.

> **Note:** You do not have to visit this dialog first. Any **Online** command that needs a device — Send, Get, Monitor Channels and the rest — opens Connection Settings automatically when no link is open.

If the port refuses to open or the Test fails, see [Troubleshooting](troubleshooting.md).

## 2. Create a configuration

Choose **File → New** (a new, empty document is also what the program starts with). Then define what travels on each bus:
1. **Connections → Communications…** opens the Communications Setup dialog, with one tab per bus (CAN 1 / CAN 2 / CAN 3). Set each bus's mode and rate, and add *sections* — the receive and transmit messages — with **New…** or **Import DBC…**. See [Communications: Messages &amp; Sections](communications.md) and [DBC Import](dbc-import.md).
2. Each message carries channel rows that place a channel's bits in the frame. Channels are created as you need them — there is no predefined catalogue. See [Channels](channels.md).
3. Optionally add on-device calculations from the **Calculations** menu: [Math Channels](math-channels.md), [User Conditions](conditions.md), [Constants](constants.md), [Lookup Tables](tables.md), [Up/Down Counters](counters.md), [Timers](timers.md) and [Integrators](integrators.md).

The window title shows the document name with an asterisk (**\***) while there are unsaved changes. Save with **File → Save**; see [Configuration Files (.ct3)](files.md) for the file formats.

## 3. Check the configuration

Run **File → Check Channels**. It reports Errors, Warnings and Info items; Send Configuration refuses to run while any **Error** exists, so it is worth fixing them before going online. **File → Config Summary…** produces a printable report of the whole configuration. See [Validation &amp; the Config Summary](validation-report.md).

## 4. Send it to the device

Choose **Online → Send Configuration** (F5). The confirmation lists what will be written — message, channel and calculation counts, and the bus settings being applied — and asks for a **Configuration Title**, which is stored on the device. Two options are offered: **Lock this configuration to this device** (only on firmware that reports a chip ID) and **Reset device after sending**.

The transfer is verified by reading the device's tables back, and the configuration is saved to flash as part of the send, so it reloads at every power-up — there is no separate "save to flash" step. Details, including access passwords and the related Get, Verify and Upload commands, are on [Online: Send, Get &amp; Flash](online.md).

## 5. Watch it run

**Online → Monitor Channels…** (F3) shows live channel values; **Online → CAN Viewer…** (F4) shows raw bus traffic. See [Monitoring Live Values](monitor.md).

**Online → Device Status…** is the quick health check: uptime, per-bus counters, active table counts, firmware protocol version, device ID, which access passwords are set, and the unit's fleet identity.

## Reading the device back

**Online → Get Configuration** reads the device's tables into the editor, replacing the current document (it warns first when there are unsaved changes). **Online → Verify Configuration** compares the device against the document without changing either.

> **Warning:** Send Configuration replaces the device's *entire* configuration. If you meant to keep what the device is running, Get it into the editor and save it before sending something else.
