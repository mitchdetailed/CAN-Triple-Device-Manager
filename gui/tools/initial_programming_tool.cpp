// CANTripleInitialProgramming — put the bootloader and firmware on a CAN
// Triple over SWD: the first programming of a factory-fresh board, and the
// way back from anything.
//
// The CAN Triple carries an ST-LINK on the board, so the same USB cable the
// Manager talks through also exposes a debug interface that works no matter
// what state the chip's flash is in: blank from the factory, bootloader
// missing, application slot corrupt, or stuck in a reset loop. This tool
// drives that interface (through a bundled OpenOCD) to program the bootloader
// and the application image directly. It depends on NO code running on the
// target, which is the whole point — the serial update path needs a healthy
// bootloader, and this is the tool for when there isn't one.
//
// Because the flasher is external, there is no brick window anywhere in this:
// pull the power mid-programming and the answer is "run it again".
//
// What it deliberately does NOT do:
//   - touch the configuration store, retained values, or access keys — this
//     tool reinstates the PROGRAM, not the device's data (the erase is
//     bounded to the pages the two images occupy, all in bank 1). The two
//     exceptions are --unlock and switching a single-bank unit to dual-bank,
//     both below, which are mass erases by definition;
//   - upload configurations. Configuration transfer belongs to the serial
//     protocol, where the device's own gates — access passwords, upload
//     policy, per-device binding — are enforced. An SWD write would bypass
//     every one of them, which for a path meant to carry SECURE configs is
//     not a shortcut but a hole.
//
// Both images are fully validated in memory before OpenOCD is even invoked:
// the .ctf through fw_image_validate() — the device's own validator, compiled
// in so the two cannot disagree — and the bootloader through the same checks
// the bootloader applies to itself (the .bl_info stamp at 0x200, sane vectors).
// A refused file costs nothing; flash is only touched by images that check out.
//
// Layout: this exe lives in the Firmware/ folder of an install, beside
// bootloader.bin, the can-triple-<version>.ctf it ships with, and an openocd/
// subfolder. The folder is self-contained — copy it wholesale onto a USB stick
// and it works on any machine with the ST-LINK driver (which the same folder's
// parent install provides).
//
//   CANTripleInitialProgramming [--yes] [--unlock] [--bootloader FILE]
//                               [--firmware FILE] [--openocd EXE]
//
// --unlock is the way back from readout protection. A licensed unit sets RDP
// level 1 on itself at boot (firmware 1.0.9, lockReadoutIfLicensed() in the
// firmware), after which the debug port can neither read nor program the
// flash, and the plain run of this tool stops at the probe. Regressing
// to level 0 makes the chip MASS-ERASE itself — bootloader, application,
// configuration, retained values, access keys and the licence page all go —
// which is precisely what level 1 promises. So --unlock does that erase first,
// waits for the reset it causes, and then programs both images exactly as a
// plain run does. The unit comes back blank and unlicensed: issue its licence
// again in the Manager, and it locks itself at the next power-up.
//
// The flash must be in DUAL-BANK mode: option bit DBANK set, which is the
// factory setting. The firmware's flash map is laid out for it, and the
// firmware refuses firmware updates and licences on a unit whose option bytes
// say single-bank ("flash write failed (0x05)"). So before programming, the
// tool reads the option register, and a single-bank unit is switched first:
// mass erase, DBANK written, option
// bytes reloaded; then a fresh probe must read DBANK set, and the chip is
// erased again in the new geometry. Reload, mass erase, program is ST's
// procedure for a bank-mode change. The erase BEFORE the reload is this
// tool's addition, so that the reset the reload causes starts an empty chip
// rather than code laid out for the other geometry. The switch costs the
// stored configuration, which the firmware would not find after it anyway,
// because the store is placed differently in the two modes. So it asks again,
// separately, unless --yes or --unlock (which erases everything already) was
// given.
//
// Exit 0 on success, 1 on any failure, so it can gate a provisioning script.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fw_image.h"

