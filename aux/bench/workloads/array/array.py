n = 3000
sieve = [0] * n
sieve[0] = 1
sieve[1] = 1

for i in range(n):
    if sieve[i] == 0:
        j = i * i
        while j < n:
            sieve[j] = 1
            j += i

count = 0
for i in range(n):
    if sieve[i] == 0:
        count += 1

print(count)
