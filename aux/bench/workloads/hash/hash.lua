local function fill(h, n)
  for i = 0, n - 1 do
    h[i] = i * i
  end
end

local function remove_odds(h, n)
  for i = 1, n - 1, 2 do
    h[i] = nil
  end
end

local function sum_lookup(h, n)
  local total = 0
  for i = 0, n - 1 do
    total = total + (h[i] or 0)
  end
  return total
end

local function count(h)
  local c = 0
  for _ in pairs(h) do c = c + 1 end
  return c
end

local n = 6000
local h = {}
fill(h, n)
remove_odds(h, n)
print(sum_lookup(h, n) + count(h))
