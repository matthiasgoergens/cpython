Add a growable ring buffer to implement deque.  Compare performance with the built-in `collections.deque`.

OK, everything up to 2199 is part of the original deque implementation.

That's long.

Let's see if we can make it simpler?

```
/* defaultdict type *********************************************************/
```

---

OK, looks like we have a basic implementation of the new deque (`meque`) as a growable ring buffer.  Next is to run the tests, but also to compare performance with the built-in `collections.deque`.  (Both runtime and memory usage.)

We also need to implement shrinking our meque.  Perhaps same way we shrink lists?
