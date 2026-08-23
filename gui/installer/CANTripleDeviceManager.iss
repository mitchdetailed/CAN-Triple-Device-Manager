; ===========================================================================
; CAN Triple Device Manager -- Windows installer (Inno Setup 6.3 or newer)
;
; Ships the standalone build in deploy/ as a single Setup executable. That
; directory is the whole product: the exe, the Qt 6 DLLs, the MinGW C/C++
; runtime, every Qt plugin subdirectory windeployqt produced, and the licence
; and notice files the CMake deploy target stages there. It is copied wholesale
; and recursively -- see the note over [Files] for why picking and choosing from
; it is not an option, and why this script adds nothing of its own to it.
;
; Build it with `cmake --build build --target installer`, which runs the
; `deploy` target first and passes /DMyAppVersion. That number is not
; maintained here: project(CANTripleDeviceManager VERSION ...) in CMakeLists.txt is
; the one place it is edited, and the same value reaches the application itself
; as -DCT_APP_VERSION, so Setup, Apps & Features and the running program are
; incapable of disagreeing. installer/README.md has the raw ISCC command for
; anyone compiling outside CMake.
;
; This file is deliberately pure ASCII. Inno Setup only reads a .iss as UTF-8
; when the file carries a BOM; without one it falls back to the system ANSI
; code page and any stray typography here would come out as mojibake.
; ===========================================================================


; ---------------------------------------------------------------------------
; Identity. Every define is wrapped in #ifndef so ISCC can override it from the
; command line (`ISCC /DMyAppVersion=1.2.3 ...`) without the script being
; edited. The CMake `installer` target relies on that for the version.
;
; MyAppPublisher and MyAppSupportURL are THE TWO LINES TO EDIT if the project
; is published under a different name or moves to a different home. Publisher
; is what Windows shows in Apps & Features and on the UAC prompt; the URL is
; what the "Publisher's website" link in Apps & Features points at. Nothing
; else in this script hard-codes either.
; ---------------------------------------------------------------------------
#ifndef MyAppName
  #define MyAppName "CAN Triple Device Manager"
#endif

; Comes from project(CANTripleDeviceManager VERSION ...) in CMakeLists.txt, which also
; supplies -DCT_APP_VERSION to the compiler -- src/main.cpp passes that straight
; to QCoreApplication::setApplicationVersion() rather than repeating the number.
; The literal below is a standalone-compile fallback, not a second place to
; maintain: the CMake target always overrides it, so a release changes the
; number in CMakeLists.txt and nowhere else. Anyone compiling this script by
; hand should pass /DMyAppVersion rather than edit it.
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

#ifndef MyAppPublisher
  #define MyAppPublisher "Minton Performance"
#endif

; The vendor folder under Program Files: the install lands in
; "...\Minton Performance\CAN Triple Device Manager". Kept as its own define
; rather than reusing MyAppPublisher because the two answer different
; questions -- this one names a directory that other products from the same
; workshop would share, while MyAppPublisher is the string Windows shows in
; Apps & Features. Set them to the same text if that is what you want; they
; are not required to agree.
#ifndef MyAppVendor
  #define MyAppVendor "Minton Performance"
#endif

#ifndef MyAppSupportURL
  #define MyAppSupportURL "https://github.com/mitchdetailed/CAN-Triple-Device-Manager"
#endif

#ifndef MyAppExeName
  #define MyAppExeName "CANTripleDeviceManager.exe"
#endif

; Where the payload and the licence come from. SourcePath is the directory
; holding this script, so the paths survive being compiled from any working
; directory -- CMake invokes ISCC from the build tree, not from installer/.
#ifndef MyRepoRoot
  #define MyRepoRoot SourcePath + "\.."
#endif
#ifndef MyDeployDir
  #define MyDeployDir MyRepoRoot + "\deploy"
#endif

