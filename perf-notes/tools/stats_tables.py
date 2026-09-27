"""Flatten summarize_stats.py HTML tables into compact text. Usage: stats_tables.py FILE [section-regex] [maxrows]"""
import re, sys
text = open(sys.argv[1]).read()
pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
maxrows = int(sys.argv[3]) if len(sys.argv) > 3 else 30
for m in re.finditer(r'<summary>(.*?)</summary>(.*?)</details>', text, re.S):
    title, body = m.group(1).strip(), m.group(2)
    if pat and not pat.search(title):
        continue
    print('==', title)
    rows = re.findall(r'<tr>(.*?)</tr>', body, re.S)
    for r in rows[:maxrows + 1]:
        cells = [re.sub(r'<.*?>', '', c).strip() for c in re.findall(r'<t[hd][^>]*>(.*?)</t[hd]>', r, re.S)]
        print('  ' + ' | '.join(cells))
