// Online → USB Mode… and the way back from a SocketCAN adapter, against a CAN
// Triple 2.0 in miniature (which device it is, which it starts as, whether it
// fell back, behind a FakeDeviceLink answering CMD_SET/GET_USB_MODE as its
// serial_proto.c does from 2.0.11). The dialog offers what changes nothing on
// a unit that starts as the Manager's port, the Manager's port on one that fell
// back, and nothing at all on one that starts as the adapter; each choice sends
// exactly what it says, "until it restarts" first setting the start mode back;
// the CAN Viewer password is asked for only when the unit wants it; a refusal
// is said inside the dialog, which stays open. Under it the session helpers: a
// unit that cannot say reads as unsupported, not as an error, and a reply of
// the wrong size or with a mode this build does not know is refused. And
// usb_adapter's search, in a sysfs made for it: only a Minton Performance
// 0483:5740 with a vendor-class interface is an adapter.

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QPair>
#include <QTemporaryDir>

#include <cstdio>

#include "../src/protocol/device_session.h"
#include "../src/protocol/usb_adapter.h"
#include "../src/ui/usb_mode_dialog.h"
#include "fake_device_link.h"

using namespace ct;

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

// The unit, on its Manager's port (the only place the dialog runs).
struct Unit {
    quint8 stored = USB_MODE_MANAGER;
    bool fellBack = false;
    bool viewerLocked = false; // a CAN Viewer password, not yet proved
    quint8 refuseWith = 0;     // refuse every SET with this (0: take them)
    QList<QPair<quint8, quint8>> sets; // (mode, store) taken

    FakeDeviceLink::Reply answer(quint8 cmd, const QByteArray &p)
    {
        if (cmd == CMD_GET_USB_MODE) {
            QByteArray r;
            r.append(char(USB_MODE_MANAGER));
            r.append(char(stored));
            r.append(char(fellBack ? USB_MODE_FELL_BACK : 0));
            return FakeDeviceLink::Reply::ack(r);
        }
        if (cmd != CMD_SET_USB_MODE)
            return FakeDeviceLink::Reply::nack(ERR_INVALID_CMD);
        if (p.size() != 2)
            return FakeDeviceLink::Reply::nack(ERR_INVALID_LEN);
        const quint8 mode = quint8(p[0]);
        const quint8 store = quint8(p[1]);
        if (mode > USB_MODE_GS_USB || store > 1)
            return FakeDeviceLink::Reply::nack(ERR_OUT_OF_BOUNDS);
        if (mode == USB_MODE_GS_USB && viewerLocked)
            return FakeDeviceLink::Reply::nack(ERR_LOCKED);
        if (refuseWith)
            return FakeDeviceLink::Reply::nack(refuseWith);
        if (store)
            stored = mode;
        fellBack = false; // a host's choice replaces a fallback
        sets.append(qMakePair(mode, store));
        return FakeDeviceLink::Reply::ack();
    }
};

device_session::UsbModeInfo infoOf(const Unit &u)
{
    device_session::UsbModeInfo i;
    i.supported = true;
    i.current = USB_MODE_MANAGER;
    i.stored = u.stored;
    i.fellBack = u.fellBack;
    return i;
}

UsbModeDialog::ProveViewer noPassword()
{
    return [](QWidget *) {
        std::printf("FAIL: the CAN Viewer password was asked for\n");
        ++fails;
        return false;
    };
}

