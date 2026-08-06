local n = 3000
local sieve = {}
for i = 0, n - 1 do sieve[i] = 0 end
sieve[0] = 1
sieve[1] = 1

for i = 0, n - 1 do
  if sieve[i] == 0 then
    local j = i * i
    while j < n do
      sieve[j] = 1
      j = j + i
    end
  end
end

local count = 0
for i = 0, n - 1 do
  if sieve[i] == 0 then count = count + 1 end
end

print(count)
