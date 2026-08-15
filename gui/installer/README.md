# Windows installer

`CANTripleDeviceManager.iss` wraps the standalone build in `deploy/` into a
single Setup executable using [Inno Setup 6](https://jrsoftware.org/isinfo.php)
(6.3 or newer — the script uses the `x64compatible` architecture identifiers,
which older 6.x releases do not know).

## Build it

```powershell
cmake --build build --target installer
```

That runs `deploy` first (which itself rebuilds the app and the manual), finds
`ISCC.exe`, and writes `dist/CANTripleDeviceManager-<version>-Setup.exe`. The
version comes from `project(CANTripleDeviceManager VERSION …)` in the top-level
`CMakeLists.txt`, and that one value goes two ways: into the script as
`/DMyAppVersion`, and into the application as `-DCT_APP_VERSION`, which
`src/main.cpp` hands to `QCoreApplication::setApplicationVersion()` instead of
repeating the literal. So the Setup filename, the Apps & Features entry and the
program's own idea of its version cannot drift apart — there is one number to
edit for a release. (The `#ifndef MyAppVersion` default inside the script is a
fallback so it still compiles standalone; a CMake build always overrides it.)

If Inno Setup lives somewhere unusual, point CMake at it once:

```powershell
cmake -S . -B build -DISCC_EXE="D:/Tools/Inno Setup 6/ISCC.exe"
```

Compiling without CMake works too — the script defaults every `#define` behind
an `#ifndef`, so it stands alone:

```powershell
& "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" `
    /DMyAppVersion=1.0.0 installer\CANTripleDeviceManager.iss
```

`deploy/` must already be populated (`cmake --build build --target deploy`).
It is gitignored, so on a fresh clone it is empty. The script checks for each
required file individually at compile time, so the error names the one that is
missing:

- the executable, `platforms\qwindows.dll`, `sqldrivers\qsqlite.dll` and the
  three MinGW runtime DLLs — each individually fatal at launch, so a
  half-populated `deploy/` would otherwise compile into a Setup that installs a
  program which cannot start on a machine you can't debug on. This matters
  because the `deploy` target only *warns* when a runtime DLL is missing from
  the compiler's `bin` directory, and warnings scroll past in a build log;
- the four licence files — not fatal at launch, fatal legally: without them the
  Setup would distribute binaries with no licence at all. Two of the four,
  `COPYING.txt` and `THIRD-PARTY-NOTICES.txt`, are also the files the wizard
  displays, so their checks double as what keeps an unbuilt `deploy/` from
  reaching the compiler as a bare "file not found" on `LicenseFile`.

## What it installs

The **whole** of `deploy/`, recursively, into `%ProgramFiles%\CAN Triple Device
Manager` — the executable, the Qt 6 DLLs, the MinGW C/C++ runtime, all eight Qt
plugin subdirectories and the compiled manual (`cantriple.qch` / `.qhc`, which
`HelpWindow` reads from beside the executable). Nothing in that tree is
optional: `platforms\qwindows.dll` is what lets Qt start at all, and
`sqldrivers\qsqlite.dll` is what lets the Help window open its collection file,
which is an SQLite database.

The licences ride along in that same tree, because the `deploy` target stages
them into it: `COPYING.txt` (the application's MIT licence, byte-for-byte the
repository's `COPYING`), `LICENSE.LGPLv3.txt`, `LICENSE.GPLv3.txt` and
`GCC-RUNTIME-EXCEPTION.txt` (the verbatim texts the bundled Qt 6 and the MinGW
runtime are used under — the GPLv3 text travels because the LGPL and the GCC
exception layer on it, not for the application), and `THIRD-PARTY-NOTICES.txt`
(what is bundled, at which version, under which licence, and where to get its
corresponding source). The four notice files are maintained in `installer/`;
the deploy target is the only thing that copies them anywhere.

Staging them into `deploy/` rather than listing them in the script's `[Files]`
is the point of the arrangement: `CANTripleDeviceManager-windows-x64.zip` is
nothing but a zipped `deploy/`, so the zip inherits exactly the same five files,
and there is one place to keep them right instead of one per channel.
The wizard is held to that same rule: `LicenseFile` and `InfoAfterFile` read
`deploy/COPYING.txt` and `deploy/THIRD-PARTY-NOTICES.txt` — the copies being
installed — rather than the repository `COPYING` and the `installer/` notices
they were staged from. Otherwise the two could disagree — and the by-hand ISCC
command above is exactly where they would, since it does not run `deploy` first:
edit the notices, compile, and the wizard shows the new text while the old text
is what lands on disk. The script has no licence entry of its own, and the
compile-time guard fails the build if the staging did not happen.

During setup, the application's licence is shown and accepted (the acceptance
page also says in one line that the bundled Qt 6 is separate work under the
LGPLv3), and the third-party notices are shown on the last wizard page — both
read from those staged copies, so each page is the file it installs.

**Everyone** sees the "install for all users / just for me" page, not only
non-administrators: `PrivilegesRequiredOverridesAllowed=dialog` puts that choice
in front of every user, including one who could elevate. The per-user route has
to be reachable without the operator knowing it exists — locked-down workshop
and fleet laptops are a normal case for this tool — and the cost is one wizard
page for everybody else.

Installing over a **running** copy is handled through the Restart Manager
(`CloseApplications=yes`), not through Inno's usual `AppMutex`: the application
creates no named mutex, and is written to tolerate several instances at once.
The Restart Manager spots the running copy by the file handles it holds
instead. It is not restarted afterwards, because the app restores nothing on
launch — no window geometry, no reopened document, no remembered port or baud
rate — so an automatic relaunch would look like the session survived when it is
in fact an empty document.

## Icons

One set of artwork in `Icons/icons/` — `app.ico` (7 sizes, 16 to 256) and the
same images as loose PNGs under `png/` — and three separate consumers. Only the
first is this script's business, but they are listed together because a missing
one is never reported as anything more useful than "why is that blank?":

| Surface | Wired by | How |
| --- | --- | --- |
| Setup.exe itself — Explorer, the download bar, the UAC prompt | this script | `SetupIconFile={#MyAppIcon}`, read from the repository at compile time |
| The installed executable — Explorer, Task Manager, Apps & Features | `resources/app.rc`, compiled by `windres` | see the RC block in the top-level `CMakeLists.txt` |
| The running window — title bar, taskbar, Alt-Tab, dialogs | `resources/app.qrc` (the PNGs) + `QApplication::setWindowIcon()` | see `src/main.cpp` |

None of it is part of the payload, and none of it gets a `[Files]` entry. Setup
embeds the `.ico` in itself, the installed program carries its own copy inside
the executable's PE resources, and the PNGs are compiled into the binary by
`rcc` — nothing at runtime reads an icon file off disk.

That is also why `UninstallDisplayIcon={app}\{#MyAppExeName}` finally does
something. The directive has always been there, but until the executable gained
a resource section there was no icon inside it for Windows to draw, so
Apps & Features showed the generic placeholder. The fix was in the exe, not
here.

`MyAppIcon` is `#define`d behind an `#ifndef` like every other identity value,
and the script refuses to compile if the file is missing — the same by-name
treatment the payload files get, for the same reason: `SetupIconFile` reads it
through a path with a `\..` in the middle, and Inno's own "file not found"
against that is much harder to read than an error naming `Icons\icons\app.ico`.

## Upgrading a 0.1.0 install (the rename)

Releases before this one installed a program called **CAN Triple Manager**, with
`CANTripleManager.exe` in it. The `AppId` GUID did **not** change with the
rename — that is deliberate, and it is the whole reason an existing install is
upgraded in place rather than joined by a second entry in Apps & Features. Three
consequences follow, and the script handles the first two:

- **The old executable and Start Menu group are deleted** by `[InstallDelete]`.
  Nothing overwrites a file that changed its name, so without those entries the
  old exe would sit in the install directory forever — runnable, and launching a
  program the shortcuts no longer point at. The old desktop shortcut goes the
  same way. Those entries are pure upgrade archaeology and can be dropped from
  the script once no 0.1.0-era install survives in the field.
- **The help cache moved twice**, because its path contains *both* the
  organization and the application name, and both were renamed — `CANTriple` →
  `Minton Performance`, and `CAN Triple Manager` → `CAN Triple Device Manager`.
  `[UninstallDelete]` lists the current path and both historical ones, so no
  machine keeps an orphan nothing will ever collect.
- **The recent-files list did not come with it.** `QSettings` keys off the same
  two names, so `HKCU\Software\CANTriple\CAN Triple Manager` (and the
  intermediate `HKCU\Software\CANTriple\CAN Triple Device Manager`) stay where
  they are and the renamed program starts with an empty list. There is no
  migration code and the installer does not touch any of those keys: rewriting a
  user's registry to paper over a rename is not an installer's job. Worth a line
  in the release notes; not worth a silent registry edit.

One thing the script cannot fix: **the install directory keeps its old name** on
an upgrade. `DefaultDirName` only chooses a path for a *fresh* install — an
upgrade reuses whatever the previous run recorded, so an upgraded machine stays
at `%ProgramFiles%\CAN Triple Manager` while a clean install lands in
`%ProgramFiles%\Minton Performance\CAN Triple Device Manager`. That is cosmetic,
and moving an installed program between directories to fix it would risk more
than it buys.

## What it removes

Uninstalling deletes the install directory and the manual cache the app copies
into `%APPDATA%\Minton Performance\CAN Triple Device Manager\help` (the help
engine writes its search index into its own collection, so it cannot run from a
read-only Program Files copy). The two pre-rename paths are cleaned up too, as
above.

That cache cleanup is best-effort, not exhaustive. `{userappdata}` is the
profile of whoever the uninstaller is *running as*: correct for a per-user
uninstall, and correct for a per-machine one when the operator is themselves a
local administrator — but if someone else's administrator credentials were used
to elevate, it clears that account's copy and leaves the operator's. It never
touches other users' copies at all, because an uninstaller cannot walk the
profiles. Residue is up to a few hundred KB per profile it could not reach,
which is cheaper than the alternatives (an Active Setup stub, or an uninstaller
that goes rummaging through `C:\Users`).

It deliberately leaves two things alone:

- **`HKCU\Software\Minton Performance\CAN Triple Device Manager`** — the
  recent-files list, and the only setting the app keeps. It survives reinstalls,
  which is what anyone reinstalling wants, and it is the user's own data. (The
  two pre-rename keys under `CANTriple` are left alone as well, for the reason
  given above.)
- **Anything you saved** — `.ct3` / `.ct3s` configurations, PDF and text
  summaries, `.asc` CAN logs. All of those go where a save dialog put them; the
  installer has no idea where that is and no business guessing.

## What it deliberately does not do

**No `.ct3` / `.ct3s` file associations.** `src/main.cpp` parses one
command-line option (`--screenshots`, a development switch) and ignores
positional arguments completely. Shell associations pass the file as a
positional argument, so double-clicking a configuration would open CAN Triple
Device Manager on a new *empty* document with nothing to say the file was
dropped — worse than no association, because the user thinks they are looking at
their configuration. The script's closing comment records the three things that
have to change before associations can be added, in order. (The application icon
is no longer one of them; what is still open is what a `.ct3` *document* should
look like, which the program's own icon is a mediocre answer to.)

**No driver bundle.** The only thing the app talks to is the ST-Link V3 virtual
COM port, through `QSerialPort` against whatever COM port Windows enumerates.
There is no vendor SDK, no libusb, and no MSVC redistributable (this is a MinGW
build, and its three runtime DLLs ship in the install directory). If the device
does not appear in **Tools → Connection Settings…**, that is ST's VCP driver
missing from the machine, not something this installer can supply — and note
that 7,372,800 baud needs an ST-Link **V3**; a V2 will not reach it.

**No code signing.** SmartScreen will warn on first run of an unsigned Setup.
Signing is a `signtool` step over `dist\*.exe` after the build; Inno's
`SignTool` directive can do it inline once there is a certificate.

## Licence and source availability

CAN Triple Device Manager is MIT-licensed; its source is published at
<https://github.com/mitchdetailed/CAN-Triple-Device-Manager>. The device
firmware and bootloader are not open source — the app's device-core portion
ships in that repository as a prebuilt library — but the MIT notice must still
accompany every distribution of the application, and the bundled Qt 6 keeps
its own LGPL obligations regardless.

The MIT text reaches the user twice: on the acceptance page during install,
and as `COPYING.txt` in the install directory. Those two are one file, not two
— the wizard reads the same staged `deploy/COPYING.txt` it goes on to install,
so what was accepted and what is on disk cannot come apart. The manual's
licence page is *not* a third — it carries the standard notice paragraphs and
points at `COPYING`, it does not reproduce the licence. The zip ships
`COPYING.txt` for the same reason the installer does; it is the same file,
from the same staging step.

The bundled Qt 6 (6.7.2, mingw_64) is used under the LGPLv3 and shipped as
unmodified dynamic libraries, which is what keeps that arrangement simple — if
you ever static-link Qt, the obligations change. The MinGW GCC 13.1.0 runtime
DLLs are GPLv3 with the GCC Runtime Library Exception, which is what allows them
to be bundled at all. Four files in `installer/` cover this, and go out with
every distribution:

- **`LICENSE.LGPLv3.txt`** — the verbatim LGPLv3. Its §4 requires a combined
  work to be accompanied by a copy of both this and the GNU GPL;
  `LICENSE.GPLv3.txt` is the second half of that pair.
- **`LICENSE.GPLv3.txt`** — the verbatim GPLv3, carried for Qt and the GCC
  runtime (the texts above layer on it); the application itself is MIT.
- **`GCC-RUNTIME-EXCEPTION.txt`** — the verbatim GCC Runtime Library Exception.
- **`THIRD-PARTY-NOTICES.txt`** — the inventory: each bundled component, its
  version, its licence and where its corresponding source can be obtained. This
  is also what the installer shows on its final page, through the `deploy/` copy
  of it rather than the one here.

Anything added to the payload later needs an entry in `THIRD-PARTY-NOTICES.txt`
and, if its licence is not already one of the above, its text staged alongside
in the same `deploy` step.
