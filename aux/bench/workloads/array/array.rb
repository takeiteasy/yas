n = 3000
sieve = Array.new(n, 0)
sieve[0] = 1
sieve[1] = 1

(0...n).each do |i|
  if sieve[i] == 0
    j = i * i
    while j < n
      sieve[j] = 1
      j += i
    end
  end
end

count = 0
(0...n).each { |i| count += 1 if sieve[i] == 0 }

puts count
