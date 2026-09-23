// A stand-in for OpenOCD that plays an STM32G473 at the level the initial
// programming tool can see: the lines it reads, the exit codes it ignores, and
// the option register it decides from. test_initial_programming points the
// tool at this with --openocd, so every path through the tool runs without a
// device on the other end. That matters most for the single-bank switch, which
// on real silicon is a mass erase and an option-byte write.
//
// It is strict on purpose. An unknown command, or a run that never reaches
// "shutdown" (a real OpenOCD would still be running as a server, and the tool
// would hang on it), fails the run, so a malformed command in the tool shows up
// here rather than on a customer's bench.
//
// The chip lives in the text file named by FAKE_OCD_STATE, one key=value per
// line, read at start and written back at exit:
//   optr      the option register the chip is running with (hex)
//   pending   option bytes programmed but not yet loaded (hex)
//   erased    1 once the chip has been mass-erased
//   programs  images programmed and verified
//   fail      a failure to play: probe-open, erase, obl-noop, unlock-fails
// Every run's -c commands are appended to the file named by FAKE_OCD_CALLS, one
// line per run, joined with " | ".
//
// What it does NOT model: timing, the reset an option-byte reload causes (the
// run just ends in an error, the worst case for the tool), and anything about
// flash contents beyond "erased" and "programmed".

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Chip {
    uint32_t optr = 0xFFEFF8AAu; // the G473 factory value: dual-bank, RDP level 0
    uint32_t pending = 0xFFEFF8AAu;
    int erased = 0;
    int programs = 0;
    std::string fail;
};

constexpr uint32_t kDbank = 1u << 22;

int rdpLevel(uint32_t optr)
{
    const uint32_t rdp = optr & 0xFFu;
    return rdp == 0xAAu ? 0 : (rdp == 0xCCu ? 2 : 1);
}

std::string env(const char *name)
{
    const char *v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

Chip load(const std::string &path)
{
    Chip c;
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return c;
    char line[256];
    std::map<std::string, std::string> kv;
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        const size_t eq = s.find('=');
        if (eq != std::string::npos)
            kv[s.substr(0, eq)] = s.substr(eq + 1);
    }
    std::fclose(f);
    if (kv.count("optr"))
        c.optr = uint32_t(std::strtoul(kv["optr"].c_str(), nullptr, 16));
    c.pending = kv.count("pending") ? uint32_t(std::strtoul(kv["pending"].c_str(), nullptr, 16))
                                    : c.optr;
    if (kv.count("erased"))
        c.erased = std::atoi(kv["erased"].c_str());
    if (kv.count("programs"))
        c.programs = std::atoi(kv["programs"].c_str());
    if (kv.count("fail"))
        c.fail = kv["fail"];
    return c;
}

void save(const std::string &path, const Chip &c)
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "optr=%08X\npending=%08X\nerased=%d\nprograms=%d\nfail=%s\n",
                 unsigned(c.optr), unsigned(c.pending), c.erased, c.programs,
                 c.fail.c_str());
    std::fclose(f);
}

void logCalls(const std::vector<std::string> &cmds)
{
    const std::string path = env("FAKE_OCD_CALLS");
    if (path.empty())
        return;
    FILE *f = std::fopen(path.c_str(), "a");
    if (!f)
        return;
    for (size_t i = 0; i < cmds.size(); ++i)
        std::fprintf(f, "%s%s", i ? " | " : "", cmds[i].c_str());
    std::fprintf(f, "\n");
    std::fclose(f);
}

