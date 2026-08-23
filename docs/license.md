# License (MIT)

CAN Triple Device Manager is open-source software, © 2026 Minton Performance, released under the MIT License: you may use, copy, modify and redistribute it freely, provided the copyright and permission notice stay with it. It is provided "as is", without warranty of any kind.

The complete license text is in the file `COPYING.txt`, installed beside the program itself — the same folder as `CANTripleDeviceManager.exe`.

## Source code

The application's complete source is at [https://github.com/mitchdetailed/CAN-Triple-Device-Manager](https://github.com/mitchdetailed/CAN-Triple-Device-Manager) (`gui/`), together with the released firmware binaries.

## The device firmware

The CAN Triple firmware and bootloader are **not open source** — they are © Minton Performance, all rights reserved, distributed as binaries with the program. A small part of the device's own code (its image validator and script engine) is compiled into this executable so the two sides agree exactly; those portions are likewise proprietary and ship in the repository as a prebuilt library the application links.

## Bundled third-party components

The program ships with libraries it did not write — the Qt 6 toolkit, used unmodified under the GNU Lesser General Public License version 3, and the MinGW GCC runtime. Those carry their own licences, which do not restrict your rights to CAN Triple Device Manager itself. What is bundled, under which terms, and where to get each component's source is set out in `THIRD-PARTY-NOTICES.txt`, in the same folder as the program.

## This documentation

The manual you are reading is part of CAN Triple Device Manager and is licensed under the same terms as the program: the MIT License.