void testOffered()
{
    // Starts as the Manager's port: offered that, and OK changes nothing.
    {
        Unit unit;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        CHECK(dialog.choice() == UsbModeDialog::ManagerPort);
        CHECK(dialog.stateText().contains(QStringLiteral("starts as it")));
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(!link.sentAny(CMD_SET_USB_MODE));
        CHECK(!dialog.unitLeft());
    }
    // Fell back: offered the Manager's port, and OK keeps it, which also stops
    // the unit turning back into the adapter when the cable comes out.
    {
        Unit unit;
        unit.stored = USB_MODE_GS_USB;
        unit.fellBack = true;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        CHECK(dialog.choice() == UsbModeDialog::ManagerPort);
        CHECK(dialog.stateText().contains(QStringLiteral("no program on this PC")));
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(unit.sets.size() == 1 && unit.sets[0] == qMakePair(USB_MODE_MANAGER, quint8(1)));
        CHECK(unit.stored == USB_MODE_MANAGER && !unit.fellBack);
        CHECK(!dialog.unitLeft());
    }
    // Fell back with nothing kept (an adapter asked for on another PC): the
    // Manager's port is still sent, as this PC's choice.
    {
        Unit unit;
        unit.fellBack = true;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.accept();
        CHECK(unit.sets.size() == 1 && unit.sets[0] == qMakePair(USB_MODE_MANAGER, quint8(1)));
    }
    // Starts as the adapter, the Manager's port now: every choice changes
    // something, so none is offered and OK does nothing until one is made.
    {
        Unit unit;
        unit.stored = USB_MODE_GS_USB;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        CHECK(dialog.choice() == UsbModeDialog::NoChoice);
        CHECK(dialog.stateText().contains(QStringLiteral("starts as a SocketCAN adapter")));
        dialog.accept();
        CHECK(dialog.result() != QDialog::Accepted);
        CHECK(!link.sentAny(CMD_SET_USB_MODE));
        // ...and the Manager's port, chosen, keeps that.
        dialog.setChoice(UsbModeDialog::ManagerPort);
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted);
        CHECK(unit.sets.size() == 1 && unit.sets[0] == qMakePair(USB_MODE_MANAGER, quint8(1)));
    }
}

void testAdapterChoices()
{
    // Until it restarts, on a unit that starts as the adapter: the start mode
    // goes back to the Manager's port first, then the adapter for now.
    {
        Unit unit;
        unit.stored = USB_MODE_GS_USB;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::AdapterForNow);
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted && dialog.unitLeft());
        CHECK(unit.sets.size() == 2);
        CHECK(unit.sets.value(0) == qMakePair(USB_MODE_MANAGER, quint8(1)));
        CHECK(unit.sets.value(1) == qMakePair(USB_MODE_GS_USB, quint8(0)));
    }
    // Until it restarts, on a unit that starts as the Manager's port: one SET.
    {
        Unit unit;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::AdapterForNow);
        dialog.accept();
        CHECK(unit.sets.size() == 1 && unit.sets[0] == qMakePair(USB_MODE_GS_USB, quint8(0)));
        CHECK(dialog.unitLeft());
    }
    // Kept.
    {
        Unit unit;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::AdapterKept);
        dialog.accept();
        CHECK(unit.sets.size() == 1 && unit.sets[0] == qMakePair(USB_MODE_GS_USB, quint8(1)));
        CHECK(unit.stored == USB_MODE_GS_USB && dialog.unitLeft());
    }
}

