// Monitor Channels' channel selection, driven offscreen against a document
// mapped through the real mapper and a DeviceLink that is never opened.
//
// The property that matters most: after a selection, the value stream still
// finds its rows. The grid is REBUILT with the chosen channels first, so every
// index the dialog keeps — the value stream's signal-to-row map, the override
// bookkeeping, the editors set as cell widgets — is made against the rows as
// they stand. The test reads the grid the way a user sees it (top to bottom,
// hidden rows skipped) AND sends a value by signal index after the selection,
// pinning that it lands on its channel's row.
//
// The selection is read from QSettings, the same key the picker writes, which
// is how it survives closing and reopening the window; the test points
// QSettings at a temporary directory so it never touches the registry.

#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

#include "../src/model/configuration.h"
#include "../src/model/device_mapper.h"
#include "../src/protocol/device_link.h"
#include "../src/ui/monitor_channels_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

const char *kSelectionKey = "monitorChannelSelection";

void addChannel(ct::Configuration &config, const QString &name, const QString &unit)
{
    ct::Channel ch;
    ch.name = name;
    ch.unit = unit;
    ch.dataType = QStringLiteral("u16");
    ch.minValue = 0;
    ch.maxValue = 65535;
    ch.userDefined = true;
    config.catalog().addOrUpdateUserChannel(ch);
}

// Four channels carried by one receive message on CAN 1, so the mapper gives
// each a signal index. Names chosen to sort differently from their bit order.
void buildDocument(ct::Configuration &config)
{
    const QStringList names = {QStringLiteral("RPM"), QStringLiteral("Coolant Temp"),
                               QStringLiteral("Oil Pressure"), QStringLiteral("Boost")};
    for (const QString &name : names)
        addChannel(config, name, QStringLiteral("x"));
    ct::CommsSection section;
    section.name = QStringLiteral("Engine");
    section.device = ct::SectionDevice::ReceiveMessage;
    section.alignment = ct::SectionAlignment::WordSwap;
    section.baseAddress = 0x640;
    for (int i = 0; i < names.size(); ++i) {
        ct::CommsChannelRow row;
        row.channelName = names.at(i);
        row.startBit = i * 16;
        row.bitLength = 16;
        row.dbcFactor = 1.0;
        section.rows.append(row);
    }
    config.bus[0].sections.append(section);
}

QTableWidget *grid(QWidget *w)
{
    auto *t = w->findChild<QTableWidget *>();
    Q_ASSERT(t);
    return t;
}

// The channel names as the user sees them: visual order, hidden rows skipped.
QStringList visibleOrder(QTableWidget *t)
{
    QStringList names;
    for (int visual = 0; visual < t->rowCount(); ++visual) {
        const int row = t->verticalHeader()->logicalIndex(visual);
        if (!t->isRowHidden(row))
            names << t->item(row, 0)->text();
    }
    return names;
}

int rowOf(QTableWidget *t, const QString &name)
{
    for (int row = 0; row < t->rowCount(); ++row)
        if (t->item(row, 0)->text() == name)
            return row;
    return -1;
}

int signalIndexOf(const ct::Configuration &config, const QString &name)
{
    const ct::MappingResult mapping = ct::mapToDevice(config);
    for (auto it = mapping.signalToChannel.constBegin(); it != mapping.signalToChannel.constEnd();
         ++it)
        if (it.value() == name)
            return it.key();
    return -1;
}

