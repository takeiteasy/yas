def build(n, acc, matches)
  while n != 0
    chunk = n.to_s
    acc = acc + chunk + ","
    matches += 1 if chunk[0, 1] == "1"
    n -= 1
  end
  acc.length + matches
end

puts build(6000, "", 0)
