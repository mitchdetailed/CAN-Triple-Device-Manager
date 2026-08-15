/*
 * serial_proto.h — portable framing + command dispatch for the CAN Triple
 * protocol v2. Every byte the device emits goes through send_frame():
 * 0x00 + COBS(header+payload+CRC) + 0x00 — including telemetry and logs,
 * so the host never has to resync around unframed noise.
 */
#ifndef SERIAL_PROTO_H
#define SERIAL_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Queue raw wire bytes for physical transmission. Must accept up to
     * ~2100 bytes per call (largest encoded response frame). */
    /* Returns whether the bytes were ACCEPTED for transmission. The transport
     * buffer is finite, so a host link busier than it can drain will refuse
     * some frames; the monitor stream needs to know that happened so it can
     * mark the gap rather than let the trace lie by omission. A transport that
     * cannot fail may simply return true. */
    bool (*send_bytes)(const uint8_t *data, uint16_t length);
    uint32_t (*uptime_ms)(void);
    bool (*flash_save)(void);
    /* Apply a bus configuration change; returns false -> NACK ERR_BUS_BUSY. */
    bool (*control_can)(const ControlCanPayload *request);
    /* Fill out[0..2] with what buses 1..3 are CURRENTLY running, for
     * CMD_READ_CAN_SETUP. The glue owns that state because it owns the CAN
     * peripherals; this layer only serialises it. NULL makes the command NACK
     * ERR_INVALID_CMD, which reads to a host as "this firmware cannot tell you"
     * and sends it back to assuming bring-up rates — the behaviour every build
     * had before the command existed. */
    void (*read_can_setup)(ControlCanPayload out[3]);
    /* Schedule an MCU reset (CMD_RESET_DEVICE). Called after the ACK is queued;
     * the glue defers the actual reset so the ACK flushes first. May be NULL. */
    void (*request_reset)(void);
    /* Fill dst with `length` unpredictable bytes for the configuration-password
     * unlock challenge; false if none are available. NULL, or a false return,
     * makes the device refuse to issue a challenge — a protected config then
     * stays locked, which is the safe direction. Never substitute a counter or
     * a timestamp: a repeated challenge makes a captured reply replayable. */
    bool (*random_bytes)(uint8_t *dst, uint16_t length);
    /* v18: write this chip's 96-bit unique device ID (CONFIG_UID_LEN bytes) for
     * CMD_GET_DEVICE_ID. NULL reports all zeroes, which reads as "no identity"
     * — a host will then decline to bind rather than bind to nothing. */
    void (*device_uid)(uint8_t out[CONFIG_UID_LEN]);
} SerialProtoCallbacks;

void serial_proto_init(const SerialProtoCallbacks *callbacks);

/* Feed received wire bytes (any chunking). Commands are parsed and answered
 * synchronously from this call — invoke from the main loop, not an ISR. */
void serial_proto_feed(const uint8_t *data, uint16_t length);

/* Frame and send an arbitrary packet (used for telemetry and logs). */
/* Returns whether the transport accepted the frame. */
bool serial_proto_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t length);

/* Telemetry helpers — no-ops while the respective stream is disabled. */
void serial_proto_stream_monitor(uint8_t bus, uint8_t direction, uint32_t can_id,
                                 uint8_t is_extended, uint8_t is_fd,
                                 uint8_t brs, uint8_t esi,
                                 const uint8_t *data, uint8_t len);
void serial_proto_stream_values(void); /* emits all active signals, chunked */

/* Framed ASCII log output (CMD_LOG). */
void serial_proto_log(const char *text, uint16_t length);

bool serial_proto_monitor_enabled(void);
bool serial_proto_values_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* SERIAL_PROTO_H */
