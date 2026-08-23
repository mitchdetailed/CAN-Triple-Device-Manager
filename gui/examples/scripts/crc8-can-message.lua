-- CRC8 over an 8-byte CAN message (SAE J1850 polynomial 0x1D, init 0x00)
--
-- Reads : "B0".."B7"  — the eight data bytes of the message, each 0..255
-- Writes: "CRC"       — the checksum, 0..255, refreshed every 2 ticks (20 ms)
--
-- Pair this with rolling-counter.lua: a counter plus a CRC is the usual way to
-- prove to a receiver that a message is fresh AND intact.
--
--
-- WHY IT LOOKS LIKE THIS
--
-- Two limits of the device VM shape the whole script, and both are worth
-- understanding before you adapt it.
--
-- 1. THERE ARE NO BITWISE OPERATORS. No xor, no shifts, no masks. A CRC is
--    defined in terms of xor and shifts, so every one of them has to be built
--    out of arithmetic:
--
--      shift left by 1   ->  * 2
--      test the top bit  ->  >= 128
--      xor of two bits   ->  "are they different?"
--
--    The trick that makes it cheap is holding the CRC as EIGHT SEPARATE 1-BIT
--    VARIABLES (c0..c7) instead of one 0..255 number. Then shifting is just
--    moving values along, and xor-ing a bit is `1 - x`. Packing them back into
--    a byte happens once, at the end.
--
-- 2. A TICK MAY SPEND ONLY 2000 COST UNITS. Most operations cost 1, but
--    division costs 4 and floor/%/min/max cost 8 each. The obvious way to pull
--    a bit out — floor(b / 128) % 2 — therefore costs 20 units, and doing that
--    64 times blows the budget on its own. The version of this script that used
--    floor and % was killed by the budget at 1997 units and produced nothing.
--    Written as below, one byte costs about 330 units, so FOUR bytes fit in a
--    tick and eight do not.
--
--    So the work is split across two ticks: this tick does bytes 0-3, the next
--    does bytes 4-7 and publishes. Measured cost is 1283 units, comfortably
--    inside the budget. Bytes 4-7 are LATCHED on the first tick so the checksum
--    covers one consistent snapshot of the message rather than four bytes from
--    one moment and four from another.
--
-- Verified against an independent implementation: bytes 01 02 03 04 05 06 07 08
-- give CRC = 93.
--
--
-- TO ADAPT IT
--   * Different polynomial: change which of c0..c7 get flipped in the
--     "fb == 1" block. The bits flipped here are 0, 2, 3 and 4 — that is
--     0x1D = 0b00011101. Bit 7 is not flipped because it is shifted out.
--   * Different init value (0xFF is common): set c0..c7 to those bits in the
--     phase 0 branch instead of leaving them 0.
--   * Fewer bytes: if you only need 4, drop the phasing entirely and publish at
--     the end of the single pass.

-- Persistent across ticks. phase says which half of the message is next;
-- s4..s7 are the latched bytes; k0..k7 carry the half-finished CRC.
local phase = state(0)
local s4 = state(0) local s5 = state(0) local s6 = state(0) local s7 = state(0)
local k0 = state(0) local k1 = state(0) local k2 = state(0) local k3 = state(0)
local k4 = state(0) local k5 = state(0) local k6 = state(0) local k7 = state(0)

function on_tick()
  -- The CRC, one bit per variable. c7 is the most significant.
  local c0 = 0 local c1 = 0 local c2 = 0 local c3 = 0
  local c4 = 0 local c5 = 0 local c6 = 0 local c7 = 0
  -- The four bytes this tick will process.
  local d0 = 0 local d1 = 0 local d2 = 0 local d3 = 0

  if phase == 0 then
    -- Latch the back half NOW, so the CRC describes the message as it was at
    -- this instant even though it finishes on the next tick.
    s4 = sig("B4") s5 = sig("B5") s6 = sig("B6") s7 = sig("B7")
    d0 = sig("B0") d1 = sig("B1") d2 = sig("B2") d3 = sig("B3")
  else
    d0 = s4 d1 = s5 d2 = s6 d3 = s7
    c0 = k0 c1 = k1 c2 = k2 c3 = k3
    c4 = k4 c5 = k5 c6 = k6 c7 = k7
  end

  local i = 0
  while i < 4 do
    -- select() picks this iteration's byte. There are no arrays, so the four
    -- locals are chosen with nested selects rather than indexed.
    local b = select(i < 1, d0, select(i < 2, d1, select(i < 3, d2, d3)))

    local j = 0
    while j < 8 do
      -- Take the top bit of the byte and shift the byte up, in one step: if
      -- bit 7 is set, subtract it before doubling. No division involved.
      local inbit = 0
      if b >= 128 then inbit = 1 b = (b - 128) * 2 else b = b * 2 end

      -- Feedback bit = top bit of the CRC xor the incoming bit. Two 0/1 values
      -- xor to 1 exactly when they differ.
      local fb = 0
      if c7 ~= inbit then fb = 1 end

      -- Shift the CRC left one place. c0 becomes 0 and c7 falls off the end.
      c7=c6 c6=c5 c5=c4 c4=c3 c3=c2 c2=c1 c1=c0 c0=0

      -- If the feedback bit is set, xor in the polynomial 0x1D by flipping
      -- bits 0, 2, 3 and 4. c0 is known to be 0 here, so it is set outright.
      if fb == 1 then c0 = 1 c2 = 1 - c2 c3 = 1 - c3 c4 = 1 - c4 end

      j = j + 1
    end
    i = i + 1
  end

  if phase == 0 then
    k0=c0 k1=c1 k2=c2 k3=c3 k4=c4 k5=c5 k6=c6 k7=c7
    phase = 1
  else
    -- Pack the eight bits back into one number, once per completed checksum.
    setSig("CRC", c0 + 2*c1 + 4*c2 + 8*c3 + 16*c4 + 32*c5 + 64*c6 + 128*c7)
    phase = 0
  end
end
