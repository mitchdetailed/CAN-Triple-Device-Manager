// CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR).
// Identical to compute_crc16() in the firmware's protocol.h.
#pragma once

#include <cstdint>
#include <cstddef>

namespace ct {

inline uint16_t crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int j = 0; j < 8; ++j)
            crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                                 : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

} // namespace ct
