----------------------------------------------------------------------------
-- The run loop of the event library (lib_event.c), compiled to bytecode at
-- build time (lj_nlev.h) and opened by luaopen_event as
--
--   local run = chunk(store)
--
-- It is written in Lua so that a handler can yield: a plain `on` handler
-- may `.await` when the event was run inside a task, and Lua
-- handlers may call coroutine.yield. Handlers are called through Lua frames
-- only; the loop itself is tail calls, so neither the number of handlers
-- nor nested runs use the C stack.
--
-- The store is shared with the C side, which adds and removes handlers
-- (lib_event.c, header comment):
--   store[name] = rec    rec.head, rec.tail: the first and last entry
--                        rec.byname: handler name -> entry
--                        rec.seq: the sequence number of the last add
--   entry                e.f: the callback; e.name: the handler name
--                        e.seq: when it was added; e.next, e.prev: links
--                        e.dead: true once removed (e.next is kept, so a
--                        run that stands on it can go on)
----------------------------------------------------------------------------

local store = ...
local type, tostring, error, select, unpack, rawget =
      type, tostring, error, select, unpack, rawget

-- Event names are strings; a number is converted to its string form, as
-- the C side converts it; anything else is an error.
local function name_of(name)
  local t = type(name)
  if t == "string" then return name end
  if t == "number" then return tostring(name) end
  error("bad argument #1 to 'run' (string expected, got " .. t .. ")", 3)
end

local walk

-- A handler that returns any value, nil included, ends the run, and
-- its values are the run's; a handler that returns nothing lets the next
-- one run. (The same rule as the stand-in, runtime/nl_62_event.lua.)
local function after(e, max, args, ...)
  if select("#", ...) > 0 then return ... end
  return walk(e.next, max, args)
end

-- Calls the entries from e on. An entry removed since the run began is
-- skipped; one added since (seq > max, and entries are in seq order) ends
-- the walk; one replaced since runs with its new callback.
walk = function(e, max, args)
  while e ~= nil do
    if e.seq > max then return end
    if not e.dead then
      return after(e, max, args, e.f(unpack(args, 1, args.n)))
    end
    e = e.next
  end
end

return function(name, ...)
  local rec = rawget(store, name_of(name))
  if rec == nil or rec.head == nil then return end
  return walk(rec.head, rec.seq, { n = select("#", ...), ... })
end
