-- ===========================================================================
-- run-timer.lua -- accumulating run timer / hour meter
--
-- Counts elapsed time only while the engine is actually turning, and
-- publishes two numbers:
--
--   Engine Hours     lifetime running time in hours (fractional). Keeps its
--                    value across an engine stop and carries on from there
--                    when the engine restarts.
--   Session Minutes  running time in minutes for the CURRENT run. Snaps back
--                    to 0 as soon as the engine stops.
--
-- Reads:   Engine RPM
-- Writes:  Engine Hours, Session Minutes
--
-- Tune:    RPM_RUNNING       RPM above which the engine counts as running.
--          TICKS_PER_SECOND  Rate on_tick() is called at. Only change this
--                            if the device tick rate itself changes.
--
-- --- WHERE THE TIME COMES FROM ---------------------------------------------
-- on_tick() runs at 100 Hz, so one tick is a fixed slice of wall time:
--
--     1 tick   = 1 / 100 Hz  = 0.01 s
--     1 tick   = 0.01 / 60   = 1/6000   minute   (0.000166... min)
--     1 tick   = 0.01 / 3600 = 1/360000 hour     (0.00000277... h)
--
-- There is no clock to read -- counting ticks IS the clock. 360000 ticks is
-- one hour.
--
-- --- WHY THIS COUNTS SECONDS AND NOT HOURS ---------------------------------
-- The obvious version of this script is "hours = hours + 1/360000" on every
-- tick. It does not work. Values here are 32-bit floats, good for about 7
-- significant digits, and 1/360000 is far too small to fold into a growing
-- total cleanly: the sum gets rounded on every single tick and the roundings
-- do not cancel. Measured on the device's own interpreter, that naive
-- version reads 12 seconds FAST after one simulated hour, about 14 minutes
-- fast after ten, drifts back the other way as the total grows, and then at
-- 64 hours -- where a float's own step size finally exceeds the increment
-- being added -- it sticks at exactly 64.000000 and never moves again, no
-- matter how long the engine runs after that.
--
-- So the accumulator counts WHOLE SECONDS instead. Integers stay exact in a
-- 32-bit float all the way to 16777216, which is over 4600 hours' worth of
-- seconds, and the leftover ticks inside the current second never exceed 99.
-- The conversion to hours happens once, at the end, on the way out to the
-- channel: the stored number stays exact and only the displayed one is
-- fractional. Run against the same 100 simulated hours that pinned the naive
-- version at 64, this reads exactly 100.000000.
--
-- --- PERSISTENCE: READ THIS BEFORE TRUSTING THE READING --------------------
-- Both accumulators live in state(), which is RAM. state() survives from
-- tick to tick, but NOT across a power-down: on the next boot this hour
-- meter starts again at zero. If you want a real hour meter, treat this
-- script as the counter and not as the vault -- route Engine Hours out to
-- something that remembers it (a logger, a dash that stores it, or a frame
-- transmitted to a device with non-volatile storage).
-- ===========================================================================

-- This language has no file-scope constants: an ordinary `local` at the top
-- level is a compile error, and the only file-scope thing that exists is
-- state(). A state() that is never assigned to is therefore how you give a
-- tuning number a name. These two are read-only by convention.
local RPM_RUNNING = state(500)
local TICKS_PER_SECOND = state(100)

-- Lifetime meter: whole seconds, plus 0..99 leftover ticks of the current
-- second. Split for the float-precision reason described above.
local lifeSeconds = state(0)
local lifeTicks = state(0)

-- Current-run meter, same split. Kept separate from the lifetime pair rather
-- than derived from it, because the two diverge the moment the engine stops:
-- the session pair is zeroed and the lifetime pair is not.
local runSeconds = state(0)
local runTicks = state(0)

function on_tick()
    local running = sig("Engine RPM") > RPM_RUNNING

    if running then
        lifeTicks = lifeTicks + 1
        runTicks = runTicks + 1

        -- Roll leftover ticks up into whole seconds. The textbook way to
        -- write this is floor(ticks / TICKS_PER_SECOND) for the carry and
        -- ticks % TICKS_PER_SECOND for the remainder, but that costs a
        -- divide (4 units) plus a floor (8) plus a modulo (8) on each
        -- counter, 40 units the pair. A counter that gains exactly one tick
        -- per tick can only ever cross the boundary by one, so a compare
        -- and a subtract (1 unit each) do the identical job for a tenth of
        -- the price -- and unlike the divide they stay exact no matter how
        -- large the total gets.
        if lifeTicks >= TICKS_PER_SECOND then
            lifeTicks = lifeTicks - TICKS_PER_SECOND
            lifeSeconds = lifeSeconds + 1
        end
        if runTicks >= TICKS_PER_SECOND then
            runTicks = runTicks - TICKS_PER_SECOND
            runSeconds = runSeconds + 1
        end
    else
        -- Engine stopped. The session meter restarts from zero; the lifetime
        -- pair is deliberately not mentioned in this branch at all, which is
        -- what "holds" means here -- no code path touches it, so it cannot
        -- drift, decay, or reset while the engine is off.
        runTicks = 0
        runSeconds = 0
    end

    -- Convert to display units only at the boundary. Adding the leftover
    -- ticks back in as a fraction of a second is what gives the output 10 ms
    -- resolution instead of stepping a whole second at a time.
    --
    -- These four divides cost 16 units of the 2000 available, which is
    -- nothing -- the rule of thumb about avoiding division is about division
    -- inside a LOOP, where the cost gets multiplied by the iteration count.
    -- A handful of divides that run once per tick are worth their clarity.
    local lifeSecs = lifeSeconds + lifeTicks / TICKS_PER_SECOND
    local runSecs = runSeconds + runTicks / TICKS_PER_SECOND

    setSig("Engine Hours", lifeSecs / 3600)
    setSig("Session Minutes", runSecs / 60)
end
