Replace functools.lru with a hacked up dict.

We only need quick access to the first element.  (Our 'finger'.)

And perhaps some direct access to the 'hash table' part?

Code is in `Modules/_functoolsmodule.c`.

Starts at about line 1044.  Goes to about line 1802.

We need a way a known element to the back of the entry list.  We can just add it to `dictobject.c` and `pycore_dict.h`, I guess.

Good, `_Py_dict_lookup` is available to us in `pycore_dict.h`.

(Note, `_PyDict_LookupIndex` is similar, but calculates the hash for you.)

`struct _dictkeysobject` and associated accessors macros and function are also in that header file.  Great.

OK, our existing lru cache stores plain objects as values, when it's unbounded, and stores its link objects, when it's bounded.  (So it's sort-of dynamically typed.)
