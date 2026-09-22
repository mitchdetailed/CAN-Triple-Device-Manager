// The Monitor Channels picker, driven offscreen: what lands on the right and in
// what order, what comes back from selection(), the Find box, and that an empty
// right-hand list means every channel. Drag-and-drop reordering is Qt's own
// InternalMove and is not simulated here; what IS pinned is that the order of
// the right-hand list is the order selection() reports, which is all the
// monitor relies on.

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cstdio>

#include "../src/model/channel_list.h"
#include "../src/ui/monitor_channel_select_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

ct::Channel channel(const QString &name, const QString &unit = QString())
{
    ct::Channel c;
    c.name = name;
    c.unit = unit;
    return c;
}

QList<ct::Channel> channels()
{
    return {channel(QStringLiteral("RPM"), QStringLiteral("rpm")),
            channel(QStringLiteral("Coolant Temp"), QStringLiteral("°C")),
            channel(QStringLiteral("Oil Pressure"), QStringLiteral("kPa")),
            channel(QStringLiteral("boost")), // lower case: sorts by name, not by case
            channel(QStringLiteral("Battery Voltage"), QStringLiteral("V"))};
}

QTreeWidget *list(QWidget *w, const char *name)
{
    auto *t = w->findChild<QTreeWidget *>(QLatin1String(name));
    Q_ASSERT(t);
    return t;
}

QPushButton *button(QWidget *w, const char *name)
{
    auto *b = w->findChild<QPushButton *>(QLatin1String(name));
    Q_ASSERT(b);
    return b;
}

QStringList names(QTreeWidget *t, bool visibleOnly = false)
{
    QStringList out;
    for (int i = 0; i < t->topLevelItemCount(); ++i)
        if (!visibleOnly || !t->topLevelItem(i)->isHidden())
            out << t->topLevelItem(i)->text(0);
    return out;
}

// Opens with the chosen names on the right IN THEIR ORDER, the rest on the left
// in name order regardless of case, and a name the document does not map
// dropped rather than listed.
void testOpensOnTheCurrentSelection()
{
    ct::MonitorChannelSelectDialog d(
        channels(), {QStringLiteral("Oil Pressure"), QStringLiteral("RPM"),
                     QStringLiteral("Not A Channel")});
    CHECK(names(list(&d, "selectedChannels"))
          == (QStringList() << QStringLiteral("Oil Pressure") << QStringLiteral("RPM")));
    CHECK(names(list(&d, "availableChannels"))
          == (QStringList() << QStringLiteral("Battery Voltage") << QStringLiteral("boost")
                            << QStringLiteral("Coolant Temp")));
    CHECK(d.selection() == (QStringList() << QStringLiteral("Oil Pressure")
                                          << QStringLiteral("RPM")));
    auto *count = d.findChild<QLabel *>(QStringLiteral("selectionCount"));
    CHECK(count && count->text() == QStringLiteral("2 channels selected"));
}

// A block highlighted on the left moves to the right in LIST order and is
// appended after what was already there; << brings rows back into name order.
void testMoveRightAndLeft()
{
    ct::MonitorChannelSelectDialog d(channels(), {QStringLiteral("RPM")});
    QTreeWidget *left = list(&d, "availableChannels");
    QTreeWidget *right = list(&d, "selectedChannels");

    CHECK(!button(&d, "moveRight")->isEnabled()); // nothing highlighted yet
    // Highlight "Coolant Temp" first and "Battery Voltage" second: the move
    // must follow the list, not the clicks.
    left->topLevelItem(2)->setSelected(true);
    left->topLevelItem(0)->setSelected(true);
    CHECK(button(&d, "moveRight")->isEnabled());
    button(&d, "moveRight")->click();
    CHECK(d.selection()
          == (QStringList() << QStringLiteral("RPM") << QStringLiteral("Battery Voltage")
                            << QStringLiteral("Coolant Temp")));
    CHECK(names(left) == (QStringList() << QStringLiteral("boost")
                                        << QStringLiteral("Oil Pressure")));
    // The moved rows are left highlighted on the right, so << takes them back.
    CHECK(button(&d, "moveLeft")->isEnabled());
    button(&d, "moveLeft")->click();
    CHECK(d.selection() == (QStringList() << QStringLiteral("RPM")));
    CHECK(names(left)
          == (QStringList() << QStringLiteral("Battery Voltage") << QStringLiteral("boost")
                            << QStringLiteral("Coolant Temp") << QStringLiteral("Oil Pressure")));
    CHECK(!button(&d, "moveLeft")->isEnabled());
}

