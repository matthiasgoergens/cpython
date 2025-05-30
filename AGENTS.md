In general, have a look at `README.rst` in this repository.  Check what dependencies are needed, and how to install them.

Once you have the dependencies installed, you can build the interpreter.
To build, navigate to the `build` directory and run:

```bash
./my_configure
```

Afterwards in that directory you can run the usual build and test commands:

```bash
nice make -j
```

Or if you want to run the tests:

```bash
nice make -j test
```

You will find the built interpreter in `build/python`, if you want to try it out with some custom code to check.  That can often be faster than running the
complete test suite.

---

Once you can reliably build the interpreter, you can start working on your changes.

I want to build a new ordered dict implementation.  The old implementation mucks around with linked lists.  But a few Python versions ago, the vanilla
dict implementation was changed to an ordered (and compact) one.  So the vanilla dict can already do 99% of what we need.

Our steps are:

(1) Introduce a copy of ordered dict that works exactly the same as the old one, and uses the same implementation.  Instead of `Objects/odictobject.c` put it in `Objects/qdictobject.c'.  Make changes to the collections module (both the Python and the C version) to expose our clone.  Make a commit for this.

(2) Similarly, also clone all the dict code that our new qdict is using.

(3) Change that clone's equivalent of `_dictkeysobject` as follows:

- Set the usable fraction to 1/2 instead of 2/3.  (You'll need to change a few tests, too.)
- In addition to the existing entries 'Py_ssize_t dk_usable;' and 'Py_ssize_t dk_nentries;' we also want a new entry called 'Py_ssize_t dk_first_entry;'

  * dk_usable will purely track the number of free entries in the hash part of the dict
  * dk_first_entry will point to the first real entry (non-dummmy, non-empty) in the dict, if there is one.
  * (dk_first_entry + dk_nentries) & mask will point one past the last real entry in the dict.

  You will need to change the code in a few places to make this work, and also change the tests.  There's special code for very small dicts (I think under 8 entries), so you will need to change that too.

  Get rid of all the linked list code in our clone, and just directly implement
  the ordered dict functionality.  Eg move_to_end will move the entry around in the hash table, and update dk_first_entry (and dk_nentry) as needed.
