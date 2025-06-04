import collections as c
import sys

x = c.meque(range(15))
print(x)

for _ in range (12):
    x.popleft()
    # x.pop()
    print(x)
# sys.exit(0)
y = 2 * x
print('y = 3 * x')
print(y)
print(y[2])
