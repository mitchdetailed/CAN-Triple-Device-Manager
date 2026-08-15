// The document's channels: the user-created ones, saved in the .ct3 file, plus
// the DEVICE channels the firmware produces about itself, which are built in.
// Provides the search used by the Select Channel and Channel Editor dialogs
// ("temp eng oil" finds Engine Oil Temp; "set" finds CruiseSetSpeed).
#pragma once

#include <QList>
#include <QStringList>

#include "channel.h"

namespace ct {

class ChannelCatalog
{
public:
    ChannelCatalog();

    // All (user-created) channels, sorted by name.
    QList<Channel> allChannels() const;
    QStringList categories() const;
    QList<Channel> channelsInCategory(const QString &category) const;
    Channel findByName(const QString &name) const; // invalid Channel if missing

    // channelLabel() for a channel known only by NAME — "Coolant Temp °C".
    // Most display sites hold a reference rather than the Channel itself, and
    // this is the one lookup they need. A name the catalogue does not know (a
    // dangling reference to a deleted channel) comes back unchanged rather than
    // blank, so a broken reference still reads as the name it is looking for.
    QString labelFor(const QString &name) const;

    // Any-order substring match: every typed word must appear somewhere in the
    // channel name (case-insensitive). Text carrying regex metacharacters is
    // used as a regular expression over the whole name instead.
    QList<Channel> search(const QString &text) const;
    static bool matchesSearch(const QString &channelName, const QString &searchText);

    void setUserChannels(const QList<Channel> &channels);
    QList<Channel> userChannels() const { return m_userChannels; }
    void addOrUpdateUserChannel(const Channel &c);
    void removeUserChannel(const QString &name);

    // Device channels — produced by the firmware, not by anything in the
    // configuration. They are always present, cannot be created, edited or
    // deleted, and are not saved in the .ct3 (a document stores references to
    // them by name, the same as any other channel). "Device OnTime", seconds
    // since the unit powered up; ten CAN diagnostics per bus: the receive
    // and transmit error counters, the three bus-state flags, the accumulated
    // protocol-error count, the frame counts, an estimated bus load, and the
    // number of times the bus has been recovered from bus-off; and the MCU
    // health block: die temperature and analogue supply, their since-boot
    // excursions, and why the unit last reset (an enumerated value — the
    // catalogue carries the labels, Channel::enumLabels).
    //
    // Every device channel is mapped on a Send whether the document reads it or
    // not, so all 36 are always live and always visible in Monitor Channels.
    // They are diagnostics: the moment you want them is a bus fault, and that is
    // the worst possible moment to find out you had to have referenced them in
    // advance. The cost is DEVCH_COUNT of MAX_SIGNALS plus 6 bytes each per tick
    // on the value stream while a monitor is open — see the reasoning in
    // mapToDevice, which is where the allocation happens.
    static const QList<Channel> &deviceChannels();
    static bool isDeviceChannel(const QString &name);
    static QString deviceOnTimeName(); // still named directly by the tests and the help text

    // The device channel published into wire_structs.h's DEVCH_* slot `id`, or
    // an invalid Channel if this build has no channel for it. The mapper's one
    // lookup: it walks the ids rather than the names, so a firmware slot that
    // gained a channel the GUI does not know about is skipped rather than
    // mis-assigned.
    static Channel deviceChannelById(int id);

    // Quantities available for user channels (Channel Type) and their display
    // units, in dropdown order. defaultUnitForQuantity() is the unit pre-selected
    // for a new channel of that quantity (not necessarily the first listed).
    static QStringList quantities();
    static QStringList unitsForQuantity(const QString &quantity);
    static QString defaultUnitForQuantity(const QString &quantity);

private:
    QList<Channel> m_userChannels;
};

} // namespace ct