// The Find box narrows the left list by name; a row it has hidden is not moved
// by >> even if it is still highlighted, and a row sent back with << honours it.
void testFindNarrowsTheLeftList()
{
    ct::MonitorChannelSelectDialog d(channels(), {});
    QTreeWidget *left = list(&d, "availableChannels");
    auto *find = d.findChild<QLineEdit *>(QStringLiteral("findChannels"));
    CHECK(find != nullptr);
    if (!find)
        return;

    left->topLevelItem(0)->setSelected(true); // Battery Voltage
    find->setText(QStringLiteral("temp"));
    CHECK(names(left, true) == (QStringList() << QStringLiteral("Coolant Temp")));
    CHECK(!button(&d, "moveRight")->isEnabled()); // the highlighted row is hidden
    left->topLevelItem(2)->setSelected(true);     // Coolant Temp, visible
    button(&d, "moveRight")->click();
    CHECK(d.selection() == (QStringList() << QStringLiteral("Coolant Temp")));
    CHECK(names(left).contains(QStringLiteral("Battery Voltage"))); // not moved

    button(&d, "moveLeft")->click(); // Coolant Temp is still highlighted on the right
    CHECK(d.selection().isEmpty());
    CHECK(names(left, true) == (QStringList() << QStringLiteral("Coolant Temp")));
    find->clear();
    CHECK(names(left, true).size() == 5);
}

// A channel list round-trips through its file in order, and the reader
// refuses what is not a channel list.
void testChannelListFile()
{
    QTemporaryDir dir;
    const QString path = dir.path() + QStringLiteral("/cooling.ct3l");
    QString error;
    CHECK(ct::writeChannelList(path, {QStringLiteral("RPM"), QStringLiteral("Coolant Temp")},
                               &error));
    QStringList names;
    CHECK(ct::readChannelList(path, &names, &error));
    CHECK(names == (QStringList() << QStringLiteral("RPM") << QStringLiteral("Coolant Temp")));

    // Legible on purpose: the names are there to be read by a person.
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll().contains("\"Coolant Temp\""));

    // Not JSON, and JSON that is something else.
    const QString junk = dir.path() + QStringLiteral("/junk.ct3l");
    QFile out(junk);
    CHECK(out.open(QIODevice::WriteOnly));
    out.write("not json");
    out.close();
    CHECK(!ct::readChannelList(junk, &names, &error));
    CHECK(names.isEmpty() && !error.isEmpty());
    const QString other = dir.path() + QStringLiteral("/other.ct3l");
    QFile out2(other);
    CHECK(out2.open(QIODevice::WriteOnly));
    out2.write("{\"fileType\": \"Something\", \"channels\": [\"RPM\"]}");
    out2.close();
    CHECK(!ct::readChannelList(other, &names, &error));
    CHECK(error.contains(QStringLiteral("not a channel list")));
}

// Loading a list REPLACES the selection with the names the configuration has,
// in the list's order; the names it does not have come back as not found, and
// a repeat is kept once.
void testApplyChannelListReportsNotFound()
{
    ct::MonitorChannelSelectDialog d(channels(), {QStringLiteral("boost")});
    CHECK(button(&d, "loadList") != nullptr);
    CHECK(button(&d, "saveList")->isEnabled()); // one channel selected

    QStringList notFound;
    d.applyChannelList({QStringLiteral("Oil Pressure"), QStringLiteral("Ghost"),
                        QStringLiteral("RPM"), QStringLiteral("Oil Pressure"),
                        QStringLiteral("Wheel Speed")},
                       &notFound);
    CHECK(d.selection() == (QStringList() << QStringLiteral("Oil Pressure")
                                          << QStringLiteral("RPM")));
    CHECK(notFound == (QStringList() << QStringLiteral("Ghost")
                                     << QStringLiteral("Wheel Speed")));
    // "boost" was replaced, not kept: it is back on the left.
    CHECK(names(list(&d, "availableChannels")).contains(QStringLiteral("boost")));

    d.applyChannelList({}, &notFound);
    CHECK(d.selection().isEmpty());
    CHECK(!button(&d, "saveList")->isEnabled());
}

// Nothing on the right is "every channel", and the count line says so.
void testEmptySelectionMeansEveryChannel()
{
    ct::MonitorChannelSelectDialog d(channels(), {});
    CHECK(d.selection().isEmpty());
    auto *count = d.findChild<QLabel *>(QStringLiteral("selectionCount"));
    CHECK(count && count->text().contains(QStringLiteral("every channel")));
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testOpensOnTheCurrentSelection();
    testMoveRightAndLeft();
    testFindNarrowsTheLeftList();
    testChannelListFile();
    testApplyChannelListReportsNotFound();
    testEmptySelectionMeansEveryChannel();

    if (fails == 0) {
        std::printf("test_monitor_select: all checks passed\n");
        return 0;
    }
    std::printf("test_monitor_select: %d check(s) failed\n", fails);
    return 1;
}
