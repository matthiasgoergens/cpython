import sys, collections
N = int(sys.argv[2]); mode = sys.argv[1]
def g(n):
    for i in range(n):
        yield 1
def inline_loop():
    for x in g(N): pass
def c_consume():
    collections.deque(g(N), 0)
def c_sum():
    sum(g(N))
globals()[mode]()