; The application icon, read straight out of the repository rather than from
; deploy/ -- Setup.exe needs it at COMPILE time to embed in itself, and it is
; not part of the payload (the installed program carries its icon inside the
; executable's own resources, from resources/app.rc). Same 7-image .ico the exe
; and the runtime window icon come from, so all three surfaces match.
#ifndef MyAppIcon
  #define MyAppIcon MyRepoRoot + "\Icons\icons\app.ico"
#endif

; Fail loudly at compile time rather than shipping an installer that cannot
; produce a working install.
;
; An empty deploy/ is the most likely mistake, because deploy/ is gitignored and
; is only populated by the `deploy` CMake target. A HALF-populated deploy/ is
; the more dangerous one: it compiles happily into a Setup that installs a
; program which dies at launch on a machine nobody can debug on. That is not
; hypothetical -- the deploy target only issues a CMake WARNING when a MinGW
; runtime DLL is missing from the compiler's bin directory, and a warning
; scrolls past in a build log.
;
; So every file whose absence matters -- fatal at launch below, fatal legally in
; the licence block further down -- gets its own #if and names itself, rather
; than a generic "something is missing" or a list of candidates to go and check.
#if !FileExists(MyDeployDir + "\" + MyAppExeName)
  #error Payload missing: run `cmake --build build --target deploy` before compiling this script.
#endif

; Qt aborts before main() gets anywhere: "no Qt platform plugin could be initialized".
#if !FileExists(MyDeployDir + "\platforms\qwindows.dll")
  #error Payload incomplete: deploy\platforms\qwindows.dll is missing -- re-run the `deploy` target.
#endif

; No SQLite driver, no manual -- cantriple.qhc is an SQLite database.
#if !FileExists(MyDeployDir + "\sqldrivers\qsqlite.dll")
  #error Payload incomplete: deploy\sqldrivers\qsqlite.dll is missing -- re-run the `deploy` target.
#endif

; The MinGW C/C++ runtime. Any one of the three missing is a Windows loader
; error box before a single line of this program runs.
#if !FileExists(MyDeployDir + "\libgcc_s_seh-1.dll")
  #error Payload incomplete: deploy\libgcc_s_seh-1.dll is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\libstdc++-6.dll")
  #error Payload incomplete: deploy\libstdc++-6.dll is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\libwinpthread-1.dll")
  #error Payload incomplete: deploy\libwinpthread-1.dll is missing -- re-run the `deploy` target.
#endif

; The licences, which now reach the install directory only as part of deploy/
; (see [Files]). Missing, they cost nothing at launch and everything legally:
; the Setup would distribute binaries with no licence texts at all — the MIT
; notice must accompany the app, and LGPLv3 section 4 requires Qt's to travel
; with the binaries. A compile error, not a warning.
;
; Two of them are load-bearing at COMPILE time as well, because LicenseFile and
; InfoAfterFile in [Setup] read the deploy/ copies rather than the sources they
; were staged from. Keeping that failure legible is what these two checks are
; for: the preprocessor runs to completion before the compiler opens either
; directive, so an unbuilt deploy/ stops here, naming the file and the target
; that produces it, rather than surfacing as Inno's own "file not found" against
; a path with a `\..` in the middle of it.
;
; One #if per file, matching the by-name treatment the runtime payload gets
; above. A combined check can only ever report the pair it tested and leave the
; reader to work out which half is actually absent.
#if !FileExists(MyDeployDir + "\COPYING.txt")
  #error Payload incomplete: deploy\COPYING.txt is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\THIRD-PARTY-NOTICES.txt")
  #error Payload incomplete: deploy\THIRD-PARTY-NOTICES.txt is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\LICENSE.LGPLv3.txt")
  #error Payload incomplete: deploy\LICENSE.LGPLv3.txt is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\LICENSE.GPLv3.txt")
  #error Payload incomplete: deploy\LICENSE.GPLv3.txt is missing -- the LGPL and the GCC exception layer on it; re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\GCC-RUNTIME-EXCEPTION.txt")
  #error Payload incomplete: deploy\GCC-RUNTIME-EXCEPTION.txt is missing -- re-run the `deploy` target.
#endif

; The device's USB drivers (ST's stsw-link009 v3, vendored verbatim in
; installer\drivers and staged into deploy\ by the deploy target). Missing,
; the program installs and runs but the hardware enumerates as an unknown
; device on any machine Windows Update cannot reach -- the offline bench
; laptop being the normal home for this tool. dpinst_amd64.exe is the one
; file [Run] executes, so it is the one checked by name.
#if !FileExists(MyDeployDir + "\Drivers\stlink\dpinst_amd64.exe")
  #error Payload incomplete: deploy\Drivers\stlink\dpinst_amd64.exe is missing -- re-run the `deploy` target.
#endif

; The firmware image this release pairs with. The deploy target stages it as
; Firmware\can-triple-<version>.ctf, reading the version from the firmware
; repository, so the exact name is not known here -- the check is that the
; folder holds at least one .ctf. An installer without it still works against
; an already-programmed unit, but the Firmware folder existing empty would
; quietly break the promise that a bench machine can restore a device with
; nothing but this install.
#if FindFirst(MyDeployDir + "\Firmware\*.ctf", 0) == 0
  #error Payload incomplete: deploy\Firmware holds no .ctf -- build the firmware, then re-run the `deploy` target.
#endif

; The initial-programming kit: the tool, the bootloader image it programs,
; and the OpenOCD that drives the board's built-in ST-LINK. Any of the three
; missing makes Firmware\ a folder that can update a healthy device but not
; program a factory-fresh or dead one, which is exactly the machine the kit
; exists for. Checked by the file each failure would otherwise surface
; through at the worst time.
#if !FileExists(MyDeployDir + "\Firmware\CANTripleInitialProgramming.exe")
  #error Payload incomplete: deploy\Firmware\CANTripleInitialProgramming.exe is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\Firmware\bootloader.bin")
  #error Payload incomplete: deploy\Firmware\bootloader.bin is missing -- build the bootloader, then re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\Firmware\openocd\bin\openocd.exe")
  #error Payload incomplete: deploy\Firmware\openocd\bin\openocd.exe is missing -- re-run the `deploy` target.
