// Standard COBS (Cheshire/Baker), matching the firmware's cobs.c.
#pragma once

#include <QByteArray>

namespace ct {

QByteArray cobsEncode(const QByteArray &src);
// Returns an empty array on malformed input (mirrors firmware returning 0).
QByteArray cobsDecode(const QByteArray &src);

} // namespace ct
