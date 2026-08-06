local function sum_nontail(n)
  if n == 0 then return 0 end
  return n + sum_nontail(n - 1)
end

local function sum_tail(n, acc)
  while n ~= 0 do
    acc = acc + n
    n = n - 1
  end
  return acc
end

local non_tail_n = 800
local tail_n = 300000

print(sum_nontail(non_tail_n) + sum_tail(tail_n, 0))
