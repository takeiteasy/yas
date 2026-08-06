def fill(h, n)
  (0...n).each { |i| h[i] = i * i }
end

def remove_odds(h, n)
  1.step(n - 1, 2) { |i| h.delete(i) }
end

def sum_lookup(h, n)
  total = 0
  (0...n).each { |i| total += h.fetch(i, 0) }
  total
end

n = 6000
h = {}
fill(h, n)
remove_odds(h, n)
puts sum_lookup(h, n) + h.length
