#include "usb_adapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "wire_structs.h"

#ifdef Q_OS_LINUX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ct {
namespace usb_adapter {

namespace {

QString readAttr(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

} // namespace

QList<AdapterUnit> adapterUnits(const QString &sysfsDevices)
{
    QList<AdapterUnit> found;
#ifdef Q_OS_LINUX
    const QDir root(sysfsDevices);
    // Devices only: an interface's directory (1-1:1.0) has a colon, a device's
    // (1-1, usb1) has none.
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (name.contains(QLatin1Char(':')))
            continue;
        const QString dev = root.filePath(name);
        if (readAttr(dev + QStringLiteral("/idVendor")) != QStringLiteral("0483")
            || readAttr(dev + QStringLiteral("/idProduct")) != QStringLiteral("5740")
            || readAttr(dev + QStringLiteral("/manufacturer")) != QStringLiteral("Minton Performance"))
            continue;
        // The adapter is one vendor-class interface; the Manager's port is CDC.
        bool vendor = false;
        const QDir devDir(dev);
        for (const QString &iface :
             devDir.entryList({name + QStringLiteral(":*")}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (readAttr(devDir.filePath(iface) + QStringLiteral("/bInterfaceClass"))
                == QStringLiteral("ff"))
                vendor = true;
        }
        if (!vendor)
            continue;
        bool busOk = false, devOk = false;
        const int bus = readAttr(dev + QStringLiteral("/busnum")).toInt(&busOk);
        const int num = readAttr(dev + QStringLiteral("/devnum")).toInt(&devOk);
        if (!busOk || !devOk)
            continue;
        AdapterUnit unit;
        unit.serial = readAttr(dev + QStringLiteral("/serial"));
        unit.usbNode = QStringLiteral("/dev/bus/usb/%1/%2")
                           .arg(bus, 3, 10, QLatin1Char('0'))
                           .arg(num, 3, 10, QLatin1Char('0'));
        found.append(unit);
    }
#else
    Q_UNUSED(sysfsDevices);
#endif
    return found;
}

bool switchToManager(const AdapterUnit &unit, bool keep, QString *error)
{
#ifdef Q_OS_LINUX
    const QByteArray node = QFile::encodeName(unit.usbNode);
    const int fd = ::open(node.constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (error)
            *error = (e == EACCES || e == EPERM)
                         ? QStringLiteral("This user may not send the unit a request (%1). "
                                          "Install the cantriple-socketcan package, whose udev "
                                          "rule allows it, or run ctriple manager with sudo.")
                               .arg(unit.usbNode)
                         : QStringLiteral("%1: %2").arg(unit.usbNode,
                                                        QString::fromLocal8Bit(std::strerror(e)));
        return false;
    }
    usbdevfs_ctrltransfer req{};
    req.bRequestType = 0x40; // OUT, vendor, device
    req.bRequest = CT_USB_REQ_SET_MODE;
    req.wValue = USB_MODE_MANAGER;
    req.wIndex = keep ? CT_USB_REQ_STORE : 0;
    req.wLength = 0;
    req.timeout = 1000; // ms
    req.data = nullptr;
    const int rc = ::ioctl(fd, USBDEVFS_CONTROL, &req);
    const int e = errno;
    ::close(fd);
    if (rc < 0) {
        if (error)
            *error = e == EPIPE
                         ? QStringLiteral("The unit refused the request%1.")
                               .arg(keep ? QStringLiteral(" to keep the Device Manager's port "
                                                          "(its firmware is older than 2.0.10)")
                                         : QString())
                         : QStringLiteral("The request to the unit failed: %1")
                               .arg(QString::fromLocal8Bit(std::strerror(e)));
        return false;
    }
    return true;
#else
    Q_UNUSED(unit);
    Q_UNUSED(keep);
    if (error)
        *error = QStringLiteral("Switching a SocketCAN adapter back is only possible from "
                                "Linux. Plug the unit in again: on a PC that cannot use the "
                                "adapter it gives the Device Manager's port within seconds.");
    return false;
#endif
}

} // namespace usb_adapter
} // namespace ct
