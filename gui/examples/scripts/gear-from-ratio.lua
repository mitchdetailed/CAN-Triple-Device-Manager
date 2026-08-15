-- gear-from-ratio.lua
--
-- Works out which gear the car is in from the driveline ratio.
--
-- The engine is bolted to the wheels through the gearbox and final drive, so
-- for any given gear the quantity (engine RPM / road speed) is a constant. Each
-- gear therefore has its own signature ratio, and identifying the gear is just
-- a matter of computing that ratio and seeing which signature it lands nearest.
--
-- Reads:   "Engine RPM"      engine speed, rev/min
--          "Vehicle Speed"   road speed, km/h
-- Writes:  "Gear"            1..6 when confident, 0 when it cannot tell
--
-- "Gear" is 0 whenever the answer would be a guess:
--   * road speed below MIN_SPEED  (ratio is meaningless, and dividing by a
--     near-zero speed explodes into a huge number that could false-match 1st)
--   * engine below MIN_RPM        (engine off, or stalled/cranking)
--   * the ratio falls in none of the six windows (clutch slipping, mid-shift
--     with the clutch down, coasting in neutral, or a bad sensor reading)
--
-- ---------------------------------------------------------------------------
-- TUNING
-- ---------------------------------------------------------------------------
-- R1..R6 are the signature ratios in "engine RPM per km/h". Measure them
-- rather than guessing: hold a steady speed in a known gear, then divide.
-- Worked example for the defaults below, using
--     ratio = gear_ratio * final_drive * 1000 / (60 * tyre_circumference_m)
-- with a 3.90 final drive and a 2.00 m tyre (so 32.5 RPM per km/h per 1.00 of
-- gearing): 1st 3.60 -> 117, 2nd 2.10 -> 68.3, 3rd 1.40 -> 45.5,
-- 4th 1.00 -> 32.5, 5th 0.80 -> 26.0, 6th 0.65 -> 21.1.
--
-- TOL_LO / TOL_HI set how wide each window is around its signature ratio.
-- 0.92 / 1.08 means "within +/-8%", which absorbs tyre wear, tyre pressure,
-- speedo error and torque-converter or clutch slip.
--
-- IMPORTANT: keep the windows from overlapping. Neighbouring gears must
-- satisfy R(n+1) * TOL_HI < R(n) * TOL_LO. With the defaults the tightest
-- pair is 5th and 6th: 21.1 * 1.08 = 22.8 sits safely below 26.0 * 0.92 =
-- 23.9. If you widen TOL far enough that two windows touch, the first
-- matching branch wins and the higher gear becomes unreachable.
-- ---------------------------------------------------------------------------

function on_tick()
    -- Tunables live inside the hook: file-scope names have to be state(),
    -- and these never change, so plain locals are the right home for them.
    local R1 = 117.0
    local R2 = 68.3
    local R3 = 45.5
    local R4 = 32.5
    local R5 = 26.0
    local R6 = 21.1

    local TOL_LO = 0.92
    local TOL_HI = 1.08

    local MIN_SPEED = 5.0     -- km/h
    local MIN_RPM = 400.0     -- rev/min

    local rpm = sig("Engine RPM")
    local kph = sig("Vehicle Speed")

    -- Default to "don't know" and only upgrade that if a window matches.
    -- Failing closed like this means every path that falls through the
    -- checks below reports 0 rather than a stale or invented gear.
    local gear = 0

    -- The division is deliberately inside this guard, not above it. That is
    -- the whole divide-by-zero defence: at a standstill kph is 0 (or sensor
    -- noise just above it) and rpm / kph would be inf or an enormous number.
    if kph >= MIN_SPEED and rpm >= MIN_RPM then
        local ratio = rpm / kph

        -- One division per tick, and it is the only one in the script. The
        -- window tests below scale the signature ratios by the tolerance
        -- factors instead of dividing ratio by each R, which would cost six
        -- more divisions at 4 units each for exactly the same answer.
        --
        -- Ordered highest ratio (1st gear) to lowest (6th) so the chain
        -- stops at the first match and short gears are decided quickest.
        if ratio >= R1 * TOL_LO and ratio <= R1 * TOL_HI then
            gear = 1
        elseif ratio >= R2 * TOL_LO and ratio <= R2 * TOL_HI then
            gear = 2
        elseif ratio >= R3 * TOL_LO and ratio <= R3 * TOL_HI then
            gear = 3
        elseif ratio >= R4 * TOL_LO and ratio <= R4 * TOL_HI then
            gear = 4
        elseif ratio >= R5 * TOL_LO and ratio <= R5 * TOL_HI then
            gear = 5
        elseif ratio >= R6 * TOL_LO and ratio <= R6 * TOL_HI then
            gear = 6
        end
        -- No else: a ratio between two windows leaves gear at 0 on purpose.
    end

    setSig("Gear", gear)
end
