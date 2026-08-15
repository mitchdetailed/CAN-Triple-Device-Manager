# CAN Triple

**A three-bus CAN gateway and calculation computer**, configured from Windows
with the **CAN Triple Device Manager** — build messages and channels, add math,
conditions, counters, timers and lookup tables, watch everything live, and
program it into the device over a single USB cable.

## Download

**[⬇ Download the latest installer](https://github.com/mitchdetailed/CAN-Triple-Device-Manager/releases/latest)**
— Windows 10/11, 64-bit.

One Setup installs everything:

| | |
|---|---|
| **CAN Triple Device Manager** | the configuration software |
| **USB drivers** | installed automatically — the device just works when plugged in |
| **Firmware** | the matching firmware release, ready to install from the app |
| **Recovery tool** | brings back a blank or unresponsive board over the same USB cable |

> **Windows SmartScreen:** the installer is not yet code-signed, so the first
> run may show *"Windows protected your PC"* with an unknown publisher. Click
> **More info**, then **Run anyway**. The warning is about the missing
> signature, not the software — the Device Manager's complete source is this
> repository.

## Getting started

1. Run the installer, then plug the CAN Triple in over USB.
2. Open **CAN Triple Device Manager** and pick the device's COM port under
   **Tools → Connection Settings…**
3. Follow **[Getting Started](docs/getting-started.md)** — from first
   connection to seeing live values.

New board fresh from the factory? Run **CAN Triple Recovery Tool** (in the
Start Menu) once — it installs the bootloader and firmware directly over USB.
After that, firmware updates happen inside the Manager
(**Online → Update Firmware…**).

## Documentation

The full manual is in **[docs/](docs/README.md)** — the same manual the
program ships offline under **Help → Contents (F1)**. Highlights:

- **[Getting Started](docs/getting-started.md)** — first session, end to end
- **[Messages & Sections](docs/communications.md)** — buses, messages, channels
- **[Math Channels & Calculations](docs/math-channels.md)** — the on-device calculations
- **[Order & Timing of Operations](docs/engine.md)** — exactly what runs when, and how often
- **[Updating Firmware](docs/firmware-update.md)** — updates, recovery, and why an update can't brick the unit
- **[Troubleshooting](docs/troubleshooting.md)**

## Building from source

The **Device Manager is open source**, and this repository is its complete
source:

- **[gui/](gui/README.md)** — the Device Manager (Qt 6 / C++, CMake). Build
  instructions in its README. A fresh clone builds as-is; the release
  packaging targets (`deploy`, `installer`) are the one part that needs
  files this repository does not carry.

The device **firmware and bootloader are not open source**. They ship in
**[firmware/](firmware/README.md)** as ready-to-install binaries — the same
files the installer carries — together with the prebuilt device-core library
the Manager links (the device's own image validator and script VM, which is
what makes a fresh clone build as-is); that folder's README says exactly what
is what.

## License

The Device Manager application is **MIT** — see
[gui/COPYING](gui/COPYING). Third-party notices for what the installer
bundles (Qt 6, the MinGW runtime, OpenOCD, the ST-Link drivers) are in
[gui/installer/THIRD-PARTY-NOTICES.txt](gui/installer/THIRD-PARTY-NOTICES.txt).

The CAN Triple **firmware and bootloader binaries** are © Minton Performance,
all rights reserved — provided for use with CAN Triple hardware, and
redistributable unmodified alongside the Device Manager.
