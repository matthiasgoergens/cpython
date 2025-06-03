import collections as c
from random import randrange

# c.deque()

# x = c.meque([1,2,3])
# print(x)
# print()


x = c.meque()

# x.appendleft(1)
# print(x)
# x.appendleft(1)
# print(x)
for i in range(20):
    d = i
    what = randrange(4)
    what = 0
    match what % 4:
        case 0:
            x.appendleft(d)
            # x.append(d)
        # case 1:
        #     x.append(d)
        # case 2:
        #     x.pop()
        # case 3:
        #     x.popleft()
print(x)