void testPasswordAndRefusals()
{
    // The CAN Viewer password: asked for once, proved, and the SET repeated.
    {
        Unit unit;
        unit.viewerLocked = true;
        int asked = 0;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), [&](QWidget *) {
            ++asked;
            unit.viewerLocked = false;
            return true;
        });
        dialog.setChoice(UsbModeDialog::AdapterKept);
        dialog.accept();
        CHECK(asked == 1);
        CHECK(dialog.result() == QDialog::Accepted && dialog.unitLeft());
        CHECK(link.countOf(CMD_SET_USB_MODE) == 2);
    }
    // Not given: said inside the dialog, which stays open.
    {
        Unit unit;
        unit.viewerLocked = true;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), [](QWidget *) { return false; });
        dialog.setChoice(UsbModeDialog::AdapterForNow);
        dialog.accept();
        CHECK(dialog.result() != QDialog::Accepted && !dialog.unitLeft());
        CHECK(dialog.statusText().contains(QStringLiteral("CAN Viewer password")));
        CHECK(unit.sets.isEmpty());
    }
    // The Manager's port never needs it (2.0.11).
    {
        Unit unit;
        unit.stored = USB_MODE_GS_USB;
        unit.viewerLocked = true;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::ManagerPort);
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted);
    }
    // Could not keep it: said, nothing changed, still open.
    {
        Unit unit;
        unit.stored = USB_MODE_GS_USB;
        unit.refuseWith = ERR_FLASH_WRITE;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::ManagerPort);
        dialog.accept();
        CHECK(dialog.result() != QDialog::Accepted);
        CHECK(dialog.statusText().contains(QStringLiteral("could not keep")));
        CHECK(unit.stored == USB_MODE_GS_USB);
        // A second try that goes through clears it.
        unit.refuseWith = 0;
        dialog.accept();
        CHECK(dialog.result() == QDialog::Accepted && dialog.statusText().isEmpty());
    }
    // Firmware that cannot keep a mode (before 2.0.10).
    {
        Unit unit;
        unit.refuseWith = ERR_OUT_OF_BOUNDS;
        FakeDeviceLink link([&unit](quint8 c, const QByteArray &p) { return unit.answer(c, p); });
        UsbModeDialog dialog(&link, infoOf(unit), noPassword());
        dialog.setChoice(UsbModeDialog::AdapterKept);
        dialog.accept();
        CHECK(dialog.statusText().contains(QStringLiteral("cannot keep")));
    }
}

void testSession()
{
    device_session::UsbModeInfo info;
    QByteArray good;
    good.append(char(USB_MODE_MANAGER));
    good.append(char(USB_MODE_GS_USB));
    good.append(char(USB_MODE_FELL_BACK));
    CHECK(device_session::parseUsbMode(good, &info));
    CHECK(info.current == USB_MODE_MANAGER && info.stored == USB_MODE_GS_USB && info.fellBack);
    CHECK(!device_session::parseUsbMode(good.left(2), &info));
    CHECK(!device_session::parseUsbMode(good + QByteArray(1, '\0'), &info));
    QByteArray unknown = good;
    unknown[1] = char(2);
    CHECK(!device_session::parseUsbMode(unknown, &info));

    // A unit that cannot say (a CAN Triple 1.x, a 2.0 before 2.0.10).
    {
        FakeDeviceLink link([](quint8, const QByteArray &) {
            return FakeDeviceLink::Reply::nack(ERR_INVALID_CMD);
        });
        device_session::UsbModeInfo i;
        QString error;
        CHECK(device_session::readUsbMode(&link, &i, &error));
        CHECK(!i.supported && !i.startsAsAdapter() && error.isEmpty());
    }
    // One that answers.
    {
        FakeDeviceLink link([&good](quint8, const QByteArray &) {
            return FakeDeviceLink::Reply::ack(good);
        });
        device_session::UsbModeInfo i;
        QString error;
        CHECK(device_session::readUsbMode(&link, &i, &error));
        CHECK(i.supported && i.startsAsAdapter() && i.fellBack);
    }
    // One that answers in a form this build does not read.
    {
        FakeDeviceLink link([](quint8, const QByteArray &) {
            return FakeDeviceLink::Reply::ack(QByteArray(5, '\0'));
        });
        device_session::UsbModeInfo i;
        QString error;
        CHECK(!device_session::readUsbMode(&link, &i, &error));
        CHECK(!error.isEmpty());
    }
    // The SET's payload.
    {
        FakeDeviceLink link([](quint8, const QByteArray &) { return FakeDeviceLink::Reply::ack(); });
        quint8 code = 0xFF;
        CHECK(device_session::setUsbMode(&link, USB_MODE_GS_USB, true, nullptr, &code));
        CHECK(code == ERR_OK);
        CHECK(link.sent.size() == 1 && link.sent[0].first == CMD_SET_USB_MODE);
        CHECK(link.sent[0].second == QByteArray::fromHex("0101"));
    }
}