namespace {

// ---------------------------------------------------------------- utilities

std::string exeDir()
{
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

bool fileExists(const std::string &p)
{
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool readFile(const std::string &p, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(n));
    const bool ok = n == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// TCL sees the path inside {braces}, where backslashes are literal — but
// forward slashes work everywhere in OpenOCD and remove the question.
std::string tclPath(std::string p)
{
    for (char &c : p)
        if (c == '\\')
            c = '/';
    return p;
}

const char *describe(uint8_t r)
{
    switch (r) {
    case FW_RESULT_OK:            return "OK";
    case FW_RESULT_BAD_MAGIC:     return "not a CAN Triple firmware image (bad magic)";
    case FW_RESULT_WRONG_PRODUCT: return "image is for a different product";
    case FW_RESULT_BAD_SIZE:      return "image size is out of range";
    case FW_RESULT_BAD_CRC:       return "image is corrupt (CRC mismatch)";
    case FW_RESULT_BL_TOO_OLD:    return "image needs a newer bootloader";
    default:                      return "rejected";
    }
}

// ------------------------------------------------------------- image checks

// The same checks the bootloader applies to itself at boot, applied BEFORE the
// old bootloader is erased: the .bl_info stamp the application reads the
// version from, and a vector table that points somewhere a bootloader can be.
bool validateBootloader(const std::vector<uint8_t> &img, uint32_t *version_out)
{
    if (img.size() < FW_BL_INFO_OFFSET + sizeof(BootloaderInfo)) {
        std::printf("  FAILED  bootloader.bin is only %zu bytes - truncated?\n",
                    img.size());
        return false;
    }
    if (img.size() > FW_BOOTLOADER_SIZE) {
        std::printf("  FAILED  bootloader.bin is %zu bytes; the slot is %u\n",
                    img.size(), (unsigned)FW_BOOTLOADER_SIZE);
        return false;
    }

    uint32_t sp = 0, reset = 0;
    std::memcpy(&sp, img.data(), 4);
    std::memcpy(&reset, img.data() + 4, 4);
    if (sp < 0x20000000u || sp > 0x20020000u) {
        std::printf("  FAILED  initial stack pointer 0x%08X is not in RAM - "
                    "not a bootloader image, or linked for the wrong part\n", sp);
        return false;
    }
    if ((reset & 1u) == 0 || reset < FW_BOOTLOADER_BASE
        || reset >= FW_BOOTLOADER_BASE + FW_BOOTLOADER_SIZE) {
        std::printf("  FAILED  reset vector 0x%08X is outside the bootloader "
                    "slot - linked for the wrong address?\n", reset);
        return false;
    }

    BootloaderInfo info;
    std::memcpy(&info, img.data() + FW_BL_INFO_OFFSET, sizeof(info));
    if (info.magic != FW_BL_INFO_MAGIC) {
        std::printf("  FAILED  no version stamp at 0x200 - not a CAN Triple "
                    "bootloader image\n");
        return false;
    }
    if (info.app_base != FW_APP_BASE) {
        std::printf("  FAILED  bootloader expects the application at 0x%08X, "
                    "this product puts it at 0x%08X - wrong flash-map "
                    "generation\n", info.app_base, (unsigned)FW_APP_BASE);
        return false;
    }
    *version_out = info.version;
    return true;
}

// ------------------------------------------------------------------ openocd

// Run OpenOCD with its output captured to a log file. The log is shown on
// failure (the interesting line is usually ST-LINK enumeration or a target
// voltage) and its last lines double as the success evidence.
//
// logPath is in-out: the caller's preference is beside the exe — where a
// technician working out of a programming folder or USB stick will look — but
// the INSTALLED copy lives under Program Files, which an unelevated process
// cannot write. That exact failure shipped once: the tool validated both
// images, said "programming...", and then refused at the log file before
// OpenOCD ever ran. So an unwritable first choice falls back to %TEMP%, and
// the path reported to the user is the one actually used.
std::string openocdPrefix(const std::string &ocd, const std::string &scripts)
{
    std::string cmd;
    cmd += "\"" + ocd + "\"";
    cmd += " -s \"" + scripts + "\"";
    cmd += " -f interface/stlink.cfg";
    cmd += " -c \"transport select swd\"";
    cmd += " -f stm32g4x_512k.cfg";
    return cmd;
}

// Print roughly the last 25 lines of an OpenOCD log — where ST-LINK
// enumeration failures, target voltage problems and verify mismatches all
// land.
void printLogTail(const std::string &text)
{
    size_t start = text.size();
    for (int lines = 0; start > 0 && lines < 25; --start)
        if (text[start - 1] == '\n')
            ++lines;
    std::fwrite(text.data() + start, 1, text.size() - start, stdout);
}

// Every exit from main goes through here, so a window opened from the Start
// Menu never vanishes before its last line can be read. A wrong answer at the
// prompt, a missing file and a failed validation all used to close it on the
// spot — only the two OpenOCD failures paused — and from the shortcut that
// looks like a tool that does nothing.
int finish(int code, bool assumeYes)
{
    if (!assumeYes) {
        std::printf("\n  Press Enter to close.");
        std::fflush(stdout);
        std::getchar();
    }
    return code;
}

// The readout-protection level OpenOCD's stm32l4x driver reports when it
// probes the flash — "RDP level 0 (0xAA)" on every probe of an open chip — or
// -1 when the log has none. The LAST report wins: it is the chip's current
// state.
int rdpLevelIn(const std::string &text)
{
    const std::string key = "RDP level ";
    const size_t at = text.rfind(key);
    if (at == std::string::npos || at + key.size() >= text.size())
        return -1;
    const char c = text[at + key.size()];
    return (c >= '0' && c <= '2') ? c - '0' : -1;
}

// FLASH_OPTR, the option register the bank-mode decision is read from: bit 22
// is DBANK, and the low byte is the readout-protection level (0xAA level 0,
// 0xCC level 2, anything else level 1). It is read with a plain "mdw", whose
// "0x40022020: ffeff8aa" line does not depend on how a particular OpenOCD
// build chooses to word its flash driver's messages.
const char kOptrRead[] = "mdw 0x40022020";
const char kOptrLine[] = "0x40022020: ";
constexpr uint32_t kOptrDbank = 1u << 22;

// The LAST value read wins, like rdpLevelIn: it is the chip's current state.
bool optrIn(const std::string &text, uint32_t *optr)
{
    const size_t at = text.rfind(kOptrLine);
    if (at == std::string::npos)
        return false;
    const char *start = text.c_str() + at + std::strlen(kOptrLine);
    char *end = nullptr;
    const unsigned long value = std::strtoul(start, &end, 16);
    if (end == start)
        return false;
    *optr = static_cast<uint32_t>(value);
    return true;
}

int rdpLevelOf(uint32_t optr)
{
    const uint32_t rdp = optr & 0xFFu;
    return rdp == 0xAAu ? 0 : (rdp == 0xCCu ? 2 : 1);
}

// "initial-programming-unlock.log" -> "initial-programming-unlock-probe.log":
// a follow-up run's log sits beside the one it follows.
std::string siblingLog(const std::string &logPath, const char *suffix)
{
    const size_t dot = logPath.rfind(".log");
    return (dot == std::string::npos ? logPath : logPath.substr(0, dot)) + suffix + ".log";
}

// A line typed at the console, all of it: a reply longer than a fixed buffer
// would otherwise leave its tail to answer the next prompt, or to satisfy the
// "Press Enter to close" pause before anyone has read the window.
std::string readLine()
{
    std::string line;
    for (int c; (c = std::getchar()) != EOF && c != '\n';)
        line += static_cast<char>(c);
    return line;
}

// YES in any case, and nothing else on the line. Someone who types "yes" means
// yes; a case-sensitive compare used to answer them with "Nothing done" and a
// window that closed on the spot.
bool askYes(const char *prompt)
{
    std::printf("%s", prompt);
    std::fflush(stdout);
    std::string reply = readLine();
    while (!reply.empty() && (reply.back() == '\r' || reply.back() == ' '
                              || reply.back() == '\t'))
        reply.pop_back();
    const size_t lead = reply.find_first_not_of(" \t");
    reply = lead == std::string::npos ? std::string() : reply.substr(lead);
    return _stricmp(reply.c_str(), "yes") == 0;
}

// What the tool says about a unit whose readout protection is set, wherever it
// finds it: the probe before anything is written, or a failed OpenOCD run whose
// log shows it. The wording is the product's, chosen 2026-09-22.
void printProtectedStatement()
{
    std::printf("\n  This unit has readout protection set. Unit appears to have licensed firmware defined.\n"
                "\n  Action Not Completed.\n");
}

// What a failed OpenOCD run most likely means, in words the person at the
// bench can act on. Matched against the bundled OpenOCD's own messages.
//
// The readout-protection statement is given ONLY when the chip reports level 1
// or 2. Advice used to follow any log containing "RDP" — and every probe of an
// OPEN chip prints "RDP level 0" — so any failure at all was reported as a
// protected unit.
void explainFailure(const std::string &text)
{
    if (text.find("open failed") != std::string::npos
        || text.find("libusb_open() failed") != std::string::npos) {
        std::printf("\n  OpenOCD could not open the ST-LINK. Check the USB cable, close\n"
                    "  anything else using the ST-LINK's debug interface\n"
                    "  (STM32CubeProgrammer, another OpenOCD), and make sure the ST-LINK\n"
                    "  USB driver is installed: Start Menu > CAN Triple Device Manager >\n"
                    "  Install ST-Link USB Drivers.\n");
        return;
    }
    if (text.find("init mode failed") != std::string::npos
        || text.find("Target voltage too low") != std::string::npos) {
        std::printf("\n  The ST-LINK was found but the CAN Triple's processor did not\n"
                    "  answer. Check the unit is powered and properly connected, then\n"
                    "  run this again.\n");
        return;
    }
    if (rdpLevelIn(text) >= 1)
        printProtectedStatement();
    // Anything else: the log tail above is the evidence, and no advice here
    // is better than advice that erases a working unit's licence.
}

// Run one command line with its output captured to logPath (which may move,
// see runOpenocd) and hand the log text back. Returns the exit code, or -1 when
// the process could not be started or the log could not be written.
int runLogged(const std::string &cmd, std::string &logPath, std::string &text)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        char tmp[MAX_PATH];
        const DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            // The same file name, in %TEMP%. One shared fallback name used to
            // be enough; with a probe, a bank switch and an unlock each keeping
            // a log of its own, it would leave only the last of them.
            const size_t slash = logPath.find_last_of("\\/");
            logPath = std::string(tmp)
                    + (slash == std::string::npos ? logPath : logPath.substr(slash + 1));
            log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (log == INVALID_HANDLE_VALUE) {
        std::printf("  FAILED  cannot write a log file (tried %s)\n",
                    logPath.c_str());
        return -1;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};

    // CreateProcess may scribble on the command line buffer, so it gets its
    // own mutable copy.
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    const BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    if (!ok) {
        CloseHandle(log);
        std::printf("  FAILED  could not start OpenOCD (%lu)\n", GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(log);

    std::vector<uint8_t> raw;
    readFile(logPath, raw);
    text.assign(raw.begin(), raw.end());
    return int(exitCode);
}

// A read-only look at the chip: the flash driver's probe, which logs "RDP level
// n" and "flash mode : dual-bank" for a person reading the log, and the option
// register itself for the tool. Nothing is halted, erased or written, so it is
// safe against a unit that is running.
int runOpenocdProbe(const std::string &ocd, const std::string &scripts,
                    std::string &logPath, std::string &text)
{
    return runLogged(openocdPrefix(ocd, scripts) + " -c \"init\" -c \"flash probe 0\""
                         + " -c \"" + kOptrRead + "\" -c \"shutdown\"",
                     logPath, text);
}

bool runOpenocd(const std::string &ocd, const std::string &scripts,
                const std::string &blPath, const std::string &appPath,
                std::string &logPath)
{
    std::string cmd = openocdPrefix(ocd, scripts);
    cmd += " -c \"program {" + tclPath(blPath) + "} 0x08000000 verify\"";
    cmd += " -c \"program {" + tclPath(appPath) + "} 0x08004000 verify\"";
    cmd += " -c \"reset run\"";
    cmd += " -c \"shutdown\"";

    std::string text;
    const int exitCode = runLogged(cmd, logPath, text);
    if (exitCode < 0)
        return false;

    // Success is BOTH the exit code and two explicit verifications in the log
    // — one per image. OpenOCD has been known to exit 0 from a shutdown that
    // never programmed anything, so the words are checked, not just the code.
    int verified = 0;
    for (size_t at = 0; (at = text.find("** Verified OK **", at)) != std::string::npos; ++at)
        ++verified;

    if (exitCode == 0 && verified >= 2)
        return true;

    std::printf("\n  FAILED  OpenOCD exit code %d, %d of 2 images verified.\n"
                "  The tail of its log (%s):\n\n",
                exitCode, verified, logPath.c_str());
    printLogTail(text);
    explainFailure(text);
    return false;
}

// --unlock: take the chip back from readout-protection level 1. Writing level 0
// into the option bytes and reloading them makes the chip mass-erase itself,
// which is what level 1 means and why a licensed unit sets it. The two OpenOCD
// commands are the L4/G4 family's documented sequence: "stm32l4x unlock" writes
// the option byte, "stm32l4x option_load" reloads the option bytes, and the
// reload resets the target and drops the debug connection. OpenOCD is expected
// to complain after that point, so success is read from the driver's own
// "unlocked" line rather than from the exit code. The tool then waits out the
// reset before the ordinary programming pass reconnects.
//
// Written from the OpenOCD reference for the stm32l4x driver; the first unit
// to go through it for real is the one to watch.
bool runOpenocdUnlock(const std::string &ocd, const std::string &scripts, std::string &logPath)
{
    std::string cmd = openocdPrefix(ocd, scripts);
    cmd += " -c \"init\"";
    cmd += " -c \"reset halt\"";
    cmd += " -c \"stm32l4x unlock 0\"";
    cmd += " -c \"stm32l4x option_load 0\"";
    cmd += " -c \"shutdown\"";

    std::string text;
    const int exitCode = runLogged(cmd, logPath, text);
    if (exitCode < 0)
        return false;
    // SUCCESS IS NOT IN THIS LOG. The bundled OpenOCD's stm32l4x driver prints
    // nothing when an unlock works — its one message is "%s failed to unlock
    // device" — while two of its FAILURE messages ("flash not unlocked",
    // "options not unlocked") contain the word this used to look for. So the
    // old check read failure as success and success as failure, and a unit
    // that unlocked perfectly was reported as refusing, every time. The chip
    // is asked instead: once the reload's reset and mass erase have run, a
    // fresh probe must report RDP level 0.
    if (text.find("failed to unlock device") == std::string::npos) {
        Sleep(2000); // the option-byte reload resets the chip and runs the mass erase
        std::string probePath = siblingLog(logPath, "-probe");
        std::string probe;
        const int probeExit = runOpenocdProbe(ocd, scripts, probePath, probe);
        if (probeExit >= 0 && rdpLevelIn(probe) == 0)
            return true;
        text += "\n--- probe after the unlock ---\n" + probe;
    }
    std::printf("\n  FAILED  the unlock did not take (OpenOCD exit code %d).\n"
                "  The tail of its log (%s):\n\n",
                exitCode, logPath.c_str());
    printLogTail(text);
    return false;
}

// Switch a single-bank unit to dual-bank, leaving it erased. Two OpenOCD runs,
// each judged by what the chip says rather than by exit codes:
//
//  1. mass erase, write DBANK, reload the option bytes. OpenOCD stops at the
//     first command that fails, so the option byte is never written over an
//     erase that did not complete. The reload resets the target and drops the
//     debug connection, so a complaint AFTER "Option written" means nothing.
//  2. a fresh session must read DBANK set and readout protection at level 0
//     from the option register, and mass-erases again in the new geometry.
//
// Every stop is safe to re-run from: a unit left single-bank is switched again,
// and one that switched but was not erased the second time is programmed by
// the ordinary pass, whose own erase covers the pages it writes.
bool runOpenocdDualBank(const std::string &ocd, const std::string &scripts,
                        std::string &logPath)
{
    std::string cmd = openocdPrefix(ocd, scripts);
    cmd += " -c \"init\"";
    cmd += " -c \"reset halt\"";
    cmd += " -c \"stm32l4x mass_erase 0\"";
    // Bank 0, FLASH_OPTR (offset 0x20 from the flash registers), value, mask:
    // DBANK alone. Every other option bit is left exactly as it was read.
    cmd += " -c \"stm32l4x option_write 0 0x20 0x00400000 0x00400000\"";
    cmd += " -c \"stm32l4x option_load 0\"";
    cmd += " -c \"shutdown\"";

    std::string text;
    const int exitCode = runLogged(cmd, logPath, text);
    if (exitCode < 0)
        return false;
    if (text.find("mass erase complete") == std::string::npos
        || text.find("Option written") == std::string::npos) {
        std::printf("\n  FAILED  the switch to dual-bank did not go through (OpenOCD exit code %d).\n"
                    "  The tail of its log (%s):\n\n",
                    exitCode, logPath.c_str());
        printLogTail(text);
        explainFailure(text);
        return false;
    }

    Sleep(2000); // the option-byte reload resets the chip
    std::string checkPath = siblingLog(logPath, "-check");
    std::string check;
    const int checkExit = runLogged(openocdPrefix(ocd, scripts)
                                        + " -c \"init\" -c \"flash probe 0\" -c \""
                                        + kOptrRead + "\" -c \"reset halt\""
                                        + " -c \"stm32l4x mass_erase 0\" -c \"shutdown\"",
                                    checkPath, check);
    uint32_t optr = 0;
    const bool read = checkExit >= 0 && optrIn(check, &optr);
    if (read && (optr & kOptrDbank) != 0 && rdpLevelOf(optr) == 0
        && check.find("mass erase complete") != std::string::npos)
        return true;

    if (read && (optr & kOptrDbank) == 0)
        std::printf("\n  FAILED  the option bytes still say single-bank (0x%08X) after the\n"
                    "  reload.",
                    static_cast<unsigned>(optr));
    else
        std::printf("\n  FAILED  the unit did not confirm the switch (OpenOCD exit code %d).",
                    checkExit);
    std::printf(" The tail of the log (%s):\n\n", checkPath.c_str());
    printLogTail(check);
    explainFailure(check);
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    std::printf("\nCAN Triple Initial Programming Tool\n"
                "-----------------------------------\n");

    const std::string dir = exeDir();
    std::string blPath = dir + "\\bootloader.bin";
    std::string appPath;
    std::string ocdPath = dir + "\\openocd\\bin\\openocd.exe";
    std::string scripts = dir + "\\openocd\\scripts";
    bool assumeYes = false;
    bool unlock = false;
    // Known before the first possible exit, so even the usage message pauses
    // exactly when a run would have.
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--yes")
            assumeYes = true;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--yes")
            assumeYes = true;
        else if (a == "--unlock")
            unlock = true;
        else if (a == "--bootloader" && hasNext)
            blPath = argv[++i];
        else if (a == "--firmware" && hasNext)
            appPath = argv[++i];
        else if (a == "--openocd" && hasNext)
            ocdPath = argv[++i];
        else {
            std::printf("usage: CANTripleInitialProgramming [--yes] [--unlock] "
                        "[--bootloader FILE] [--firmware FILE] [--openocd EXE]\n");
            return finish(1, assumeYes);
        }
    }

    // No --firmware: take the highest-versioned can-triple-*.ctf beside the
    // exe. Decided by the VERSION IN THE IMAGE HEADER, not the filename — the
    // filename is a courtesy, the header is load-bearing.
    if (appPath.empty()) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\can-triple-*.ctf").c_str(), &fd);
        uint64_t best = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::string cand = dir + "\\" + fd.cFileName;
                std::vector<uint8_t> img;
                if (!readFile(cand, img))
                    continue;
                const FwImageHeader *hdr =
                    fw_image_header(img.data(), (uint32_t)img.size());
                if (!hdr)
                    continue;
                const uint64_t v = ((uint64_t)hdr->fw_version_major << 32)
                                 | ((uint64_t)hdr->fw_version_minor << 16)
                                 | hdr->fw_version_patch;
                if (appPath.empty() || v > best) {
                    best = v;
                    appPath = cand;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (appPath.empty()) {
            std::printf("  FAILED  no can-triple-*.ctf found beside this "
                        "program and no --firmware given\n");
            return finish(1, assumeYes);
        }
    }

    if (!fileExists(ocdPath)) {
        // Developer convenience: fall back to the PlatformIO package this
        // project builds with, so the tool runs from a checkout too.
        const char *profile = std::getenv("USERPROFILE");
        if (profile) {
            const std::string pio = std::string(profile)
                + "\\.platformio\\packages\\tool-openocd";
            if (fileExists(pio + "\\bin\\openocd.exe")) {
                ocdPath = pio + "\\bin\\openocd.exe";
                scripts = pio + "\\openocd\\scripts";
            }
        }
    }
    if (!fileExists(ocdPath)) {
        std::printf("  FAILED  openocd not found at %s\n", ocdPath.c_str());
        return finish(1, assumeYes);
    }
    // The 512K-bank target config normally sits in the bundled scripts dir;
    // when running against the PlatformIO fallback it is not there, so look
    // beside the exe as well and pass it by absolute path in that case.
    if (!fileExists(scripts + "\\stm32g4x_512k.cfg")
        && !fileExists(dir + "\\stm32g4x_512k.cfg")) {
        std::printf("  FAILED  stm32g4x_512k.cfg not found in %s or beside "
                    "this program - without it OpenOCD uses the stock 128 KB "
                    "flash map\n", scripts.c_str());
        return finish(1, assumeYes);
    }

    // ---------------------------------------------------- validate both images

    std::vector<uint8_t> bl, app;
    if (!readFile(blPath, bl)) {
        std::printf("  FAILED  cannot read %s\n", blPath.c_str());
        return finish(1, assumeYes);
    }
    if (!readFile(appPath, app)) {
        std::printf("  FAILED  cannot read %s\n", appPath.c_str());
        return finish(1, assumeYes);
    }

    uint32_t blVersion = 0;
    if (!validateBootloader(bl, &blVersion))
        return finish(1, assumeYes);

    // The device's own validator, against the bootloader ABOUT TO BE
    // INSTALLED — not whatever the unit currently runs, which may be nothing.
    const uint8_t r = fw_image_validate(app.data(), (uint32_t)app.size(), blVersion);
    if (r != FW_RESULT_OK) {
        std::printf("  FAILED  %s: %s\n", appPath.c_str(), describe(r));
        return finish(1, assumeYes);
    }
    const FwImageHeader *hdr = fw_image_header(app.data(), (uint32_t)app.size());

    std::printf("\n  bootloader : %s  (v%u, %zu bytes)\n",
                blPath.c_str(), blVersion, bl.size());
    std::printf("  firmware   : %s  (%u.%u.%u, %zu bytes, validated)\n",
                appPath.c_str(), hdr->fw_version_major, hdr->fw_version_minor,
                hdr->fw_version_patch, app.size());
    std::printf("\n  This programs the bootloader at 0x08000000 and the\n"
                "  application at 0x08004000 over the built-in ST-LINK.\n"
                "  The stored configuration and retained values are NOT touched,\n"
                "  unless the unit's flash turns out to be in single-bank mode:\n"
                "  switching it to dual-bank erases the chip, and you are asked first.\n"
                "  The firmware installed here is a starting point - pick the\n"
                "  firmware you want inside the CAN Triple Device Manager\n"
                "  (Online > Update Firmware) once the device is running.\n"
                "  Safe to re-run at any time, including after a failed attempt.\n\n");
    if (unlock)
        std::printf("  --unlock: readout protection is removed FIRST, and that is a mass\n"
                    "  erase of the whole chip. The bootloader, the firmware, the stored\n"
                    "  configuration, retained values, access passwords and the firmware\n"
                    "  licence are all erased before the two images are programmed. The\n"
                    "  unit comes back blank and unlicensed: issue its licence again in\n"
                    "  the Manager (Online > Firmware License Manager), and it locks itself\n"
                    "  at the next power-up.\n\n");

    if (!assumeYes && !askYes("  Type YES to program the device: ")) {
        std::printf("  Nothing done - type YES to program the device.\n");
        return finish(1, assumeYes);
    }

    if (unlock) {
        std::printf("\n  removing readout protection (this erases the chip)...\n");
        std::string unlockLog = dir + "\\initial-programming-unlock.log";
        if (!runOpenocdUnlock(ocdPath, scripts, unlockLog)) {
            std::printf("\n  Nothing was programmed. Fix the cause above and run again.\n");
            return finish(1, assumeYes);
        }
    }

    // ------------------------------------------------- the flash's bank mode
    //
    // Read before anything is programmed: a single-bank unit is switched first,
    // and the switch erases the chip, so the programming pass must come after
    // it. The same read settles readout protection, which would fail the
    // programming pass at its first verify.
    std::printf("\n  reading the device's option bytes...\n");
    std::string probeLog = dir + "\\initial-programming-probe.log";
    std::string probe;
    const int probeExit = runOpenocdProbe(ocdPath, scripts, probeLog, probe);
    uint32_t optr = 0;
    if (probeExit < 0) {
        std::printf("\n  Nothing was programmed. Fix the cause above and run again.\n");
        return finish(1, assumeYes);
    }
    if (!optrIn(probe, &optr)) {
        std::printf("\n  FAILED  could not read the device (OpenOCD exit code %d).\n"
                    "  The tail of its log (%s):\n\n",
                    probeExit, probeLog.c_str());
        printLogTail(probe);
        explainFailure(probe);
        std::printf("\n  Nothing was programmed. Fix the cause above and run again.\n");
        return finish(1, assumeYes);
    }
    if (rdpLevelOf(optr) != 0) {
        // Only reachable without --unlock: the unlock's own check demands level 0.
        printProtectedStatement();
        return finish(1, assumeYes);
    }

    bool switchedToDualBank = false;
    if ((optr & kOptrDbank) == 0) {
        std::printf("\n  This unit's flash is in SINGLE-BANK mode (option bytes 0x%08X).\n"
                    "  The CAN Triple firmware needs dual-bank: in single-bank mode it\n"
                    "  refuses firmware updates and licences (\"flash write failed (0x05)\")\n"
                    "  and stalls while saving a configuration. Switching to dual-bank\n"
                    "  ERASES THE WHOLE CHIP, stored configuration and retained values\n"
                    "  included - after the switch the firmware would not find them anyway.\n"
                    "  To keep the configuration: close this window, use Get Configuration\n"
                    "  in the CAN Triple Device Manager and save it, run this again, and\n"
                    "  Send it back once the unit is running.\n\n",
                    static_cast<unsigned>(optr));
        if (!assumeYes && !unlock
            && !askYes("  Type YES to switch to dual-bank and program the device: ")) {
            std::printf("  Nothing done - the unit is still in single-bank mode.\n");
            return finish(1, assumeYes);
        }
        std::printf("\n  switching the flash to dual-bank (this erases the chip)...\n");
        std::string bankLog = dir + "\\initial-programming-dualbank.log";
        if (!runOpenocdDualBank(ocdPath, scripts, bankLog)) {
            std::printf("\n  Nothing was programmed. Fix the cause above and run again -\n"
                        "  a unit still in single-bank mode is switched again.\n");
            return finish(1, assumeYes);
        }
        switchedToDualBank = true;
        std::printf("  done - the flash is in dual-bank mode and erased.\n");
    }

    std::printf("\n  programming (the device resets when this finishes)...\n");
    std::string logPath = dir + "\\initial-programming-openocd.log";
    if (!runOpenocd(ocdPath, scripts, blPath, appPath, logPath)) {
        std::printf("\n  The device was NOT necessarily left working - run this "
                    "tool again once the\n  cause above is fixed. Nothing about "
                    "a failed attempt is unrecoverable.\n");
        return finish(1, assumeYes);
    }

    std::printf("\n  DONE - bootloader v%u and firmware %u.%u.%u installed "
                "and verified.\n  The device has been reset and is running.\n"
                "\n  This firmware is a starting point. To run a different one, open\n"
                "  the CAN Triple Device Manager and use Online > Update Firmware.\n",
                blVersion, hdr->fw_version_major, hdr->fw_version_minor,
                hdr->fw_version_patch);
    if (switchedToDualBank)
        std::printf("\n  The flash was switched to dual-bank mode, which erased the chip:\n"
                    "  the unit has no stored configuration. Send it one from the Manager.\n");
    if (unlock)
        std::printf("\n  The unit is blank and unlicensed. Issue its licence in the Manager\n"
                    "  (Online > Firmware License Manager); it locks itself at the next\n"
                    "  power-up after that.\n");
    return finish(0, assumeYes);
}
