import sys, dis, io, collections, re
sys.path.insert(0, '.')
import run_benchmark as rb
r = rb.Richards()
for i in range(20): r.run(1)
cnt = collections.Counter()
import types
for name, obj in vars(rb).items():
    fns = []
    if isinstance(obj, type):
        fns = [v for v in vars(obj).values() if isinstance(v, types.FunctionType)]
    elif isinstance(obj, types.FunctionType):
        fns = [obj]
    for f in fns:
        s = io.StringIO()
        dis.dis(f, adaptive=True, file=s)
        for line in s.getvalue().splitlines():
            m = re.search(r'\s([A-Z_]+[A-Z])\s', line)
            if m and ('LOAD_ATTR' in m.group(1) or 'STORE_ATTR' in m.group(1)):
                cnt[m.group(1)] += 1
                if m.group(1) in ('LOAD_ATTR', 'STORE_ATTR'):
                    print(f.__qualname__, line.strip())
print(cnt)
