# Firmware findings that affect the GUI (from deep-reading `../src`, July 2026)

> **Status: all of the issues below are fixed in the v2 firmware that now
> lives in [`firmware/`](firmware/README.md)** — flash it to get framed
> telemetry (live Monitor/Viewer), working flash persistence, runtime bus
> control, transmit messages, and standard DBC Motorola extraction. This
> document is kept as the reference for what the *original* firmware does,
> since the GUI still degrades gracefully against it (v2-only steps are
> skipped when the device NACKs them).

Issues the GUI works around today, ordered by impact. Fixing them in the
firmware unlocks a cleaner protocol; none block basic use.

## Blocking for advertised features

1. **Flash save/load is broken twice over.** *(Both confirmed on-target and
   fixed in v2 — see `firmware/README.md` "Flash map".)*
   - **The config region isn't real memory.** `routing_engine_save_flash()`
     targets `0x0801C000`, but the G473CB's dual-bank flash is **not
     contiguous**: bank 1 is `0x08000000-0x0800FFFF`, then an unpopulated
     **gap `0x08010000-0x0803FFFF`**, then bank 2 at `0x08040000-0x0804FFFF`
     (verified via SWD; OpenOCD reports `gap detected from 0x08010000 to
     0x0803ffff`). `0x0801C000` sits in the gap, so programming it fails with
     PGSERR/PGAERR (`HAL_FLASH_GetError()` = 0xA8 → `ERR_FLASH_WRITE`/0x05)
     even though the erase "succeeds" — the HAL erases *by page number* and
     quietly clears a real bank-2 page instead. It also uses "page 56, bank 1",
     which doesn't exist (bank 1 has pages 0-31).
   - **The tables wouldn't fit anyway.** They total 250×8 + 500×59 + 100×18 +
     100×18 = **35,100 B** vs the 16 KB reserved.
   *v2 fix: config lives at `0x08040000` (bank 2, 36 KB), page numbers are
   computed against the bank's physical base, only the used prefix of each
   table is stored, and the image carries magic + version + CRC.*

