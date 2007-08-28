------------------
-- Support code --
------------------

-- Table that associates to each NPC pointer the handler function that is
-- called when a player starts talking to an NPC.
local npcs = {}

-- Table that associates to each Character pointer its state with respect to
-- NPCs (only one at a time). A state is an array with four fields:
-- . 1: pointer to the NPC the player is currently talking to.
-- . 2: coroutine running the NPC handler.
-- . 3: next event the NPC expects from the server.
--      (1 = npc_next, 2 = npc_choose, 3 = quest_reply, 4 = 1+3)
-- . 4: countdown (in minutes) before the state is deleted.
-- . 5: name of the expected quest variable. (optional)
local states = {}

-- Array containing the function registered by atinit.
local init_fun = {}

-- Tick timer used during update to clean obsolete states.
local timer

-- Creates an NPC and associates the given handler.
-- Note: Cannot be called until map initialization has started.
function create_npc(id, x, y, handler)
  local npc = tmw.npc_create(id, x, y)
  npcs[npc] = handler
end

-- Sends an npc message to a player.
-- Note: Does not wait for the player to acknowledge the message.
function do_message(npc, ch, msg)
  -- Wait for the arrival of a pending acknowledgment, if any.
  coroutine.yield(0)
  tmw.npc_message(npc, ch, msg)
  -- An acknowledgment is pending, but do not wait for its arrival.
  coroutine.yield(1)
end

-- Sends an NPC question to a player and waits for its answer.
function do_choice(npc, ch, ...)
  -- Wait for the arrival of a pending acknowledgment, if any.
  coroutine.yield(0)
  tmw.npc_choice(npc, ch, ...)
  -- Wait for player choice.
  return coroutine.yield(2)
end

-- Gets the value of a quest variable.
-- Calling this function while an acknowledment is pending is desirable, so
-- that lag cannot be perceived by the player.
function get_quest_var(npc, ch, name)
  -- Query the server and return immediatly if a value is available.
  local value = tmw.chr_get_quest(ch, name)
  if value then return value end
  -- Wait for database reply.
  return coroutine.yield(3, name)
end

-- Processes as much of an NPC handler as possible.
local function process_npc(w, ...)
  local co = w[2]
  local pending = (w[3] == 4)
  local first = true
  while true do
    local b, v, u
    if first then
      -- First time, resume with the arguments the coroutine was waiting for.
      b, v, u = coroutine.resume(co, ...)
      first = false
    else
      -- Otherwise, simply resume.
      b, v, u = coroutine.resume(co)
    end
    if not b or not v then
      -- Either there was an error, or the handler just finished its work.
      return
    end
    if v == 2 then
      -- The coroutine needs a user choice from the server, so wait for it.
      w[3] = 2
      break
    end
    if v == 3 then
      -- The coroutine needs the value of a quest variable from the server.
      w[5] = u
      if pending then
        -- The coroutine has also sent a message to the user, so do not
        -- forget about it, as it would flood the user with new messages.
        w[3] = 4
      else
        w[3] = 3
      end
      break
    end
    if pending then
      -- The coroutine is about to interact with the user. But the previous
      -- action has not been acknowledged by the user yet, so wait for it.
      w[3] = 1
      break
    end
    if v == 1 then
      -- A message has just been sent. But the coroutine can keep going in case
      -- there is still some work to do while waiting for user acknowledgment.
      pending = true
    end
  end
  -- Restore the countdown, as there was some activity.
  w[4] = 5
  return true
end

-- Called by the game whenever a player starts talking to an NPC.
-- Creates a coroutine based on the registered NPC handler.
function npc_start(npc, ch)
  states[ch] = nil
  local h = npcs[npc]
  if not h then return end
  local w = { npc, coroutine.create(h) }
  if process_npc(w, npc, ch) then
    states[ch] = w
    if not timer then
      timer = 600
    end
  end
end

-- Called by the game whenever a player keeps talking to an NPC.
-- Checks that the NPC expects it, and processes the respective coroutine.
function npc_next(npc, ch)
  local w = states[ch]
  if not (w and w[1] == npc and w[3] == 1 and process_npc(w)) then
    states[ch] = nil
  end
end

-- Called by the game whenever a player selects a particular reply.
-- Checks that the NPC expects it, and processes the respective coroutine.
function npc_choose(npc, ch, u)
  local w = states[ch]
  if not (w and w[1] == npc and w[3] == 2 and process_npc(w, u)) then
    states[ch] = nil
  end
end

-- Called by the game whenever the value of a quest variable is known.
-- Checks that the NPC expects it, and processes the respective coroutine.
-- Note: the check for NPC correctness is missing, but it should never matter.
function quest_reply(ch, name, value)
  local w = states[ch]
  if w then
    local w3 = w[3]
    if (w3 == 3 or w3 == 4) and w[5] == name then
      w[5] = nil
      if process_npc(w, value) then
        return
      end
    end
  end
  states[ch] = nil
end

-- Called by the game every tick for each NPC.
function npc_update(npc)
end

-- Called by the game every tick.
-- Cleans obsolete connections.
function update()
  -- Run every minute only, in order not to overload the server.
  if not timer then return end
  timer = timer - 1
  if timer ~= 0 then return end
  -- Free connections that have been inactive for 3-4 minutes.
  for k, w in pairs(states) do
    local t = w[4] - 1
    if t == 0 then
      states[k] = nil
    else
      w[4] = t
    end
  end
  -- Restart timer if there are still some pending states.
  if next(states) then
    timer = 600
  else
    timer = nil
  end
end

-- Registers a function so that is is executed during map initialization.
function atinit(f)
  init_fun[#init_fun + 1] = f
end

-- Called by the game for creating NPCs embedded into maps.
-- Delays the creation until map initialization is performed.
-- Note: Assumes that the "npc_handler" global field contains the NPC handler.
function create_npc_delayed(id, x, y)
  -- Bind the name to a local variable first, as it will be reused.
  local h = npc_handler
  atinit(function() create_npc(id, x, y, h) end)
  npc_handler = nil
end

-- Called during map initialization.
-- Executes all the functions registered by atinit.
function initialize()
  for i,f in ipairs(init_fun) do
    f()
  end
  init_fun = nil
end

-- Below are some convenience methods added to the engine API

tmw.chr_money_change = function(ch, amount)
  return tmw.chr_inv_change(ch, 0, amount)
end

tmw.chr_money = function(ch)
  return tmw.chr_inv_count(ch, 0)
end