std::vector<std::string> words(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ')
            ++i;
        if (i >= s.size())
            break;
        size_t j = i;
        if (s[i] == '{') { // a TCL-braced path, spaces and all
            j = s.find('}', i);
            if (j == std::string::npos)
                j = s.size();
            else
                ++j;
        } else {
            while (j < s.size() && s[j] != ' ')
                ++j;
        }
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool fileExists(const std::string &p)
{
    FILE *f = std::fopen(p.c_str(), "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<std::string> cmds;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cmds.push_back(argv[++i]);
        else if ((std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "-f") == 0)
                 && i + 1 < argc)
            ++i; // search paths and config files mean nothing here
    }
    logCalls(cmds);

    const std::string statePath = env("FAKE_OCD_STATE");
    if (statePath.empty()) {
        std::printf("Error: FAKE_OCD_STATE is not set - refusing to guess a chip\n");
        return 3;
    }
    Chip chip = load(statePath);
    // Every exit goes through here, so the chip's state is never lost.
    const auto quit = [&](int code) {
        save(statePath, chip);
        std::fflush(stdout);
        return code;
    };

    std::printf("xPack Open On-Chip Debugger 0.12.0 (fake_openocd for tests)\n");
    for (const std::string &cmd : cmds) {
        const std::vector<std::string> w = words(cmd);
        if (w.empty())
            continue;
        if (cmd == "transport select swd") {
            continue;
        } else if (cmd == "init") {
            if (chip.fail == "probe-open") {
                std::printf("Error: open failed\nError: No Valid JTAG Interface Configured.\n");
                return quit(1);
            }
            std::printf("Info : STLINK V3J15M7 (API v3) VID:PID 0483:3754\n"
                        "Info : [stm32g4x.cpu] Examination succeed\n");
        } else if (cmd == "reset halt") {
            std::printf("[stm32g4x.cpu] halted due to debug-request, current mode: Thread\n");
        } else if (cmd == "reset run") {
            continue;
        } else if (cmd == "flash probe 0") {
            std::printf("Info : device idcode = 0x20036469 (STM32G47/G48xx - Rev 'unknown' : 0x2003)\n"
                        "Info : RDP level %d (0x%02X)\n"
                        "Warn : overriding size register by configured bank size - MAY CAUSE TROUBLE\n"
                        "Info : flash size = 512 KiB\n"
                        "Info : flash mode : %s-bank\n"
                        "flash 'stm32l4x' found at 0x08000000\n",
                        rdpLevel(chip.optr), unsigned(chip.optr & 0xFFu),
                        (chip.optr & kDbank) ? "dual" : "single");
        } else if (cmd == "mdw 0x40022020") {
            std::printf("0x40022020: %08x \n", unsigned(chip.optr));
        } else if (cmd == "stm32l4x mass_erase 0") {
            if (chip.fail == "erase" || rdpLevel(chip.optr) != 0) {
                std::printf("stm32l4x mass erase failed\n");
                return quit(1); // OpenOCD stops at the first failed command
            }
            chip.erased = 1;
            chip.programs = 0;
            std::printf("stm32l4x mass erase complete\n");
        } else if (w.size() >= 5 && w[0] == "stm32l4x" && w[1] == "option_write") {
            if (w[2] != "0" || w.size() > 6) {
                std::printf("Error: bad option_write arguments: %s\n", cmd.c_str());
                return quit(1);
            }
            const uint32_t offset = uint32_t(std::strtoul(w[3].c_str(), nullptr, 0));
            const uint32_t value = uint32_t(std::strtoul(w[4].c_str(), nullptr, 0));
            const uint32_t mask =
                w.size() > 5 ? uint32_t(std::strtoul(w[5].c_str(), nullptr, 0)) : 0xFFFFFFFFu;
            if (offset != 0x20) {
                std::printf("Error: option_write to offset 0x%X is not FLASH_OPTR\n",
                            unsigned(offset));
                return quit(1);
            }
            chip.pending = (chip.pending & ~mask) | (value & mask);
            std::printf("stm32l4x Option written.\n"
                        "INFO: a reset or power cycle is required for the new settings to take effect\n");
        } else if (cmd == "stm32l4x unlock 0") {
            if (chip.fail == "unlock-fails") {
                std::printf("Error: stm32l4x failed to unlock device\n");
                return quit(1);
            }
            chip.pending = (chip.pending & ~0xFFu) | 0xAAu;
        } else if (cmd == "stm32l4x option_load 0") {
            if (chip.fail != "obl-noop") {
                // Leaving level 1 is a mass erase the chip performs itself.
                if (rdpLevel(chip.optr) != 0 && rdpLevel(chip.pending) == 0) {
                    chip.erased = 1;
                    chip.programs = 0;
                }
                chip.optr = chip.pending;
            }
            // The reload resets the target under the debugger's feet. Ending
            // the run in an error is the worst a real one does here.
            std::printf("Error: [stm32g4x.cpu] DP error after the option-byte reload\n");
            return quit(1);
        } else if (w.size() == 4 && w[0] == "program" && w[3] == "verify") {
            if (rdpLevel(chip.optr) != 0) {
                std::printf("Error: failed erasing sectors 0 to 2\n");
                return quit(1);
            }
            std::string path = w[1];
            if (path.size() >= 2 && path.front() == '{' && path.back() == '}')
                path = path.substr(1, path.size() - 2);
            if (!fileExists(path)) {
                std::printf("Error: couldn't open %s\n", path.c_str());
                return quit(1);
            }
            chip.programs += 1;
            std::printf("** Programming Started **\n** Programming Finished **\n"
                        "** Verify Started **\n** Verified OK **\n");
        } else if (cmd == "shutdown") {
            std::printf("shutdown command invoked\n");
            return quit(0);
        } else {
            std::printf("Error: invalid command name \"%s\"\n", cmd.c_str());
            return quit(1);
        }
    }
    std::printf("Error: no shutdown - a real OpenOCD would still be running\n");
    return quit(2);
}
