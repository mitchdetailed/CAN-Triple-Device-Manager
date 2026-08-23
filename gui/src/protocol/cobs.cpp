#include "cobs.h"

namespace ct {

QByteArray cobsEncode(const QByteArray &src)
{
    QByteArray out;
    out.reserve(src.size() + src.size() / 254 + 2);
    int codeIndex = 0;
    out.append(char(0)); // placeholder for first code byte
    quint8 code = 1;
    for (unsigned char b : src) {
        if (b == 0) {
            out[codeIndex] = char(code);
            codeIndex = out.size();
            out.append(char(0));
            code = 1;
        } else {
            out.append(char(b));
            if (++code == 0xFF) {
                out[codeIndex] = char(code);
                codeIndex = out.size();
                out.append(char(0));
                code = 1;
            }
        }
    }
    out[codeIndex] = char(code);
    return out;
}

// Mirrors the firmware's cobs_decode(): same loop, same lenient bounds check.
QByteArray cobsDecode(const QByteArray &src)
{
    QByteArray out;
    out.reserve(src.size());
    int read = 0;
    const int len = src.size();
    while (read < len) {
        const quint8 code = quint8(src[read]);
        if (code == 0)
            return {}; // zero can't appear inside encoded data
        if (read + code > len && code != 1)
            return {};
        ++read;
        for (quint8 i = 1; i < code && read < len; ++i, ++read)
            out.append(src[read]);
        if (code != 0xFF && read != len)
            out.append(char(0));
    }
    return out;
}

} // namespace ct
