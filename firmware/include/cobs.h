#ifndef COBS_H
#define COBS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode a block of data using Consistent Overhead Byte Stuffing (COBS).
 * @param src Pointer to the source data to encode.
 * @param src_len Length of the source data.
 * @param dst Pointer to the buffer where encoded data will be written.
 *            The destination buffer should be at least (src_len + src_len/254 + 2) bytes.
 * @return The length of the encoded data written to dst.
 */
size_t cobs_encode(const uint8_t *src, size_t src_len, uint8_t *dst);

/**
 * Decode a COBS-encoded block of data.
 * @param src Pointer to the encoded data.
 * @param src_len Length of the encoded data (including the overhead byte, excluding the 0 delimiter).
 * @param dst Pointer to the buffer where decoded data will be written.
 * @return The length of the decoded data, or 0 if an error occurred.
 */
size_t cobs_decode(const uint8_t *src, size_t src_len, uint8_t *dst);

#ifdef __cplusplus
}
#endif

#endif // COBS_H
