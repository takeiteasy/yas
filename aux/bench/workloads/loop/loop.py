def sum_nontail(n):
    if n == 0:
        return 0
    return n + sum_nontail(n - 1)


def sum_tail(n, acc):
    while n != 0:
        acc += n
        n -= 1
    return acc


non_tail_n = 800
tail_n = 300000

print(sum_nontail(non_tail_n) + sum_tail(tail_n, 0))
