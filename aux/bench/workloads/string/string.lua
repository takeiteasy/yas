local function build(n, acc, matches)
  while n ~= 0 do
    local chunk = tostring(n)
    acc = acc .. chunk .. ","
    if chunk:sub(1, 1) == "1" then
      matches = matches + 1
    end
    n = n - 1
  end
  return #acc + matches
end

print(build(6000, "", 0))
