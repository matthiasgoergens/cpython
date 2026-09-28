import sys
N = int(sys.argv[2]); mode = sys.argv[1]
def g(n):
    for i in range(n):
        yield i
class V:
    __slots__=('x',)
    def __init__(self, x): self.x = x
    def __add__(self, o): return self
    def add(self, o): return self
def gen_sum(): return sum(g(N))
def py_loop():
    t = 0
    for i in g(N): t += i
    return t
def genexpr_any(): return any(x < 0 for x in range(N))
def loop_only():
    for i in range(N): pass
def dunder():
    v = V(1)
    for i in range(N): v + v
def plain_call():
    v = V(1)
    for i in range(N): v.add(v)
def sorted_key(): sorted(range(N), key=lambda x: x)
def map_lambda(): list(map(lambda x: x, range(N)))
globals()[mode]()
