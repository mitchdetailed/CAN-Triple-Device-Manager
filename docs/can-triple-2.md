# The CAN Triple 2.0

The CAN Triple 2.0 runs the same configurations as the CAN Triple and is set up with the same windows. What differs is how it connects, what it measures, and what its lights say. This page covers those differences.

## Connecting

The CAN Triple 2.0 is its own USB device: connect its USB port straight to the computer, with no ST-LINK in between. Windows installs its built-in serial driver the first time and calls the port *USB Serial Device*.
- **Tools → Connection Settings…** lists it as **COM*n* — CAN Triple 2.0 (USB)** and chooses it before any other port.
- The **Baud rate** does not apply to a USB device, so it is greyed out. The status bar shows **Connected: COM*n* (USB)**.
- Each unit keeps its own COM port number on a given computer: the unit's USB serial number is its chip's unique ID, the **MCU ID** that **Device Info** shows.

The CAN Triple 2.0's USB port is isolated from the rest of the unit, so a laptop on the bench and the vehicle's ground never meet through it.

## Restarts

Because the unit is the USB device, its COM port disappears whenever it restarts and comes back when it is running again, usually within a second. After a restart this program asked for, it waits for the port and connects again by itself:
- **Online → Reset Device**;
- **Send Configuration** with **Reset device after sending** ticked;
- **Update Firmware**, which has always done so.

A restart it did not ask for, such as the power being cut, shows as **Not connected**. Connect again once the unit is running.

Anything unlocked with a password stays unlocked only while the computer stays connected. Unplug the USB cable, or restart, sleep or shut down the computer, and the unit asks again: the Send, Get and CAN Viewer passwords included. (Closing this program and opening it again with the cable still in is not a disconnect, so the unit cannot tell.)

## Termination

The first boards (revision 2.00) cannot switch their 120 Ω termination resistors. With one of them connected, the **Termination Resistor** setting in [Communications Setup](communications.md) is greyed out. Fit a 120 Ω resistor at each end of the bus instead. The setting is kept in the configuration for units that can switch it.

## CAN FD data rates

A CAN Triple 2.0 runs CAN FD data phases of 1, 2, 4, 5 and 8 Mbit/s (a CAN Triple runs 1 and 2). Every rate the Device Manager offers is exact on its CAN clock, and the unit compensates for the time its own bits take to come back through the transceiver, without which no node can transmit a data phase much above 4 Mbit/s. Its transceivers are certified for CAN FD up to 5 Mbit/s; 8 Mbit/s works in simpler networks: short buses with few nodes. Choose the rate as **FD Data** in [Communications Setup](communications.md).

## Supply readings

The CAN Triple 2.0 measures its own supply. Four device channels carry the readings (see [Channels](channels.md)):

<table>
<tr><th>Channel</th><th>Meaning</th></tr>
<tr><td>Device Supply Voltage</td><td>The supply at the unit's power input,
averaged over the last 0.1 s.</td></tr>
<tr><td>Device Supply Voltage Minimum</td><td>The lowest single reading since
power-up. It catches a cranking dip that the average smooths away.</td></tr>
<tr><td>Device Supply Voltage Maximum</td><td>The highest single reading since
power-up.</td></tr>
<tr><td>Device USB Supply Voltage</td><td>The 5 V the USB port provides:
about 5.3 V with a computer connected, 0 without one.</td></tr>
</table>

The minimum and maximum count only while a supply is connected. A unit running from USB alone, or one whose supply has just been unplugged, does not record 0 V as its minimum. The CAN Triple (1.x) does not measure its supply: these channels read 0 there, and **Monitor Channels** leaves them out when one is connected.

## The lights