2. **`CMD_CONTROL_CAN` (0x0D) and `CMD_STREAM_VALUES` (0x0F) are defined but
   not handled** in `serial_parser.c` — both NACK `ERR_INVALID_CMD`. Bus
   baud/mode/listen-only cannot be changed over serial (bring-up is hardcoded
   in `events_Startup()`: CAN1 classic 1M, CAN2 FD 500k/2M + termination,
   CAN3 not started), and the always-on streams cannot be silenced.
   *GUI behavior: the bus Mode/Rate/FD/Termination fields carry the note
   "(applied on Send Configuration — firmware v2)", so they are editable and
   simply have no effect on a v1 device; streams are consumed either way.*
   Note that CONTROL_CAN is write-only even in v2 — reading the setup back is
   `CMD_READ_CAN_SETUP`, a separate command added later (#21).

3. **No periodic transmit composer.** The routing engine can receive, parse,
   route, and inject, but there is no engine that *composes* frames from
   signal values and transmits them cyclically — MoTeC-style "Transmit
   Message" sections have nothing to run on. Suggest a `TransmitMessageConfig`
   table (can_id, bus, rate, signal packing list) serviced from
   `events_100Hz`/`events_50Hz`. *GUI behavior: transmit sections can be
   edited and saved in the document, but upload validation reports them as
   requiring firmware support.*

4. **Bit extraction: the start bit is the signal's LSB (both byte orders).**
   The bit index table reads `7,6,5,4,3,2,1,0, 15,14,13,12,11,10,9,8,
   23,22,...`: bits are counted 0..7 **right-to-left** within a byte (bit 0 =
   LSB), bytes are counted **left-to-right** from 0, so bit `S` sits at
   `byte S/8, bit S%8`. From the start bit the walk ascends the bit within the
   byte and, on each byte boundary, steps to the **next** byte for **Intel**
   (`byte_order 0`, GUI "Word Swap") or the **previous** byte for **Motorola**
   (`byte_order 1`, GUI "Normal") — the `dbc_decode`/`dbc_encode` convention
   (`backend_functions.c`), with which the engine now agrees exactly.
   Examples: Intel 16-bit at start 0 = `data[1]<<8 | data[0]`; Motorola
   16-bit over bytes 2..3 (`value = data[2]<<8 | data[3]`) has its LSB at
   byte 3 bit 0 = `start_bit 24`; the top-of-frame Motorola 16-bit field
   (`data[0]<<8 | data[1]`) is `start_bit 8`. A Motorola field whose walk
   would step past byte 0 (e.g. start 7, 16 bits) is rejected.
   `engine_extract_raw`/`engine_pack_raw`, the GUI's `computeExtraction`, and
   the validation occupancy walk all use this exact traversal, so GUI
   acceptance and firmware extraction agree bit-for-bit.

## Robustness (GUI has workarounds, but worth fixing)

5. ~~**≥128-byte host→device bursts break RX.**~~ **FIXED — and this entry was
   left stale long enough to mislead a debugging session, so read the fix before
   trusting anything below it.**

   The v1 fault was real: `HAL_UARTEx_RxEventCallback`'s half-transfer re-arm
   failed with `HAL_BUSY` → `uart_abort`, recovered only by the 1 Hz re-init,
   and an exactly-256-byte fill never re-armed at all (dead until power cycle).
   The suggested cure was circular DMA.

   **That cure is already implemented.** `hdma_usart1_rx.Init.Mode =
   DMA_CIRCULAR` (`stm32g4xx_hal_msp.c`), with a wrap-aware callback copying
   into a 2 KB ring drained every main-loop pass. The DMA is never re-armed, so
   a long host burst cannot wedge reception. Nothing about frame size stops the
   GUI's 127-byte cap being raised today.

   Two things the fix did NOT cover, both still live:

   - **`serialRingWrite()` drops silently when the ring is full** (`main.c`),
     justified in a comment as "the host retries every command". That was not
     true: `DeviceLink` retried on TIMEOUT only, so a dropped byte became a bad
     CRC, became a NACK, and aborted the whole transfer — seen as
     `Sending channels (64/82) — Device error: invalid CRC (0x03)`. The host now
     retries `ERR_INVALID_CRC` specifically, so the comment is finally accurate.
     Two halves each assuming the other handled it is how this survived.

   - **An overrun deafens the device for up to a second.** `HAL_UART_ErrorCallback`
     answers ORE/NE/FE/PE with `HAL_DMA_Abort` + `HAL_UART_Abort` and defers
     recovery to the 1 Hz tick. The host's 5 retries × 250 ms = 1.25 s only just
     outlasts it. Re-arming in the callback, or ticking the recovery faster,
     would turn a one-second blackout into a hiccup.

   What actually corrupted the frame in the observed failure is NOT established.
   With circular DMA in place the plausible causes are line-level bit errors at
   7,372,800 baud, the ORE path above, or `rxBuffer` being lapped when the USART
   IRQ is delayed — that third one is much less likely at 1024 B than it was at
   256, but "less likely" is not "diagnosed". The retry makes all three
   survivable; none of them is diagnosed.

   **Opportunity this unblocked — now TAKEN (see #23).** `MAX_TX_PAYLOAD` was
   112 bytes and `WRITE_CHUNK_SIGNALS` 2 records per frame *solely* because of
   the v1 bug. The cap is now **496** inside a **512**-byte wire frame, and
   `rxBuffer` went 256 B → **1024** in the same change, because a frame larger
   than that buffer can be lapped before the callback copies it out — the two
   had to move together, exactly as this entry said. 1024 gives ≥2 frames of
   slack; 512 would have been exactly one and is not enough. Nothing else in the
   receive path needed touching: `MAX_RAW_PACKET`, `s_rx_accum`, `s_decode_buf`,
   `s_raw_buf` and the serial ring were all already 2048 (each verified, not
   assumed). The device→host cap (`MAX_RESPONSE_PAYLOAD` 2030) did not change.

6. **Command responses can be silently dropped.** Responses transmit from the
   UART ISR with blocking `HAL_UART_Transmit`; if a monitor/value-stream
   packet is mid-flight from the main loop, the ISR transmit returns
   `HAL_BUSY` and the ACK is lost. *GUI retries every command.* A TX queue
   drained from one context would fix it.

7. **Monitor/value streams and logs are sent completely UNFRAMED.** Only
   `serial_parser.c`'s static `transmit_packet()` adds the 0x55 header, CRC,
   COBS, and delimiter — but `routing_engine.c` bypasses it and calls
   `serial_transmit_packet()` directly (lines ~177/245/253/347/536), and
   `user_code.c`'s implementation is a bare `HAL_UART_Transmit` of the
   payload bytes with the `cmd` argument discarded. So on the wire, monitor
   frames are raw 76-byte structs, value streams raw buffers, and
   `routing_engine_log` raw ASCII. **The GUI's Monitor Channels and CAN
   Viewer windows stay empty until this is fixed.** Suggested fix:
   1. Expose a public framed transmit in `serial_parser.c/.h`
      (e.g. `serial_send_framed(cmd, payload, len)` wrapping
      `transmit_packet`) and call it from all five `routing_engine.c` sites.
      Give it its own encode buffer if it can run from a different context
      than command parsing (the current static buffers are shared).
   2. Give logs a dedicated command id (e.g. `CMD_LOG 0x90`) instead of
      reusing `CMD_ACK` — a framed 1-byte log would be indistinguishable
      from a real ACK and would falsely complete a pending GUI command.
   3. Emit a leading `0x00` before each framed response in
      `transmit_packet` (one line), mirroring the GUI's frames.
   4. Route printf debug output through framing too, or compile it out —
      today `onReceive` printf-logs every CAN frame plus 1 Hz stats on the
      same UART (via DMA, racing the blocking response transmits, which can
      return HAL_BUSY and silently drop a response).
   *GUI side: command traffic already survives the noise (suffix resync +
   retries), and the framed 0x82/0x83 demux is implemented and waiting — it
   starts working the moment the firmware frames its streams.*

## Correctness details

8. **`MATH_OP_SCALE` is an intentional alias of `MATH_OP_MUL` (`A*B`).** The
   two-input `MathConfig` has no offset field, so the old "(InputA * factor) +
   offset" intent was never expressible. `protocol.h` documents the op as
   "A * B (kept for compatibility)" and the GUI labels it "Scale (A × B)", so it
   is honest, not a bug; the earlier validation warning was removed (resolved).
9b. **The signal record is 48 bytes (v15), and MAX_SIGNALS is 768.**
    *(Both figures are HISTORY — the record is 64 B and MAX_SIGNALS is 1000 since
    #23, which took the label back to 32 bytes and the name cap back to 31. The
    bit-packing, the 2-byte mux window and the reasoning for all of it below are
    still current; every number in this entry describes v15.)*
    `CanSignalConfig` was 72 B and, at 500 entries, was 36 KB — **69% of the
    whole config image**, so it was the only place real capacity existed. Three
    changes got it to 48 B (`518 -> 768` signals in the same region):
    - `label` 32 -> 16 B. The engine NEVER reads the label (grep it): it exists
      only so a Get can reconstruct channel names, i.e. 44% of the record was
      doing no runtime work. Names are now capped at 15 UTF-8 bytes
      (`MAX_CHANNEL_NAME_BYTES`) by the name editors and DBC import, and the
      mapper's clip + collision warning moved with it as the backstop for
      documents saved before the cap.
    - the eight small fields are bit-packed into `msg_and_flags` (msg_idx 9,
      byte_order, is_active) and `bits` (start_bit 9, bit_length-1 6,
      value_type 6, decimal_places 4, mux_byte_offset 6).
    - `mux_id`/`mux_mask` 32 -> 16 bits.
    Packing is done by HAND, not with C bitfields, whose layout is ABI-defined
    while this is a wire format. The GUI mirrors the same layout in
    `wire_structs.h`, so `test_firmware_link` — the one place both headers are
    visible — packs with the GUI's setters, reinterprets the bytes as the
    firmware's struct and reads back with the firmware's accessors. That test
    was negative-checked: a deliberate one-bit shift error fails it.
    **Consequences to know:** the compound selector window is now 2 bytes, so a
    multiplexor field must lie within 16 bits of its offset — `dbc_import`
    rejects wider ones, and the mapper ERRORS rather than truncating (a
    truncated mask would become 0, which means "always active", silently
    un-gating the signal). `mux_byte_offset` is 6 bits, exactly enough for a
    64-byte FD frame with zero headroom. `decimal_places` is now 4-bit
    unsigned, so negative precision is unreachable (the GUI never emitted it).
    `applySignalScaling`/`inverseSignalScaling` hoist their packed reads once
    per call — they run per signal per frame across up to 768 signals.
    `WRITE_CHUNK_SIGNALS` doubles 1 -> 2 records per frame (4 + 2x48 = 100 of
    112), so a full Send spends **half** the round trips on signals it used to;
    `READ_CHUNK_SIGNALS` goes 28 -> 42. Flash: `FLASH_STORE_VERSION` ->
    15, CFG_TOTAL 52,768 of 53,248 with 480 B spare (778 signals would fit
    exactly but leave zero slack, hence 768).

9d. **Decrementors are integrators with a sign and a seed (v17).** Rather than a
    second table, `MAX_INTEGRATORS` went 4 → 8 and `IntegratorConfig` gained
    `start_value` plus `INTEGFLAG_COUNT_DOWN` / `INTEGFLAG_PRESERVE`. A
    decrementor is COUNT_DOWN + a start value at the peak + `min_value` as the
    floor. One table, one evaluation pass, one set of bugs.
    - The record grew 26 → 30 B for **zero** flash cost: PAD8(26) and PAD8(30)
      are both 32, so the slot was already paying for it. Widening the table
      4 → 8 is what costs — CFG_TOTAL 52,896 → **53,024 of 53,248, 224 B
      spare**. Growing the region is now the only way to add anything: it can
      go 53,248 → 57,344, but note that in SINGLE-bank mode the preserve ring
      sits flush under the config store and its base is already only 4 KB above
      the app ceiling, so 57,344 consumes exactly the last of that headroom
      (`PRESERVE_SINGLE_BASE >= 0x08010000` would hold with nothing to spare).
    - **`start_value` is applied at config LOAD and on record WRITE, not on a
      reset edge.** A decrementor that boots at zero is useless, so
      `engine_load_config` calls `engine_seed_integrators()` after
      `resetRuntime()`. The write path matters just as much and is easy to miss:
      an upload opens with `CLEAR_CONFIG`, which zeroes every value slot, so
      seeding only at load would leave a just-sent decrementor parked on its
      minimum for the whole session — until someone rebooted the device and it
      "fixed itself". `engine_table_write` seeds the records it writes. Start
      and reset are separate fields on purpose: they usually match, but
      conflating them would make "boot full, reset to a different mark"
      unexpressible.
    - **Ordering matters and is load-bearing:** load config (seeds
      `start_value`) → `preserveRestoreAtBoot` (seeds the retained total). The
      retained value must win, or Preserve would silently do nothing. Reversing
      those two lines is a real bug the test suite pins.
    - **Preserve keys namespace both tables.** Counters keep 0..MAX_COUNTERS-1;
      integrators start at `PRESERVE_KEY_INTEGRATOR_BASE` (= MAX_COUNTERS), and
      the boot loop walks `PRESERVE_KEY_COUNT`. Without the base, counter 0 and
      integrator 0 would share key 0 and restore into each other's slots.
    - **Wear is NOT symmetric with counters.** A counter is event-driven and
      often unchanged for a whole minute, so change detection makes its flush
      free. A running integrator changes every step, so it appends a record at
      essentially every 60 s flush. With N such values the active page (~254
      records) fills in ~254/N flushes, i.e. an erase roughly every 254/N
      minutes of running. The GUI reports that estimate rather than hiding it.
      `PRESERVE_MAX` stays 20 — raising it would trade endurance for a limit
      nobody has hit. On a SINGLE-bank part this also means the brief
      erase stall (#18's asymmetry — same bank as the executing code, at most
      one 100 Hz tick) goes from "essentially never" for counters to roughly
      once per 254/N minutes for integrators.
    - Retention itself needs no work per flash mode: the ring already places
      itself from the DBANK bit at runtime (#18). Note `COUNTERFLAG_PRESERVE`'s
      comment in `protocol.h` claimed the opposite — "on a single-bank device
      the firmware disables persistence entirely" — for long after #18 fixed it,
      and pointed at the very file that disproves it. Corrected at v17.

9c. **Integrators are rate accumulators, not time integrals (v16).**
    `IntegratorConfig` (26 B, `MAX_INTEGRATORS` = 4, cmds `0x23`/`0x24`) adds its
    input to its output slot `rate_hz` times a second — `out += input`, with the
    rate scaling the result. That is a deliberate choice over `input * dt`: with
    a true integral the rate would only affect precision, and a *configurable*
    rate would be close to pointless. To integrate a rate channel into a total,
    scale it with a Math channel first.
    - **The phase counts in Hz·ms.** It gains `elapsed_ms * rate_hz` per tick and
      spends 1000 per step, so a rate that doesn't divide 1000 keeps its
      remainder and averages exactly `rate_hz` steps/second. A truncated integer
      period over-fires instead: 7 Hz stored as 142 ms gives 704 steps per 100 s
      rather than 700. `test_firmware_link` runs that exact case for 100
      simulated seconds, so the shortcut cannot creep back in.
    - `steps = phase / 1000` is a division, not a loop, so an unusually long tick
      cannot spin — the same hazard `clampRoll` was fixed for.
    - **The enable gate freezes the phase** instead of discarding steps, so
      gating pauses accumulation cleanly. Reset is edge-triggered, applies even
      while disabled (like a counter's), and zeroes the phase.
    - The reset seed is clamped **only when `max > min`**, respecting the
      "`hi <= lo` = clamping off" convention `clampRoll` already uses. The
      counter clamps its seed unconditionally, which pins an unclamped counter's
      reset value to `min` — the integrator deliberately doesn't inherit that.
    - v16 had no PRESERVE flag, so totals reset at power-up; v17 added one (see
      9d). The GUI still warns when an integrator has no reset channel.
    Flash: `FLASH_STORE_VERSION` → 16, `FLASH_NUM_TABLES` 11 → 12, and the table
    adds 4 × PAD8(26) = 128 B, taking CFG_TOTAL 52,768 → 52,896 of the 53,248 B
    region. (v17 widened the table again — see 9d for the current figure.)

9a. **Conditions hold up to 3 comparisons (v14).** `ConditionConfig` grew
    13 → 35 bytes: `ConditionTerm terms[3]` (10 B each) + `dest_signal_idx` +
    `term_count` (1..3) + `joiners` (one bit per gap, 0 = AND, 1 = OR) +
    `is_active`. `executeConditions()` folds them **strictly left to right**,
    `((t0 J0 t1) J1 t2)`, NOT C's precedence — the GUI prints that bracketing so
    the two can't drift. Every term is evaluated (no short-circuit) to keep the
    pass constant-time, and every term's `input_a_signal_idx` is bounds-checked
    up front so one bad index skips the whole condition rather than silently
    contributing a false. Commands 0x08/0x09 are reused: the size change makes a
    version mismatch fail the length check (`4 + count*item_size`) cleanly.
    Chunks moved to WRITE 3 (4+3×35=109 ≤ 112) and READ 50 (4+50×35=1754).
    Flash: `FLASH_STORE_VERSION` → 14, the conditions table grew 1600 → 4000 B
    (PAD8 16 → 40 × 100), CFG_TOTAL 49,504 → 51,904 of the 53,248 B region —
    still fits, 1,344 B spare, so the region did NOT need to grow.
9. **Conditions are boolean logic channels (v5)**: each writes `1.0`/`0.0`
   to its `dest_signal_idx` and has no side effects. The v2–v4 action model
   (block/force routing, set value, mute bus) was removed, so routing follows
   only each message's static `route_bus_mask`.
10. **No bounds checks** on math/condition signal indices
    (`input_a_idx`, `input_b_idx`, `dest_signal_idx`) — out-of-range values
    corrupt RAM. The GUI validates, but the firmware should too. (v5 condition
    execution does bounds-check `input_a`/`dest` before writing.)
11. **Routed/injected frames carry the full payload (≤64 B) — resolved.**
    `CAN_Message` and `InjectCanPayload` hold `data[64]`; the TX path derives the
    FD DLC from the actual length (up to 64) and BRS follows `CAN_Message.brs`, so
    gateway routing, relays, and injection no longer truncate FD frames. (Earlier
    firmware used an 8-byte `TxMessage.data[8]`; its validation warning was
    removed.) The remaining FD caveat is #13: a **≤8-byte FD** frame is
    re-transmitted as classic because the frame format is inferred from `len > 8`
    rather than the FDF flag.
12. **Compound (multiplexed) messages** (v8) — a message is still matched by
    `(can_id, src_bus, ext)`, but each signal now carries an optional mux
    selector (`mux_byte_offset` / `mux_id` / `mux_mask`). When `mux_mask != 0`
    the signal is extracted from a received frame only while
    `(selector & mux_mask) == (mux_id & mux_mask)`, where `selector` is the
    up-to-4-byte little-endian window of the frame at `data[mux_byte_offset..]`.
    A `mux_mask` of 0 means "always active" (used by ordinary single-message
    sections). A GUI compound section carries channels only inside identifiers —
    no shared always-present set; each identifier's rows carry its
    `byteOffset`/`id`/`idMask`, and a channel needed in every variant is defined
    in each identifier. `CanSignalConfig` grew 63 → 72 bytes for these fields
    (padded flash slot 64 → 72; worst-case image ~47 KB, still inside the 48 KB
    region). A gated-out signal holds its previous value. **Compound TRANSMIT
    (v10):** the composer detects a compound transmit message from its mux-gated
    signals and sends one variant frame per identifier — `MSGFLAG_TX_SEQUENTIAL`
    selects Batch (all each period) vs Sequential (one per period, round-robin).
    Each variant packs one identifier's signals and writes that identifier's
    selector into the frame. No struct sizes changed.
13. `DeviceStatus.bus_state[]` is hardcoded `{1,1,1}` at init and never
    reflects real bus state; on the **monitor stream**, BRS and ESI are read
    from the FDCAN RX header and carried in `MonitorStreamPayload.flags`
    (bits 2/3), but `is_fd` (bit 1) is still inferred from `len > 8` rather than
    the FDF flag — so a ≤8-byte FD frame without BRS is reported as classic
    (the GUI's `.asc` export treats BRS as implying FD to compensate); NACKs
    don't echo the failing command (correlation is by ordering). (An earlier
    note here about the Motorola range check validating only `start_bit` is
    obsolete — see #4: both directions now validate the full walk.)
14. **Constants (v6)** are a dedicated 100-entry `ConstantConfig` table
    (`dest_signal_idx`, `value`, `is_active`, 7 bytes; commands 0x14/0x15).
    `executeConstants()` writes each active constant's value into its slot at
    the start of every evaluation pass — in both `engine_process_can()` and
    `engine_tick()`, before math — and does bounds-check `dest_signal_idx`.
    Because a constant slot is rewritten every pass, nothing else should target
    it; the GUI gives each constant its own generated channel to guarantee
    that. Adding the table bumped `PROTOCOL_VERSION` and `FLASH_STORE_VERSION`
    to 6 (older flash images are rejected on load).
15. **Flash-resident config (v7)** — the config tables live in the bank-2 flash
    region and the engine reads them in place, so ~40 KB of RAM was reclaimed
    (RAM ~62% → ~32%). Layout: a 64-byte header at offset 0, then fixed
    8-byte-padded record slots per table (padded so each slot is an independent
    doubleword program). `CLEAR` erases the region; `WRITE_*` programs records
    (the engine iterates only the written prefix `g_count[]`, so 0xFF slots are
    never read as active); `SAVE` commits the header (counts + bus setup + CRC
    over header + live records) marking it valid; boot validates. Correctness
    rests on the cooperative main loop (`serialRingDrain` does the programming;
    `trigger_CAN_RX`/`engine_tick` read config; they never interleave) and on
    config being in bank 2 while code runs from bank 1 (no read-during-program
    stall). The **wire protocol is unchanged** — only `FLASH_STORE_VERSION`
    bumped to 7. Behavior change: `CLEAR_CONFIG` erases the single flash copy,
    so it also clears the persisted config (the old two-copy design let
    Load-from-Flash undo a clear). The header also holds a 32-byte
    **configuration name** (`CMD_WRITE_CONFIG_NAME`/`CMD_READ_CONFIG_NAME`,
    0x16/0x17): the GUI Send dialog forces a non-empty ≤32-byte title, it
    persists with the image, and Get reads it back. `CMD_RESET_DEVICE` (0x18)
    ACKs, then the glue calls `NVIC_SystemReset()` ~150 ms later (via a
    `HAL_GetTick()` deadline serviced in `events_100Hz`) so the ACK — and any
    queued telemetry — drains over the UART before the core reboots.
16. **Message relay (v11)** is a dedicated 32-entry `RelayConfig` table
    (`address`, `bitmask`, `flags`, `src_bus`, `forward_bus_mask`, 11 bytes;
    `ENGINE_TABLE_RELAYS` = 7; commands 0x19/0x1A) — the first config table that
    is not a "message". `engine_process_relays()` runs at the top of
    `engine_process_can()` on **every** received frame, before the message-table
    match, so a relayed frame need not correspond to any configured message. A
    rule fires when it is active, listens on the frame's bus (`src_bus`), matches
    the frame's extended-ness, and `(can_id & bitmask) == (address & bitmask)`
    (`RELAYFLAG_INVERT` flips that test); it then re-transmits the frame verbatim
    to every bus in `forward_bus_mask` except the source (the GUI mapper already
    masks the source bit out, and the firmware skips `dest == src_bus` as a
    belt-and-braces guard). Because forwarding reuses the same `transmitFrame`
    path as gateway routing, it likewise carries the full ≤64-byte payload (#11).
    Adding the table bumped `PROTOCOL_VERSION`/`FLASH_STORE_VERSION` to
    11 and `FLASH_NUM_TABLES` to 8; worst-case image stays inside the 48 KB
    region. The relay table is unknown to older firmware, so the GUI's Send/Get
    treat `WRITE/READ_RELAY_CFG` as optional (a NACK is tolerated).
17. **Lookup tables (v12, 1-axis widened to 16 sites in v13).**
    *(The 2-axis half of this entry is HISTORY — the 4x4 it describes has been
    replaced by the 8x8, and every figure below that mentions the 4x4, the
    52 KB region or `FLASH_NUM_TABLES` 11 was current at v13 and is not now. See
    #23. The 1-axis 2x16 description is still accurate.)*
    `Table4x4Config`
    (2 axes, up to 4 sites each, 105 B, commands 0x1D/0x1E) was unchanged at v13. The
    1-axis table became **2x16** in v13 and is **split across two records**: a
    combined 16-site record is 134 B, over the 112 B host→device payload cap, so
    it ships as `Table2x16Def` (70 B — axis, destination, flags, `x_count`,
    `x_sites[16]`; commands 0x1F/0x20) plus `Table2x16Out` (64 B — `outputs[16]`;
    commands 0x21/0x22). They are two parallel tables
    (`ENGINE_TABLE_TABLES_2X16_DEF/OUT` = 8/9, `TABLES_4X4` = 10, 8 of each)
    **paired by index** — the `Out` record has no flags or count of its own.
    Each carries an `x_count` / `y_count` so a table can be **partial** — only
    the populated sites are used, and the lookup bounds `axisResolve` by the
    count.
    **Torn-upload guard:** `flash_store_slot()` does NOT zero-fill past the
    written prefix (unlike `engine_table_read`), so an un-programmed `Out` slot
    reads as `0xFF…` = NaN. `executeTables2x16` therefore evaluates only
    `t < min(count[DEF], count[OUT])`, and the GUI writes **Out before Def** so a
    table cannot go live before its values land. Note `flash_store_validate()`
    does not cross-check that the two counts are equal, so the runtime `min()` is
    the real defence, not an optimisation. `executeTables2x16`
    / `executeTables4x4` run right after `executeConstants()` in both
    `engine_process_can()` and `engine_tick()`, so a table can feed math /
    conditions in the same pass (a table reading a later calc's output lags one
    pass, like any ordering dependency). `axisResolve()` returns a `(i0,i1,w)`
    triple — value = out[i0]*(1-w)+out[i1]*w — so interpolate (w = frac) and
    discrete-centered (nearest site, w = 0, transition at frac 0.5 = the
    midpoint) share one path; the 4x4 uses a bilinear blend of the four bracketed
    cells, which collapses correctly when an axis is discrete (i0==i1, w=0).
    Inputs clamp to the end (used) sites. The 4x4 record (105 → padded 112)
    pushed `MAX_PADDED_RECORD` from 72 to 112 (the `flash_store_write` scratch).
    v13: `FLASH_STORE_VERSION`/`PROTOCOL_VERSION` → 13, `FLASH_NUM_TABLES` → 11,
    worst-case image ~48.4 KB — which no longer fit the old 48 KB region (only
    160 B of slack remained), so `FLASH_STORE_CAPACITY` grew to **52 KB (53248)**
    and the preserve ring moved to `0x0804D000`. The capacity **must stay a
    multiple of 4096**: in single-bank mode the region sits at
    (top of flash − capacity) and is erased by 4 KB page number, so an unaligned
    capacity would compute a page starting below the region and erase the wrong
    2 KB (a `_Static_assert` in `user_code.c` now enforces this).
    The v12 2x8 commands **0x1B/0x1C are retired and not reused** — the v13 `Def`
    record is coincidentally also 70 B, so honouring the old IDs would let a v12
    host's 2x8 record pass the length check and be stored as a 2x16 definition.
    Unknown to older firmware, so Send/Get treat the table commands as
    optional.
18. **Retained counter values (`COUNTERFLAG_PRESERVE`).** Counters flagged
    Preserve keep their output across power cycles. `preserve_store.c` is a tiny
    **2-page append-log ring**, injected-driver like `flash_store` and, like it,
    **placed from the DBANK bit at runtime so it works in BOTH flash modes**:
    dual-bank puts it in the free space *above* the config store at
    `0x0804D000` (two 2 KB bank-2 pages, 26/27); single-bank puts it immediately
    *below* the store at `0x08011000` (two 4 KB bank-1 pages, 17/18) — below,
    because there the config store runs to the top of flash — in the slack
    between the 64 KB app ceiling and the store. Both layouts are
    `_Static_assert`ed. The ring logic itself is page-size agnostic (page size
    only sets records-per-page), and `test_preserve` runs its whole suite at
    **both 2 KB and 4 KB** to prove it; 4 KB pages actually halve the erase
    count, since more records fit before a compaction.
    Records are 8-byte tagged unions —
    `{key (counter index), SignalDataType, csum, value}` — so a `u32` counter is
    not laundered through float32. The design goal is FEWEST ERASES:
    `preserve_sync()` is change-detected (an unchanged 60 s flush touches no
    flash), each record is one doubleword (~254/page), and a full page compacts by
    carrying only the latest record per live key into the other page (the sole
    steady-state erase — measured ~4 erases per 1000 changed writes). The commit
    word (`seq` + config-CRC tag) is written LAST, so a compaction interrupted by
    power loss leaves the previous page authoritative; each record has a check
    byte so a torn append is detected and recompacted. The store is tagged with
    the config header CRC (`flash_store_config_crc()`); a config change (or `SAVE`)
    re-tags it → reformat, so retained values never reattach to different counters
    ("invalidate on change"). Boot (`preserveRestoreAtBoot`) seeds each PRESERVE
    counter after `engine_load_config`; `events_1Hz` flushes every 60 s.
    Works in **both** flash modes (see above); it was dual-bank-only until the
    ring's geometry became runtime-derived, and on a single-bank part it had
    silently done nothing. `events_Startup` logs
    `flash: DBANK=n cfg@… pagesz=… retainedCounters=n` over `CMD_LOG` at every
    boot so the active layout — and whether retention is live — is observable
    rather than guessed. One asymmetry: a dual-bank erase runs read-while-write
    (app in bank 1, ring in bank 2) and stalls nothing, whereas the single-bank
    erase is in the executing bank and halts the core for its duration — at most
    once per 60 s flush and only when the active page fills, so the engine
    misses at most one 100 Hz tick. The GUI caps Preserve at 20 counters and
    shows a running "N of 20" budget. A torn record that raises an uncorrectable ECC error
    on read → NMI → `NVIC_SystemReset`; a `.noinit` boot-loop guard
    (`g_preserve_guard`, survives the warm reset) detects a scan that faulted and
    `preserve_format()`s the store rather than re-scanning, so a corrupt record
    cannot brick the boot. **No wire/protocol change** — the flag already
    round-tripped; this only makes the firmware act on it.
19. **Dual-bank flash: 64 KB code ceiling + pinned linker script.** In dual-bank
    mode (DBANK=1, the shipped config) the 128 KB is two NON-CONTIGUOUS 64 KB
    banks — bank 1 `0x08000000-0x0800FFFF` (the linked app), an unpopulated gap
    `0x08010000-0x0803FFFF`, then bank 2 `0x08040000-0x0804FFFF` (config store +
    preserve ring, addressed as raw flash, not linked). ST's stock
    `STM32G473CBTX_FLASH.ld` declares one flat 128 KB `FLASH` region, so a >64 KB
    image would link cleanly but place its overflow in the dead gap (flashes, will
    not boot). `firmware/STM32G473CB_dualbank_64k.ld` (selected via
    `board_build.ldscript`) caps `FLASH` LENGTH at 64K so the linker hard-errors
    at the real ceiling, and the board JSON's `upload.maximum_size` is 65536 so
    PlatformIO's size report matches. It also adds a `.noinit` RAM section (used
    by #18's guard). If ever reflashed single-bank (DBANK=0, contiguous 128 KB),
    restore ST's 128K length.
20. **Version reset to 1; the fleet identity moves into the build (v1).**
    *(The numbers in this list are finding ids, not firmware versions — #20 has
    always been the access/identity entry and keeps its place.)*
    `PROTOCOL_VERSION` and `FLASH_STORE_VERSION` are both **1**, and the dead
    `PROTOCOL_VERSION_V2`…`_V17` ladder is deleted. Nothing branched on those
    constants — grep them — so they recorded the development history of a product
    no customer has ever held. Nothing has shipped, so nothing in the world
    remembers the old numbers, and a version whose only two readers are halves of
    one repository that are always rebuilt together is a liability rather than a
    compatibility guarantee.
    - **The `.ct3` FILE schema is deliberately NOT reset.** `kConfigSchemaVersion`
      stays on its own count (9). Files written at 2 through 8 are sitting on
      people's disks, and `readWrapper()`'s "saved by a newer version of CAN
      Triple Device Manager" guard would refuse every one of them if the schema
      restarted at 1. A file format's version answers to the files in the world;
      a wire version answers to the two programs at either end of a cable.
    - **Consequence for users, unchanged from every earlier `FLASH_STORE_VERSION`
      bump:** a stored image from an earlier build fails validation, so **the
      first boot after flashing comes up on bring-up defaults and the
      configuration has to be sent again**. There is no migration path and
      deliberately no attempt at one — reinterpreting an older header would mean
      reading one record as another, and the failure mode of getting that wrong is
      a device that believes it is protected when it is not.
    - **The identity is COMPILED IN and there is no command that can change it.**
      `firmware/include/fleet_identity.h` builds a `FleetIdentity` out of the
      `CT_VENDOR_ID` / `CT_MODEL_ID` / `CT_SERIAL_NUMBER` / `CT_FLAGS` /
      `CT_FLEET_KEY` defines. Those are **no longer written in
      `platformio.ini`**: the tracked file carries no identity values, and
      `firmware/scripts/build_flags.py` emits the `-DCT_*` flags from
      `firmware/identity.local.ini`, a gitignored `KEY=VALUE` file created by
      copying `identity.local.ini.example`. Missing file = unprovisioned build;
      malformed file = hard build error naming the line. The `'"..."'` quoting
      this note used to warn about is gone with the flags — vendor and model go
      in as plain unquoted text, spaces and all, and the script adds the quoting
      the define needs. (`CT_FLAGS` has no identity-file key; it stays 0 until
      something reads its bits.) An identity no packet can change is one no
      attacker can change, one no flash
      erase can lose, and one that cannot drift out of step with a configuration
      sent later. The cost is that `CT_SERIAL_NUMBER` is per-UNIT, so **each board
      needs its own compile-and-upload** — no provisioning tool, no database, and
      nothing reflashable away, in exchange for not flashing fifty boards from one
      binary. (The STM32 96-bit UID needs no provisioning at all and is still
      there for the v18 device binding, but silicon chooses that number, not you,
      so it can never be the serial you print on an invoice.)
    - **Commands.** `CMD_READ_UPDATE_ID` → **`CMD_READ_FLEET_ID` 0x2F** (answers
      `FleetIdentityPublic`, 41 B, fleet key replaced by a `key_present` byte),
      `CMD_UPDATE_ID_PROVE` → **`CMD_FLEET_ID_PROVE` 0x31** (unchanged behaviour:
      host challenges, device answers HMAC-SHA256(fleet key, challenge)).
      **`CMD_WRITE_UPDATE_ID` is GONE and 0x30 has been REUSED** — it is
      `CMD_READ_CAN_SETUP` now (see #21). Unlike the retired 0x1B/0x1C and
      0x25-0x28, that id really was safe to reuse, because this is protocol v1
      and no host anywhere holds an older meaning for it; the retirements exist
      only because a v12 or v18 host could otherwise get a plausible-looking
      answer out of a device that no longer means the same thing by the id, and
      no such host exists for 0x30. Both fleet
      commands stay answerable unconditionally: a locked-down unit still has to be
      able to say "is this update for me?".
    - **Header record swap.** The 20-byte `UpdateIdentityRecord` comes out of the
      flash header and a **`uint16_t config_version` goes in**; the 13-byte
      `AccessKeyRecord` stays. `flash_store_commit()` / `flash_store_validate()`
      take the version by value instead of an identity pointer. The header area is
      still 256 bytes, so **no table offset moves** and CFG_TOTAL is unchanged —
      but the *layout* differs, which is exactly why the version has to be
      enforced rather than tolerated.
    - **`config_version` is the one runtime field**, because it has to move every
      time a configuration is released and baking it in would mean a reflash per
      revision. It arrives as an **optional 2-byte payload on `CMD_SAVE_TO_FLASH`**
      — empty payload leaves the stored version alone — riding the commit rather
      than having a command of its own, so a version cannot land without its
      configuration or a configuration without its version. Unlike the access keys
      it does **not** survive `engine_clear_config()`: a device with no
      configuration is running revision nothing, and reporting a stale version for
      tables that are gone would make the host's "is this newer?" check answer
      about a config that does not exist.
    - **The access keys still outlive `CLEAR_CONFIG`** and still live inside the
      header CRC along with the config version, so editing either in a flash dump
      invalidates the image and a tampered header reads as empty rather than
      booting. That is weaker than a signature and still worth having. It is no
      defence against the dump itself: the keys are four bytes sitting in flash and
      the only real backstop is STM32G4 readout protection, a programming step
      rather than anything this firmware can do.
    - **OPEN: access keys are not durable on their own.** `CMD_WRITE_ACCESS_KEYS`
      calls `engine_set_access_keys()` and stops there, so a new key is in force
      for the session but does not reach flash until the next header commit —
      in practice the next `SAVE_TO_FLASH`. Set a password, power-cycle before
      sending a configuration, and the password is gone: the device comes back
      **unprotected while the operator believes it is locked**, which is the worst
      direction for that failure to point and is invisible until someone tries to
      rely on the lock.

      Committing inside the write handler was tried and **reverted**, because it
      cannot work where the keys currently live. They sit in the flash header, and
      STM32 flash programs each doubleword once per erase — so a header already
      written since the last erase cannot be rewritten. The first password after
      an erase would stick and every change after it would fail with
      `ERR_FLASH_WRITE`. That is worse than the present behaviour: it works just
      long enough to be trusted. Rewriting the whole 52 KB region instead is not
      available either — there is nowhere to stage the live records — and a commit
      issued between a `CLEAR_CONFIG` and its `SAVE_TO_FLASH` would mark a
      half-uploaded configuration valid, which is exactly what the
      erase/write/commit ordering exists to prevent.

      Two things hold the line meanwhile. `AccessPasswordsDialog` warns, at the
      moment a password is set, that it reaches the device's memory only when a
      configuration is sent. And `testAccessKeyDurability` in
      `test/test_firmware_link.cpp` asserts that the key is **not** in flash
      before the commit and **is** after it, so the one-line "obvious" fix cannot
      be reintroduced without a failing test explaining why.

      The real fix is a small flash page of its own for device state — an
      append-log like `preserve_store.c` already runs for retained counters,
      erased only when it fills. That is its own layout, its own wear behaviour
      and its own tests, and it should be done deliberately rather than folded
      into a command handler.
21. **`CMD_READ_CAN_SETUP` (0x30) — the bus setup can finally be read back.**
    Answers `ControlCanPayload[3]`, buses 1..3 positionally; the request carries
    no payload.

    This closes the last hole in Get Configuration. `CMD_CONTROL_CAN` was
    write-only, and no table on the wire records a bus: the message and signal
    records describe what is carried, never what carries it. So a Get could
    recover every message, channel and calculation and then had to **guess** the
    modes and bitrates — `mapFromDevice` assumed the bring-up rates (CAN1 1M,
    CAN2 500k + FD 2M, CAN3 500k) and appended a note telling the user to go and
    check Communications Setup before sending. Assuming was strictly better than
    the alternative, since the new-document defaults would have re-rated a
    running bus on the next Send, but it was still the one part of a Get that was
    not simply true, and a configuration that comes back subtly different from
    the one that went out is a poor answer to "what is on this device".

    Four things about it are decisions, not mechanics:
    - **It reports the LIVE setup, not the stored image.** The glue owns that
      state because it owns the peripherals (`read_can_setup` in
      `SerialProtoCallbacks`); `serial_proto.c` only serialises it. After a boot
      live and stored agree; mid-session, after a Send that has not been saved,
      they do not — and what the buses are actually running is the honest answer
      to what the device is doing right now.
    - **It is gated on `ACCESS_FN_GET`.** A bus map says as much about a
      proprietary setup as the message table does, so letting it out from behind
      the Get password would have been a hole in that password rather than a
      convenience.
    - **`bus_idx` is stamped by the protocol layer**, not taken from the glue.
      The reply is positional, so a host that cross-checks the index must not be
      able to be handed something inconsistent with the slot it arrived in.
    - **A NULL `read_can_setup` NACKs `ERR_INVALID_CMD`**, which reads to a host
      as "this firmware cannot tell you" and sends it back to assuming bring-up
      rates. That is deliberately the same answer older firmware gives, so the
      host needs one code path for both.

    *GUI side: the read is the last step of a Get and is optional exactly like
    the relay and table reads (`ConfigTransfer`). An empty `busSetup` means
    **unknown**, never "off" — a distinction worth keeping, because "off" is a
    thing a Send would then act on. One lossy edge is recorded rather than
    hidden: mode 2 (listen-only) reads back as enabled, since the document models
    only enabled/disabled and calling it Off would let a later Send silently stop
    a live bus. `mapFromDevice` raises a note whenever that conversion happens.*
22. **Advanced math (ops 9–30): `MathConfig` grows 18 → 24 bytes inside the
    flash slot it already occupied.** The flash store pads every record to an
    8-byte boundary, and `PAD8(18)` = 24 — so the third operand costs **zero
    config-flash**; the six new bytes were being erased and written as pad all
    along. Offsets 0–17 are unchanged; bytes 18–23 carry `input_c_type` plus
    `input_c_val[4]` — four RAW bytes hand-packed in the #9b discipline (no
    union, no bitfield): float32 LE when the operand is a constant, u16 signal
    index in the first two bytes (rest zero) when it is a channel — and a
    reserved byte written 0.
    - **`FLASH_STORE_VERSION` 1 → 2 — because of the CRC, not the layout.**
      Slot geometry is identical (same 24-byte slots, same counts, same
      offsets), and a pre-existing image can only contain ops 0–8, none of
      which read C. But `imageCrc()` hashes `item_size` bytes per live record,
      and `item_size` is `sizeof(MathConfig)`: a v1 image whose header CRC
      covered 18 bytes per math record fails validation the moment this build
      hashes 24 (the extra six being the 0xFF pad the old write path left).
      That rejection is safe but **config-dependent** — a math-free v1 image
      would have sailed through. The bump trades that lottery for the uniform
      #20 behaviour: every v1 image reads as empty, the host re-sends. The GUI
      still doesn't trust pad bytes: `mapFromDevice` normalises C to an unused
      const 0 for any op of arity < 3, so 0xFF garbage never reaches the
      document.
    - **No `PROTOCOL_VERSION` bump either — the length check is the guard.**
      Table writes must satisfy `length == 4 + count*item_size`, so an old
      GUI's 18-byte records NACK `ERR_INVALID_LEN` against new firmware and a
      new GUI's 24-byte records NACK against old firmware, the same
      cross-version behaviour the v14 condition-record change relied on.
      Chunks follow the record: `WRITE_CHUNK_MATH` 6 → 4 per frame
      (4 + 4×24 = 100 of 112), `READ_CHUNK_MATH` 100 → 84 per request
      (4 + 84×24 = 2020).
    - Code cost of the whole op set measured ≈ 1.5 KB of flash. The libm
      calls (fabsf/fmodf/sqrtf/floorf/ceilf/roundf) link fine — the linker
      script's `/DISCARD/` libm block is a proven no-op on this toolchain.

    *GUI side: `.ct3` schema 9 → 10, so an older build refuses a file carrying
    C operands instead of silently dropping them; schema ≤ 9 files keep
    loading, the missing c\* keys defaulting to an unused const-0 operand.
    Only ops 26–30 read C; unary ops read A alone, and every unused operand is
    carried on the wire as its default.*
23. **The capacity expansion: bigger tables, a wider label, an 8x8 in place of
    the 4x4, and the payload cap finally raised.** One change, because the parts
    do not separate: the tables only fit in a bigger flash region, the bigger
    signal record only pays for itself with more signal slots, and the 8x8 only
    fits on the wire with the cap raised.

    **What moved.** `MAX_MESSAGES` 250 → **500**, `MAX_SIGNALS` 768 → **1000**,
    `MAX_TIMERS` 20 → **50**, `CanSignalConfig` 48 → **64 B** (the label goes
    back to 32, undoing the one part of #9b that cost the user something),
    `MAX_TABLES_4X4`/`Table4x4Config` **removed** in favour of
    `MAX_TABLES_8X8` = 8 with `TABLE_8X8_SITES` = 8, `MAX_TX_PAYLOAD`
    112 → **496**, `MAX_TX_WIRE_BYTES` 127 → **512**, `rxBuffer` 256 → **1024**,
    `FLASH_STORE_CAPACITY` 53248 → **98304**, `FLASH_STORE_VERSION` 3 → **4**,
    `FLASH_NUM_TABLES` 12 → **13**, `.ct3` schema 11 → **12**.
    `PROTOCOL_VERSION` stays **1** — nothing is deployed, and the length check
    (`4 + count*item_size`) is the real cross-version guard, as it has been
    since v14.

    **The flash arithmetic, since every figure here has to carry it.** CFG_TOTAL
    is generated from `FLASH_TABLE_LIST` and `_Static_assert`ed against the
    region, so these are derived, not quoted: 256 header + 500×16 + 1000×64 +
    100×24 + 100×40 + 50×32 + 50×24 + 100×8 + 32×16 + 8×72 + 8×64 + 8×80 +
    64×32 + 8×32 = **86,800 B**, against **53,152 B** before. That is what
    forced the region 52 KB → **96 KB** (98304, still a multiple of 4096 — the
    single-bank layout places the region at (top of flash − capacity) and erases
    it by 4 KB page number, so an unaligned capacity computes a page starting
    *below* the region; the `_Static_assert` in `user_code.c` and the "52 KB"
    prose beside it moved with the number). 11,504 B of slack remains, which is
    the room the next record-size change gets before the region has to move
    again.

    **Why `FLASH_STORE_VERSION` went to 4.** Not one reason but four, any of
    which alone would invalidate a stored image: every table offset in the
    region moved (the signal record grew and three capacities changed), the
    header's `counts[]` array grew with `FLASH_NUM_TABLES`, a table was removed
    and two added, and `imageCrc()` hashes `item_size` bytes per live record.
    An older image read under this build would not be subtly wrong — it would be
    reading the signal table where the message table used to end. The bump makes
    the rejection uniform and legible instead of leaving it to whichever table a
    particular configuration happened to use. Same consequence as every previous
    bump: **the first boot after flashing comes up on bring-up defaults and the
    configuration has to be sent again.**

    **`MAX_MESSAGES` is now capped by a 9-bit field, and that is the binding
    limit on this axis.** `SIG_MSG_IDX_MASK` is 0x1FF: message indices 0..510 are
    addressable and 511 is `SIG_MSG_NONE`, the marker for a virtual signal. 500
    fits with 11 to spare; **512 would not**, and the symptom would not be a
    rejected configuration — message 511's signals would read back as virtual and
    detach from their message silently. Raising this again means widening the
    packed field on both sides of the wire first, and there is a comment at the
    `#define` saying so.

    **The 8x8 replaces the 4x4, and the row split is the interesting part.** A
    combined 8x8 record is 2+2+2+1+1+1 + 8×4 + 8×4 + 64×4 = **329 bytes** — over
    the payload cap even at 496, and over `MAX_PADDED_RECORD` (112), so it can be
    neither sent nor stored. Following the v13 2x16 precedent it ships as
    `Table8x8Def` (73 B, padding to 80; commands **0x34/0x35**) plus **one
    `Table8x8Row` record per grid row** (32 B; commands **0x36/0x37**). Table `t`
    owns Def index `t` and Row indices `t*8 .. t*8+7`, so the row table holds 64
    records.
    - **`PAD8(32) == 32` is the whole reason for this shape.** Those eight row
      slots are byte-contiguous — 256 bytes — so the engine takes ONE pointer at
      row `t*8` and indexes `grid[y*8 + x]`, exactly as `Table4x4Config.outputs[y*4+x]`
      did. No reassembly buffer, no cross-record arithmetic, no RAM. Any other
      chunking loses it, and a row record that grew by one byte would pad to 40
      and turn that pointer into a walk across the wrong memory.
      `test_firmware_link` asserts the 32 and the contiguity directly.
    - **Torn-upload safety is inherited from v13 and is per table.** Only the Def
      carries `TABLEFLAG_ACTIVE`; the rows carry no flags. The host sends **rows
      before def** (mirroring `config_transfer.cpp`'s existing Out-before-Def
      ordering), and the engine evaluates table `t` only while
      `count[DEF] > t && count[ROW] >= (t+1)*8`. Unprogrammed flash reads 0xFF =
      NaN, and that guard is what stops a half-uploaded grid poisoning an output
      channel. Note it is NOT the 2x16's global `min()` of two counts: one
      complete table beside a second table's Def must keep evaluating while the
      second does not.
    - **`MAX_PADDED_RECORD` stays 112** — confirmed, not assumed: the peak padded
      slot is now the 8x8 Def at 80 (the row is 32), and 112 was set by the
      retired 4x4's 105 → 112. Nothing has replaced it, so the scratch buffer is
      simply over-provisioned rather than wrong.
    - **The 4x4's ids 0x1D/0x1E are RETIRED, not reused** — the same discipline
      the v12 2x8's 0x1B/0x1C got. The length check would probably have caught a
      stale host (105-byte records against a 73-byte Def), but "probably" is not
      what a replaced table gets, and the two failures do not read the same to
      whoever is holding the cable: `ERR_INVALID_CMD` says the device does not
      have this feature, a length NACK says the record is malformed.

    **Raising the payload cap cashes in what #5 describes.** 112 and 127 survived
    only because of a v1 RX-DMA fault that circular DMA fixed long ago. The cap
    is now 496 in a 512-byte frame — worst case 4 + 496 + 2 = 502 raw, +1 COBS
    code byte per 254 bytes and 2 delimiters = **506 ≤ 512**, and the existing
    compile-time COBS assert still holds. `rxBuffer` 256 → 1024 went with it (a
    frame larger than the buffer can be lapped before the callback copies it
    out; 1024 is ≥2 frames of slack, 512 would be exactly one). Everything else
    in the receive path was already 2048 and was verified rather than assumed.
    Chunk sizes were **recomputed** from each record size, not scaled: signals
    7/frame (4 + 7×64 = 452), messages 49, timers 24, 8x8 def 6, 8x8 row 15;
    reads, bounded by the unchanged 2030-byte response cap, are signals **31**
    (4 + 31×64 = 1988; 32 would be 2052), timers 50, 8x8 def 8, 8x8 row 32.
    `test_firmware_link` asserts every write chunk is **maximal**, because a
    chunk one record short of the cap is a slower Send and nothing else would
    ever report it.

    **RAM: +4,288 B measured, to 47,756 of 131,072 (36.4%)** — from the built
    ELF, not an estimate. The survey predicted +3,522 to 46,990 and was 768 B
    low because it counted only the engine's tables and forgot the `rxBuffer`
    256 → 1024 growth that the payload-cap raise in this same change requires.
    Worth recording as its own small lesson: the two halves of this revision
    were costed separately and the RAM arithmetic fell down the gap between
    them. A quoted capacity figure that nobody measured is exactly what cost
    this project a debugging session once already (the 76 KB
    linker figure). No action needed either way — but note
    `timed_out[MAX_MESSAGES]` in `applyReceiveTimeouts` is now a 500-byte stack
    frame. Measured safe against ~82.5 KB of real headroom (`_Min_Stack_Size` is
    a link-time floor, not a ceiling), and left on the stack deliberately with a
    comment recording that reasoning, so the next reader does not move it in a
    panic.

    **Verified on hardware, and the estimate was wrong by 2x.** CLEAR_CONFIG at
    the 96 KB region measures **1055-1106 ms**, not the estimated ~571 ms: the
    real cost is ~22.9 ms per 2 KB dual-bank page, not the 11.9 ms extrapolated
    from the 52 KB figure. Against the old `kFlashTimeoutMs` of 1500 ms that is
    1.36x margin, and a CLEAR that times out is the one overrun that corrupts a
    whole Send — it is retransmitted, the device erases twice, and the transfer
    runs a stale ACK ahead of the device until two frames merge and one arrives
    as a fragment. **`kFlashTimeoutMs` therefore went 1500 → 3000** (~2.7x
    margin), which costs nothing normally since the timeout only elapses when
    something has already failed.

    This is the second time an erase-duration estimate has been optimistic on
    this board. Re-measure it, do not extrapolate it, whenever the region grows.

    *GUI side: `.ct3` schema 11 → 12. The `tables4x4` key becomes `tables8x8`,
    and a schema-11 file's 4x4 tables **load** rather than being refused — each
    lands in the top-left of an 8x8 with its counts intact. That migration is a
    straight parse rather than a reshaping, because the document's 4x4 row always
    carried variable-length site lists and an `outputs` grid strided by its own X
    width, never a fixed 4: a saved 4x4 already IS an 8x8 whose sites stop early.
    The channel-name cap goes 15 → **31 bytes** at every site that enforces it —
    the edit/add/channel-editor dialogs, `device_mapper`'s label fill and its
    collision check, DBC import truncation, validation and the help pages — and
    it remains a UTF-8 BYTE count, not a character count, because one non-ASCII
    character is 2-4 bytes and a legal-looking 31-character name can still
    overrun the label.*
24. **Triggered transmit, `MAX_CONDITIONS` 100 → 250, and condition outputs
    that finally survive a Get.** Three changes in one release because they
    share one `FLASH_STORE_VERSION` bump — and only one of them costs
    anything.

    **Triggered transmit costs zero config flash.** "Cyclic / Triggered" was a
    *document* field that reached the device nowhere and was forced back to
    Cyclic by every Get. `CanMessageConfig` now carries `uint16_t
    tx_trigger_cond` and `uint8_t tx_trigger_flags`, taken **in place** from
    three of the four bytes of the retired v20 per-message key (`reserved[4]`
    became a one-byte `reserved`). The record is still **14 bytes**, `PAD8(14)`
    is still 16, so no offset moved, `CFG_TOTAL` is unchanged and every chunk
    constant on both sides stayed exactly as it was. `PROTOCOL_VERSION` stays
    **1**: the record is the same size, so an older host's write still
    satisfies `4 + count*item_size` — and it writes zeros there, which decode
    to "cyclic", the behaviour it intended.
    - **The flags are their own byte because `flags` has no room left.**
      `MSGFLAG_*` occupies 0x01–0x20 and 0x40/0x80 are the `MSGPROT_*` level,
      whose values are pinned so shipped 2.2.x flash decodes to the right tier
      and cannot be borrowed. So `TXTRIG_ENABLED` (0x01) and
      `TXTRIG_RESET_ON_TX` (0x02) live in `tx_trigger_flags`, with
      `TX_TRIGGER_COND_NONE` (0xFFFF) marking an unnamed condition — not a
      magic "disabled" value, since `TXTRIG_ENABLED` decides that, but a
      marker that survives a round trip so a Get cannot invent condition 0 out
      of an unset field.
    - **`serial_proto.c`'s two `reserved` scrubs narrowed from four bytes to
      one**, and the narrowing is only safe because of the store bump below.
      The scrub had two jobs. Erasing legacy PBKDF2 key material is **done**,
      by the store version rather than by the loop: those bytes only ever held
      key material on a 2.2.1 unit updated in place, such a unit's image is
      store v6, this firmware is store v10, and a mismatched version is
      refused at load — a refused image is read as absent, so nothing in it
      reaches the engine or a Get whatever the loop does. Keeping a reserved
      field actually reserved is still live, and that is why the last byte is
      still zeroed on the way in and on the way out. Scrubbing the other three
      now would silently delete every Triggered transmit on the way to flash,
      and blank every one on the way back.
    - **The engine gates on the condition's published value, read in the 5 ms
      transmit slot.** `executeConditions` already runs on every calculation
      tick *and* on every matching received frame, so a condition watching bus
      data is fresher than 200 Hz already and one watching calculated data
      cannot beat the 100 Hz that produced its inputs — re-evaluating the
      expression here would cost a table pass every 5 ms and could not change
      a single answer. Checking in the 5 ms slot is the feature: a condition
      that comes true is acted on within 5 ms however slow the message's own
      rate is.
    - **The interval is phased from the trigger, not from a free-running
      grid.** While disarmed the period accumulator is **parked at `period`**,
      so the first slot after the condition goes true finds it already past
      due and transmits at once; each triggered transmission then **zeroes**
      the accumulator rather than subtracting the period, so the run restarts
      from every send. A 1 Hz message whose condition becomes true at 1.2 s
      sends at 1.2, 2.2, 3.2 — not at 2.0, 3.0, which is what keeping the
      remainder would have given.
    - **A broken reference makes the message SILENT, never cyclic.** Unset
      sentinel, index past the used count, inactive condition record,
      destination slot out of range: every one of them answers false. Falling
      back to "no gate" would put frames on a customer's wire precisely when
      the configuration says it does not know whether they belong there.
      Silence is the recoverable failure; unexpected traffic is not.
    - **`TXTRIG_RESET_ON_TX` needed the engine's first per-condition runtime
      state.** Conditions were purely combinational — that is exactly what
      lets `executeConditions` run from both the tick and the receive path
      without caring how often — and `g_cond_consumed[MAX_CONDITIONS]` (250 B,
      not persisted, cleared by `resetRuntime`) does not change that: it is
      memory of the *transmission*, not of the expression, and it only ever
      forces the published value down. It exists because a plain zero-write
      into the value slot would be overwritten within milliseconds by the next
      `executeConditions`. The latch clears on `!met` rather than on a timer,
      which is what makes this an edge trigger instead of a rate limiter —
      nothing has to agree on a duration — and it is set only after the frame
      was **accepted** by the outgoing queue, since consuming the edge for a
      frame a full ring refused would lose the transmission entirely rather
      than delaying it. A power cycle re-arms everything, which is the only
      defensible answer: the alternative is a unit that boots refusing to send
      because of an edge it consumed before it was last switched off.

    **`MAX_CONDITIONS` 100 → 250 is the half that costs something.**
    `ConditionConfig` is 35 B and `PAD8(35)` is 40, so the table goes 4,000 →
    **10,000 B** and `CFG_TOTAL` 120,368 → **126,368** of the 131,072 B region
    — **4,704 B spare**, where 10,704 were. Conditions are the **fourth** table
    in `FLASH_TABLE_LIST`, so counters, timers, constants, relays, both
    lookup-table pairs, integrators, the script region and the CRC8 rules all
    shift 6,000 bytes down: a v9 image read under this build would be misread
    record-for-record before its CRC ever ran, not merely rejected.
    **`FLASH_STORE_VERSION` 9 → 10**, with the consequence every bump since v6
    has had — **a unit updated to this firmware reads its stored configuration
    as absent and needs one re-Send.** Triggered transmit would have needed no
    bump at all; shipping the two together means the field pays that price
    once.
    - **The real ceiling is the channel pool, not the region.** Each condition
      owns an output slot, so 250 of them claim up to 250 of the 1,000
      `MAX_SIGNALS` slots. Another 250 signals would cost 16,000 B against
      4,704 spare, and this raise already spent more than half the slack the
      layout had carried since the script table arrived. Read the remainder as
      the constraint it now is: one more signal costs 64 B, one more message
      16, and the next table that wants thousands has nowhere to come from but
      another table.

    **A condition output could not survive a Get, and the fix is not in the
    signal record.** `mapToDevice` now stamps the destination slot with
    `typeOutputSignal(destIdx, "boolean", 0)`, which it never used to do —
    which is why a condition output came home with no data type at all, or
    declared float while carrying nothing but 0 and 1. Constants, lookup-table
    outputs and device channels all stamp their slot from the type the document
    declares for them; a condition's type is knowable without being declared
    anywhere, and that is exactly the case that got missed. That alone does not
    fix the round trip, though: on the wire Boolean **is**
    `SIGNAL_TYPE_UINT8`, the same eight bits as u8 and always was, so
    `inferDataType` reads every condition output back as "u8" however carefully
    it was written. The **condition table** is the one piece
    of unambiguous evidence — a channel a condition writes is a boolean
    because a condition writes it, whatever the type byte says — so
    `Configuration::forceConditionOutputsBoolean()` re-types from the table
    after the catalogue is installed. It rewrites dataType, minimum, maximum
    and decimal places together (a "boolean" still holding a 0–100 range is
    not one) and leaves device channels alone, those being the firmware's
    definition rather than the document's.

    *GUI side: "Conditions" is **User Conditions** everywhere — the
    Calculations menu (mnemonic `o` → `C`, since Counters own U and Constants
    own n), both dialog titles, every validation location ("User Condition 3")
    and the Config Summary heading. The section editor gains a **Transmit
    Condition** combo and a **Reset User Condition once Triggered** tickbox,
    shown only while Triggered is selected. The combo lists the document's User
    Conditions rather than opening the channel picker — the requirement is
    literally "only User Conditions", and the picker has no filter and a New
    Channel… button that would mean nothing here — and it stores the
    condition's **output channel**, not its row index: a `ConditionRow` has no
    name and no stable id, so an index would silently re-point the moment a row
    above it was inserted, deleted or reordered. `mapToDevice` resolves name →
    index in a deferred pass once the condition table is built (messages are
    mapped long before it exists, and predicting the numbering would mean
    duplicating the skip rules for inactive and malformed rows); `mapFromDevice`
    resolves index → name, and a message whose index names nothing comes back
    **cyclic** rather than carrying a dangling reference. Triggered with no
    usable condition is an **Error** at both ends, never a silent fall back to
    cyclic. `.ct3` schema 16 → **17**, and that bump is not optional either:
    `"cyclic": false` has been a legal, inert value in every file since the
    beginning, so an older Manager would read a Triggered section as an
    ordinary message and send one that transmits continuously.
    `EXPECTED_STORE_VERSION` moved 9 → **10** with the firmware.*
