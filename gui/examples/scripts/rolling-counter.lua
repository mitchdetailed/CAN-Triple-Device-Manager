-- rolling-counter.lua ---------------------------------------------------
--
-- Rolling "alive" counter (the companion to a CRC in a message-integrity
-- scheme), plus the receive-side check that catches a dead transmitter.
--
-- Transmit side:
--   "Alive Counter" is written every tick and cycles 0,1,2,...,15,0,...
--   A receiver that sees the counter freeze knows our messages stopped
--   being produced, even if something is still replaying the last frame.
--
-- Receive side:
--   "Rx Counter" is the same kind of counter arriving from another ECU.
--   If it does NOT change for 10 ticks in a row, "Counter Stuck" goes 1.
--   Any change clears it back to 0 on that same tick.
--
-- Channels read:     Rx Counter
-- Channels written:  Alive Counter, Counter Stuck
--
-- Tunables (both are locals at the top of on_tick):
--   ROLL_MAX     highest value before wrapping to 0.  15 gives the usual
--                4-bit counter; use 255 for an 8-bit one.
--   STUCK_TICKS  consecutive unchanged ticks before we declare a fault.
--                At a 10 ms tick, 10 ticks = 100 ms of silence.
-- ------------------------------------------------------------------------

-- state() values survive from tick to tick.  They must be declared at file
-- scope and initialised with a plain constant.
local alive     = state(0)  -- value we transmit this tick, 0..ROLL_MAX
local lastRx    = state(0)  -- previous sample of "Rx Counter"
local sameTicks = state(0)  -- ticks in a row that Rx Counter has not moved
local primed    = state(0)  -- 0 until lastRx holds a real bus sample

function on_tick()
  local ROLL_MAX    = 15
  local STUCK_TICKS = 10

  -- ---- transmit side ---------------------------------------------------
  -- Publish first, then advance, so the very first tick sends 0.
  setSig("Alive Counter", alive)
  alive = alive + 1

  -- Wrap with a compare-and-reset rather than "alive % (ROLL_MAX + 1)".
  -- '%' costs 8 budget units and the division needed to build one costs 4;
  -- this compare costs 1.  It is exact here because alive only ever moves
  -- by 1 per tick, so it can never skip past the limit.
  if alive > ROLL_MAX then
    alive = 0
  end

  -- ---- receive side ----------------------------------------------------
  local rx = sig("Rx Counter")

  if primed == 0 then
    -- First tick after boot.  Adopt whatever is on the bus as the baseline.
    -- Without this we would compare against the state() default of 0 and
    -- invent a "no change" we never actually observed -- which would make
    -- the fault trip one tick early whenever the peer happens to sit at 0.
    primed    = 1
    lastRx    = rx
    sameTicks = 0
  elseif rx == lastRx then
    -- Stop counting once the threshold is reached.  sameTicks is a float,
    -- and a peer that stays dead for hours would eventually push it past
    -- the range where +1 is still exact.  Nothing above the threshold is
    -- worth knowing, so parking it there keeps the latch rock solid.
    if sameTicks < STUCK_TICKS then
      sameTicks = sameTicks + 1
    end
  else
    -- Counter moved: healthy peer.  Rebase and clear immediately.
    lastRx    = rx
    sameTicks = 0
  end

  setSig("Counter Stuck", select(sameTicks >= STUCK_TICKS, 1, 0))
end
