// The initial programming tool, end to end, against fake_openocd.
//
// The tool is how a blank or broken CAN Triple gets firmware, and the one place
// a customer's unit meets a mass erase, so its decisions are worth pinning:
// when it asks, what it tells the person at the bench, which OpenOCD commands
// it sends and in what order, and what it leaves the chip in. The real OpenOCD
// cannot be used for that - the interesting paths erase whatever is attached -
// so fake_openocd plays the chip (see its header), and this runs the REAL tool
// binary against it, with its real prompts, from a kit folder laid out like an
// install's Firmware folder.
//
// SAFETY: the tool is always given --openocd <fake_openocd.exe>; the test
// refuses to run if that file is missing, because the tool would otherwise fall
// back to a real OpenOCD; and USERPROFILE points into the kit, so the
// PlatformIO fallback cannot be found either. Nothing here reaches an ST-LINK.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

#include "fw_image.h"

namespace {

int fails = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (cond) {                                                              \
            std::printf("  PASS  %s\n", what);                                   \
        } else {                                                                 \
            std::printf("  FAIL  %s\n", what);                                   \
            ++fails;                                                             \
        }                                                                        \
    } while (0)

constexpr quint32 kDualBank = 0xFFEFF8AAu;   // the G473 factory option register
constexpr quint32 kSingleBank = 0xFFAFF8AAu; // the same with DBANK (bit 22) clear
constexpr quint32 kDualLocked = 0xFFEFF8BBu; // dual-bank, readout protection level 1
constexpr quint32 kSingleLocked = 0xFFAFF8BBu;

// A bootloader image the tool's own checks accept: stack pointer in RAM, a
// Thumb reset vector inside the slot, and the version stamp at 0x200.
QByteArray makeBootloader()
{
    QByteArray img(4096, char(0xFF));
    const quint32 sp = 0x20020000u;
    const quint32 reset = FW_BOOTLOADER_BASE + 0x101u;
    std::memcpy(img.data(), &sp, 4);
    std::memcpy(img.data() + 4, &reset, 4);
    BootloaderInfo info{};
    info.magic = FW_BL_INFO_MAGIC;
    info.version = FW_BOOTLOADER_VERSION;
    info.app_base = FW_APP_BASE;
    std::memcpy(img.data() + FW_BL_INFO_OFFSET, &info, sizeof(info));
    return img;
}

// A .ctf the device's validator accepts: the same construction as
// test_firmware_link's makeFirmwareImage.
QByteArray makeFirmware(quint16 major, quint16 minor, quint16 patch)
{
    const quint32 size = 8192;
    QByteArray img(int(size), char(0));
    for (int i = 0; i < img.size(); ++i)
        img[i] = char((i * 7 + 11) & 0xFF);
    FwImageHeader hdr{};
    hdr.magic = FW_IMAGE_MAGIC;
    hdr.header_version = FW_IMAGE_HDR_VERSION;
    hdr.product_id = FW_PRODUCT_CAN_TRIPLE;
    hdr.image_size = size;
    hdr.fw_version_major = major;
    hdr.fw_version_minor = minor;
    hdr.fw_version_patch = patch;
    hdr.flash_store_version = 20;
    hdr.min_bootloader_version = 1;
    std::memcpy(img.data() + FW_IMAGE_HEADER_OFFSET, &hdr, sizeof(hdr));
    const quint32 crcOff = FW_IMAGE_CRC_OFFSET;
    quint32 crc = fw_crc32_update(FW_CRC32_INIT, img.constData(), crcOff);
    crc = fw_crc32_update(crc, img.constData() + crcOff + 4, size - crcOff - 4);
    crc = fw_crc32_final(crc);
    std::memcpy(img.data() + crcOff, &crc, sizeof(crc));
    return img;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

struct Chip {
    quint32 optr = 0;
    quint32 pending = 0;
    int erased = 0;
    int programs = 0;
};

Chip readChip(const QString &path)
{
    Chip c;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return c;
    for (const QByteArray &line : f.readAll().split('\n')) {
        const int eq = line.indexOf('=');
        if (eq < 0)
            continue;
        const QByteArray key = line.left(eq);
        const QByteArray value = line.mid(eq + 1).trimmed();
        if (key == "optr")
            c.optr = value.toUInt(nullptr, 16);
        else if (key == "pending")
            c.pending = value.toUInt(nullptr, 16);
        else if (key == "erased")
            c.erased = value.toInt();
        else if (key == "programs")
            c.programs = value.toInt();
    }
    return c;
}

struct Run {
    int exitCode = -1;
    QString out;        // everything the tool printed
    QStringList calls;  // one line per OpenOCD run: its -c commands
    Chip chip;          // the chip afterwards
};

// One scenario: a fresh kit, a chip in `optr` (plus a failure to play), the
// tool run with `args` and `input` typed at its prompts.
Run runTool(const QString &tool, const QString &fake, quint32 optr, const char *failMode,
            const QStringList &args, const QByteArray &input)
{
    Run r;
    QTemporaryDir kit;
    if (!kit.isValid())
        return r;
    const QString exe = kit.filePath(QStringLiteral("CANTripleInitialProgramming.exe"));
    QFile::copy(tool, exe);
    writeFile(kit.filePath(QStringLiteral("bootloader.bin")), makeBootloader());
    writeFile(kit.filePath(QStringLiteral("can-triple-1.0.14.ctf")), makeFirmware(1, 0, 14));
    QDir(kit.path()).mkpath(QStringLiteral("openocd/scripts"));
    writeFile(kit.filePath(QStringLiteral("openocd/scripts/stm32g4x_512k.cfg")),
              QByteArray("# stand-in: fake_openocd ignores its config files\n"));

    const QString state = kit.filePath(QStringLiteral("chip.txt"));
    const QString calls = kit.filePath(QStringLiteral("calls.txt"));
    writeFile(state, QStringLiteral("optr=%1\npending=%1\nerased=0\nprograms=0\nfail=%2\n")
                         .arg(optr, 8, 16, QLatin1Char('0'))
                         .arg(QString::fromLatin1(failMode))
                         .toLatin1());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("FAKE_OCD_STATE"), QDir::toNativeSeparators(state));
    env.insert(QStringLiteral("FAKE_OCD_CALLS"), QDir::toNativeSeparators(calls));
    env.insert(QStringLiteral("USERPROFILE"), QDir::toNativeSeparators(kit.path()));

    QProcess p;
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(exe, QStringList{QStringLiteral("--openocd"), QDir::toNativeSeparators(fake)} + args);
    if (!p.waitForStarted(10000))
        return r;
    p.write(input);
    p.closeWriteChannel(); // EOF: the closing "Press Enter" returns at once
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished(5000);
        r.out = QStringLiteral("(timed out)\n") + QString::fromLocal8Bit(p.readAll());
        return r;
    }
    r.exitCode = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = QString::fromLocal8Bit(p.readAll());
    QFile cf(calls);
    if (cf.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : cf.readAll().split('\n'))
            if (!line.trimmed().isEmpty())
                r.calls << QString::fromLatin1(line.trimmed());
    }
    r.chip = readChip(state);
    return r;
}

