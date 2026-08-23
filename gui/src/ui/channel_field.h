// The read-only "Input :" / "Output :" boxes that name a channel, throughout
// the calculation and comms editors.
//
// Those boxes were doing two jobs with one string: showing the channel to the
// user AND being the only record of which channel the row refers to — every one
// of them ends up in something like `row.aChannel = m_aEdit->text()`. That is
// fine while the text is exactly the name, and it is why simply appending a
// unit to them would not be a display change at all: it would rewrite the
// configuration to refer to a channel called "Coolant Temp °C", which does not
// exist.
//
// So the two jobs are separated here. The widget DISPLAYS "Coolant Temp °C" and
// REMEMBERS "Coolant Temp" in a property; setChannelField() writes both and
// channelField() is the only correct way to read the name back. A call site that
// still says ->text() gets the decorated string, so the rule is simply: a
// channel-bearing line edit is never read with text().
//
// test_channel_labels drives the converted dialogs and asserts that what reaches
// the document is the bare name, which is what makes the rule enforceable rather
// than a convention.
#pragma once

#include <QApplication>
#include <QLineEdit>
#include <QString>
#include <QVariant>

#include "../model/channel_catalog.h"

namespace ct {

// The property the bare name lives in. Named with a prefix because it shares a
// namespace with every other dynamic property Qt and the styles use.
inline const char *kChannelNameProperty = "ct_channel_name";

// Show `name` decorated with its unit, and remember `name` itself.
inline void setChannelField(QLineEdit *edit, const QString &name, const ChannelCatalog &catalog)
{
    if (!edit)
        return;
    edit->setProperty(kChannelNameProperty, name);
    edit->setText(catalog.labelFor(name));
    // The decorated text can outrun the box; the tooltip keeps the whole thing
    // reachable, and an empty field gets no tooltip rather than an empty one.
    edit->setToolTip(name.isEmpty() ? QString() : catalog.labelFor(name));
}

// The bare channel name a field refers to. Falls back to the visible text for a
// field that was never set through setChannelField() — an undecorated widget
// still answers correctly, so a half-converted dialog degrades to the old
// behaviour rather than to nonsense.
inline QString channelField(const QLineEdit *edit)
{
    if (!edit)
        return QString();
    const QVariant v = edit->property(kChannelNameProperty);
    return v.isValid() ? v.toString() : edit->text();
}

// Re-point every LIVE channel field, in every open window, from `oldName` to
// `newName`. The widget half of the problem Configuration::channelRenamed
// solves for row data: while the picker's Edit… commits a rename, the row
// editors in the modal chain above it hold channel names in these fields, and
// each editor's OK reads EVERY field back into its row — never just the one
// that was picked. A field left holding the old name would write it straight
// back into a row family the rename walk just fixed. The property that
// separates a field's identity from its label is also what makes the field
// findable here, so every editor built on setChannelField() is covered without
// knowing about renames at all. Returns how many fields were re-pointed.
inline int renameOpenChannelFields(const QString &oldName, const QString &newName,
                                   const ChannelCatalog &catalog)
{
    int updated = 0;
    const auto repoint = [&](QLineEdit *edit) {
        const QVariant v = edit->property(kChannelNameProperty);
        if (v.isValid() && v.toString().compare(oldName, Qt::CaseInsensitive) == 0) {
            setChannelField(edit, newName, catalog);
            ++updated;
        }
    };
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *top : tops) {
        // A parentless QLineEdit is itself a top-level widget (the tests hold
        // fields that way); findChildren() never reports its own root.
        if (auto *edit = qobject_cast<QLineEdit *>(top))
            repoint(edit);
        for (QLineEdit *edit : top->findChildren<QLineEdit *>())
            repoint(edit);
    }
    return updated;
}

} // namespace ct
