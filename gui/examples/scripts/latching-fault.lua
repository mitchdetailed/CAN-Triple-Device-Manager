-- Latching oil-pressure fault with a deliberate reset.
--
-- This is the shape a real warning lamp wants. A lamp that follows the sensor
-- instantly flickers on every pressure dip and is gone by the time the driver
-- looks down; a lamp that latches forever can never be cleared after a repair.
-- So: debounce the trip, latch the result, and make the reset conditional.
--
--   1. TRIP    "Oil Pressure" below OIL_MIN_PSI for TRIP_TICKS consecutive
--              ticks, WHILE "Engine RPM" is above RPM_MIN. The RPM gate is
--              what stops a parked car (0 psi, 0 rpm) from crying oil fault.
--   2. LATCH   Once tripped the fault stays set even after pressure recovers,
--              so a fault that happened at 4000 rpm is still on the dash when
--              the driver coasts to a stop.
--   3. RESET   "Fault Reset" clears the latch ONLY when the trip condition is
--              no longer present. Holding the reset button through a genuine
--              fault does nothing, which is the whole point of a latch.
--
-- Reads:   "Oil Pressure"        psi
--          "Engine RPM"          rpm
--          "Fault Reset"         momentary; >= 0.5 counts as pressed
--
-- Writes:  "Oil Pressure Fault"  1 = latched fault, 0 = healthy
--          "Oil Fault Debounce"  ms the trip condition has held so far, 0 when
--                                it is not present. Handy on a debug gauge to
--                                see how close a marginal engine is getting.
--
-- Tune the three constants at the top of on_tick().

local latched   = state(0)  -- 1 once tripped; only "Fault Reset" clears it
local lowTicks  = state(0)  -- consecutive ticks the trip condition has held

function on_tick()
    -- ---- tuning ----------------------------------------------------------
    local OIL_MIN_PSI = 15   -- below this is "low oil pressure"
    local RPM_MIN     = 500  -- engine must be turning faster than this
    local TRIP_TICKS  = 50   -- on_tick runs at 100 Hz, so 50 ticks = 0.5 s
    -- ----------------------------------------------------------------------

    local psi = sig("Oil Pressure")
    local rpm = sig("Engine RPM")

    -- The raw, undebounced trip condition. Kept as a plain 0/1 number rather
    -- than a boolean so the same value can be compared, latched and counted
    -- without caring how the VM represents true.
    local bad = 0
    if rpm > RPM_MIN and psi < OIL_MIN_PSI then
        bad = 1
    end

    -- Debounce by counting consecutive ticks. Any single tick where the
    -- condition is absent slams the count back to 0, so a one-tick sensor
    -- glitch or a momentary dip during a gear change can never arm the latch.
    -- The count is capped at TRIP_TICKS so a fault held for ten minutes does
    -- not grow the number without bound.
    if bad == 1 then
        if lowTicks < TRIP_TICKS then
            lowTicks = lowTicks + 1
        end
    else
        lowTicks = 0
    end

    -- Latch. Note there is no "else latched = 0" here, and that omission is
    -- the entire feature: once set, nothing in this block ever clears it.
    if lowTicks >= TRIP_TICKS then
        latched = 1
    end

    -- Reset, checked AFTER the latch so that a reset pressed on the very tick
    -- the fault trips still loses. The "bad == 0" guard is what makes the
    -- reset conditional: with the engine still starved of oil the button is
    -- inert, and the lamp comes straight back rather than being dismissable.
    -- Compare against 0.5 rather than == 1 so a noisy or scaled digital input
    -- still reads as pressed.
    if sig("Fault Reset") >= 0.5 and bad == 0 then
        latched  = 0
        lowTicks = 0
    end

    setSig("Oil Pressure Fault", latched)

    -- lowTicks * 10 converts ticks to milliseconds at the 100 Hz tick rate.
    -- A multiply costs 1 unit; the division you might reach for first costs 4,
    -- so scale by the reciprocal constant instead of dividing whenever the
    -- rate is known at authoring time.
    setSig("Oil Fault Debounce", lowTicks * 10)
end
