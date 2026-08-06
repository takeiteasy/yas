def fill(h, n):
    for i in range(n):
        h[i] = i * i


def remove_odds(h, n):
    for i in range(1, n, 2):
        del h[i]


def sum_lookup(h, n):
    total = 0
    for i in range(n):
        total += h.get(i, 0)
    return total


n = 6000
h = {}
fill(h, n)
remove_odds(h, n)
print(sum_lookup(h, n) + len(h))
