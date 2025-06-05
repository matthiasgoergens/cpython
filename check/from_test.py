from functools import lru_cache
import unittest

class TestLRU(unittest.TestCase):
    def test_lru_method(self):
        class X(int):
            f_cnt = 0
            @lru_cache(2)
            def f(self, x):
                self.f_cnt += 1
                return x*10+self
        a = X(5)
        b = X(5)
        c = X(7)
        print("a = X(5)")
        self.assertEqual(X.f.cache_info(), (0, 0, 2, 0))

        for x in 1, 2, 2, 3, 1, 1, 1, 2, 3, 3:
            self.assertEqual(a.f(x), x*10 + 5)
            print(f"{a.f.cache_info()}\t{a.f_cnt}")
        self.assertEqual((a.f_cnt, b.f_cnt, c.f_cnt), (6, 0, 0))
        self.assertEqual(X.f.cache_info(), (4, 6, 2, 2))

        print("b = X(5)")
        for x in 1, 2, 1, 1, 1, 1, 3, 2, 2, 2:
            self.assertEqual(b.f(x), x*10 + 5)
            print(f"{b.f.cache_info()}\t{b.f_cnt}")
        self.assertEqual((a.f_cnt, b.f_cnt, c.f_cnt), (6, 4, 0))
        self.assertEqual(X.f.cache_info(), (10, 10, 2, 2))

        print("c = X(7)")
        print(f"{c.f.cache_info()}\t{c.f_cnt}")
        for x in 2, 1, 1, 1, 1, 2, 1, 3, 2, 1:
            self.assertEqual(c.f(x), x*10 + 7)
            print(f"{c.f.cache_info()}\t{c.f_cnt}")
        # TODO(Matthias): this test is very weird.
        self.assertEqual((a.f_cnt, b.f_cnt, c.f_cnt), (6, 4, 5))
        self.assertEqual(X.f.cache_info(), (15, 15, 2, 2))

        self.assertEqual(a.f.cache_info(), X.f.cache_info())
        self.assertEqual(b.f.cache_info(), X.f.cache_info())
        self.assertEqual(c.f.cache_info(), X.f.cache_info())

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
