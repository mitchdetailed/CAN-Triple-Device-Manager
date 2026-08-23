-- ---------------------------------------------------------------------------
-- Exponential moving average (EMA) -- smooth a noisy analogue sensor
--
-- Stops a twitchy sensor from making the gauge jitter, using exactly one
-- remembered number.  A rolling/windowed average would need an array of past
-- samples and this language has no arrays, so the EMA is the filter to reach
-- for here: it keeps only the running output and folds each new sample into
-- it.
--
--   Reads:   "Oil Pressure Raw"   noisy sensor reading (PSI here, any unit)
--   Writes:  "Oil Pressure"       filtered reading, same unit as the input
--
-- TUNING -- ALPHA is the smoothing factor, 0 < ALPHA <= 1.  Each tick the
-- output moves ALPHA of the way from where it is to the newest reading.
-- ALPHA = 1 is no filtering at all; smaller is smoother and slower.
--
-- How ALPHA maps to a time constant: the device runs on_tick at 100 Hz, so
-- one tick is 10 ms.  The step response is 1 - (1 - ALPHA)^n after n ticks,
-- which passes 63% -- one time constant, tau -- after roughly 1/ALPHA ticks:
--
--       tau (seconds)  ~=  0.01 / ALPHA          ALPHA  ~=  0.01 / tau
--
--       ALPHA = 0.10   ->  tau ~= 0.1 s   light smoothing, still snappy
--       ALPHA = 0.02   ->  tau ~= 0.5 s   the default below; ~98% in 2 s
--       ALPHA = 0.005  ->  tau ~= 2.0 s   heavy, for a deliberately slow dial
--
-- Those divisions are in this comment only.  The code below never divides:
-- ALPHA is typed in directly as the tuning number and applied with a single
-- multiply.  Division costs 4 units against the 2000-unit tick budget where a
-- multiply costs 1, so gains in a filter are always stored in multiply form.
-- ---------------------------------------------------------------------------

local smoothed = state(0)
local seeded = state(0)

function on_tick()
  -- Ordinary locals must live inside the hook -- only state() may sit at
  -- file scope -- so the tuning constant is declared here.
  local ALPHA = 0.02

  local raw = sig("Oil Pressure Raw")

  if seeded == 0 then
    -- First tick after power-on: adopt the reading outright instead of
    -- filtering towards it from state()'s initial 0.  Without this the
    -- output would ramp up from zero over a couple of seconds on every
    -- boot, which looks exactly like a real pressure event and is not one.
    -- A separate flag is used rather than testing "smoothed == 0" because
    -- zero is a legitimate pressure and would re-seed the filter forever.
    smoothed = raw
    seeded = 1
  else
    -- The EMA proper, written as "current + ALPHA * error".  Algebraically
    -- this is the same as ALPHA*raw + (1-ALPHA)*smoothed, but it is one
    -- multiply instead of two and it reads as what it does: move a fixed
    -- fraction of the remaining distance towards the new sample.
    smoothed = smoothed + ALPHA * (raw - smoothed)
  end

  setSig("Oil Pressure", smoothed)
end
