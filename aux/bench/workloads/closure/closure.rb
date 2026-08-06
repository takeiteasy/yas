n = 40000
lst = (1..n).to_a
squared = lst.map { |x| x * x }
big = squared.select { |x| x > 400000000 }
puts big.reduce(0) { |acc, x| acc + x }
