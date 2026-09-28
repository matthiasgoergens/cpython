import os,re,glob,sys
rows=[]
for d in sorted(os.listdir('.')):
    tot=0; c={}
    for f in glob.glob(d+'/*'):
        for line in open(f):
            line=line.strip()
            m=re.match(r'opcode\[(\w+)\]\.execution_count : (\d+)',line)
            if m: tot+=int(m.group(2)); continue
            m=re.match(r'Calls via PyEval_EvalFrame\[(\d)\] : (\d+)',line)
            if m: c[int(m.group(1))]=c.get(int(m.group(1)),0)+int(m.group(2))
            m=re.match(r'Calls to Python functions inlined: (\d+)',line)
            if m: c['inl']=c.get('inl',0)+int(m.group(1))
    if not tot: continue
    per=lambda k: 1e4*c.get(k,0)/tot
    rows.append((per(0),d,per(2),per(4),per(6),per(8),per(9),1e4*c.get('inl',0)/tot))
print(f"{'bench':32s} {'C->Py/10k':>9s} {'gen':>6s} {'fvec':>6s} {'slot':>6s} {'api':>6s} {'meth':>6s} {'inlined':>7s}")
for r in sorted(rows,reverse=True)[:int(sys.argv[1]) if len(sys.argv)>1 else 40]:
    print(f"{r[1]:32s} {r[0]:9.1f} {r[2]:6.1f} {r[3]:6.1f} {r[4]:6.1f} {r[5]:6.1f} {r[6]:6.1f} {r[7]:7.1f}")
