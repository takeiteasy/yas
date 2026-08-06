import functools

n = 40000
lst = list(range(1, n + 1))
squared = list(map(lambda x: x * x, lst))
big = list(filter(lambda x: x > 400000000, squared))
# functools.reduce, not the sum() builtin - sum() is a C-level loop with
# no per-element closure call, unlike every other variant's fold here.
print(functools.reduce(lambda a, x: a + x, big, 0))
