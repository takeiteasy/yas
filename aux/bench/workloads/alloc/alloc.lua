local function build(n, acc)
  while n ~= 0 do
    acc = {n, acc}
    n = n - 1
  end
  return acc
end

local function suml(lst, acc)
  while lst do
    acc = acc + lst[1]
    lst = lst[2]
  end
  return acc
end

print(suml(build(50000, nil), 0))
