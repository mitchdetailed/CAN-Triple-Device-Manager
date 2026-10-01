// Capping a name field at the DEVICE LABEL BUDGET, which is a byte count.
//
// QLineEdit::setMaxLength counts QChars, and the budget is UTF-8 BYTES. For an
// ASCII name the two agree, which is why the difference stayed invisible: the
// channel fields have been capped at 31 "characters" for as long as the label
// has been 31 bytes, and every name anyone typed fitted both. One non-ASCII
// character is 2-4 bytes, so "Temperatur Kühlmittel" is 21 characters and 22
// bytes — a legal-looking name that the mapper then has to clip on the way to
// the device, which it does with a warning nobody should have had to read.
//
// Capping here means the field cannot hold a name that will not fit, so the
// clip in device_mapper.cpp goes back to being what it was written as: a
// backstop for configurations saved before the limit existed.
#pragma once

#include <QLineEdit>
#include <QString>

#include "../model/configuration.h"

namespace ct {

// The longest prefix of `text` that fits `budget` UTF-8 bytes, never splitting
// a codepoint. Chops whole CHARACTERS from the end rather than bytes, so the
// result is always a valid string — truncating the byte array instead can leave
// a lead byte behind, and fromUtf8 turns that into U+FFFD, which is three bytes
// and can push a "shortened" name back over the budget.
inline QString clipToUtf8Bytes(const QString &text, int budget)
{
    if (text.toUtf8().size() <= budget)
        return text;
    QString out = text;
    while (!out.isEmpty() && out.toUtf8().size() > budget)
        out.chop(1);
    return out;
}

// Hold `edit` to `budget` UTF-8 bytes as it is typed.
//
// On textChanged rather than through a QValidator: a validator returning
// Invalid rejects the whole edit, so pasting a long name would leave the field
// EMPTY rather than holding as much of it as fits — and the paste is exactly
// the case a typing cap is for. This keeps the prefix, which is also what the
// mapper's clip would have produced, so what the user sees is what would have
// been sent.
//
// The cursor is put back where the edit left it, bounded by the new length, or
// a paste into the middle of a full field would drop the caret to position 0.
inline void limitToUtf8Bytes(QLineEdit *edit, int budget)
{
    if (!edit)
        return;
    QObject::connect(edit, &QLineEdit::textChanged, edit, [edit, budget](const QString &text) {
        const QString clipped = clipToUtf8Bytes(text, budget);
        if (clipped == text)
            return; // the common path: no signal storm, no cursor move
        const int cursor = edit->cursorPosition();
        // Blocked because setText re-emits textChanged, and the re-entry would
        // fight the cursor restore below rather than do any more clipping.
        const QSignalBlocker block(edit);
        edit->setText(clipped);
        edit->setCursorPosition(qMin(cursor, clipped.size()));
    });
}

// The longest names the document's TARGET keeps whole, in UTF-8 bytes: a CAN
// Triple 2.0 whose names are in its label store keeps 32 of every one, the CAN
// Triple 1.x 31 of a channel's and 17 of a message's or relay's (in its
// records). A dialog with no document is held to the 1.x's.
inline int channelNameLimit(const Configuration *config)
{
    return config ? config->capacity().channelNameBytes() : MAX_CHANNEL_NAME_BYTES;
}

inline int messageNameLimit(const Configuration *config)
{
    return config ? config->capacity().messageNameBytes() : MAX_MESSAGE_NAME_BYTES;
}

} // namespace ct
