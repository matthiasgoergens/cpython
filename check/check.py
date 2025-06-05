from functools import lru_cache


@lru_cache(maxsize=2)
def fibonacci(n):
    if n < 0:
        raise ValueError("Input cannot be negative")
    elif n == 0:
        return 0
    elif n == 1:
        return 1
    else:
        return fibonacci(n - 2) + fibonacci(n - 1)


def main():
    print(fibonacci(100))
    print(fibonacci.cache_info())
    try:
        fibonacci(-1)
    except ValueError:
        pass
    print(fibonacci.cache_info())


if __name__ == "__main__":
    main()