bool says(const Run &r, const char *text)
{
    return r.out.contains(QString::fromLatin1(text));
}

void dumpOnFailure(const Run &r, int failsBefore)
{
    if (fails == failsBefore)
        return;
    std::printf("      --- tool output ---\n%s\n      --- OpenOCD runs ---\n",
                qPrintable(r.out));
    for (const QString &c : r.calls)
        std::printf("      %s\n", qPrintable(c));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString dir = QCoreApplication::applicationDirPath();
    const QString tool = dir + QStringLiteral("/CANTripleInitialProgramming.exe");
    const QString fake = dir + QStringLiteral("/fake_openocd.exe");
    if (!QFileInfo::exists(tool) || !QFileInfo::exists(fake)) {
        // Refuse outright: without the fake the tool would go looking for a
        // real OpenOCD, and a real OpenOCD programs the unit on the desk.
        std::printf("test_initial_programming: %s or %s is missing - build both first\n",
                    qPrintable(tool), qPrintable(fake));
        return 1;
    }

    int before = fails;
    std::printf("answering anything but YES runs nothing\n");
    {
        const Run r = runTool(tool, fake, kDualBank, "", {}, "no\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "Nothing done - type YES"), "says nothing was done");
        CHECK(r.calls.isEmpty(), "OpenOCD never ran");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("a dual-bank unit is programmed with no switch and no erase\n");
    {
        const Run r = runTool(tool, fake, kDualBank, "", {}, "yes\n");
        CHECK(r.exitCode == 0, "exit code 0");
        CHECK(says(r, "DONE"), "reports DONE");
        CHECK(!says(r, "SINGLE-BANK"), "no bank-mode warning");
        CHECK(r.calls.size() == 2, "two OpenOCD runs: the probe, then programming");
        CHECK(!r.calls.join(QLatin1Char('\n')).contains(QStringLiteral("mass_erase"))
                  && !r.calls.join(QLatin1Char('\n')).contains(QStringLiteral("option_write")),
              "no erase and no option-byte write");
        CHECK(r.chip.optr == kDualBank && r.chip.erased == 0 && r.chip.programs == 2,
              "option bytes untouched, both images programmed");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("declining the switch leaves a single-bank unit exactly as it was\n");
    {
        const Run r = runTool(tool, fake, kSingleBank, "", {}, "yes\nno\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "SINGLE-BANK"), "names the single-bank mode");
        CHECK(says(r, "ERASES THE WHOLE CHIP"), "says the switch erases the chip");
        CHECK(says(r, "still in single-bank mode"), "says the unit was left as it was");
        CHECK(r.calls.size() == 1, "only the read-only probe ran");
        CHECK(r.chip.optr == kSingleBank && r.chip.pending == kSingleBank
                  && r.chip.erased == 0 && r.chip.programs == 0,
              "chip untouched");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("a single-bank unit is switched, erased, then programmed\n");
    {
        const Run r = runTool(tool, fake, kSingleBank, "", {}, "yes\nYes\n");
        CHECK(r.exitCode == 0, "exit code 0");
        CHECK(says(r, "Type YES to switch to dual-bank"), "asks a second time");
        CHECK(says(r, "switched to dual-bank mode"), "the closing text says what happened");
        CHECK(r.chip.optr == kDualBank, "option bytes now exactly the factory value");
        CHECK(r.chip.erased == 1 && r.chip.programs == 2, "erased, then both images programmed");
        CHECK(r.calls.size() == 4, "four runs: probe, switch, confirm-and-erase, program");
        if (r.calls.size() == 4) {
            const QString sw = r.calls[1];
            const int erase = sw.indexOf(QStringLiteral("mass_erase"));
            const int write = sw.indexOf(QStringLiteral("option_write 0 0x20 0x00400000 0x00400000"));
            const int load = sw.indexOf(QStringLiteral("option_load 0"));
            CHECK(erase >= 0 && write > erase && load > write,
                  "switch run: erase, then DBANK alone, then the reload");
            CHECK(r.calls[2].contains(QStringLiteral("mdw 0x40022020"))
                      && r.calls[2].contains(QStringLiteral("mass_erase")),
                  "second run reads the option register and erases again");
            CHECK(r.calls[3].contains(QStringLiteral("program")), "last run programs");
        }
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("--yes switches a single-bank unit without asking\n");
    {
        const Run r = runTool(tool, fake, kSingleBank, "", {QStringLiteral("--yes")}, "");
        CHECK(r.exitCode == 0, "exit code 0");
        CHECK(!says(r, "Type YES"), "no prompt at all");
        CHECK(r.chip.optr == kDualBank && r.chip.programs == 2, "switched and programmed");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("an option write that does not take is caught before programming\n");
    {
        const Run r = runTool(tool, fake, kSingleBank, "obl-noop", {}, "yes\nyes\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "still say single-bank"), "says the option bytes did not change");
        CHECK(says(r, "Nothing was programmed"), "says nothing was programmed");
        CHECK(r.chip.programs == 0 && r.chip.optr == kSingleBank, "nothing programmed");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("a failed erase stops the switch before the option bytes are written\n");
    {
        const Run r = runTool(tool, fake, kSingleBank, "erase", {}, "yes\nyes\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "did not go through"), "says the switch failed");
        CHECK(r.chip.pending == kSingleBank && r.chip.optr == kSingleBank,
              "option bytes never written");
        CHECK(r.calls.size() == 2, "no run after the failed switch");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("readout protection stops the tool before anything is written\n");
    {
        const Run r = runTool(tool, fake, kDualLocked, "", {}, "yes\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "This unit has readout protection set. Unit appears to have licensed "
                      "firmware defined.")
                  && says(r, "Action Not Completed."),
              "gives the readout-protection statement, word for word");
        CHECK(!says(r, "--unlock"), "does not name the unlock option");
        CHECK(r.calls.size() == 1 && r.chip.programs == 0, "only the probe ran");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("an ST-LINK that cannot be opened is named, not blamed on protection\n");
    {
        const Run r = runTool(tool, fake, kDualBank, "probe-open", {}, "yes\n");
        CHECK(r.exitCode == 1, "exit code 1");
        CHECK(says(r, "could not open the ST-LINK"), "names the ST-LINK and its driver");
        CHECK(!says(r, "readout protection"), "does not blame readout protection");
        CHECK(r.chip.programs == 0, "nothing programmed");
        dumpOnFailure(r, before);
    }

    before = fails;
    std::printf("--unlock on a locked single-bank unit: unlock, switch unasked, program\n");
    {
        const Run r = runTool(tool, fake, kSingleLocked, "", {QStringLiteral("--unlock")},
                              "yes\n");
        CHECK(r.exitCode == 0, "exit code 0");
        CHECK(says(r, "removing readout protection"), "unlocked first");
        CHECK(says(r, "SINGLE-BANK") && !says(r, "Type YES to switch"),
              "explains the switch but does not ask again");
        CHECK(r.chip.optr == kDualBank && r.chip.programs == 2,
              "level 0, dual-bank, both images programmed");
        dumpOnFailure(r, before);
    }

    if (fails == 0)
        std::printf("test_initial_programming: all checks passed\n");
    else
        std::printf("test_initial_programming: %d check(s) FAILED\n", fails);
    return fails == 0 ? 0 : 1;
}