// Every mapped channel — the four above plus the device channels the mapper
// always gives a slot to — in the grid's name order.
QStringList allNamesSorted(const ct::Configuration &config)
{
    QStringList names = ct::mapToDevice(config).signalToChannel.values();
    std::sort(names.begin(), names.end(), [](const QString &a, const QString &b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    return names;
}

QString labelText(QWidget *w)
{
    auto *label = w->findChild<QLabel *>(QStringLiteral("selectionLabel"));
    return label ? label->text() : QString();
}

// No selection: name order, every row visible, Show All has nothing to do.
void testDefaultIsNameOrder()
{
    QSettings().remove(QLatin1String(kSelectionKey));
    ct::Configuration config;
    buildDocument(config);
    ct::DeviceLink link;
    ct::MonitorChannelsDialog d(&link, &config);
    const QStringList all = allNamesSorted(config);
    CHECK(all.size() > 4); // the device channels are in the grid as well
    CHECK(visibleOrder(grid(&d)) == all);
    CHECK(labelText(&d) == QStringLiteral("Showing all %1 channels").arg(all.size()));
    auto *showAll = d.findChild<QPushButton *>(QStringLiteral("showAllChannels"));
    CHECK(showAll && !showAll->isEnabled());
}

// A stored selection: the chosen rows first, in the chosen order, the rest
// hidden — and a value arriving by signal index still finds its row.
void testSelectionOrdersAndHides()
{
    QSettings().setValue(QLatin1String(kSelectionKey),
                         QStringList() << QStringLiteral("RPM") << QStringLiteral("Boost"));
    ct::Configuration config;
    buildDocument(config);
    ct::DeviceLink link;
    ct::MonitorChannelsDialog d(&link, &config);
    QTableWidget *t = grid(&d);

    const QStringList all = allNamesSorted(config);
    CHECK(visibleOrder(t) == (QStringList() << QStringLiteral("RPM") << QStringLiteral("Boost")));
    CHECK(t->isRowHidden(rowOf(t, QStringLiteral("Coolant Temp"))));
    CHECK(t->isRowHidden(rowOf(t, QStringLiteral("Oil Pressure"))));
    CHECK(labelText(&d) == QStringLiteral("Showing 2 of %1 channels").arg(all.size()));

    // The rows were BUILT in that order: the chosen two first, then the rest
    // in name order (hidden). The value below must still find its channel.
    CHECK(t->item(0, 0)->text() == QStringLiteral("RPM"));
    CHECK(t->item(1, 0)->text() == QStringLiteral("Boost"));
    QStringList rest = all;
    rest.removeAll(QStringLiteral("RPM"));
    rest.removeAll(QStringLiteral("Boost"));
    bool restInNameOrder = t->rowCount() == all.size();
    for (int row = 2; restInNameOrder && row < t->rowCount(); ++row)
        restInNameOrder = t->item(row, 0)->text() == rest.at(row - 2);
    CHECK(restInNameOrder);

    ct::SignalValueEntry entry;
    entry.signal_idx = static_cast<quint16>(signalIndexOf(config, QStringLiteral("RPM")));
    entry.physical_value = 1234.0f;
    emit link.signalValues(QList<ct::SignalValueEntry>() << entry);
    CHECK(t->item(rowOf(t, QStringLiteral("RPM")), 1)->text() == QStringLiteral("1234"));
    CHECK(t->item(rowOf(t, QStringLiteral("Boost")), 1)->text() == QStringLiteral("—"));

    // Show All: name order back, nothing hidden, the stored selection cleared.
    auto *showAll = d.findChild<QPushButton *>(QStringLiteral("showAllChannels"));
    CHECK(showAll && showAll->isEnabled());
    if (showAll)
        showAll->click();
    CHECK(visibleOrder(t) == all);
    CHECK(labelText(&d) == QStringLiteral("Showing all %1 channels").arg(all.size()));
    CHECK(QSettings().value(QLatin1String(kSelectionKey)).toStringList().isEmpty());
}

// A rebuild (the document changed, or a Get) makes new rows; the selection is
// applied to them again. A chosen name the document does not map is skipped
// for now and kept in the stored selection.
void testSelectionSurvivesRebuildAndSkipsUnmappedNames()
{
    QSettings().setValue(QLatin1String(kSelectionKey),
                         QStringList() << QStringLiteral("Ghost") << QStringLiteral("Oil Pressure")
                                       << QStringLiteral("RPM"));
    ct::Configuration config;
    buildDocument(config);
    ct::DeviceLink link;
    ct::MonitorChannelsDialog d(&link, &config);
    QTableWidget *t = grid(&d);
    CHECK(visibleOrder(t)
          == (QStringList() << QStringLiteral("Oil Pressure") << QStringLiteral("RPM")));
    CHECK(labelText(&d)
          == QStringLiteral("Showing 2 of %1 channels").arg(allNamesSorted(config).size()));

    d.rebuild();
    t = grid(&d);
    CHECK(visibleOrder(t)
          == (QStringList() << QStringLiteral("Oil Pressure") << QStringLiteral("RPM")));
    CHECK(QSettings().value(QLatin1String(kSelectionKey)).toStringList().size() == 3);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    // QSettings goes to a scratch directory, never to this machine's registry.
    QTemporaryDir scratch;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    QCoreApplication::setOrganizationName(QStringLiteral("ct-test"));
    QCoreApplication::setApplicationName(QStringLiteral("test_monitor_channels"));

    testDefaultIsNameOrder();
    testSelectionOrdersAndHides();
    testSelectionSurvivesRebuildAndSkipsUnmappedNames();

    if (fails == 0) {
        std::printf("test_monitor_channels: all checks passed\n");
        return 0;
    }
    std::printf("test_monitor_channels: %d check(s) failed\n", fails);
    return 1;
}