void writeAttr(const QString &dir, const QString &name, const QString &value)
{
    QDir().mkpath(dir);
    QFile f(dir + QLatin1Char('/') + name);
    if (f.open(QIODevice::WriteOnly))
        f.write(value.toLatin1() + '\n');
}

void addDevice(const QString &root, const QString &name, const QString &vendor,
               const QString &product, const QString &manufacturer, const QString &serial,
               int bus, int dev, const QStringList &interfaceClasses)
{
    const QString d = root + QLatin1Char('/') + name;
    writeAttr(d, QStringLiteral("idVendor"), vendor);
    writeAttr(d, QStringLiteral("idProduct"), product);
    writeAttr(d, QStringLiteral("manufacturer"), manufacturer);
    writeAttr(d, QStringLiteral("serial"), serial);
    writeAttr(d, QStringLiteral("busnum"), QString::number(bus));
    writeAttr(d, QStringLiteral("devnum"), QString::number(dev));
    for (int i = 0; i < interfaceClasses.size(); ++i) {
        const QString iface = QStringLiteral("%1/%2:1.%3").arg(d, name).arg(i);
        writeAttr(iface, QStringLiteral("bInterfaceClass"), interfaceClasses[i]);
        // sysfs lists the interfaces at the top too.
        writeAttr(QStringLiteral("%1/%2:1.%3").arg(root, name).arg(i),
                  QStringLiteral("bInterfaceClass"), interfaceClasses[i]);
    }
}

void testAdapterSearch()
{
#ifdef Q_OS_LINUX
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();
    const QString mp = QStringLiteral("Minton Performance");
    addDevice(root, QStringLiteral("1-1"), QStringLiteral("0483"), QStringLiteral("5740"), mp,
              QStringLiteral("2035344746325012004F002B"), 1, 7, {QStringLiteral("ff")});
    // On its Manager's port: CDC, not an adapter.
    addDevice(root, QStringLiteral("1-2"), QStringLiteral("0483"), QStringLiteral("5740"), mp,
              QStringLiteral("2035344746325012004F00AA"), 1, 8,
              {QStringLiteral("02"), QStringLiteral("0a")});
    // Somebody else's STM32 on the shared ID, vendor class and all.
    addDevice(root, QStringLiteral("1-3"), QStringLiteral("0483"), QStringLiteral("5740"),
              QStringLiteral("STMicroelectronics"), QStringLiteral("0123456789AB"), 1, 9,
              {QStringLiteral("ff")});
    // A different product from the same maker of chips.
    addDevice(root, QStringLiteral("2-1"), QStringLiteral("0483"), QStringLiteral("374e"), mp,
              QStringLiteral("X"), 2, 3, {QStringLiteral("ff")});

    const QList<usb_adapter::AdapterUnit> found = usb_adapter::adapterUnits(root);
    CHECK(found.size() == 1);
    if (!found.isEmpty()) {
        CHECK(found[0].serial == QStringLiteral("2035344746325012004F002B"));
        CHECK(found[0].shortId() == QStringLiteral("4f002b"));
        CHECK(found[0].usbNode == QStringLiteral("/dev/bus/usb/001/007"));
    }

    // A node that is not there: said, with the node in it.
    usb_adapter::AdapterUnit gone;
    gone.serial = QStringLiteral("2035344746325012004F002B");
    gone.usbNode = root + QStringLiteral("/no-such-node");
    QString error;
    CHECK(!usb_adapter::switchToManager(gone, false, &error));
    CHECK(error.contains(gone.usbNode));
#else
    CHECK(usb_adapter::adapterUnits().isEmpty());
    QString error;
    CHECK(!usb_adapter::switchToManager(usb_adapter::AdapterUnit{}, false, &error));
    CHECK(!error.isEmpty());
#endif
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testOffered();
    testAdapterChoices();
    testPasswordAndRefusals();
    testSession();
    testAdapterSearch();

    if (fails == 0)
        std::printf("test_usb_mode: all checks passed\n");
    else
        std::printf("test_usb_mode: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
