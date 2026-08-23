-- ---------------------------------------------------------------------------
-- Slew-rate limiter
--
-- Stops a commanded output changing faster than a set rate, so a step change
-- on the input comes out the other side as a smooth ramp. Use it to tame a
-- twitchy request before it drives an actuator, a fan, or a gauge needle.
--
-- Reads:  "Target Position"     where we are being asked to go, 0..100 %
-- Writes: "Commanded Position"  rate-limited position, 0..100 %
--
-- Tune:
--   MAX_RATE  how fast the output is allowed to move, in % per second
--   TICK_HZ   script tick rate of the device; leave at 100
--
-- The per-tick step falls out of those two:
--   step = MAX_RATE / TICK_HZ = 25 / 100 = 0.25 % per tick
-- so a full 0 -> 100 % swing takes 100 / 0.25 = 400 ticks = 4 seconds.
-- Change MAX_RATE alone and the step follows; nothing else needs editing.
--
-- On boot "Commanded Position" starts at 0 and ramps up to meet the target,
-- which is the safe direction to wake up in for a position actuator.
-- ---------------------------------------------------------------------------

-- The one value that has to survive between ticks: where the output is now.
local cmd = state(0)

function on_tick()
  local MAX_RATE = 25         -- % per second
  local TICK_HZ = 100         -- ticks per second

  -- One division per tick costs 4 units and buys a constant that documents
  -- itself. That is the only '/' in the script -- everywhere else below uses
  -- compares and adds at 1 unit each, because division is 4x their price.
  local step = MAX_RATE / TICK_HZ

  -- Clamp the request so a garbage CAN value cannot walk the output off the
  -- end of the scale. clamp() is 8 units, but it runs once per tick, not in
  -- a loop, so it is affordable here.
  local target = clamp(sig("Target Position"), 0, 100)

  local delta = target - cmd

  -- Three compares and an add at 1 unit each, rather than the one-line
  -- equivalent 'cmd = cmd + clamp(delta, neg(step), step)' which pays 8 for
  -- the clamp. The branch form is also where you would hang separate rise
  -- and fall rates: give each branch its own step constant.
  if delta > step then
    cmd = cmd + step          -- more than a step above us: move one step up
  elseif delta < neg(step) then
    cmd = cmd - step          -- more than a step below us: move one step down
  else
    cmd = target              -- already within one step: land exactly on it
  end

  setSig("Commanded Position", cmd)
end
