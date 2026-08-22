// Where this program keeps files, and which of the two roots each kind lives
// under.
//
// ONE place that knows the layout, because three separate files had each
// spelled a folder name out for themselves — the firmware updater's backups,
// the script editor's scripts, and the communications templates — and a fourth
// was about to. Three copies of a folder name is three chances for one of them
// to drift, and the symptom of that drift is a user's files quietly appearing
// in two places with no way to tell which is which.
//
// ---------------------------------------------------------------------------
// TWO ROOTS, AND WHY IT IS NOT ONE
//
//   THE INSTALL DIRECTORY — beside the executable.
//
//       Configurations              .ct3 / .ct3s
//       Communications Templates    .ct3t
//       Firmware                    the paired .ctf, staged by the installer
//
//     The first two are the product's own file structure, sited with the
//     program so a machine has ONE library that every account on it shares,
//     wherever the program was installed. That is a deliberate choice with a
//     real cost, and the cost is spelled out below rather than discovered
//     later.
//
//     Firmware is the odd one out and is READ ONLY to this program: the
//     installer stages can-triple-<version>.ctf there and nothing here ever
//     writes to it, which is why it is absent from the [Dirs] grants below and
//     should stay absent. All the program does is open Select Firmware Image
//     where the image already is.
//
//   DOCUMENTS/CAN Triple Device Manager — the user's own folder.
//
//       Firmware Update Backups     the device's config, read back before an update
//       Scripts                     saved Device Script .lua files
//
//     These two are NOT moved, and that is not indecision. They shipped in an
//     earlier release, so a user's existing backups and scripts are already
//     sitting there; relocating the folder would not move the files, it would
//     abandon them somewhere the program no longer looks. A firmware backup in
//     particular is the file somebody goes hunting for after an update went
//     wrong, which is the worst possible time to find it missing.
//
// ---------------------------------------------------------------------------
// THE INSTALL DIRECTORY IS NOT WRITABLE BY DEFAULT, AND WHAT IS DONE ABOUT IT
//
// The installer is PrivilegesRequired=admin into {autopf}, so the program lives
// in Program Files, whose ACL grants write to TrustedInstaller, SYSTEM and
// Administrators and to nobody else. The account the program RUNS as is
// normally none of those: a standard user is refused outright, and even an
// administrator runs with a filtered token unless the program was explicitly
// elevated, so the write is refused for them too. The executable is x64, and
// 64-bit processes never receive UAC file virtualization — that VirtualStore
// fallback only ever applied to 32-bit binaries — so a refused write does not
// quietly land somewhere else that works. It simply fails.
//
// So the INSTALLER grants it, on these two subdirectories and nothing else:
//
//     [Dirs] Name: "{app}\Configurations";           Permissions: users-modify
//     [Dirs] Name: "{app}\Communications Templates"; Permissions: users-modify
//
// That is what makes the layout above work at all, and it is the reason those
// two entries must not be dropped from the .iss. Three consequences follow, and
// a caller should know them:
//
//   1. A build that no installer ever ran — a development build, a copy of
//      deploy/ on a stick — puts these folders beside the executable wherever
//      that is, and those locations are ordinarily writable. Nothing special
//      happens; it simply works.
//   2. An INSTALLED copy whose [Dirs] entries never ran (installed by an older
//      Setup, or an environment whose policy strips the ACE) refuses the write.
//      ensureWritableDirectory() below is what turns that into a sentence
//      naming the folder rather than a silent failure.
//   3. Every account on the machine shares one library and can overwrite
//      another's files. On a shared bench that is the point. It is still worth
//      knowing that the ACL says it is allowed.
//
// Uninstall leaves user-created files behind: Inno removes what it INSTALLED,
// so a .ct3 written into {app}\Configurations afterwards survives, and the
// directory survives with it. Nothing is destroyed. See the .iss's
// "Deliberately NOT removed" block, which says so where an uninstaller's
// behaviour is actually looked up.
#pragma once

#include <QString>

namespace ct {

// The install directory — QCoreApplication::applicationDirPath(). The parent of
// the two product folders below.
QString programRoot();

// Configurations (.ct3, .ct3s). Where the Open and Save dialogs start when the
// document has nowhere of its own yet.
QString configurationsDirectory();

// Communications templates (.ct3t) — Save… / Load… in Communications Setup.
QString commsTemplatesDirectory();

// The firmware image the installer staged — {app}\Firmware, holding
// can-triple-<version>.ctf. Where Update Firmware's Select Firmware Image
// opens, so a bench machine that never saw the repositories has the paired
// image one click away.
//
// Nothing here creates it: cmake/stage_firmware.cmake fills the folder at
// deploy time and the installer ships it. A build no installer ever ran has
// none, so a caller checks exists() and does without rather than making an
// empty one to point at.
QString firmwareImagesDirectory();

// Documents/CAN Triple Device Manager — the parent of the two user folders,
// unchanged from the releases that created them.
QString userFilesRoot();
QString firmwareBackupsDirectory();
QString deviceScriptsDirectory();

// mkpath, and then prove the result is actually writable.
//
// mkpath alone is not enough here and that is the whole reason this is not a
// one-liner: it returns TRUE for a directory that already exists, so an
// installed copy whose folder was created by Setup but never granted
// users-modify passes the check and fails at the write — surfacing as "Access
// is denied" from somewhere deep in a file writer, with no clue which folder or
// why. The probe is a temporary file created and removed in the folder itself,
// because being allowed to write there is the only question worth asking and
// the ACL is too subtle to answer it by reading.
//
// `error` gets a sentence naming the folder and, when it is under Program
// Files, what to do about it. Passing nullptr asks for best-effort.
bool ensureWritableDirectory(const QString &dir, QString *error = nullptr);

} // namespace ct
