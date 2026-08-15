// CANTripleRecovery — bring a CAN Triple back from anything, over SWD.
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
//   - touch the configuration store, retained values, or access keys — a
//     recovery reinstates the PROGRAM, not the device's data (the erase is
//     bounded to the pages the two images occupy, all in bank 1);
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
//   CANTripleRecovery [--yes] [--bootloader FILE] [--firmware FILE]
//                     [--openocd EXE]
//
// Exit 0 on success, 1 on any failure, so it can gate a provisioning script.

#include <cstdint>
#include <cstdio>
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
// technician working out of a recovery folder or USB stick will look — but
// the INSTALLED copy lives under Program Files, which an unelevated process
// cannot write. That exact failure shipped once: the tool validated both
// images, said "programming...", and then refused at the log file before
// OpenOCD ever ran. So an unwritable first choice falls back to %TEMP%, and
// the path reported to the user is the one actually used.
bool runOpenocd(const std::string &ocd, const std::string &scripts,
                const std::string &blPath, const std::string &appPath,
                std::string &logPath)
{
    std::string cmd;
    cmd += "\"" + ocd + "\"";
    cmd += " -s \"" + scripts + "\"";
    cmd += " -f interface/stlink.cfg";
    cmd += " -c \"transport select swd\"";
    cmd += " -f stm32g4x_512k.cfg";
    cmd += " -c \"program {" + tclPath(blPath) + "} 0x08000000 verify\"";
    cmd += " -c \"program {" + tclPath(appPath) + "} 0x08004000 verify\"";
    cmd += " -c \"reset run\"";
    cmd += " -c \"shutdown\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        char tmp[MAX_PATH];
        const DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            logPath = std::string(tmp) + "recovery-openocd.log";
            log = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (log == INVALID_HANDLE_VALUE) {
        std::printf("  FAILED  cannot write a log file (tried %s)\n",
                    logPath.c_str());
        return false;
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
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(log);

    // Success is BOTH the exit code and two explicit verifications in the log
    // — one per image. OpenOCD has been known to exit 0 from a shutdown that
    // never programmed anything, so the words are checked, not just the code.
    std::vector<uint8_t> raw;
    readFile(logPath, raw);
    const std::string text(raw.begin(), raw.end());
    int verified = 0;
    for (size_t at = 0; (at = text.find("** Verified OK **", at)) != std::string::npos; ++at)
        ++verified;

    if (exitCode == 0 && verified >= 2)
        return true;

    std::printf("\n  FAILED  OpenOCD exit code %lu, %d of 2 images verified.\n"
                "  The tail of its log (%s):\n\n",
                exitCode, verified, logPath.c_str());
    // Print roughly the last 25 lines — where ST-LINK enumeration failures,
    // target voltage problems and verify mismatches all land.
    size_t start = text.size();
    for (int lines = 0; start > 0 && lines < 25; --start)
        if (text[start - 1] == '\n')
            ++lines;
    std::fwrite(text.data() + start, 1, text.size() - start, stdout);
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    std::printf("\nCAN Triple Recovery\n"
                "-------------------\n");

    const std::string dir = exeDir();
    std::string blPath = dir + "\\bootloader.bin";
    std::string appPath;
    std::string ocdPath = dir + "\\openocd\\bin\\openocd.exe";
    std::string scripts = dir + "\\openocd\\scripts";
    bool assumeYes = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--yes")
            assumeYes = true;
        else if (a == "--bootloader" && hasNext)
            blPath = argv[++i];
        else if (a == "--firmware" && hasNext)
            appPath = argv[++i];
        else if (a == "--openocd" && hasNext)
            ocdPath = argv[++i];
        else {
            std::printf("usage: CANTripleRecovery [--yes] [--bootloader FILE] "
                        "[--firmware FILE] [--openocd EXE]\n");
            return 1;
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
            return 1;
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
        return 1;
    }
    // The 512K-bank target config normally sits in the bundled scripts dir;
    // when running against the PlatformIO fallback it is not there, so look
    // beside the exe as well and pass it by absolute path in that case.
    if (!fileExists(scripts + "\\stm32g4x_512k.cfg")
        && !fileExists(dir + "\\stm32g4x_512k.cfg")) {
        std::printf("  FAILED  stm32g4x_512k.cfg not found in %s or beside "
                    "this program - without it OpenOCD uses the stock 128 KB "
                    "flash map\n", scripts.c_str());
        return 1;
    }

    // ---------------------------------------------------- validate both images

    std::vector<uint8_t> bl, app;
    if (!readFile(blPath, bl)) {
        std::printf("  FAILED  cannot read %s\n", blPath.c_str());
        return 1;
    }
    if (!readFile(appPath, app)) {
        std::printf("  FAILED  cannot read %s\n", appPath.c_str());
        return 1;
    }

    uint32_t blVersion = 0;
    if (!validateBootloader(bl, &blVersion))
        return 1;

    // The device's own validator, against the bootloader ABOUT TO BE
    // INSTALLED — not whatever the unit currently runs, which may be nothing.
    const uint8_t r = fw_image_validate(app.data(), (uint32_t)app.size(), blVersion);
    if (r != FW_RESULT_OK) {
        std::printf("  FAILED  %s: %s\n", appPath.c_str(), describe(r));
        return 1;
    }
    const FwImageHeader *hdr = fw_image_header(app.data(), (uint32_t)app.size());

    std::printf("\n  bootloader : %s  (v%u, %zu bytes)\n",
                blPath.c_str(), blVersion, bl.size());
    std::printf("  firmware   : %s  (%u.%u.%u, %zu bytes, validated)\n",
                appPath.c_str(), hdr->fw_version_major, hdr->fw_version_minor,
                hdr->fw_version_patch, app.size());
    std::printf("\n  This programs the bootloader at 0x08000000 and the\n"
                "  application at 0x08004000 over the built-in ST-LINK.\n"
                "  The stored configuration and retained values are NOT touched.\n"
                "  Safe to re-run at any time, including after a failed attempt.\n\n");

    if (!assumeYes) {
        std::printf("  Type YES to program the device: ");
        char answer[16] = {0};
        if (!std::fgets(answer, sizeof(answer), stdin)
            || std::strncmp(answer, "YES", 3) != 0) {
            std::printf("  Nothing done.\n");
            return 1;
        }
    }

    std::printf("\n  programming (the device resets when this finishes)...\n");
    std::string logPath = dir + "\\recovery-openocd.log";
    if (!runOpenocd(ocdPath, scripts, blPath, appPath, logPath)) {
        std::printf("\n  The device was NOT necessarily left working - run this "
                    "tool again once the\n  cause above is fixed. Nothing about "
                    "a failed attempt is unrecoverable.\n");
        if (!assumeYes) {
            std::printf("\n  Press Enter to close.");
            std::getchar();
        }
        return 1;
    }

    std::printf("\n  DONE - bootloader v%u and firmware %u.%u.%u installed "
                "and verified.\n  The device has been reset and is running.\n",
                blVersion, hdr->fw_version_major, hdr->fw_version_minor,
                hdr->fw_version_patch);
    if (!assumeYes) {
        std::printf("\n  Press Enter to close.");
        std::getchar();
    }
    return 0;
}
