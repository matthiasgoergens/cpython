import collections as c
import time
import random
import sys

def append_only(n, imp):
    d = imp()
    start = time.perf_counter()
    try:
        for i in range(n):
            # d.append((i, object()))
            d.append(i)
        end = time.perf_counter()
        return end - start
    except KeyboardInterrupt:
        end = time.perf_counter()
        t = end - start
        s = f"{imp.__name__:<10}\t{n:<10}\t{t}"
        print(f"\n{s} interrupted during run", file=sys.stderr)
        sys.stderr.flush()
        raise

# TODO(Matthias): this one is probably broken in meque.
def mixed_append_and_rotate(n, imp):
    d = imp()
    start = time.perf_counter()
    try:
        for i in range(n):
            # d.extend([2*i, 2*i+1])
            d.append(2*i)
            d.append(2*i+1)
            d.rotate(-1)
        end = time.perf_counter()
        return end - start
    except KeyboardInterrupt:
        end = time.perf_counter()
        t = end - start
        s = f"{imp.__name__:<10}\t{n:<10}\t{t}"
        print(f"\n{s} interrupted during run", file=sys.stderr)
        sys.stderr.flush()
        raise

def mixed_append_and_popleft(n, imp):
    d = imp()
    start = time.perf_counter()
    try:
        for i in range(n):
            d.append(2*i)
            d.append(2*i+1)
            d.popleft()
        end = time.perf_counter()
        return end - start
    except KeyboardInterrupt:
        end = time.perf_counter()
        t = end - start
        s = f"{imp.__name__:<10}\t{n:<10}\t{t}"
        print(f"\n{s} interrupted during run", file=sys.stderr)
        sys.stderr.flush()
        raise

def append_left_and_get(n, imp):
    d = imp()
    start = time.perf_counter()
    try:
        for i in range(n):
            d.appendleft(i)
            d[len(d)//2]
        end = time.perf_counter()
        return end - start
    except KeyboardInterrupt:
        end = time.perf_counter()
        t = end - start
        s = f"{imp.__name__:<10}\t{n:<10}\t{t}"
        print(f"\n{s} interrupted during run", file=sys.stderr)
        sys.stderr.flush()
        raise

def run(benchmark, implementations):
    while True:
        n = random.randrange(1_000_000)
        impps = list(implementations)
        random.shuffle(impps)
        for imp in impps:
            t = benchmark(n, imp)
            try:
                print(f"{imp.__name__:<10}\t{n:<10}\t{t}")
            except KeyboardInterrupt:
                print(f"{imp.__name__:<10}\t{n:<10}\t{t} interrupted during print", file=sys.stderr)
                sys.stderr.flush()
                raise
            sys.stdout.flush()

# def append_left_only(n, imp):
#     d = imp()
#     for i in range(n):
#         d.appendleft(i)
#     return d

if __name__ == "__main__":
    # run(benchmark=mixed_append_and_rotate, implementations=[c.meque, c.deque, list])
    run(benchmark=append_left_and_get, implementations=[c.meque, c.deque])
