-- ---------------------------------------------------------------------------
-- Peak hold with slow decay -- a peak-boost readout
--
-- Reads:
--   "Boost Pressure"      live manifold pressure, in whatever unit the
--                         channel carries (psi, bar, kPa -- the script does
--                         not care, it only ever compares against itself)
--
-- Writes:
--   "Boost Peak"          jumps to any new maximum in a SINGLE tick, then
--                         bleeds back down toward the live value at
--                         DECAY_PER_SEC units per second
--   "Boost Peak Session"  hard maximum since power-up. Never decays.
--
-- Tune (top of on_tick):
--   DECAY_PER_SEC   how fast "Boost Peak" bleeds off, in channel units per
--                   second. At 2.0 on a psi channel the number falls 2 psi
--                   every second, so a 14 psi spike is still on screen about
--                   seven seconds later. Bigger = snappier and more honest
--                   about the present; smaller = the spike hangs around.
--
-- The device calls on_tick() at 100 Hz, so one second is 100 ticks.
-- ---------------------------------------------------------------------------

-- Only state() may live at file scope; these three survive across ticks.
local peak = state(0)         -- the decaying peak, in channel units
local sessionPeak = state(0)  -- the hard maximum, in channel units
local primed = state(0)       -- 0 until the first tick has seeded the peaks

function on_tick()
    -- The tunables sit inside on_tick() because file scope in this language is
    -- reserved for state() -- a plain "local X = 2.0" up there is a compile
    -- error. Locals are free to re-create every tick, so nothing is lost.
    local DECAY_PER_SEC = 2.0
    local TICK_SECONDS = 0.01   -- on_tick runs at 100 Hz

    local live = sig("Boost Pressure")

    -- First tick only: start both peaks AT the live reading instead of at the
    -- state() initialiser of 0. This matters on a gauge-pressure channel that
    -- reads negative under vacuum -- without it, both outputs would report a
    -- 0 the engine never actually made, right up until boost first crossed
    -- zero. "primed" is the standard once-only idiom: a state flag, not a
    -- constructor, because there is no hook that runs before the first tick.
    if primed == 0 then
        primed = 1
        peak = live
        sessionPeak = live
    end

    -- The decay step is a MULTIPLY by the tick period, not a DIVIDE by the
    -- tick rate. Same number (units/s x s/tick = units/tick), but "*" costs 1
    -- unit and "/" costs 4. Out here in straight-line code that saving is
    -- noise; the habit is the point, because this exact expression inside a
    -- loop body is where division starts eating the 2000-unit tick budget.
    local decayStep = DECAY_PER_SEC * TICK_SECONDS

    -- One max() does both halves of "peak hold with decay":
    --   live >= peak  ->  live wins, so the peak jumps up in a single tick
    --   live <  peak  ->  the peak steps down by decayStep, but is floored at
    --                     live, so it settles onto the live trace and stops
    --                     rather than sinking past it
    -- Spelling this as if/else takes four more lines and behaves identically.
    peak = max(peak - decayStep, live)

    -- The session peak is the same idea with the decay term removed. That
    -- missing "- decayStep" is the entire difference between the two outputs.
    sessionPeak = max(sessionPeak, live)

    setSig("Boost Peak", peak)
    setSig("Boost Peak Session", sessionPeak)
end
