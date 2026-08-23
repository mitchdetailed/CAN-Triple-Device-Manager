# CAN Triple firmware

The device firmware and bootloader are **not open source**. This folder holds
the released binaries — the same files the Windows installer carries — plus
the small prebuilt library the Device Manager links.

## The device binaries

| file | what it is |
|---|---|
| `bootloader.bin` | the bootloader the **CAN Triple Initial Programming Tool** installs over USB |
| `can-triple-<version>.ctf` | the firmware image the Manager installs via **Online → Update Firmware…** |

A `.ctf` is a packed, checksummed firmware image. The device validates it
before committing anything, and a failed or interrupted update cannot brick
the unit — see [Updating Firmware](../docs/firmware-update.md).

## The device-core library

`lib/libct_device_core.a` is the device's own image validator and script VM,
compiled for the desktop (MinGW x64). The Manager links it so that the app
runs the *device's* code — the same validator the bootloader uses, the same
VM the unit runs — rather than a re-implementation that agrees by
inspection. `include/` holds its interface headers; the build links the
library automatically, which is what lets a fresh clone of this repository
build as-is. The Manager's own source (everything under `gui/`) is MIT.

Everything in this folder is © Minton Performance, all rights reserved —
provided for use with CAN Triple hardware, and redistributable unmodified
alongside the Device Manager.
