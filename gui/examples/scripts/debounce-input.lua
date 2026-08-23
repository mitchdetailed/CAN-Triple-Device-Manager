-- debounce-input.lua ---------------------------------------------------------
--
-- Debounce a noisy digital input.
--
-- READS
--   "Switch Raw"      raw contact state. Nominally 0 or 1; anything >= 0.5 is
--                     treated as a 1 so a floating/analog-ish input still
--                     behaves.
--
-- WRITES
--   "Switch"          the debounced state, always exactly 0 or 1.
--   "Switch Pending"  how many consecutive ticks "Switch Raw" has DISAGREED
--                     with "Switch" (0 .. HOLD_TICKS-1, and briefly 0 again on
--                     the tick it commits). Purely diagnostic -- put it on a
--                     gauge while you tune HOLD_TICKS, or delete the setSig.
--
-- HOW IT WORKS
--   "Switch" only moves once the raw input has held the NEW value for
--   HOLD_TICKS consecutive ticks. Any tick where raw falls back to the value
--   currently published resets the counter to zero, so a glitch shorter than
--   HOLD_TICKS is rejected outright and never reaches "Switch" at all. This is
--   the integrating form of a debouncer: it does not care how many times the
--   contact chatters, only that it eventually stops.
--
-- LATENCY -- the example measures its own delay
--   on_tick() runs every 10 ms. With HOLD_TICKS = 5 a genuine edge appears on
--   "Switch" exactly 5 ticks = 50 ms after the raw input changed: raw goes
--   high during tick 1, the counter reads 1,2,3,4 at the end of ticks 1..4,
--   and on tick 5 it reaches 5 and "Switch" commits. Both edges (rising and
--   falling) cost the same 5 ticks. Latency is HOLD_TICKS * 10 ms, full stop.
--
-- TUNE
--   HOLD_TICKS  ticks the new value must hold before it is believed.
--               5 = 50 ms. Raise it for a dirtier contact; every +1 buys you
--               10 ms more glitch immunity and costs 10 ms more latency.
--
-- BUDGET NOTE
--   There is no division, no floor() and no % anywhere here -- those cost 4
--   and 8 units. Debouncing is pure counting and comparison, which are 1 unit
--   each, so the whole tick lands around a couple of dozen units out of 2000.
--   Resist the urge to write things like floor(cnt/HOLD_TICKS); a comparison
--   says the same thing for an eighth of the price.
-- -----------------------------------------------------------------------------

-- Persistent across ticks. These MUST be file scope -- state() is illegal
-- inside a function.
local stable = state(0)  -- the value currently published on "Switch"
local cnt = state(0)     -- consecutive ticks raw has disagreed with 'stable'

function on_tick()
  local HOLD_TICKS = 5

  -- Square the input up to a hard 0 or 1 first. Everything downstream then
  -- compares two clean integers, so == is safe (comparing raw floats for
  -- equality would be asking for trouble on a noisy line).
  local raw = select(sig("Switch Raw") >= 0.5, 1, 0)

  if raw == stable then
    -- Agreement. Whatever was building up was a glitch: throw it away.
    -- This one line is the whole debouncer. A spike of up to HOLD_TICKS-1
    -- ticks dies here having never touched "Switch".
    cnt = 0
  else
    cnt = cnt + 1
    if cnt >= HOLD_TICKS then
      -- The new value has now held long enough to be believed.
      stable = raw
      cnt = 0
    end
  end

  setSig("Switch", stable)
  setSig("Switch Pending", cnt)
end
