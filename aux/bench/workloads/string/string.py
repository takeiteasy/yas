def build(n, acc, matches):
    while n != 0:
        chunk = str(n)
        acc = acc + chunk + ","
        if chunk[0:1] == "1":
            matches += 1
        n -= 1
    return len(acc) + matches


print(build(6000, "", 0))
