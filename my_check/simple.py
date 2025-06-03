import collections as c

x = c.meque()
print(x)
# x.appendleft('a')
# print(x)

# for i in range(20):
#     x.append(i)
#     # print(repr(x))
#     print(x)
def p(x):
    print(list(x))

# for i in range(100):
#     # x.appendleft(2*i)
#     # print(x)
#     x.append(2*i+1)
#     # print(x)
#     # print(list(x))
#     list(x)
# # x.append(10)

print(x)
y = 3*x
# print(list(x))
print('y = 3*x')
print(y)

print("Waiting for KeyboardInterrupt")
try:
    while True:
        pass
except KeyboardInterrupt:
    pass
# print(x)
