from functools import lru_cache


class X(int):
    f_cnt = 0

    @lru_cache(2)
    def f(self, x):
        self.f_cnt += 1
        return x * 10 + self


def main():
    c = X(7)
    for x in 2, 1, 1, 1, 1, 2, 1, 3, 2, 1:
        assert c.f(x) == x * 10 + 7
        print(f"{c.f.cache_info()}\t{c.f_cnt}")


if __name__ == "__main__":
    main()
