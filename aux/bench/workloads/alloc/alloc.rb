def build(n, acc)
  while n != 0
    acc = [n, acc]
    n -= 1
  end
  acc
end

def suml(lst, acc)
  while lst
    n, lst = lst
    acc += n
  end
  acc
end

puts suml(build(50000, nil), 0)
