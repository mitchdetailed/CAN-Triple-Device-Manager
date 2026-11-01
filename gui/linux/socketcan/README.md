# cantriple-socketcan: the CAN Triple 2.0 on Linux

The CAN Triple 2.0 can present itself to a PC as a **gs_usb CAN adapter**: the
protocol of the Linux kernel's own `gs_usb` driver. Its three buses then become
ordinary SocketCAN interfaces, with no driver to build or install: `candump`,
`cansend`, `ip link`, python-can, SavvyCAN and every other SocketCAN program
use them as they are. This package is the small amount of glue that needs:

- a udev rule that binds `gs_usb` to the unit and names its interfaces after
  the unit and the bus: `ct4f002b-can1`, `ct4f002b-can2`, `ct4f002b-can3` for
  the unit whose MCU ID ends `…4F002B`, its CAN1 to CAN3;
- `ctriple`, which switches the unit between SocketCAN mode and the Device
  Manager's serial port, keeps the mode across power-ups, and brings the
  interfaces up and down;
- an optional bring-up at set bit rates whenever the unit appears
  (`/etc/default/cantriple-socketcan`, off unless you turn it on).

## What it needs

- A CAN Triple 2.0 on firmware **2.0.7 or later** (2.0.10 or later to keep the
  mode across power-ups).
- Linux **6.2 or later** for everything (6.6, a long-term release, or later
  recommended): three channels, CAN FD, hardware timestamps, bus state and
  error counters, bus-error reports. 5.18 to 6.1 have the channels, FD and the
  timestamps but not `ip -details`' error counters or bus-error reports.
- Debian, Ubuntu, Mint or another dpkg system for the package; the files
  themselves work anywhere with udev.

## Install

```bash
sudo apt install ./cantriple-socketcan_1.0.0_all.deb
```

## Use

```bash
ctriple                                       # every unit, in either mode
ctriple socketcan                             # become the SocketCAN adapter
sudo ctriple up --bitrate 500000              # all three buses at 500k
sudo ctriple up --bitrate 500000 --dbitrate 2000000 --bus 2   # CAN2 as CAN FD
candump ct4f002b-can1
cansend ct4f002b-can1 123#DEADBEEF
ctriple manager                               # back to the Device Manager's port
```

`ip link` works on the interfaces as on any other: `sudo ip link set
ct4f002b-can1 type can bitrate 250000 listen-only on`. Linux works out the
bit timing from what the unit says its controller can do, and the unit applies
it as given, CAN FD data phase included (to 8 Mbit/s); only the FD data phase's
transmitter delay compensation is the unit's own.

While a bus is up under Linux it runs Linux's bit rate and mode; the unit's own
configuration keeps running on it, its transmit messages going out among
yours. Taken down, the bus returns to the configuration's setup. Nothing set
from Linux is stored on the unit.

### Every power-up as the adapter

```bash
ctriple socketcan --persist
```

The unit now starts as the SocketCAN adapter, for a PC or a vehicle logger that
should find it ready. A PC that cannot use the adapter (Windows, or Linux
without this package) still reaches the unit: if a PC has not configured it 5 s
after it began, or no `gs_usb` driver has claimed it 60 s after that, the unit
gives that PC the Device Manager's port instead, until the USB cable comes out.
From firmware 2.0.11 that holds for an adapter asked for without `--persist`
too, on every PC but the one that asked. `ctriple` says when that has happened.
`ctriple manager --persist` makes it start as the Manager's port again, and so
does the Device Manager: **Online > USB Mode**, from any PC. On Linux the
Device Manager's **Online > Connect** also lists a unit that is the adapter
and switches it back.

### Bringing the buses up by themselves

In `/etc/default/cantriple-socketcan`, set `AUTO_UP=yes` and the bit rates
(one for every bus, or per bus with `CAN1_BITRATE` and so on). Every time the
unit appears in SocketCAN mode its buses then come up at those rates. It is off
by default because a bus brought up at the wrong bit rate puts error frames on
everything connected to it.

### A CAN Viewer password

A unit with a CAN Viewer password asks for it before becoming the adapter, as
the Device Manager's CAN Viewer does: `ctriple` prompts, or takes
`--password` or `CTRIPLE_PASSWORD`.

## Every option, in full

```bash
man ctriple               # every command, option, message and exit status
man cantriple-socketcan   # setup, modes, fallback, bit timing, errors, config, udev, troubleshooting
```

The pages are in this directory too (`man -l ctriple.1`).

## Remove

```bash
sudo apt remove cantriple-socketcan
```

## Build

```bash
sh build-deb.sh
```

in this directory (or from anywhere, by its path) writes
`build/cantriple-socketcan_<version>_all.deb` here (needs `dpkg-deb`; no root).
The version is `ctriple`'s `VERSION`.

## Until the adapter has its own USB ID

SocketCAN mode uses ST's shared `0483:5740`, as the Manager's serial port does,
so the kernel's `gs_usb` table cannot name the unit and the udev rule tells the
driver the ID instead (matching only the unit's own manufacturer string and its
vendor-class interface). With a product ID of its own, a one-line addition to
`gs_usb` makes the binding automatic and only the names remain for the rule.