#endif
#if !FileExists(MyDeployDir + "\Firmware\openocd\scripts\stm32g4x_512k.cfg")
  #error Payload incomplete: deploy\Firmware\openocd\scripts\stm32g4x_512k.cfg is missing -- re-run the `deploy` target.
#endif

; Not a payload file -- this one is checked in -- but SetupIconFile reads it at
; compile time through the same `\..` path, so it gets the same treatment for
; the same reason: a by-name error here beats Inno's "file not found" against a
; path with a `\..` in the middle of it.
#if !FileExists(MyAppIcon)
  #error Icon missing: Icons\icons\app.ico is not in the repository.
#endif


[Setup]
; The AppId is this product's permanent identity on every machine it is ever
; installed on. Windows matches an upgrade to an existing install by AppId
; alone -- change it and the next release installs alongside the old one
; instead of over it, leaving two entries in Apps & Features and two copies on
; disk. THIS GUID MUST NEVER CHANGE, for any version, ever. It was generated
; once, on 2026-08-04, for this project only.
AppId={{B0893867-2A64-40AF-AB98-FC8E4273F700}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppSupportURL}
AppSupportURL={#MyAppSupportURL}
AppUpdatesURL={#MyAppSupportURL}
; Stated as a mixed-licence payload, because that is what it is. The
; application is the smaller part of what lands in {app}: most of the installed
; bytes are Qt 6 under the LGPL v3, plus the MinGW runtime under GPL v3 with the
; GCC Runtime Library Exception. A blanket "(C) Minton Performance" over the
; whole install directory would be a claim over other people's code.
AppCopyright=Copyright (C) 2026 {#MyAppPublisher}. Application: MIT License. Bundled Qt 6: GNU LGPL v3 -- see THIRD-PARTY-NOTICES.txt.

; Off {#MyAppName} rather than a literal, so the install directory, the
; Apps & Features entry and the Start Menu group cannot end up naming three
; different products -- which is exactly what a hard-coded copy invites the
; next time this program is renamed.
;
; The vendor folder is the Program Files convention for a publisher who may
; ship more than one program: "...\Minton Performance\CAN Triple Device
; Manager". Note it only affects a FRESH install -- see the UsePreviousAppDir
; note further down; an upgrade stays wherever the previous run recorded.
DefaultDirName={autopf}\{#MyAppVendor}\{#MyAppName}
DefaultGroupName={#MyAppName}
; Without this, {group} resolves to the PREVIOUS install's folder name, so an
; upgrade from 0.1.0 would delete the old Start Menu folder in [InstallDelete]
; and then recreate it under that same old name -- the rename would never reach
; the Start Menu. DisableProgramGroupPage=yes below means the user never sees
; the page and so can never correct it by hand, which is what makes the default
; actively wrong here rather than merely surprising.
UsePreviousGroup=no
; One program, one shortcut -- there is nothing for the user to decide on the
; Start Menu folder page, so it is skipped. AllowNoIcons is still set so the
; "Don't create a Start Menu folder" box appears if that page is ever restored.
DisableProgramGroupPage=yes
AllowNoIcons=yes

; The licence is presented and accepted before anything is written. Both of
; these read the deploy/ copies -- the very files [Files] installs -- and not
; the sources the deploy target staged them from (the repository's own
; COPYING, and installer\THIRD-PARTY-NOTICES.txt).
;
; That is the whole point of pointing them here. Sourcing the wizard from one
; place and the install directory from another is two copies of the same two
; documents, and the gap between them opens for real on the by-hand ISCC route
; in installer/README.md, which unlike the CMake `installer` target does not run
; `deploy` first: edit the notices, compile, and the acceptance page shows the
; new text while the old text is what lands in {app}. Nobody would catch it, and
; the document the user agreed to would not be the document on their disk.
; Reading both from deploy/ closes that by construction, rather than by everyone
; remembering to re-run a target.
;
; The by-name guards near the top cover both files, so an unbuilt deploy/ fails
; with a message naming the file and the target instead of a bare "file not
; found" from the compiler.
;
; COPYING is the APPLICATION's licence and only that. It is not the licence of
; most of what is about to be installed, and an acceptance page showing it alone
; invites the reader to conclude otherwise. Two things fix that without adding a
; page anyone has to get past to install: the [Messages] override below says so
; in one sentence on the acceptance page itself, and InfoAfterFile puts the full
; third-party notices up at the end of the run, where nobody is waiting.
LicenseFile={#MyDeployDir}\COPYING.txt
InfoAfterFile={#MyDeployDir}\THIRD-PARTY-NOTICES.txt

OutputDir={#MyRepoRoot}\dist
OutputBaseFilename=CANTripleDeviceManager-{#MyAppVersion}-Setup

; ~70 MB of Qt DLLs compresses hard and they are all installed together, so
; solid compression is a straight win: one dictionary across the whole set
; instead of one per file.
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; 64-bit MinGW build (Qt 6.7 mingw_64, GCC 13.1). It cannot run on 32-bit
; Windows and it must install to the real Program Files, not the WOW64
; redirection of it -- ArchitecturesInstallIn64BitMode is what makes {autopf}
; resolve to "C:\Program Files" instead of "C:\Program Files (x86)".
; x64compatible (rather than plain x64) also admits Windows on ARM, where the
; x64 emulator runs this build fine.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Qt 6.7 dropped Windows 8.1 and earlier; 10.0 is the floor the platform plugin
; actually supports, so refuse the install rather than let it fail at launch.
MinVersion=10.0

; Default to a machine-wide install in Program Files, but keep a per-user
; install reachable -- a locked-down workshop or fleet laptop is the normal case
; for this tool, and someone who cannot elevate should not dead-end on the UAC
; prompt. "dialog" is what makes that route findable, and it is worth being
; precise about the cost: it shows the install-mode page to EVERY user, not just
; to those who lack administrator rights. One extra page for an administrator is
; the price of the other case working at all without anyone knowing a switch
; exists.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

; Setup.exe's own icon in Explorer, on the download bar and on the UAC prompt.
; Without it the generated installer wears Inno's stock icon, which is the
; visual half of the same problem the version resource below fixes.
SetupIconFile={#MyAppIcon}

; The uninstaller's icon in Apps & Features, taken from the installed
; executable. That directive has always been here, but until the exe gained a
; version resource and an icon (resources/app.rc, compiled by windres -- see
; CMakeLists.txt) there was nothing in the file for Windows to draw, so the
; entry showed the generic placeholder. It resolves to a real icon now.
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

; Version resource for Setup.exe itself. Without these the generated installer
; shows up in Task Manager and on the UAC prompt as an unnamed "Setup", which
; is exactly the sort of thing that makes people cancel.
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup
VersionInfoCompany={#MyAppPublisher}
; Same honesty as AppCopyright above, trimmed to fit a version resource field.
VersionInfoCopyright=Copyright (C) 2026 {#MyAppPublisher}. Application: MIT License. Bundled Qt 6: GNU LGPL v3.

; Installing over a running copy.
;
; There is deliberately no AppMutex here. AppMutex is how Inno normally detects
; a running instance, but it only works if the application creates a named
; mutex -- and this one does not: there is no CreateMutex, QSharedMemory,
; QLockFile or QLocalServer anywhere in src/, and main() does no instance check
; at all. The app is written to tolerate several copies running at once
; (HelpWindow::prepareCollection even has a branch for a second instance
; holding the help engine open).
;
; So detection goes through the Restart Manager instead, which identifies the
; offending process by the file handles it holds rather than by any cooperation
; from the program. That works here precisely because the exe and its DLLs are
; the files being replaced. Setup asks the running copy to close; because the
; app's closeEvent runs the usual "save your changes?" prompt, an operator with
; unsaved work still gets asked, and cancelling that prompt simply falls back to
; Setup's "close it yourself" page.
;
; RestartApplications=no because relaunching would be a lie. The app restores
; nothing: no window geometry, no reopened document, no remembered COM port or
; baud rate (all of that is chosen fresh every run). A copy silently brought
; back up after the install would look like the session survived when in fact
; it is an empty document.
CloseApplications=yes
RestartApplications=no

; [UninstallDelete] below names a path in a roaming profile. That is a per-user
; constant in an admin-mode installer, which normally earns a compile-time
; warning. The warning is about *installing* into a user area, and this script
; never does: the only user-area operation is deleting a directory the
; application itself generated. Which profile {userappdata} actually resolves to
; during an elevated uninstall is a real limitation, but a different one -- it
; is spelled out over the [UninstallDelete] entries rather than waved away here.
UsedUserAreasWarning=no


[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"


[Messages]
; The default wording ("Please read the following License Agreement...") reads
; as though the agreement on screen governs everything being installed. It does
; not, so one clause is added naming the other licence in the payload. This is a
; label above the licence memo, not the memo itself -- it has room for a
; sentence, not for an inventory, which is what THIRD-PARTY-NOTICES.txt and the
; InfoAfterFile page are for.
LicenseLabel3=Please read the following License Agreement. You must accept its terms before continuing. It covers CAN Triple Device Manager itself; the bundled Qt 6 libraries are used under the GNU LGPL v3 (see THIRD-PARTY-NOTICES.txt).


[Tasks]
; Unchecked by default. A desktop icon is a preference, not a default.
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked


[InstallDelete]
; Clear the Qt plugin directories before laying down the new ones.
;
; Ordinary DLLs need no such treatment: they are loaded by name from the exe's
; import table, so one left behind by an older release is inert. Plugins are
; the exception -- Qt *scans* these directories at startup and tries to load
; what it finds. A plugin from a different Qt build refuses to load, and
; depending on which directory it is in the symptom ranges from a missing image
; format to "could not find or load the Qt platform plugin windows", with
; nothing on screen to suggest a leftover file is the cause. Cheaper to wipe.
Type: filesandordirs; Name: "{app}\generic"
Type: filesandordirs; Name: "{app}\iconengines"
Type: filesandordirs; Name: "{app}\imageformats"
Type: filesandordirs; Name: "{app}\networkinformation"
Type: filesandordirs; Name: "{app}\platforms"
Type: filesandordirs; Name: "{app}\sqldrivers"
Type: filesandordirs; Name: "{app}\styles"
Type: filesandordirs; Name: "{app}\tls"

; Leftovers from a 0.1.0-era install, back when this program was called
; "CAN Triple Manager".
;
; The AppId is unchanged -- deliberately, it is what makes this an upgrade
; rather than a second product -- so Setup finds the existing install and
; installs over it. Everything it lays down is now named CANTripleDeviceManager,
; and nothing overwrites a file that has changed its name: without these
; entries the old executable would sit in the install directory forever,
; runnable, launching a program the shortcuts no longer point at. Same for the
; Start Menu group, which is created from DefaultGroupName and therefore now
; appears under the new name beside the old one.
;
; Note {app} here is the OLD directory on such an upgrade. DefaultDirName only
; chooses a path for a FRESH install; an upgrade reuses whatever the previous
; run recorded, so a machine that had 0.1.0 keeps "...\CAN Triple Manager" as
; its install directory under the new name. That is cosmetic and not worth
; moving an installed program over; a clean install gets the new path.
;
; Every one of these can be deleted from this script once no 0.1.0-era install
; is left in the field. They are pure upgrade archaeology and they do nothing
; on a clean machine.
Type: files; Name: "{app}\CANTripleManager.exe"
Type: filesandordirs; Name: "{autoprograms}\CAN Triple Manager"
; The old desktop shortcut, if the user asked for one. It points at the
; executable being deleted above, so it is dead either way -- and if the
; desktopicon task is left unticked on this run it is simply not replaced,
; which is the same answer the user gives by leaving it unticked.
Type: files; Name: "{autodesktop}\CAN Triple Manager.lnk"


[Dirs]
; THE PRODUCT'S OWN FILE STRUCTURE, and the permission that makes it usable.
;
; The program keeps two libraries beside its own executable:
;
;     {app}\Configurations              .ct3 / .ct3s configurations
;     {app}\Communications Templates    .ct3t communications templates
;
; Program Files grants write to TrustedInstaller, SYSTEM and Administrators and
; to nobody else, and the account the program RUNS as is normally none of those
; -- a standard user is refused outright, and even an administrator runs with a
; filtered token unless the program was explicitly elevated. The executable is
; x64, and 64-bit processes never receive UAC file virtualization (that
; VirtualStore fallback only ever applied to 32-bit binaries), so without the
; grant below a Save into either folder does not quietly land somewhere else
; that works. It simply fails.
;
; users-modify is therefore not decoration. Drop these two lines and the two
; features that write here stop working for every non-elevated user, which is
; every user. ct::ensureWritableDirectory() probes for exactly this and reports
; it by name, so the failure is at least legible -- but it is still a failure.
;
; SCOPE, deliberately: the grant is on these two subdirectories and NOT on
; {app}. Everything else the installer lays down -- the executable, the Qt DLLs,
; the firmware payload, the OpenOCD kit -- keeps the default read-only ACL, so a
; writable folder here cannot become a way to replace something that gets
; loaded or run. Neither folder is on any DLL or executable search path, and
; nothing in them is ever executed: a .ct3 and a .ct3t are both encrypted
; blob, both parsed by this program's own readers.
;
; WHAT IT COSTS, stated rather than discovered: every account on the machine
; shares one library and may overwrite or delete another account's files. On a
; shared workshop bench that is the point of siting them here. It is still true.
Name: "{app}\Configurations"; Permissions: users-modify
Name: "{app}\Communications Templates"; Permissions: users-modify

[Files]
; The entire deploy/ tree, recursively, with no filtering. Every part of it is
; load-bearing and the non-obvious ones are the ones that look most droppable:
;
;   platforms\qwindows.dll  -- mandatory. Without it Qt aborts before main()
;                              gets anywhere, with "no Qt platform plugin could
;                              be initialized".
;   sqldrivers\qsqlite.dll  -- the Help window's collection file (cantriple.qhc)
;                              is an SQLite database; no driver, no manual.
;   libgcc_s_seh-1.dll,     -- the MinGW runtime this build links against
;   libstdc++-6.dll,           (GCC 13.1). Not a system component and not the
;   libwinpthread-1.dll        MSVC redistributable -- nothing on the target
;                              machine provides these, and the copies bundled
;                              with Qt's own GCC 11.2 are too old (see the
;                              deploy target's comment in CMakeLists.txt).
;   cantriple.qch/.qhc      -- the compiled manual. HelpWindow reads the pair
;                              from applicationDirPath(); these two files are
;                              the only thing the program ever reads out of its
;                              install directory.
;   Drivers\stlink\*        -- ST's stsw-link009 v3 driver package, verbatim.
;                              [Run] executes dpinst out of the INSTALLED copy,
;                              so the drivers must land in {app} first; keeping
;                              them there afterwards also gives a technician
;                              something to run by hand on a machine where the
;                              per-user install skipped the elevated step.
;   Firmware\*.ctf          -- the firmware image this release pairs with,
;                              named with the firmware's own version. The
;                              known place Online -> Update Firmware browses
;                              to, and what lets a bench machine restore a
;                              unit with nothing but this install.
;   Firmware\CANTripleInitialProgramming.exe, bootloader.bin, openocd\
;                           -- the initial-programming kit: programs the
;                              bootloader and application over the board's
;                              built-in ST-LINK, so a factory-fresh unit gets
;                              its first firmware -- and a bricked one comes
;                              back to life -- with no other tooling. The
;                              folder is self-contained on purpose -- copied
;                              wholesale onto a USB stick it still works.
;   COPYING.txt and the     -- the licences. The MIT notice must accompany the
;   three notice files         app and LGPLv3 section 4 requires Qt's to travel
;                              with the binaries; the CMake deploy target
;                              stages them into deploy/ so that this installer and
;                              CANTripleDeviceManager-windows-x64.zip inherit
;                              them from the SAME place. That is why there is no
;                              [Files] entry of their own below any more: a
;                              second entry would install the same bytes twice
;                              and give two things to keep in step. The wizard
;                              draws from here too -- LicenseFile and
;                              InfoAfterFile in [Setup] point at these copies --
;                              so the pages the user reads and the files on
;                              their disk are one set, not two.
;
; ignoreversion, not the default version comparison: these binaries are a
; matched set produced by one windeployqt run against one Qt build. "Newer
; version already present" is not a reason to keep a file here -- a Qt DLL from
; another install being preserved because its version resource happens to be
; higher is a mismatched-set bug waiting to happen. Always overwrite.
;
; One entry, deliberately. Everything this installer places in {app} comes
; through this line, including the licences -- see the note on them above, and
; the compile-time guard near the top that refuses to build if the deploy target
; failed to stage them.
Source: "{#MyDeployDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion


[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
; The initial programming tool earns a Start Menu entry because it is the
; first thing run on a factory-fresh board and the last resort for a dead
; one: an operator staring at either should not also have to know which
; subfolder of Program Files holds it. It is a console program; the shortcut
; opens its own window and the tool waits for Enter before closing, so
; double-click works.
Name: "{group}\CAN Triple Initial Programming Tool"; Filename: "{app}\Firmware\CANTripleInitialProgramming.exe"; WorkingDir: "{app}\Firmware"
; Apps & Features is the canonical way to uninstall, but this tool lives on
; offline bench machines whose operators go to the Start Menu for everything,
; so the entry earns its place.
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
; {autodesktop} follows the install scope: all users for an admin install, the
; installing user's desktop for a per-user one.
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon


[Run]
; The device's USB drivers, installed into the machine's driver store so the
; hardware enumerates the moment it is first plugged in. /S is ST's own
; silent switch -- the exact invocation their
; stlink_winusb_silent_install.bat uses -- and installing BEFORE the device
; is connected is what ST's readme asks for, which an installer that runs
; before anyone unboxes hardware satisfies by construction. On a machine
; where the driver already exists (Windows Update got there first, or an ST
; tool installed it) dpinst is a no-op that keeps the newer of the two.
;
; Admin installs only: the driver store is machine state and dpinst cannot
; write it unelevated. A per-user install skips this entry silently rather
; than failing it -- the program itself is fully functional without the
; driver until the hardware is plugged in, and installer\README.md covers
; running dpinst by hand for that case.
;
; dpinst's exit code is a bit-field, not a status -- staging a driver for
; hardware that is not yet connected sets the high word, which is this
; installer's NORMAL outcome on a fresh bench machine. [Run] ignores exit
; codes by default; that default is load-bearing here, so no Flags that
; would change it.
Filename: "{app}\Drivers\stlink\dpinst_amd64.exe"; Parameters: "/S"; \
    StatusMsg: "Installing ST-Link USB drivers..."; Check: IsAdminInstallMode

; runasoriginaluser matters more than it looks. On an elevated install this
; entry would otherwise start the app as the *administrator* account, and the
; app's per-user state would be created in that account's profile: the recent
; files list under HKCU\Software\Minton Performance, and the help cache under its
; %APPDATA%. The person who then uses the program would see neither. Launch it
; as the user who started Setup so its state lands where it belongs.
Filename: "{app}\{#MyAppExeName}"; \
    Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; \
    Flags: nowait postinstall skipifsilent runasoriginaluser


[UninstallDelete]
; The app copies its manual out of the read-only install directory into the
; user's roaming profile, because the Qt help engine writes to its own
; collection (search index, filters) and cannot do that in Program Files. See
; HelpWindow::prepareCollection() in src/ui/help_window.cpp. Those copies are
; generated data, not user data -- they are byte-for-byte the cantriple.qch /
; cantriple.qhc this installer just placed, plus the .cantriple\fts full-text
; index the engine builds from them -- so removing them is a clean-up, not a
; loss. Leaving them behind orphans a few hundred KB per user profile that
; nothing will ever collect.
;
; Be clear about whose profile that is, because it is not "everyone who ran the
; program":
;
;   Per-user install -- the uninstaller runs unelevated as that user, so
;   {userappdata} is their profile and this removes exactly the right cache.
;   The common case on the locked-down machines this tool lives on, and the
;   case this entry is really for.
;
;   Per-machine install -- the uninstaller is elevated, and {userappdata} is
;   the profile of the account it is elevated AS. If the operator is a local
;   administrator that is still their own profile and the cleanup lands. If they
;   had to supply someone else's administrator credentials, it deletes that
;   administrator's copy -- which is almost certainly not there -- and leaves
;   the operator's own behind.
;
; Either way it never touches OTHER users' copies. Every profile that has opened
; the manual has its own, and an uninstaller has no way to walk them. So this is
; a best-effort tidy-up, not a guarantee, and the honest description of the
; residue is "up to a few hundred KB in each profile the cleanup could not
; reach". Kept anyway: it is free, its failure mode is deleting nothing, and it
; is correct in the case that dominates. Doing better would mean an Active Setup
; stub or an uninstaller that walks C:\Users -- both worse than the leak.
; The path is %APPDATA%\<organization>\<application name>\help, and the second
; component is the application name main.cpp passes to
; QCoreApplication::setApplicationName() -- so renaming the program moved the
; cache. Both names are listed: the current one, and the one a machine that ran
; 0.1.0 still has sitting in its profile. The old entry costs one line and is
; the only thing that will ever collect that directory; without it the rename
; turns a tidy-up into a permanent orphan.
; Three spellings, because BOTH halves of the path have been renamed: the
; organization went CANTriple -> Minton Performance and the application went
; CAN Triple Manager -> CAN Triple Device Manager. Only the first line describes
; where a current build writes; the other two are the caches earlier builds left
; behind, and each costs one line to collect.
Type: filesandordirs; Name: "{userappdata}\Minton Performance\CAN Triple Device Manager\help"
Type: filesandordirs; Name: "{userappdata}\CANTriple\CAN Triple Device Manager\help"
Type: filesandordirs; Name: "{userappdata}\CANTriple\CAN Triple Manager\help"
; Only if empty: the parent directories are shared namespace. Any other
; Minton Performance tool that ever stores something there keeps its data.
Type: dirifempty; Name: "{userappdata}\Minton Performance\CAN Triple Device Manager"
Type: dirifempty; Name: "{userappdata}\Minton Performance"
Type: dirifempty; Name: "{userappdata}\CANTriple\CAN Triple Device Manager"
Type: dirifempty; Name: "{userappdata}\CANTriple\CAN Triple Manager"
Type: dirifempty; Name: "{userappdata}\CANTriple"

; The vendor folder in Program Files. Inno removes {app} itself but never its
; parent, so without this an uninstall leaves an empty "Minton Performance"
; directory behind. dirifempty for the same reason as above: a second program
; from the same workshop installed alongside this one must keep its home.
; The two libraries, and then their parents, innermost first -- dirifempty only
; fires on an empty directory, so the order matters: {app} cannot go until its
; subdirectories have, and the vendor folder cannot go until {app} has. A user
; who saved even one configuration keeps all of it, which is the point.
Type: dirifempty; Name: "{app}\Configurations"
Type: dirifempty; Name: "{app}\Communications Templates"
Type: dirifempty; Name: "{app}"
Type: dirifempty; Name: "{autopf}\{#MyAppVendor}"

; Deliberately NOT removed:
;
;   HKCU\Software\Minton Performance\CAN Triple Device Manager -> recentFiles (REG_MULTI_SZ)
;       The only setting the program keeps, and it is the user's own list of
;       paths. It costs nothing to leave, it survives a reinstall (which is the
;       behaviour anyone reinstalling actually wants), and an uninstall that
;       silently discards it has thrown away something the user made. Anyone who
;       wants a truly clean slate can delete that key by hand.
;
;   HKCU\Software\CANTriple\CAN Triple Manager -> recentFiles (REG_MULTI_SZ)
;   HKCU\Software\CANTriple\CAN Triple Device Manager -> recentFiles (REG_MULTI_SZ)
;       The same value under the two pre-rename spellings -- the first from
;       before the program was renamed, the second from the window between that
;       rename and the organization becoming Minton Performance. QSettings keys
;       the branch off BOTH names, so neither rename migrated the list -- each
;       started a new one, and an earlier user's recent files sit behind at the
;       old key. Nothing here copies them across and nothing here deletes them; that
;       is a decision for a release note, not for an installer quietly rewriting
;       a user's registry. Listed so the leftover is on the record rather than a
;       surprise. (The help cache above is different and IS collected: it is
;       regenerable data this program wrote for itself, not something a user
;       made.)
;
;   .ct3 / .ct3s configurations, PDF and text summaries, .asc CAN logs,
;   .ct3t communications templates
;       Written wherever the user chose in a save dialog. The installer has no
;       idea where those are and no business guessing.
;
;   {app}\Configurations  and  {app}\Communications Templates
;       THE EXCEPTION, and the one worth reading. These two are created by the
;       [Dirs] section above and are where the program OFFERS to save, so most
;       of a user's configurations and templates will be in them.
;
;       Inno removes what it INSTALLED. Nothing was installed into either folder
;       -- they ship empty -- so every file in them is the user's own and every
;       one of them SURVIVES the uninstall, as does the folder holding it. That
;       is the intended outcome: an uninstaller that deleted them would be
;       deleting the user's work, and a reinstall puts the program back around
;       files that never went anywhere.
;
;       The cost is an orphan: uninstall a machine for good and those two
;       folders sit in Program Files under a vendor directory with nothing else
;       in it. Left deliberately, because "your configurations are still there"
;       is a better surprise than the other one. The dirifempty entries below
;       collect them when they ARE empty, so a machine that never saved anything
;       leaves nothing behind.
;
;   Documents\CAN Triple Device Manager\
;       THE PROGRAM CREATES THIS ITSELF, which the paragraph above does not
;       cover, so it is listed separately rather than left to be discovered.
;       Two sub-folders, each made on demand the first time its feature is
;       used and never at install time:
;
;           Firmware Update Backups    the device's configuration, read back
;                                      and saved before an update overwrites it
;           Scripts                    the default home for saved .lua Device
;                                      Scripts
;
;       Not removed, and the folder name is the reason: everything under it is
;       the user's own work, sitting inside the user's own Documents. An
;       uninstaller that deletes a folder below "Documents" has deleted
;       documents, whatever was actually in it.
;
;       Nothing else is written anywhere: no logs, no temp files, no autosave,
;       no crash-recovery file.


; ---------------------------------------------------------------------------
; No [Registry] section, and specifically NO .ct3 / .ct3s file associations.
;
; This is not an oversight. src/main.cpp parses exactly one command-line option
; (--screenshots, an undocumented development switch) and ignores positional
; arguments entirely -- see main.cpp lines 141-151. Shell associations work by
; handing the file's path to the program as a positional argument, so
; double-clicking a configuration would launch CAN Triple Device Manager
; showing a new, empty document with no indication that the file was ignored.
; That is a worse experience than no association at all: the user believes
; their configuration opened and is looking at a blank one.
;
; To add them later, in this order:
;   1. Teach main.cpp to accept a positional file argument and open it, via
;      QCommandLineParser::positionalArguments(). MainWindow already has the
;      loader (Open... at main_window.cpp:402 picks the .ct3/.ct3s reader from
;      the file's contents rather than its extension, so one code path handles
;      both), and it needs the same maybeSave/error handling the menu item has.
;   2. Decide what a .ct3 DOCUMENT should look like in Explorer. The application
;      icon exists now (Icons\icons\app.ico, wired into SetupIconFile above and
;      into the executable itself), and DefaultIcon could simply point at the
;      exe -- but a document that wears the program's own icon is a common and
;      mediocre answer. A distinct document icon is the better one.
;   3. Only then add the [Registry] entries, with ChangesAssociations=yes in
;      [Setup] so Explorer is told to refresh, and uninsdeletekey/
;      uninsdeletevalue flags on every one of them so uninstalling does not
;      leave dead ProgIDs behind.
; ---------------------------------------------------------------------------
