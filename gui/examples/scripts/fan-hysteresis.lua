-- Two-stage cooling fan controller with hysteresis.
--
-- Reads:   "Coolant Temp"  -- degrees C
-- Writes:  "Fan Stage"     -- 0 = off, 1 = low, 2 = high
--
-- Each stage has a separate turn-ON and turn-OFF temperature. Between the
-- two, the controller does nothing at all and simply holds whatever stage
-- it was already in. That gap is the hysteresis, and it is the whole point:
-- a coolant temp parked at 92 or at 100 must not chatter the relay on and
-- off every tick, which is exactly what a single-threshold compare would do
-- as the reading dithers by a tenth of a degree.
--
--   Stage 1 (low):   ON at >= 95     OFF at < 90     hold window 90..95
--   Stage 2 (high):  ON at >= 103    OFF at < 98     hold window 98..103
--
-- TUNABLE CONSTANTS -- the language has no file-scope constants (top-level
-- locals must be state()), so the four thresholds are written inline below.
-- Change them in one place each; they are marked with a trailing comment.
-- Keep OFF strictly below ON for each stage or the hysteresis disappears.
--
-- Cost note: this is all compares and constant stores. There is no division
-- and no floor/%/min/max anywhere -- those cost 4 and 8 units where a
-- compare costs 1 -- so a tick is only a couple of dozen units against the
-- 2000 budget. Two-sided thresholds are naturally cheap; reach for
-- comparison logic rather than arithmetic whenever you have the choice.

-- Persistent across ticks. This is what remembers the current stage while
-- the temperature sits inside a hold window. Starts at 0, so a script that
-- loads on an already-hot engine sees temp >= 95 on its very first tick and
-- commands the fan up immediately.
local stage = state(0)

function on_tick()
  local t = sig("Coolant Temp")

  -- Rising edges, lowest stage first. Each one only ever raises `stage`,
  -- so a large jump (cold start straight to 110) climbs to 2 in one tick.
  if t >= 95 and stage < 1 then     -- stage 1 ON threshold
    stage = 1
  end
  if t >= 103 then                  -- stage 2 ON threshold
    stage = 2
  end

  -- Falling edges, highest stage first. Each one only ever lowers `stage`.
  -- These are plain `if`s rather than an elseif chain on purpose: a sudden
  -- drop (thermostat opens, 105 -> 70) falls through both tests and lands
  -- on 0 in the same tick instead of taking one tick per stage.
  if t < 98 and stage > 1 then      -- stage 2 OFF threshold
    stage = 1
  end
  if t < 90 then                    -- stage 1 OFF threshold
    stage = 0
  end

  -- Anything landing in 90..95 or 98..103 matched none of the four tests
  -- above, so `stage` is republished unchanged. That is the hold.
  setSig("Fan Stage", stage)
end
