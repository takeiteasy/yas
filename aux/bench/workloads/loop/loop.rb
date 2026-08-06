def sum_nontail(n)
  return 0 if n == 0
  n + sum_nontail(n - 1)
end

def sum_tail(n, acc)
  while n != 0
    acc += n
    n -= 1
  end
  acc
end

non_tail_n = 800
tail_n = 300000

puts sum_nontail(non_tail_n) + sum_tail(tail_n, 0)
