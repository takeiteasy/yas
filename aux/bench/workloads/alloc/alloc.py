def build(n, acc):
    while n != 0:
        acc = (n, acc)
        n -= 1
    return acc


def suml(lst, acc):
    while lst is not None:
        n, lst = lst
        acc += n
    return acc


print(suml(build(50000, None), 0))
