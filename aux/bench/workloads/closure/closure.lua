-- Lua has no built-in map/filter/reduce - defined locally as closures,
-- same as yas's local `filter` (base.ys has MAP/FOLDL but not FILTER).
local function map(fn, t)
  local out = {}
  for i = 1, #t do out[i] = fn(t[i]) end
  return out
end

local function filter(pred, t)
  local out = {}
  for i = 1, #t do
    if pred(t[i]) then out[#out + 1] = t[i] end
  end
  return out
end

local function foldl(fn, init, t)
  local acc = init
  for i = 1, #t do acc = fn(acc, t[i]) end
  return acc
end

local n = 40000
local lst = {}
for i = 1, n do lst[i] = i end
local squared = map(function(x) return x * x end, lst)
local big = filter(function(x) return x > 400000000 end, squared)
print(foldl(function(a, x) return a + x end, 0, big))