The CAN Triple 2.0 has four lights: **PWR** for the unit and one for each bus. Each can show several colours. How bright they all are is a setting the unit keeps, from 100 % down to 5 %: **Online → LED Brightness…** (see [LED Brightness…](online.md#brightness)).

### PWR

The first line that applies is what the light shows:

<table>
<tr><th>Light</th><th>Meaning</th></tr>
<tr><td>Amber, flashing quickly</td><td>A firmware update is arriving. Leave the
power on.</td></tr>
<tr><td>Amber</td><td>An update has arrived and been checked. It installs at the
next restart; the light stays amber while it installs.</td></tr>
<tr><td>Red</td><td>The supply is out of range: under 8 V or over
32 V for more than half a second.</td></tr>
<tr><td>Red, flashing</td><td>Something needs attention: the stored configuration
belongs to another unit, the device script is not running (it stopped on an error,
or this firmware refused it), or the unit restarted on its own in the last minute
because its watchdog found it stuck. <b>Online → Device Status</b> names
the first two. A watchdog restart shows as <b>Independent Watchdog</b> in the <b>Device Last
Reset Reason</b> channel in <b>Monitor Channels</b>.</td></tr>
<tr><td>Magenta, flashing</td><td><b>Monitor Channels</b> is overriding channel
values, so outputs are not what the configuration would compute.</td></tr>
<tr><td>Blue</td><td>Connected to a computer. It flickers while this program
exchanges data with the unit.</td></tr>
<tr><td>Green</td><td>Running.</td></tr>
</table>

Blue and green are steady when a configuration is stored, and flash slowly when none is. A unit fresh from the factory has none, and runs with every bus off.

### CAN1, CAN2, CAN3

<table>
<tr><th>Light</th><th>Meaning</th></tr>
<tr><td>Off</td><td>The bus is off.</td></tr>
<tr><td>Green</td><td>The bus is running: dim when it is quiet, and pulsing
bright ten times a second while frames are sent or received and for a second
after the last one.</td></tr>
<tr><td>Blue</td><td>The bus is listening only. Dim and pulsing the same
way.</td></tr>
<tr><td>Amber</td><td>Errors on the bus: error warning or error-passive. The
usual causes are that nothing else on the bus acknowledges frames, or that the
bit rate is wrong. Dim and pulsing the same way.</td></tr>
<tr><td>Red, flashing</td><td>Bus-off: the unit has stopped transmitting on this
bus after too many errors, and is restarting it.</td></tr>
</table>

A CAN Triple 2.0 whose PWR light flashes red very fast and which does not answer at all has no valid firmware to run. It needs to be reprogrammed.

## Sending a configuration

A CAN Triple 2.0 keeps running its current configuration while a new one is sent: messages keep going out and calculations keep running. It switches over when the Send finishes, with a short pause while the new configuration is put in place: under a second for most configurations, a little over a second for very large ones. If the Send is interrupted (the cable pulled, the program closed, the power lost before the end) the unit carries on with the configuration it had. If the power is lost during the switch-over itself, the unit finishes it the next time it starts. Frames the old configuration queued that have not gone out by the switch-over are dropped, not sent under the new one.

## When a bus cannot take every frame

A transmit message on a CAN Triple 2.0 never has more than one period's frames waiting to go out. If they have not gone when its next period comes, because nothing on the bus acknowledges frames or the configuration asks more of the bus than it can carry, they are given the new values instead of more frames queueing behind them. When the bus recovers, the unit sends the frame it was retrying and then each message's newest values, not a burst of old frames. A multiplexed message sent one variant at a time keeps its turn order. When the configuration asks more of a bus than it can carry, every transmit message on that bus gives up the same share of its periods: none keeps its full rate while another falls behind. The values that were replaced count in **Device CAN*n* Tx Dropped** (see [Channels](channels.md)). Relayed frames and frames sent from the CAN Viewer are frames in their own right: they are never replaced, and wait their turn as before.

<a id="names"></a>

## Names

A CAN Triple 2.0 keeps 32 bytes of every name: channels (including constants, calculations and table outputs), messages and message relays. A CAN Triple keeps 31 bytes of a channel's name and 17 of a message's or a relay's. The name fields take 32 when the document's target is a CAN Triple 2.0, and a Get Configuration returns the names whole. The limit is in UTF-8 bytes: an accented or non-Latin character takes 2-4 of them, so a name using them holds fewer characters.

A configuration sent with an earlier Device Manager, or one the unit was holding before its firmware was updated to keep names this way, has to be sent again.

## Transmit CRC8

A CAN Triple 2.0 runs up to 100 [Transmit CRC8](communications.md#crc8) messages (a CAN Triple runs 20) and stamps the checksum into any byte of a CAN FD frame, Byte 0 to Byte 63. Its recipes have two more element types: **ID (Whole)**, the identifier's bytes in one row, and **Data Run**, a range of frame bytes counting up or down that leaves the CRC's own byte out. A checksum over the identifier and a whole 64-byte frame takes two rows.

## Device scripts

A CAN Triple 2.0 runs [device scripts](device-scripts.md) on its floating-point hardware and charges them its own costs, which it reports to the Device Manager; the script editor shows and simulates with them when the document's target is a CAN Triple 2.0. See "The budget" in Device Scripts.

## Updating its firmware

The CAN Triple 2.0 takes firmware built for it and no other. **Update Firmware** shows which board a file is for, and refuses a CAN Triple file for a CAN Triple 2.0 and the reverse, naming both. The CAN Triple 2.0 firmware that came with this Device Manager is in its install folder under `Firmware\CAN Triple 2.0`, where Browse… opens when a CAN Triple 2.0 is connected.

It keeps copies of its firmware: the version it runs, the one before it and the one it left the factory with. A new version runs on trial until the Device Manager, or a minute of running, confirms it; one that does not start properly is replaced by the version before it, by the unit itself. See [Updating Firmware](firmware-update.md).

Its firmware is numbered on its own, from 2.0.0. Where this help says a feature needs device firmware 1.0.13, 1.0.14 or 1.0.15, that is the CAN Triple's firmware: every CAN Triple 2.0 firmware has those features.

## See also
- [Getting Started](getting-started.md)
- [Channels](channels.md)
- [Troubleshooting](troubleshooting.md)
