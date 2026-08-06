n = 40000
lst = list(range(1, n + 1))
squared = list(map(lambda x: x * x, lst))
big = list(filter(lambda x: x > 400000000, squared))
print(sum(big))
