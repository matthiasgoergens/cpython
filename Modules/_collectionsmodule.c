#include "Python.h"
#include "pycore_call.h"          // _PyObject_CallNoArgs()
#include "pycore_dict.h"          // _PyDict_GetItem_KnownHash()
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_moduleobject.h"  // _PyModule_GetState()
#include "pycore_pyatomic_ft_wrappers.h"
#include "pycore_typeobject.h"    // _PyType_GetModuleState()

#include <stddef.h>

typedef struct {
    PyTypeObject *deque_type;
    PyTypeObject *meque_type;
    PyTypeObject *defdict_type;
    PyTypeObject *dequeiter_type;
    PyTypeObject *mequeiter_type;
    PyTypeObject *dequereviter_type;
    PyTypeObject *mequereviter_type;
    PyTypeObject *tuplegetter_type;
} collections_state;

static inline collections_state *
get_module_state(PyObject *mod)
{
    void *state = _PyModule_GetState(mod);
    assert(state != NULL);
    return (collections_state *)state;
}

static inline collections_state *
get_module_state_by_cls(PyTypeObject *cls)
{
    void *state = _PyType_GetModuleState(cls);
    assert(state != NULL);
    return (collections_state *)state;
}

static struct PyModuleDef _collectionsmodule;

static inline collections_state *
find_module_state_by_def(PyTypeObject *type)
{
    PyObject *mod = PyType_GetModuleByDef(type, &_collectionsmodule);
    assert(mod != NULL);
    return get_module_state(mod);
}

/*[clinic input]
module _collections
class _tuplegetter "_tuplegetterobject *" "clinic_state()->tuplegetter_type"
class _collections.deque "dequeobject *" "clinic_state()->deque_type"
class _collections.meque "mequeobject *" "clinic_state()->meque_type"
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=d178039753446059]*/

typedef struct dequeobject dequeobject;
typedef struct mequeobject mequeobject;

/* We can safely assume type to be the defining class,
 * since tuplegetter is not a base type */
#define clinic_state() (get_module_state_by_cls(type))
#include "clinic/_collectionsmodule.c.h"
#undef clinic_state

/*[python input]
class dequeobject_converter(self_converter):
    type = "dequeobject *"
[python start generated code]*/
/*[python end generated code: output=da39a3ee5e6b4b0d input=b6ae4a3ff852be2f]*/

/* collections module implementation of a deque() datatype
   Written and maintained by Raymond D. Hettinger <python@rcn.com>
*/

/* The block length may be set to any number over 1.  Larger numbers
 * reduce the number of calls to the memory allocator, give faster
 * indexing and rotation, and reduce the link to data overhead ratio.
 * Making the block length a power of two speeds-up the modulo
 * and division calculations in deque_item() and deque_ass_item().
 */

#define BLOCKLEN 64
#define CENTER ((BLOCKLEN - 1) / 2)
#define MAXFREEBLOCKS 16

/* Data for deque objects is stored in a doubly-linked list of fixed
 * length blocks.  This assures that appends or pops never move any
 * other data elements besides the one being appended or popped.
 *
 * Another advantage is that it completely avoids use of realloc(),
 * resulting in more predictable performance.
 *
 * Textbook implementations of doubly-linked lists store one datum
 * per link, but that gives them a 200% memory overhead (a prev and
 * next link for each datum) and it costs one malloc() call per data
 * element.  By using fixed-length blocks, the link to data ratio is
 * significantly improved and there are proportionally fewer calls
 * to malloc() and free().  The data blocks of consecutive pointers
 * also improve cache locality.
 *
 * The list of blocks is never empty, so d.leftblock and d.rightblock
 * are never equal to NULL.  The list is not circular.
 *
 * A deque d's first element is at d.leftblock[leftindex]
 * and its last element is at d.rightblock[rightindex].
 *
 * Unlike Python slice indices, these indices are inclusive on both
 * ends.  This makes the algorithms for left and right operations
 * more symmetrical and it simplifies the design.
 *
 * The indices, d.leftindex and d.rightindex are always in the range:
 *     0 <= index < BLOCKLEN
 *
 * And their exact relationship is:
 *     (d.leftindex + d.len - 1) % BLOCKLEN == d.rightindex
 *
 * Whenever d.leftblock == d.rightblock, then:
 *     d.leftindex + d.len - 1 == d.rightindex
 *
 * However, when d.leftblock != d.rightblock, the d.leftindex and
 * d.rightindex become indices into distinct blocks and either may
 * be larger than the other.
 *
 * Empty deques have:
 *     d.len == 0
 *     d.leftblock == d.rightblock
 *     d.leftindex == CENTER + 1
 *     d.rightindex == CENTER
 *
 * Checking for d.len == 0 is the intended way to see whether d is empty.
 */

typedef struct BLOCK {
    struct BLOCK *leftlink;
    PyObject *data[BLOCKLEN];
    struct BLOCK *rightlink;
} block;

struct dequeobject {
    PyObject_VAR_HEAD
    block *leftblock;
    block *rightblock;
    Py_ssize_t leftindex;       /* 0 <= leftindex < BLOCKLEN */
    Py_ssize_t rightindex;      /* 0 <= rightindex < BLOCKLEN */
    size_t state;               /* incremented whenever the indices move */
    Py_ssize_t maxlen;          /* maxlen is -1 for unbounded deques */
    Py_ssize_t numfreeblocks;
    block *freeblocks[MAXFREEBLOCKS];
    PyObject *weakreflist;
};

#define dequeobject_CAST(op)    ((dequeobject *)(op))

/* For debug builds, add error checking to track the endpoints
 * in the chain of links.  The goal is to make sure that link
 * assignments only take place at endpoints so that links already
 * in use do not get overwritten.
 *
 * CHECK_END should happen before each assignment to a block's link field.
 * MARK_END should happen whenever a link field becomes a new endpoint.
 * This happens when new blocks are added or whenever an existing
 * block is freed leaving another existing block as the new endpoint.
 */

#ifndef NDEBUG
#define MARK_END(link)  link = NULL;
#define CHECK_END(link) assert(link == NULL);
#define CHECK_NOT_END(link) assert(link != NULL);
#else
#define MARK_END(link)
#define CHECK_END(link)
#define CHECK_NOT_END(link)
#endif

/* A simple freelisting scheme is used to minimize calls to the memory
   allocator.  It accommodates common use cases where new blocks are being
   added at about the same rate as old blocks are being freed.
 */

static inline block *
newblock(dequeobject *deque) {
    block *b;
    if (deque->numfreeblocks) {
        deque->numfreeblocks--;
        return deque->freeblocks[deque->numfreeblocks];
    }
    b = PyMem_Malloc(sizeof(block));
    if (b != NULL) {
        return b;
    }
    PyErr_NoMemory();
    return NULL;
}

static inline void
freeblock(dequeobject *deque, block *b)
{
    if (deque->numfreeblocks < MAXFREEBLOCKS) {
        deque->freeblocks[deque->numfreeblocks] = b;
        deque->numfreeblocks++;
    } else {
        PyMem_Free(b);
    }
}

static PyObject *
deque_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    dequeobject *deque;
    block *b;

    /* create dequeobject structure */
    deque = (dequeobject *)type->tp_alloc(type, 0);
    if (deque == NULL)
        return NULL;

    b = newblock(deque);
    if (b == NULL) {
        Py_DECREF(deque);
        return NULL;
    }
    MARK_END(b->leftlink);
    MARK_END(b->rightlink);

    assert(BLOCKLEN >= 2);
    Py_SET_SIZE(deque, 0);
    deque->leftblock = b;
    deque->rightblock = b;
    deque->leftindex = CENTER + 1;
    deque->rightindex = CENTER;
    deque->state = 0;
    deque->maxlen = -1;
    deque->numfreeblocks = 0;
    deque->weakreflist = NULL;

    return (PyObject *)deque;
}

/*[clinic input]
@critical_section
_collections.deque.pop as deque_pop

    deque: dequeobject

Remove and return the rightmost element.
[clinic start generated code]*/

static PyObject *
deque_pop_impl(dequeobject *deque)
/*[clinic end generated code: output=2e5f7890c4251f07 input=55c5b6a8ad51d72f]*/
{
    PyObject *item;
    block *prevblock;

    if (Py_SIZE(deque) == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    item = deque->rightblock->data[deque->rightindex];
    deque->rightindex--;
    Py_SET_SIZE(deque, Py_SIZE(deque) - 1);
    deque->state++;

    if (deque->rightindex < 0) {
        if (Py_SIZE(deque)) {
            prevblock = deque->rightblock->leftlink;
            assert(deque->leftblock != deque->rightblock);
            freeblock(deque, deque->rightblock);
            CHECK_NOT_END(prevblock);
            MARK_END(prevblock->rightlink);
            deque->rightblock = prevblock;
            deque->rightindex = BLOCKLEN - 1;
        } else {
            assert(deque->leftblock == deque->rightblock);
            assert(deque->leftindex == deque->rightindex+1);
            /* re-center instead of freeing a block */
            deque->leftindex = CENTER + 1;
            deque->rightindex = CENTER;
        }
    }
    return item;
}

/*[clinic input]
@critical_section
_collections.deque.popleft as deque_popleft

     deque: dequeobject

Remove and return the leftmost element.
[clinic start generated code]*/

static PyObject *
deque_popleft_impl(dequeobject *deque)
/*[clinic end generated code: output=62b154897097ff68 input=1571ce88fe3053de]*/
{
    PyObject *item;
    block *prevblock;

    if (Py_SIZE(deque) == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    assert(deque->leftblock != NULL);
    item = deque->leftblock->data[deque->leftindex];
    deque->leftindex++;
    Py_SET_SIZE(deque, Py_SIZE(deque) - 1);
    deque->state++;

    if (deque->leftindex == BLOCKLEN) {
        if (Py_SIZE(deque)) {
            assert(deque->leftblock != deque->rightblock);
            prevblock = deque->leftblock->rightlink;
            freeblock(deque, deque->leftblock);
            CHECK_NOT_END(prevblock);
            MARK_END(prevblock->leftlink);
            deque->leftblock = prevblock;
            deque->leftindex = 0;
        } else {
            assert(deque->leftblock == deque->rightblock);
            assert(deque->leftindex == deque->rightindex+1);
            /* re-center instead of freeing a block */
            deque->leftindex = CENTER + 1;
            deque->rightindex = CENTER;
        }
    }
    return item;
}

/* The deque's size limit is d.maxlen.  The limit can be zero or positive.
 * If there is no limit, then d.maxlen == -1.
 *
 * After an item is added to a deque, we check to see if the size has
 * grown past the limit. If it has, we get the size back down to the limit
 * by popping an item off of the opposite end.  The methods that can
 * trigger this are append(), appendleft(), extend(), and extendleft().
 *
 * The macro to check whether a deque needs to be trimmed uses a single
 * unsigned test that returns true whenever 0 <= maxlen < Py_SIZE(deque).
 */

#define NEEDS_TRIM(deque, maxlen) ((size_t)(maxlen) < (size_t)(Py_SIZE(deque)))

static inline int
deque_append_lock_held(dequeobject *deque, PyObject *item, Py_ssize_t maxlen)
{
    if (deque->rightindex == BLOCKLEN - 1) {
        block *b = newblock(deque);
        if (b == NULL)
            return -1;
        b->leftlink = deque->rightblock;
        CHECK_END(deque->rightblock->rightlink);
        deque->rightblock->rightlink = b;
        deque->rightblock = b;
        MARK_END(b->rightlink);
        deque->rightindex = -1;
    }
    Py_SET_SIZE(deque, Py_SIZE(deque) + 1);
    deque->rightindex++;
    deque->rightblock->data[deque->rightindex] = item;
    if (NEEDS_TRIM(deque, maxlen)) {
        PyObject *olditem = deque_popleft_impl(deque);
        Py_DECREF(olditem);
    } else {
        deque->state++;
    }
    return 0;
}

/*[clinic input]
@critical_section
_collections.deque.append as deque_append

    deque: dequeobject
    item: object
    /

Add an element to the right side of the deque.
[clinic start generated code]*/

static PyObject *
deque_append_impl(dequeobject *deque, PyObject *item)
/*[clinic end generated code: output=9c7bcb8b599c6362 input=b0eeeb09b9f5cf18]*/
{
    if (deque_append_lock_held(deque, Py_NewRef(item), deque->maxlen) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static inline int
deque_appendleft_lock_held(dequeobject *deque, PyObject *item,
                           Py_ssize_t maxlen)
{
    if (deque->leftindex == 0) {
        block *b = newblock(deque);
        if (b == NULL)
            return -1;
        b->rightlink = deque->leftblock;
        CHECK_END(deque->leftblock->leftlink);
        deque->leftblock->leftlink = b;
        deque->leftblock = b;
        MARK_END(b->leftlink);
        deque->leftindex = BLOCKLEN;
    }
    Py_SET_SIZE(deque, Py_SIZE(deque) + 1);
    deque->leftindex--;
    deque->leftblock->data[deque->leftindex] = item;
    if (NEEDS_TRIM(deque, maxlen)) {
        PyObject *olditem = deque_pop_impl(deque);
        Py_DECREF(olditem);
    } else {
        deque->state++;
    }
    return 0;
}

/*[clinic input]
@critical_section
_collections.deque.appendleft as deque_appendleft

    deque: dequeobject
    item: object
    /

Add an element to the left side of the deque.
[clinic start generated code]*/

static PyObject *
deque_appendleft_impl(dequeobject *deque, PyObject *item)
/*[clinic end generated code: output=9a192edbcd0f20db input=236c2fbceaf08e14]*/
{
    if (deque_appendleft_lock_held(deque, Py_NewRef(item), deque->maxlen) < 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyObject*
finalize_iterator(PyObject *it)
{
    if (PyErr_Occurred()) {
        if (PyErr_ExceptionMatches(PyExc_StopIteration))
            PyErr_Clear();
        else {
            Py_DECREF(it);
            return NULL;
        }
    }
    Py_DECREF(it);
    Py_RETURN_NONE;
}

/* Run an iterator to exhaustion.  Shortcut for
   the extend/extendleft methods when maxlen == 0. */
static PyObject*
consume_iterator(PyObject *it)
{
    PyObject *(*iternext)(PyObject *);
    PyObject *item;

    iternext = *Py_TYPE(it)->tp_iternext;
    while ((item = iternext(it)) != NULL) {
        Py_DECREF(item);
    }
    return finalize_iterator(it);
}

/*[clinic input]
@critical_section
_collections.deque.extend as deque_extend

    deque: dequeobject
    iterable: object
    /

Extend the right side of the deque with elements from the iterable.
[clinic start generated code]*/

static PyObject *
deque_extend_impl(dequeobject *deque, PyObject *iterable)
/*[clinic end generated code: output=8b5ffa57ce82d980 input=85861954127c81da]*/
{
    PyObject *it, *item;
    PyObject *(*iternext)(PyObject *);
    Py_ssize_t maxlen = deque->maxlen;

    /* Handle case where id(deque) == id(iterable) */
    if ((PyObject *)deque == iterable) {
        PyObject *result;
        PyObject *s = PySequence_List(iterable);
        if (s == NULL)
            return NULL;
        result = deque_extend((PyObject*)deque, s);
        Py_DECREF(s);
        return result;
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL)
        return NULL;

    if (maxlen == 0)
        return consume_iterator(it);

    /* Space saving heuristic.  Start filling from the left */
    if (Py_SIZE(deque) == 0) {
        assert(deque->leftblock == deque->rightblock);
        assert(deque->leftindex == deque->rightindex+1);
        deque->leftindex = 1;
        deque->rightindex = 0;
    }

    iternext = *Py_TYPE(it)->tp_iternext;
    while ((item = iternext(it)) != NULL) {
        if (deque_append_lock_held(deque, item, maxlen) == -1) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
    }
    return finalize_iterator(it);
}

/*[clinic input]
@critical_section
_collections.deque.extendleft as deque_extendleft

    deque: dequeobject
    iterable: object
    /

Extend the left side of the deque with elements from the iterable.
[clinic start generated code]*/

static PyObject *
deque_extendleft_impl(dequeobject *deque, PyObject *iterable)
/*[clinic end generated code: output=ba44191aa8e35a26 input=640dabd086115689]*/
{
    PyObject *it, *item;
    PyObject *(*iternext)(PyObject *);
    Py_ssize_t maxlen = deque->maxlen;

    /* Handle case where id(deque) == id(iterable) */
    if ((PyObject *)deque == iterable) {
        PyObject *result;
        PyObject *s = PySequence_List(iterable);
        if (s == NULL)
            return NULL;
        result = deque_extendleft_impl(deque, s);
        Py_DECREF(s);
        return result;
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL)
        return NULL;

    if (maxlen == 0)
        return consume_iterator(it);

    /* Space saving heuristic.  Start filling from the right */
    if (Py_SIZE(deque) == 0) {
        assert(deque->leftblock == deque->rightblock);
        assert(deque->leftindex == deque->rightindex+1);
        deque->leftindex = BLOCKLEN - 1;
        deque->rightindex = BLOCKLEN - 2;
    }

    iternext = *Py_TYPE(it)->tp_iternext;
    while ((item = iternext(it)) != NULL) {
        if (deque_appendleft_lock_held(deque, item, maxlen) == -1) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
    }
    return finalize_iterator(it);
}

static PyObject *
deque_inplace_concat(PyObject *self, PyObject *other)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyObject *result;

    // deque_extend is thread-safe
    result = deque_extend((PyObject*)deque, other);
    if (result == NULL)
        return result;
    Py_INCREF(deque);
    Py_DECREF(result);
    return (PyObject *)deque;
}

/*[clinic input]
@critical_section
_collections.deque.copy as deque_copy

    deque: dequeobject

Return a shallow copy of a deque.
[clinic start generated code]*/

static PyObject *
deque_copy_impl(dequeobject *deque)
/*[clinic end generated code: output=6409b3d1ad2898b5 input=51d2ed1a23bab5e2]*/
{
    PyObject *result;
    dequeobject *old_deque = deque;
    collections_state *state = find_module_state_by_def(Py_TYPE(deque));
    if (Py_IS_TYPE(deque, state->deque_type)) {
        dequeobject *new_deque;
        PyObject *rv;

        new_deque = (dequeobject *)deque_new(state->deque_type, NULL, NULL);
        if (new_deque == NULL)
            return NULL;
        new_deque->maxlen = old_deque->maxlen;
        /* Fast path for the deque_repeat() common case where len(deque) == 1
         *
         * It's safe to not acquire the per-object lock for new_deque; it's
         * invisible to other threads.
         */
        if (Py_SIZE(deque) == 1) {
            PyObject *item = old_deque->leftblock->data[old_deque->leftindex];
            rv = deque_append_impl(new_deque, item);
        } else {
            rv = deque_extend_impl(new_deque, (PyObject *)deque);
        }
        if (rv != NULL) {
            Py_DECREF(rv);
            return (PyObject *)new_deque;
        }
        Py_DECREF(new_deque);
        return NULL;
    }
    if (old_deque->maxlen < 0)
        result = PyObject_CallOneArg((PyObject *)(Py_TYPE(deque)),
                                     (PyObject *)deque);
    else
        result = PyObject_CallFunction((PyObject *)(Py_TYPE(deque)), "Oi",
                                       deque, old_deque->maxlen, NULL);
    if (result != NULL && !PyObject_TypeCheck(result, state->deque_type)) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s() must return a deque, not %.200s",
                     Py_TYPE(deque)->tp_name, Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

/*[clinic input]
@critical_section
_collections.deque.__copy__ as deque___copy__ = _collections.deque.copy

Return a shallow copy of a deque.
[clinic start generated code]*/

static PyObject *
deque___copy___impl(dequeobject *deque)
/*[clinic end generated code: output=7c5821504342bf23 input=f5464036f9686a55]*/
{
    return deque_copy_impl(deque);
}

static PyObject *
deque_concat_lock_held(dequeobject *deque, PyObject *other)
{
    PyObject *new_deque, *result;
    int rv;

    collections_state *state = find_module_state_by_def(Py_TYPE(deque));
    rv = PyObject_IsInstance(other, (PyObject *)state->deque_type);
    if (rv <= 0) {
        if (rv == 0) {
            PyErr_Format(PyExc_TypeError,
                         "can only concatenate deque (not \"%.200s\") to deque",
                         Py_TYPE(other)->tp_name);
        }
        return NULL;
    }

    new_deque = deque_copy_impl(deque);
    if (new_deque == NULL)
        return NULL;

    // It's safe to not acquire the per-object lock for new_deque; it's
    // invisible to other threads.
    result = deque_extend_impl((dequeobject *)new_deque, other);
    if (result == NULL) {
        Py_DECREF(new_deque);
        return NULL;
    }
    Py_DECREF(result);
    return new_deque;
}

static PyObject *
deque_concat(PyObject *self, PyObject *other)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(deque);
    result = deque_concat_lock_held(deque, other);
    Py_END_CRITICAL_SECTION();
    return result;
}

static int
deque_clear(PyObject *self)
{
    block *b;
    block *prevblock;
    block *leftblock;
    Py_ssize_t leftindex;
    Py_ssize_t n, m;
    PyObject *item;
    PyObject **itemptr, **limit;
    dequeobject *deque = dequeobject_CAST(self);

    if (Py_SIZE(deque) == 0)
        return 0;

    /* During the process of clearing a deque, decrefs can cause the
       deque to mutate.  To avoid fatal confusion, we have to make the
       deque empty before clearing the blocks and never refer to
       anything via deque->ref while clearing.  (This is the same
       technique used for clearing lists, sets, and dicts.)

       Making the deque empty requires allocating a new empty block.  In
       the unlikely event that memory is full, we fall back to an
       alternate method that doesn't require a new block.  Repeating
       pops in a while-loop is slower, possibly re-entrant (and a clever
       adversary could cause it to never terminate).
    */

    b = newblock(deque);
    if (b == NULL) {
        PyErr_Clear();
        goto alternate_method;
    }

    /* Remember the old size, leftblock, and leftindex */
    n = Py_SIZE(deque);
    leftblock = deque->leftblock;
    leftindex = deque->leftindex;

    /* Set the deque to be empty using the newly allocated block */
    MARK_END(b->leftlink);
    MARK_END(b->rightlink);
    Py_SET_SIZE(deque, 0);
    deque->leftblock = b;
    deque->rightblock = b;
    deque->leftindex = CENTER + 1;
    deque->rightindex = CENTER;
    deque->state++;

    /* Now the old size, leftblock, and leftindex are disconnected from
       the empty deque and we can use them to decref the pointers.
    */
    m = (BLOCKLEN - leftindex > n) ? n : BLOCKLEN - leftindex;
    itemptr = &leftblock->data[leftindex];
    limit = itemptr + m;
    n -= m;
    while (1) {
        if (itemptr == limit) {
            if (n == 0)
                break;
            CHECK_NOT_END(leftblock->rightlink);
            prevblock = leftblock;
            leftblock = leftblock->rightlink;
            m = (n > BLOCKLEN) ? BLOCKLEN : n;
            itemptr = leftblock->data;
            limit = itemptr + m;
            n -= m;
            freeblock(deque, prevblock);
        }
        item = *(itemptr++);
        Py_DECREF(item);
    }
    CHECK_END(leftblock->rightlink);
    freeblock(deque, leftblock);
    return 0;

  alternate_method:
    while (Py_SIZE(deque)) {
        item = deque_pop_impl(deque);
        assert (item != NULL);
        Py_DECREF(item);
    }
    return 0;
}

/*[clinic input]
@critical_section
_collections.deque.clear as deque_clearmethod

    deque: dequeobject

Remove all elements from the deque.
[clinic start generated code]*/

static PyObject *
deque_clearmethod_impl(dequeobject *deque)
/*[clinic end generated code: output=79b2513e097615c1 input=3a22e9605d20c5e9]*/
{
    (void)deque_clear((PyObject *)deque);
    Py_RETURN_NONE;
}

static PyObject *
deque_inplace_repeat_lock_held(dequeobject *deque, Py_ssize_t n)
{
    Py_ssize_t i, m, size;
    PyObject *seq;
    PyObject *rv;

    size = Py_SIZE(deque);
    if (size == 0 || n == 1) {
        return Py_NewRef(deque);
    }

    if (n <= 0) {
        (void)deque_clear((PyObject *)deque);
        return Py_NewRef(deque);
    }

    if (size == 1) {
        /* common case, repeating a single element */
        PyObject *item = deque->leftblock->data[deque->leftindex];

        if (deque->maxlen >= 0 && n > deque->maxlen)
            n = deque->maxlen;

        deque->state++;
        for (i = 0 ; i < n-1 ; ) {
            if (deque->rightindex == BLOCKLEN - 1) {
                block *b = newblock(deque);
                if (b == NULL) {
                    Py_SET_SIZE(deque, Py_SIZE(deque) + i);
                    return NULL;
                }
                b->leftlink = deque->rightblock;
                CHECK_END(deque->rightblock->rightlink);
                deque->rightblock->rightlink = b;
                deque->rightblock = b;
                MARK_END(b->rightlink);
                deque->rightindex = -1;
            }
            m = n - 1 - i;
            if (m > BLOCKLEN - 1 - deque->rightindex)
                m = BLOCKLEN - 1 - deque->rightindex;
            i += m;
            while (m--) {
                deque->rightindex++;
                deque->rightblock->data[deque->rightindex] = Py_NewRef(item);
            }
        }
        Py_SET_SIZE(deque, Py_SIZE(deque) + i);
        return Py_NewRef(deque);
    }

    if ((size_t)size > PY_SSIZE_T_MAX / (size_t)n) {
        return PyErr_NoMemory();
    }

    seq = PySequence_List((PyObject *)deque);
    if (seq == NULL)
        return seq;

    /* Reduce the number of repetitions when maxlen would be exceeded */
    if (deque->maxlen >= 0 && n * size > deque->maxlen)
        n = (deque->maxlen + size - 1) / size;

    for (i = 0 ; i < n-1 ; i++) {
        rv = deque_extend_impl(deque, seq);
        if (rv == NULL) {
            Py_DECREF(seq);
            return NULL;
        }
        Py_DECREF(rv);
    }
    Py_INCREF(deque);
    Py_DECREF(seq);
    return (PyObject *)deque;
}

static PyObject *
deque_inplace_repeat(PyObject *self, Py_ssize_t n)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(deque);
    result = deque_inplace_repeat_lock_held(deque, n);
    Py_END_CRITICAL_SECTION();
    return result;
}

static PyObject *
deque_repeat(PyObject *self, Py_ssize_t n)
{
    dequeobject *deque = dequeobject_CAST(self);
    dequeobject *new_deque;
    PyObject *rv;

    Py_BEGIN_CRITICAL_SECTION(deque);
    new_deque = (dequeobject *)deque_copy_impl(deque);
    Py_END_CRITICAL_SECTION();
    if (new_deque == NULL)
        return NULL;
    // It's safe to not acquire the per-object lock for new_deque; it's
    // invisible to other threads.
    rv = deque_inplace_repeat_lock_held(new_deque, n);
    Py_DECREF(new_deque);
    return rv;
}

/* The rotate() method is part of the public API and is used internally
as a primitive for other methods.

Rotation by 1 or -1 is a common case, so any optimizations for high
volume rotations should take care not to penalize the common case.

Conceptually, a rotate by one is equivalent to a pop on one side and an
append on the other.  However, a pop/append pair is unnecessarily slow
because it requires an incref/decref pair for an object located randomly
in memory.  It is better to just move the object pointer from one block
to the next without changing the reference count.

When moving batches of pointers, it is tempting to use memcpy() but that
proved to be slower than a simple loop for a variety of reasons.
Memcpy() cannot know in advance that we're copying pointers instead of
bytes, that the source and destination are pointer aligned and
non-overlapping, that moving just one pointer is a common case, that we
never need to move more than BLOCKLEN pointers, and that at least one
pointer is always moved.

For high volume rotations, newblock() and freeblock() are never called
more than once.  Previously emptied blocks are immediately reused as a
destination block.  If a block is left-over at the end, it is freed.
*/

static int
_deque_rotate(dequeobject *deque, Py_ssize_t n)
{
    block *b = NULL;
    block *leftblock = deque->leftblock;
    block *rightblock = deque->rightblock;
    Py_ssize_t leftindex = deque->leftindex;
    Py_ssize_t rightindex = deque->rightindex;
    Py_ssize_t len=Py_SIZE(deque), halflen=len>>1;
    int rv = -1;

    if (len <= 1)
        return 0;
    if (n > halflen || n < -halflen) {
        n %= len;
        if (n > halflen)
            n -= len;
        else if (n < -halflen)
            n += len;
    }
    assert(len > 1);
    assert(-halflen <= n && n <= halflen);

    deque->state++;
    while (n > 0) {
        if (leftindex == 0) {
            if (b == NULL) {
                b = newblock(deque);
                if (b == NULL)
                    goto done;
            }
            b->rightlink = leftblock;
            CHECK_END(leftblock->leftlink);
            leftblock->leftlink = b;
            leftblock = b;
            MARK_END(b->leftlink);
            leftindex = BLOCKLEN;
            b = NULL;
        }
        assert(leftindex > 0);
        {
            PyObject **src, **dest;
            Py_ssize_t m = n;

            if (m > rightindex + 1)
                m = rightindex + 1;
            if (m > leftindex)
                m = leftindex;
            assert (m > 0 && m <= len);
            rightindex -= m;
            leftindex -= m;
            src = &rightblock->data[rightindex + 1];
            dest = &leftblock->data[leftindex];
            n -= m;
            do {
                *(dest++) = *(src++);
            } while (--m);
        }
        if (rightindex < 0) {
            assert(leftblock != rightblock);
            assert(b == NULL);
            b = rightblock;
            CHECK_NOT_END(rightblock->leftlink);
            rightblock = rightblock->leftlink;
            MARK_END(rightblock->rightlink);
            rightindex = BLOCKLEN - 1;
        }
    }
    while (n < 0) {
        if (rightindex == BLOCKLEN - 1) {
            if (b == NULL) {
                b = newblock(deque);
                if (b == NULL)
                    goto done;
            }
            b->leftlink = rightblock;
            CHECK_END(rightblock->rightlink);
            rightblock->rightlink = b;
            rightblock = b;
            MARK_END(b->rightlink);
            rightindex = -1;
            b = NULL;
        }
        assert (rightindex < BLOCKLEN - 1);
        {
            PyObject **src, **dest;
            Py_ssize_t m = -n;

            if (m > BLOCKLEN - leftindex)
                m = BLOCKLEN - leftindex;
            if (m > BLOCKLEN - 1 - rightindex)
                m = BLOCKLEN - 1 - rightindex;
            assert (m > 0 && m <= len);
            src = &leftblock->data[leftindex];
            dest = &rightblock->data[rightindex + 1];
            leftindex += m;
            rightindex += m;
            n += m;
            do {
                *(dest++) = *(src++);
            } while (--m);
        }
        if (leftindex == BLOCKLEN) {
            assert(leftblock != rightblock);
            assert(b == NULL);
            b = leftblock;
            CHECK_NOT_END(leftblock->rightlink);
            leftblock = leftblock->rightlink;
            MARK_END(leftblock->leftlink);
            leftindex = 0;
        }
    }
    rv = 0;
done:
    if (b != NULL)
        freeblock(deque, b);
    deque->leftblock = leftblock;
    deque->rightblock = rightblock;
    deque->leftindex = leftindex;
    deque->rightindex = rightindex;

    return rv;
}

/*[clinic input]
@critical_section
_collections.deque.rotate as deque_rotate

    deque: dequeobject
    n: Py_ssize_t = 1
    /

Rotate the deque n steps to the right.  If n is negative, rotates left.
[clinic start generated code]*/

static PyObject *
deque_rotate_impl(dequeobject *deque, Py_ssize_t n)
/*[clinic end generated code: output=96c2402a371eb15d input=5bf834296246e002]*/
{
    if (!_deque_rotate(deque, n))
        Py_RETURN_NONE;
    return NULL;
}

/*[clinic input]
@critical_section
_collections.deque.reverse as deque_reverse

    deque: dequeobject

Reverse *IN PLACE*.
[clinic start generated code]*/

static PyObject *
deque_reverse_impl(dequeobject *deque)
/*[clinic end generated code: output=bdeebc2cf8c1f064 input=26f4167fd623027f]*/
{
    block *leftblock = deque->leftblock;
    block *rightblock = deque->rightblock;
    Py_ssize_t leftindex = deque->leftindex;
    Py_ssize_t rightindex = deque->rightindex;
    Py_ssize_t n = Py_SIZE(deque) >> 1;
    PyObject *tmp;

    while (--n >= 0) {
        /* Validate that pointers haven't met in the middle */
        assert(leftblock != rightblock || leftindex < rightindex);
        CHECK_NOT_END(leftblock);
        CHECK_NOT_END(rightblock);

        /* Swap */
        tmp = leftblock->data[leftindex];
        leftblock->data[leftindex] = rightblock->data[rightindex];
        rightblock->data[rightindex] = tmp;

        /* Advance left block/index pair */
        leftindex++;
        if (leftindex == BLOCKLEN) {
            leftblock = leftblock->rightlink;
            leftindex = 0;
        }

        /* Step backwards with the right block/index pair */
        rightindex--;
        if (rightindex < 0) {
            rightblock = rightblock->leftlink;
            rightindex = BLOCKLEN - 1;
        }
    }
    Py_RETURN_NONE;
}

/*[clinic input]
@critical_section
_collections.deque.count as deque_count

    deque: dequeobject
    value as v: object
    /

Return number of occurrences of value.
[clinic start generated code]*/

static PyObject *
deque_count_impl(dequeobject *deque, PyObject *v)
/*[clinic end generated code: output=2ca26c49b6ab0400 input=4ef67ef2b34dc1fc]*/
{
    block *b = deque->leftblock;
    Py_ssize_t index = deque->leftindex;
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t count = 0;
    size_t start_state = deque->state;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        CHECK_NOT_END(b);
        item = Py_NewRef(b->data[index]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0)
            return NULL;
        count += cmp;

        if (start_state != deque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return NULL;
        }

        /* Advance left block/index pair */
        index++;
        if (index == BLOCKLEN) {
            b = b->rightlink;
            index = 0;
        }
    }
    return PyLong_FromSsize_t(count);
}

static int
deque_contains_lock_held(dequeobject *deque, PyObject *v)
{
    block *b = deque->leftblock;
    Py_ssize_t index = deque->leftindex;
    Py_ssize_t n = Py_SIZE(deque);
    size_t start_state = deque->state;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        CHECK_NOT_END(b);
        item = Py_NewRef(b->data[index]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp) {
            return cmp;
        }
        if (start_state != deque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return -1;
        }
        index++;
        if (index == BLOCKLEN) {
            b = b->rightlink;
            index = 0;
        }
    }
    return 0;
}

static int
deque_contains(PyObject *self, PyObject *v)
{
    dequeobject *deque = dequeobject_CAST(self);
    int result;
    Py_BEGIN_CRITICAL_SECTION(deque);
    result = deque_contains_lock_held(deque, v);
    Py_END_CRITICAL_SECTION();
    return result;
}

static Py_ssize_t
deque_len(PyObject *self)
{
    PyVarObject *deque = _PyVarObject_CAST(self);
    return FT_ATOMIC_LOAD_SSIZE(deque->ob_size);
}

/*[clinic input]
@critical_section
@text_signature "($self, value, [start, [stop]])"
_collections.deque.index as deque_index

    deque: dequeobject
    value as v: object
    start: object(converter='_PyEval_SliceIndexNotNone', type='Py_ssize_t', c_default='0') = NULL
    stop: object(converter='_PyEval_SliceIndexNotNone', type='Py_ssize_t', c_default='Py_SIZE(deque)') = NULL
    /

Return first index of value.

Raises ValueError if the value is not present.
[clinic start generated code]*/

static PyObject *
deque_index_impl(dequeobject *deque, PyObject *v, Py_ssize_t start,
                 Py_ssize_t stop)
/*[clinic end generated code: output=df45132753175ef9 input=90f48833a91e1743]*/
{
    Py_ssize_t i, n;
    PyObject *item;
    block *b = deque->leftblock;
    Py_ssize_t index = deque->leftindex;
    size_t start_state = deque->state;
    int cmp;

    if (start < 0) {
        start += Py_SIZE(deque);
        if (start < 0)
            start = 0;
    }
    if (stop < 0) {
        stop += Py_SIZE(deque);
        if (stop < 0)
            stop = 0;
    }
    if (stop > Py_SIZE(deque))
        stop = Py_SIZE(deque);
    if (start > stop)
        start = stop;
    assert(0 <= start && start <= stop && stop <= Py_SIZE(deque));

    for (i=0 ; i < start - BLOCKLEN ; i += BLOCKLEN) {
        b = b->rightlink;
    }
    for ( ; i < start ; i++) {
        index++;
        if (index == BLOCKLEN) {
            b = b->rightlink;
            index = 0;
        }
    }

    n = stop - i;
    while (--n >= 0) {
        CHECK_NOT_END(b);
        item = Py_NewRef(b->data[index]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp > 0)
            return PyLong_FromSsize_t(stop - n - 1);
        if (cmp < 0)
            return NULL;
        if (start_state != deque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return NULL;
        }
        index++;
        if (index == BLOCKLEN) {
            b = b->rightlink;
            index = 0;
        }
    }
    PyErr_SetString(PyExc_ValueError, "deque.index(x): x not in deque");
    return NULL;
}

/* insert(), remove(), and delitem() are implemented in terms of
   rotate() for simplicity and reasonable performance near the end
   points.  If for some reason these methods become popular, it is not
   hard to re-implement this using direct data movement (similar to
   the code used in list slice assignments) and achieve a performance
   boost (by moving each pointer only once instead of twice).
*/

/*[clinic input]
@critical_section
_collections.deque.insert as deque_insert

    deque: dequeobject
    index: Py_ssize_t
    value: object
    /

Insert value before index.
[clinic start generated code]*/

static PyObject *
deque_insert_impl(dequeobject *deque, Py_ssize_t index, PyObject *value)
/*[clinic end generated code: output=ef4d2c15d5532b80 input=dbee706586cc9cde]*/
{
    Py_ssize_t n = Py_SIZE(deque);
    PyObject *rv;

    if (deque->maxlen == Py_SIZE(deque)) {
        PyErr_SetString(PyExc_IndexError, "deque already at its maximum size");
        return NULL;
    }
    if (index >= n)
        return deque_append_impl(deque, value);
    if (index <= -n || index == 0)
        return deque_appendleft_impl(deque, value);
    if (_deque_rotate(deque, -index))
        return NULL;
    if (index < 0)
        rv = deque_append_impl(deque, value);
    else
        rv = deque_appendleft_impl(deque, value);
    if (rv == NULL)
        return NULL;
    Py_DECREF(rv);
    if (_deque_rotate(deque, index))
        return NULL;
    Py_RETURN_NONE;
}

static int
valid_index(Py_ssize_t i, Py_ssize_t limit)
{
    /* The cast to size_t lets us use just a single comparison
       to check whether i is in the range: 0 <= i < limit */
    return (size_t) i < (size_t) limit;
}

static PyObject *
deque_item_lock_held(dequeobject *deque, Py_ssize_t i)
{
    block *b;
    PyObject *item;
    Py_ssize_t n, index=i;

    if (!valid_index(i, Py_SIZE(deque))) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return NULL;
    }

    if (i == 0) {
        i = deque->leftindex;
        b = deque->leftblock;
    } else if (i == Py_SIZE(deque) - 1) {
        i = deque->rightindex;
        b = deque->rightblock;
    } else {
        i += deque->leftindex;
        n = (Py_ssize_t)((size_t) i / BLOCKLEN);
        i = (Py_ssize_t)((size_t) i % BLOCKLEN);
        if (index < (Py_SIZE(deque) >> 1)) {
            b = deque->leftblock;
            while (--n >= 0)
                b = b->rightlink;
        } else {
            n = (Py_ssize_t)(
                    ((size_t)(deque->leftindex + Py_SIZE(deque) - 1))
                    / BLOCKLEN - n);
            b = deque->rightblock;
            while (--n >= 0)
                b = b->leftlink;
        }
    }
    item = b->data[i];
    return Py_NewRef(item);
}

static PyObject *
deque_item(PyObject *self, Py_ssize_t i)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(deque);
    result = deque_item_lock_held(deque, i);
    Py_END_CRITICAL_SECTION();
    return result;
}

static int
deque_del_item(dequeobject *deque, Py_ssize_t i)
{
    PyObject *item;
    int rv;

    assert (i >= 0 && i < Py_SIZE(deque));
    if (_deque_rotate(deque, -i))
        return -1;
    item = deque_popleft_impl(deque);
    rv = _deque_rotate(deque, i);
    assert (item != NULL);
    Py_DECREF(item);
    return rv;
}

/*[clinic input]
@critical_section
_collections.deque.remove as deque_remove

    deque: dequeobject
    value: object
    /

Remove first occurrence of value.
[clinic start generated code]*/

static PyObject *
deque_remove_impl(dequeobject *deque, PyObject *value)
/*[clinic end generated code: output=54cff28b8ef78c5b input=60eb3f8aa4de532a]*/
{
    PyObject *item;
    block *b = deque->leftblock;
    Py_ssize_t i, n = Py_SIZE(deque), index = deque->leftindex;
    size_t start_state = deque->state;
    int cmp, rv;

    for (i = 0 ; i < n; i++) {
        item = Py_NewRef(b->data[index]);
        cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0) {
            return NULL;
        }
        if (start_state != deque->state) {
            PyErr_SetString(PyExc_IndexError,
                            "deque mutated during iteration");
            return NULL;
        }
        if (cmp > 0) {
            break;
        }
        index++;
        if (index == BLOCKLEN) {
            b = b->rightlink;
            index = 0;
        }
    }
    if (i == n) {
        PyErr_SetString(PyExc_ValueError, "deque.remove(x): x not in deque");
        return NULL;
    }
    rv = deque_del_item(deque, i);
    if (rv == -1) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static int
deque_ass_item_lock_held(dequeobject *deque, Py_ssize_t i, PyObject *v)
{
    block *b;
    Py_ssize_t n, len=Py_SIZE(deque), halflen=(len+1)>>1, index=i;

    if (!valid_index(i, len)) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return -1;
    }
    if (v == NULL)
        return deque_del_item(deque, i);

    i += deque->leftindex;
    n = (Py_ssize_t)((size_t) i / BLOCKLEN);
    i = (Py_ssize_t)((size_t) i % BLOCKLEN);
    if (index <= halflen) {
        b = deque->leftblock;
        while (--n >= 0)
            b = b->rightlink;
    } else {
        n = (Py_ssize_t)(
                ((size_t)(deque->leftindex + Py_SIZE(deque) - 1))
                / BLOCKLEN - n);
        b = deque->rightblock;
        while (--n >= 0)
            b = b->leftlink;
    }
    Py_SETREF(b->data[i], Py_NewRef(v));
    return 0;
}

static int
deque_ass_item(PyObject *self, Py_ssize_t i, PyObject *v)
{
    dequeobject *deque = dequeobject_CAST(self);
    int result;
    Py_BEGIN_CRITICAL_SECTION(deque);
    result = deque_ass_item_lock_held(deque, i, v);
    Py_END_CRITICAL_SECTION();
    return result;
}

static void
deque_dealloc(PyObject *self)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyTypeObject *tp = Py_TYPE(deque);
    Py_ssize_t i;

    PyObject_GC_UnTrack(deque);
    if (deque->weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }
    if (deque->leftblock != NULL) {
        (void)deque_clear(self);
        assert(deque->leftblock != NULL);
        freeblock(deque, deque->leftblock);
    }
    deque->leftblock = NULL;
    deque->rightblock = NULL;
    for (i=0 ; i < deque->numfreeblocks ; i++) {
        PyMem_Free(deque->freeblocks[i]);
    }
    tp->tp_free(deque);
    Py_DECREF(tp);
}

static int
deque_traverse(PyObject *self, visitproc visit, void *arg)
{
    dequeobject *deque = dequeobject_CAST(self);
    Py_VISIT(Py_TYPE(deque));

    block *b;
    PyObject *item;
    Py_ssize_t index;
    Py_ssize_t indexlo = deque->leftindex;
    Py_ssize_t indexhigh;

    for (b = deque->leftblock; b != deque->rightblock; b = b->rightlink) {
        for (index = indexlo; index < BLOCKLEN ; index++) {
            item = b->data[index];
            Py_VISIT(item);
        }
        indexlo = 0;
    }
    indexhigh = deque->rightindex;
    for (index = indexlo; index <= indexhigh; index++) {
        item = b->data[index];
        Py_VISIT(item);
    }
    return 0;
}

/*[clinic input]
_collections.deque.__reduce__ as deque___reduce__

    deque: dequeobject

Return state information for pickling.
[clinic start generated code]*/

static PyObject *
deque___reduce___impl(dequeobject *deque)
/*[clinic end generated code: output=cb85d9e0b7d2c5ad input=991a933a5bc7a526]*/
{
    PyObject *state, *it;

    state = _PyObject_GetState((PyObject *)deque);
    if (state == NULL) {
        return NULL;
    }

    it = PyObject_GetIter((PyObject *)deque);
    if (it == NULL) {
        Py_DECREF(state);
        return NULL;
    }

    // It's safe to access deque->maxlen here without holding the per object
    // lock for deque; deque->maxlen is only assigned during construction.
    if (deque->maxlen < 0) {
        return Py_BuildValue("O()NN", Py_TYPE(deque), state, it);
    }
    else {
        return Py_BuildValue("O(()n)NN", Py_TYPE(deque), deque->maxlen, state, it);
    }
}

PyDoc_STRVAR(reduce_doc, "Return state information for pickling.");

static PyObject *
deque_repr(PyObject *deque)
{
    PyObject *aslist, *result;
    int i;

    i = Py_ReprEnter(deque);
    if (i != 0) {
        if (i < 0)
            return NULL;
        return PyUnicode_FromString("[...]");
    }

    aslist = PySequence_List(deque);
    if (aslist == NULL) {
        Py_ReprLeave(deque);
        return NULL;
    }
    Py_ssize_t maxlen = dequeobject_CAST(deque)->maxlen;
    if (maxlen >= 0)
        result = PyUnicode_FromFormat("%s(%R, maxlen=%zd)",
                                      _PyType_Name(Py_TYPE(deque)), aslist,
                                      maxlen);
    else
        result = PyUnicode_FromFormat("%s(%R)",
                                      _PyType_Name(Py_TYPE(deque)), aslist);
    Py_ReprLeave(deque);
    Py_DECREF(aslist);
    return result;
}

static PyObject *
deque_richcompare(PyObject *v, PyObject *w, int op)
{
    PyObject *it1=NULL, *it2=NULL, *x, *y;
    Py_ssize_t vs, ws;
    int b, cmp=-1;

    collections_state *state = find_module_state_by_def(Py_TYPE(v));
    if (!PyObject_TypeCheck(v, state->deque_type) ||
        !PyObject_TypeCheck(w, state->deque_type)) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    /* Shortcuts */
    vs = Py_SIZE(v);
    ws = Py_SIZE(w);
    if (op == Py_EQ) {
        if (v == w)
            Py_RETURN_TRUE;
        if (vs != ws)
            Py_RETURN_FALSE;
    }
    if (op == Py_NE) {
        if (v == w)
            Py_RETURN_FALSE;
        if (vs != ws)
            Py_RETURN_TRUE;
    }

    /* Search for the first index where items are different */
    it1 = PyObject_GetIter(v);
    if (it1 == NULL)
        goto done;
    it2 = PyObject_GetIter(w);
    if (it2 == NULL)
        goto done;
    for (;;) {
        x = PyIter_Next(it1);
        if (x == NULL && PyErr_Occurred())
            goto done;
        y = PyIter_Next(it2);
        if (x == NULL || y == NULL)
            break;
        b = PyObject_RichCompareBool(x, y, Py_EQ);
        if (b == 0) {
            cmp = PyObject_RichCompareBool(x, y, op);
            Py_DECREF(x);
            Py_DECREF(y);
            goto done;
        }
        Py_DECREF(x);
        Py_DECREF(y);
        if (b < 0)
            goto done;
    }
    /* We reached the end of one deque or both */
    Py_XDECREF(x);
    Py_XDECREF(y);
    if (PyErr_Occurred())
        goto done;
    switch (op) {
    case Py_LT: cmp = y != NULL; break;  /* if w was longer */
    case Py_LE: cmp = x == NULL; break;  /* if v was not longer */
    case Py_EQ: cmp = x == y;    break;  /* if we reached the end of both */
    case Py_NE: cmp = x != y;    break;  /* if one deque continues */
    case Py_GT: cmp = x != NULL; break;  /* if v was longer */
    case Py_GE: cmp = y == NULL; break;  /* if w was not longer */
    }

done:
    Py_XDECREF(it1);
    Py_XDECREF(it2);
    if (cmp == 1)
        Py_RETURN_TRUE;
    if (cmp == 0)
        Py_RETURN_FALSE;
    return NULL;
}

/*[clinic input]
@critical_section
@text_signature "([iterable[, maxlen]])"
_collections.deque.__init__ as deque_init

    deque: dequeobject
    iterable: object = NULL
    maxlen as maxlenobj: object = NULL

A list-like sequence optimized for data accesses near its endpoints.
[clinic start generated code]*/

static int
deque_init_impl(dequeobject *deque, PyObject *iterable, PyObject *maxlenobj)
/*[clinic end generated code: output=7084a39d71218dcd input=2b9e37af1fd73143]*/
{
    Py_ssize_t maxlen = -1;
    if (maxlenobj != NULL && maxlenobj != Py_None) {
        maxlen = PyLong_AsSsize_t(maxlenobj);
        if (maxlen == -1 && PyErr_Occurred())
            return -1;
        if (maxlen < 0) {
            PyErr_SetString(PyExc_ValueError, "maxlen must be non-negative");
            return -1;
        }
    }
    deque->maxlen = maxlen;
    if (Py_SIZE(deque) > 0)
        (void)deque_clear((PyObject *)deque);
    if (iterable != NULL) {
        PyObject *rv = deque_extend_impl(deque, iterable);
        if (rv == NULL)
            return -1;
        Py_DECREF(rv);
    }
    return 0;
}

/*[clinic input]
@critical_section
_collections.deque.__sizeof__ as deque___sizeof__

    deque: dequeobject

Return the size of the deque in memory, in bytes.
[clinic start generated code]*/

static PyObject *
deque___sizeof___impl(dequeobject *deque)
/*[clinic end generated code: output=4d36e9fb4f30bbaf input=762312f2d4813535]*/
{
    size_t res = _PyObject_SIZE(Py_TYPE(deque));
    size_t blocks;
    blocks = (size_t)(deque->leftindex + Py_SIZE(deque) + BLOCKLEN - 1) / BLOCKLEN;
    assert(((size_t)deque->leftindex + (size_t)Py_SIZE(deque) - 1) ==
           ((blocks - 1) * BLOCKLEN + (size_t)deque->rightindex));
    res += blocks * sizeof(block);
    return PyLong_FromSize_t(res);
}

static PyObject *
deque_get_maxlen(PyObject *self, void *Py_UNUSED(closure))
{
    dequeobject *deque = dequeobject_CAST(self);
    if (deque->maxlen < 0)
        Py_RETURN_NONE;
    return PyLong_FromSsize_t(deque->maxlen);
}

static PyObject *deque_reviter(dequeobject *deque);

/*[clinic input]
_collections.deque.__reversed__ as deque___reversed__

    deque: dequeobject

Return a reverse iterator over the deque.
[clinic start generated code]*/

static PyObject *
deque___reversed___impl(dequeobject *deque)
/*[clinic end generated code: output=3e7e7e715883cf2e input=3d494c25a6fe5c7e]*/
{
    return deque_reviter(deque);
}

/* deque object ********************************************************/

static PyGetSetDef deque_getset[] = {
    {"maxlen", deque_get_maxlen, NULL,
     "maximum size of a deque or None if unbounded"},
    {0}
};

static PyObject *deque_iter(PyObject *deque);

static PyMethodDef deque_methods[] = {
    DEQUE_APPEND_METHODDEF
    DEQUE_APPENDLEFT_METHODDEF
    DEQUE_CLEARMETHOD_METHODDEF
    DEQUE___COPY___METHODDEF
    DEQUE_COPY_METHODDEF
    DEQUE_COUNT_METHODDEF
    DEQUE_EXTEND_METHODDEF
    DEQUE_EXTENDLEFT_METHODDEF
    DEQUE_INDEX_METHODDEF
    DEQUE_INSERT_METHODDEF
    DEQUE_POP_METHODDEF
    DEQUE_POPLEFT_METHODDEF
    DEQUE___REDUCE___METHODDEF
    DEQUE_REMOVE_METHODDEF
    DEQUE___REVERSED___METHODDEF
    DEQUE_REVERSE_METHODDEF
    DEQUE_ROTATE_METHODDEF
    DEQUE___SIZEOF___METHODDEF
    {"__class_getitem__",       Py_GenericAlias,
        METH_O|METH_CLASS,       PyDoc_STR("See PEP 585")},
    {NULL,              NULL}   /* sentinel */
};

static PyMemberDef deque_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(dequeobject, weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot deque_slots[] = {
    {Py_tp_dealloc, deque_dealloc},
    {Py_tp_repr, deque_repr},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_doc, (void *)deque_init__doc__},
    {Py_tp_traverse, deque_traverse},
    {Py_tp_clear, deque_clear},
    {Py_tp_richcompare, deque_richcompare},
    {Py_tp_iter, deque_iter},
    {Py_tp_getset, deque_getset},
    {Py_tp_init, deque_init},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, deque_new},
    {Py_tp_free, PyObject_GC_Del},
    {Py_tp_methods, deque_methods},
    {Py_tp_members, deque_members},

    // Sequence protocol
    {Py_sq_length, deque_len},
    {Py_sq_concat, deque_concat},
    {Py_sq_repeat, deque_repeat},
    {Py_sq_item, deque_item},
    {Py_sq_ass_item, deque_ass_item},
    {Py_sq_contains, deque_contains},
    {Py_sq_inplace_concat, deque_inplace_concat},
    {Py_sq_inplace_repeat, deque_inplace_repeat},
    {0, NULL},
};

static PyType_Spec deque_spec = {
    .name = "collections.deque",
    .basicsize = sizeof(dequeobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
              Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_SEQUENCE |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = deque_slots,
};

/*********************** Deque Iterator **************************/

typedef struct {
    PyObject_HEAD
    block *b;
    Py_ssize_t index;
    dequeobject *deque;
    size_t state;          /* state when the iterator is created */
    Py_ssize_t counter;    /* number of items remaining for iteration */
} dequeiterobject;

#define dequeiterobject_CAST(op)    ((dequeiterobject *)(op))

static PyObject *
deque_iter(PyObject *self)
{
    dequeiterobject *it;
    dequeobject *deque = dequeobject_CAST(self);

    collections_state *state = find_module_state_by_def(Py_TYPE(deque));
    it = PyObject_GC_New(dequeiterobject, state->dequeiter_type);
    if (it == NULL)
        return NULL;
    Py_BEGIN_CRITICAL_SECTION(deque);
    it->b = deque->leftblock;
    it->index = deque->leftindex;
    it->deque = (dequeobject*)Py_NewRef(deque);
    it->state = deque->state;
    it->counter = Py_SIZE(deque);
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static int
dequeiter_traverse(PyObject *op, visitproc visit, void *arg)
{
    dequeiterobject *dio = dequeiterobject_CAST(op);
    Py_VISIT(Py_TYPE(dio));
    Py_VISIT(dio->deque);
    return 0;
}

static int
dequeiter_clear(PyObject *op)
{
    dequeiterobject *dio = dequeiterobject_CAST(op);
    Py_CLEAR(dio->deque);
    return 0;
}

static void
dequeiter_dealloc(PyObject *dio)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyTypeObject *tp = Py_TYPE(dio);
    PyObject_GC_UnTrack(dio);
    (void)dequeiter_clear(dio);
    PyObject_GC_Del(dio);
    Py_DECREF(tp);
}

static PyObject *
dequeiter_next_lock_held(dequeiterobject *it, dequeobject *deque)
{
    PyObject *item;

    if (it->deque->state != it->state) {
        it->counter = 0;
        PyErr_SetString(PyExc_RuntimeError,
                        "deque mutated during iteration");
        return NULL;
    }
    if (it->counter == 0)
        return NULL;
    assert (!(it->b == it->deque->rightblock &&
              it->index > it->deque->rightindex));

    item = it->b->data[it->index];
    it->index++;
    it->counter--;
    if (it->index == BLOCKLEN && it->counter > 0) {
        CHECK_NOT_END(it->b->rightlink);
        it->b = it->b->rightlink;
        it->index = 0;
    }
    return Py_NewRef(item);
}

static PyObject *
dequeiter_next(PyObject *op)
{
    PyObject *result;
    dequeiterobject *it = dequeiterobject_CAST(op);
    // It's safe to access it->deque without holding the per-object lock for it
    // here; it->deque is only assigned during construction of it.
    dequeobject *deque = it->deque;
    Py_BEGIN_CRITICAL_SECTION2(it, deque);
    result = dequeiter_next_lock_held(it, deque);
    Py_END_CRITICAL_SECTION2();

    return result;
}

static PyObject *
dequeiter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Py_ssize_t i, index=0;
    PyObject *deque;
    dequeiterobject *it;
    collections_state *state = get_module_state_by_cls(type);
    if (!PyArg_ParseTuple(args, "O!|n", state->deque_type, &deque, &index))
        return NULL;
    assert(type == state->dequeiter_type);

    it = (dequeiterobject*)deque_iter(deque);
    if (!it)
        return NULL;
    /* consume items from the queue */
    for(i=0; i<index; i++) {
        PyObject *item = dequeiter_next((PyObject *)it);
        if (item) {
            Py_DECREF(item);
        } else {
            /*
             * It's safe to read directly from it without acquiring the
             * per-object lock; the iterator isn't visible to any other threads
             * yet.
             */
            if (it->counter) {
                Py_DECREF(it);
                return NULL;
            } else
                break;
        }
    }
    return (PyObject*)it;
}

static PyObject *
dequeiter_len(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    dequeiterobject *it = dequeiterobject_CAST(op);
    Py_ssize_t len = FT_ATOMIC_LOAD_SSIZE(it->counter);
    return PyLong_FromSsize_t(len);
}

PyDoc_STRVAR(length_hint_doc, "Private method returning an estimate of len(list(it)).");

static PyObject *
dequeiter_reduce(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    dequeiterobject *it = dequeiterobject_CAST(op);
    PyTypeObject *ty = Py_TYPE(it);
    // It's safe to access it->deque without holding the per-object lock for it
    // here; it->deque is only assigned during construction of it.
    dequeobject *deque = it->deque;
    Py_ssize_t size, counter;
    Py_BEGIN_CRITICAL_SECTION2(it, deque);
    size = Py_SIZE(deque);
    counter = it->counter;
    Py_END_CRITICAL_SECTION2();
    return Py_BuildValue("O(On)", ty, deque, size - counter);
}

static PyMethodDef dequeiter_methods[] = {
    {"__length_hint__", dequeiter_len, METH_NOARGS, length_hint_doc},
    {"__reduce__", dequeiter_reduce, METH_NOARGS, reduce_doc},
    {NULL,              NULL}           /* sentinel */
};

static PyType_Slot dequeiter_slots[] = {
    {Py_tp_dealloc, dequeiter_dealloc},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_traverse, dequeiter_traverse},
    {Py_tp_clear, dequeiter_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, dequeiter_next},
    {Py_tp_methods, dequeiter_methods},
    {Py_tp_new, dequeiter_new},
    {0, NULL},
};

static PyType_Spec dequeiter_spec = {
    .name = "collections._deque_iterator",
    .basicsize = sizeof(dequeiterobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = dequeiter_slots,
};

/*********************** Deque Reverse Iterator **************************/

static PyObject *
deque_reviter(dequeobject *deque)
{
    dequeiterobject *it;
    collections_state *state = find_module_state_by_def(Py_TYPE(deque));

    it = PyObject_GC_New(dequeiterobject, state->dequereviter_type);
    if (it == NULL)
        return NULL;
    Py_BEGIN_CRITICAL_SECTION(deque);
    it->b = deque->rightblock;
    it->index = deque->rightindex;
    it->deque = (dequeobject*)Py_NewRef(deque);
    it->state = deque->state;
    it->counter = Py_SIZE(deque);
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static PyObject *
dequereviter_next_lock_held(dequeiterobject *it, dequeobject *deque)
{
    PyObject *item;
    if (it->counter == 0)
        return NULL;

    if (it->deque->state != it->state) {
        it->counter = 0;
        PyErr_SetString(PyExc_RuntimeError,
                        "deque mutated during iteration");
        return NULL;
    }
    assert (!(it->b == it->deque->leftblock &&
              it->index < it->deque->leftindex));

    item = it->b->data[it->index];
    it->index--;
    it->counter--;
    if (it->index < 0 && it->counter > 0) {
        CHECK_NOT_END(it->b->leftlink);
        it->b = it->b->leftlink;
        it->index = BLOCKLEN - 1;
    }
    return Py_NewRef(item);
}

static PyObject *
dequereviter_next(PyObject *self)
{
    PyObject *item;
    dequeiterobject *it = dequeiterobject_CAST(self);
    // It's safe to access it->deque without holding the per-object lock for it
    // here; it->deque is only assigned during construction of it.
    dequeobject *deque = it->deque;
    Py_BEGIN_CRITICAL_SECTION2(it, deque);
    item = dequereviter_next_lock_held(it, deque);
    Py_END_CRITICAL_SECTION2();
    return item;
}

static PyObject *
dequereviter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // TODO(Matthias): check whether index!=0 is exercised anywhere at all.
    // Any tests or docs?  If not, axe it.
    Py_ssize_t i, index=0;
    PyObject *deque;
    dequeiterobject *it;
    collections_state *state = get_module_state_by_cls(type);
    if (!PyArg_ParseTuple(args, "O!|n", state->deque_type, &deque, &index))
        return NULL;
    assert(type == state->dequereviter_type);

    it = (dequeiterobject *)deque_reviter((dequeobject *)deque);
    if (!it)
        return NULL;
    /* consume items from the queue */
    for(i=0; i<index; i++) {
        PyObject *item = dequereviter_next((PyObject *)it);
        if (item) {
            Py_DECREF(item);
        } else {
            /*
             * It's safe to read directly from it without acquiring the
             * per-object lock; the iterator isn't visible to any other threads
             * yet.
             */
            if (it->counter) {
                Py_DECREF(it);
                return NULL;
            } else
                break;
        }
    }
    return (PyObject*)it;
}

static PyType_Slot dequereviter_slots[] = {
    {Py_tp_dealloc, dequeiter_dealloc},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_traverse, dequeiter_traverse},
    {Py_tp_clear, dequeiter_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, dequereviter_next},
    {Py_tp_methods, dequeiter_methods},
    {Py_tp_new, dequereviter_new},
    {0, NULL},
};

static PyType_Spec dequereviter_spec = {
    .name = "collections._deque_reverse_iterator",
    .basicsize = sizeof(dequeiterobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = dequereviter_slots,
};

/* deque type as growable ring buffer, meque *********************************************************/

// TODO(Matthias): implement meque type



/*[python input]
class mequeobject_converter(self_converter):
    type = "mequeobject *"
[python start generated code]*/
/*[python end generated code: output=da39a3ee5e6b4b0d input=76e5c03d44497c3a]*/

struct mequeobject {
    PyObject_VAR_HEAD
    /* Vector of pointers to list elements.  list[0] is ob_item[0], etc. */
    PyObject **ob_item;
    //  allocated needs to be a power of two
    Py_ssize_t allocated;
    Py_ssize_t first_element;
    Py_ssize_t maxlen;

    size_t state;               /* incremented whenever the indices move, to eg detect mutations during iteration */
};

#define mequeobject_CAST(op)    ((mequeobject *)(op))


static PyObject *
meque_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    mequeobject *meque;

    /* create dequeobject structure */
    meque = (mequeobject *)type->tp_alloc(type, 0);
    if (meque == NULL)
        return NULL;

    assert(BLOCKLEN >= 2);
    Py_SET_SIZE(meque, 0);
    meque->ob_item = NULL;
    meque->allocated = 0;
    meque->first_element = 0;
    meque->maxlen = -1;
    meque->state = 0;

    return (PyObject *)meque;
}

/*[clinic input]
@critical_section
_collections.meque.pop as meque_pop

    meque: mequeobject

Remove and return the rightmost element.
[clinic start generated code]*/

static PyObject *
meque_pop_impl(mequeobject *meque)
/*[clinic end generated code: output=21dbf03cd7259ec9 input=4b3754030e7a5478]*/
{
    // TODO(Matthias): do we need to lock anything?
    PyObject *item;
    if (Py_SIZE(meque) == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty meque");
        return NULL;
    }
    item = meque->ob_item[(meque->first_element + Py_SIZE(meque) - 1) & (meque->allocated - 1)];
    Py_SET_SIZE(meque, Py_SIZE(meque) - 1);
    meque->state++;
    // TODO(Matthias): consider shrinking the meque, if we are below some threshold.
    // Or perhaps only shrink on inserts?
    return item;
}

/*[clinic input]
@critical_section
_collections.meque.popleft as meque_popleft

     meque: mequeobject

Remove and return the leftmost element.
[clinic start generated code]*/

static PyObject *
meque_popleft_impl(mequeobject *meque)
/*[clinic end generated code: output=905cd2f73f5c42a6 input=398b4abbe7f594df]*/
{
    // TODO(Matthias): do we need to lock anything?
    PyObject *item;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    if (Py_SIZE(meque) == 0) {
        PyErr_SetString(PyExc_IndexError, "popleft from an empty meque");
        return NULL;
    }
    item = meque->ob_item[meque->first_element];
    meque->first_element = (meque->first_element + 1) & mask;
    Py_SET_SIZE(meque, Py_SIZE(meque) - 1);
    meque->state++;
    // TODO(Matthias): consider shrinking the meque, if we are below some threshold.
    // Or perhaps only shrink on inserts?
    return item;
}

static int meque_grow_ensure(mequeobject *meque, Py_ssize_t min_size)
{
    if (meque->allocated >= min_size) {
        return 0;
    }

    Py_ssize_t old_size = meque->allocated;
    Py_ssize_t old_mask = old_size - 1;
    // round up min_size to the next power of 2
    while(min_size > meque->allocated) {
        meque->allocated = (meque->allocated == 0) ? 1 : meque->allocated * 2;
    }

    PyObject **ob_item = (PyObject **)PyMem_Realloc(meque->ob_item, meque->allocated * sizeof(PyObject *));
    if(ob_item == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    meque->ob_item = ob_item;

    // Check if there's a wrap-around
    Py_ssize_t last_element = (meque->first_element + Py_SIZE(meque)) & old_mask;
    if (Py_SIZE(meque) > 1 && last_element <= meque->first_element) {
        // There's a wrap-around, copy the wrapped elements to the new space
        Py_ssize_t wrapped_size = last_element;
        memcpy(
            meque->ob_item + old_size,
            meque->ob_item,
            wrapped_size * sizeof(PyObject *));
    }
    return 0;
}

static inline int
meque_append_lock_held(mequeobject *meque, PyObject *item, Py_ssize_t maxlen)
{
    if (Py_SIZE(meque) == meque->allocated) {
        int result = meque_grow_ensure(meque, Py_SIZE(meque)+1);
        if(result != 0) {
            return result;
        }
    }
    meque->ob_item[(meque->first_element + Py_SIZE(meque)) & (meque->allocated - 1)] = item;
    Py_SET_SIZE(meque, Py_SIZE(meque) + 1);
    if (NEEDS_TRIM(meque, maxlen)) {
        PyObject *olditem = meque_pop_impl(meque);
        Py_DECREF(olditem);
    } else {
        meque->state++;
    }
    return 0;

}

/*[clinic input]
@critical_section
_collections.meque.append as meque_append

    meque: mequeobject
    item: object
    /

Add an element to the right side of the meque.
[clinic start generated code]*/

static PyObject *
meque_append_impl(mequeobject *meque, PyObject *item)
/*[clinic end generated code: output=a9bb7d97f92757c8 input=5dc707e82fab58b9]*/
{
    if (meque_append_lock_held(meque, Py_NewRef(item), meque->maxlen) < 0)
        return NULL;
    Py_RETURN_NONE;
}


static inline int
meque_appendleft_lock_held(mequeobject *meque, PyObject *item,
                           Py_ssize_t maxlen)
{
    if (Py_SIZE(meque) >= meque->allocated) {
        int result = meque_grow_ensure(meque, Py_SIZE(meque)+1);
        if(result != 0) {
            return result;
        }
    }
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    meque->first_element = (meque->first_element - 1) & mask;
    meque->ob_item[meque->first_element] = item;
    Py_SET_SIZE(meque, Py_SIZE(meque) + 1);
    if (NEEDS_TRIM(meque, maxlen)) {
        PyObject *olditem = meque_pop_impl(meque);
        Py_DECREF(olditem);
    } else {
        meque->state++;
    }
    return 0;
}


/*[clinic input]
@critical_section
_collections.meque.appendleft as meque_appendleft

    meque: mequeobject
    item: object
    /

Add an element to the left side of the meque.
[clinic start generated code]*/

static PyObject *
meque_appendleft_impl(mequeobject *meque, PyObject *item)
/*[clinic end generated code: output=6632ff0d636f208d input=d666903e8064ceb7]*/
{
    if (meque_appendleft_lock_held(meque, Py_NewRef(item), meque->maxlen) < 0)
        return NULL;
    Py_RETURN_NONE;
}


/*[clinic input]
@critical_section
_collections.meque.extend as meque_extend

    meque: mequeobject
    iterable: object
    /

Extend the right side of the deque with elements from the iterable.
[clinic start generated code]*/

static PyObject *
meque_extend_impl(mequeobject *meque, PyObject *iterable)
/*[clinic end generated code: output=f89404334ceb151f input=14167af70bc2fbf4]*/
{
    PyObject *it, *item;
    PyObject *(*iternext)(PyObject *);
    Py_ssize_t maxlen = meque->maxlen;

    /* Handle case where id(meque) == id(iterable) */
    if ((PyObject *)meque == iterable) {
        PyObject *result;
        PyObject *s = PySequence_List(iterable);
        if (s == NULL)
            return NULL;
        result = meque_extend((PyObject*)meque, s);
        Py_DECREF(s);
        return result;
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL)
        return NULL;

    if (maxlen == 0)
        return consume_iterator(it);

    iternext = *Py_TYPE(it)->tp_iternext;
    while ((item = iternext(it)) != NULL) {
        if (meque_append_lock_held(meque, item, maxlen) == -1) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
    }
    return finalize_iterator(it);
}

/*[clinic input]
@critical_section
_collections.meque.extendleft as meque_extendleft

    meque: mequeobject
    iterable: object
    /

Extend the left side of the deque with elements from the iterable.
[clinic start generated code]*/

static PyObject *
meque_extendleft_impl(mequeobject *meque, PyObject *iterable)
/*[clinic end generated code: output=b79929ea34c76705 input=aebb2457196e597c]*/
{
    PyObject *it, *item;
    PyObject *(*iternext)(PyObject *);
    Py_ssize_t maxlen = meque->maxlen;

    /* Handle case where id(meque) == id(iterable) */
    if ((PyObject *)meque == iterable) {
        PyObject *result;
        PyObject *s = PySequence_List(iterable);
        if (s == NULL)
            return NULL;
        result = meque_extendleft_impl(meque, s);
        Py_DECREF(s);
        return result;
    }

    it = PyObject_GetIter(iterable);
    if (it == NULL)
        return NULL;

    if (maxlen == 0)
        return consume_iterator(it);

    iternext = *Py_TYPE(it)->tp_iternext;
    while ((item = iternext(it)) != NULL) {
        if (meque_appendleft_lock_held(meque, item, maxlen) == -1) {
            Py_DECREF(item);
            Py_DECREF(it);
            return NULL;
        }
    }
    return finalize_iterator(it);
}

static PyObject *
meque_inplace_concat(PyObject *self, PyObject *other)
{
    mequeobject *meque = mequeobject_CAST(self);
    PyObject *result;

    // deque_extend is thread-safe
    result = meque_extend((PyObject*)meque, other);
    if (result == NULL)
        return result;
    Py_INCREF(meque);
    Py_DECREF(result);
    return (PyObject *)meque;
}

/*[clinic input]
@critical_section
_collections.meque.copy as meque_copy

    meque: mequeobject

Return a shallow copy of a deque.
[clinic start generated code]*/

static PyObject *
meque_copy_impl(mequeobject *meque)
/*[clinic end generated code: output=99b41209bacdd683 input=2baf9f303bf5ef08]*/
{
    PyObject *result;
    mequeobject *old_meque = meque;
    collections_state *state = find_module_state_by_def(Py_TYPE(meque));
    if (Py_IS_TYPE(meque, state->meque_type)) {
        mequeobject *new_meque;
        PyObject *rv;

        new_meque = (mequeobject *)meque_new(state->meque_type, NULL, NULL);
        if (new_meque == NULL)
            return NULL;
        new_meque->maxlen = old_meque->maxlen;
        /* Fast path for the deque_repeat() common case where len(deque) == 1
         *
         * It's safe to not acquire the per-object lock for new_deque; it's
         * invisible to other threads.
         */
        if (Py_SIZE(meque) == 1) {
            PyObject *item = old_meque->ob_item[old_meque->first_element];
            rv = meque_append_impl(new_meque, item);
        } else {
            rv = meque_extend_impl(new_meque, (PyObject *)meque);
        }
        if (rv != NULL) {
            Py_DECREF(rv);
            return (PyObject *)new_meque;
        }
        Py_DECREF(new_meque);
        return NULL;
    }
    if (old_meque->maxlen < 0)
        result = PyObject_CallOneArg((PyObject *)(Py_TYPE(meque)),
                                     (PyObject *)meque);
    else
        result = PyObject_CallFunction((PyObject *)(Py_TYPE(meque)), "Oi",
                                       meque, old_meque->maxlen, NULL);
    if (result != NULL && !PyObject_TypeCheck(result, state->meque_type)) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s() must return a meque, not %.200s",
                     Py_TYPE(meque)->tp_name, Py_TYPE(result)->tp_name);
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

/*[clinic input]
@critical_section
_collections.meque.__copy__ as meque___copy__ = _collections.meque.copy

Return a shallow copy of a deque.
[clinic start generated code]*/

static PyObject *
meque___copy___impl(mequeobject *meque)
/*[clinic end generated code: output=c69b84a6d52873ea input=674df1beccbf86da]*/
{
    return meque_copy_impl(meque);
}

static PyObject *
meque_concat_lock_held(mequeobject *meque, PyObject *other)
{
    PyObject *new_meque, *result;
    int rv;

    collections_state *state = find_module_state_by_def(Py_TYPE(meque));
    rv = PyObject_IsInstance(other, (PyObject *)state->meque_type);
    if (rv <= 0) {
        if (rv == 0) {
            PyErr_Format(PyExc_TypeError,
                         "can only concatenate meque (not \"%.200s\") to meque",
                         Py_TYPE(other)->tp_name);
        }
        return NULL;
    }

    new_meque = meque_copy_impl(meque);
    if (new_meque == NULL)
        return NULL;

    // It's safe to not acquire the per-object lock for new_deque; it's
    // invisible to other threads.
    result = meque_extend_impl((mequeobject *)new_meque, other);
    if (result == NULL) {
        Py_DECREF(new_meque);
        return NULL;
    }
    Py_DECREF(result);
    return new_meque;
}

static PyObject *
meque_concat(PyObject *self, PyObject *other)
{
    mequeobject *meque = mequeobject_CAST(self);
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(meque);
    result = meque_concat_lock_held(meque, other);
    Py_END_CRITICAL_SECTION();
    return result;
}

static int
meque_clear(PyObject *self)
{
    mequeobject *meque = mequeobject_CAST(self);
    if (meque->ob_item == NULL)
        return 0;                         /* already cleared */

    /* decref all elements */
    Py_ssize_t n    = Py_SIZE(meque);
    Py_ssize_t mask = meque->allocated - 1;
    Py_ssize_t idx  = meque->first_element;

    for (Py_ssize_t k = 0; k < n; k++, idx++) {
        Py_CLEAR(meque->ob_item[idx & mask]);      /* safe even if element NULL */
    }

    /* reset header */
    PyMem_Free(meque->ob_item);
    meque->ob_item       = NULL;
    meque->allocated     = 0;
    meque->first_element = 0;
    Py_SET_SIZE(meque, 0);
    meque->state++;

    return 0;
}

/*[clinic input]
@critical_section
_collections.meque.clear as meque_clearmethod

    meque: mequeobject

Remove all elements from the deque.
[clinic start generated code]*/

static PyObject *
meque_clearmethod_impl(mequeobject *meque)
/*[clinic end generated code: output=7dcd0c6af389ba2d input=0614792bc7ccd074]*/
{
    (void)meque_clear((PyObject *)meque);
    Py_RETURN_NONE;
}

// TODO(Matthias): I don't think this works.
static PyObject *
meque_inplace_repeat_lock_held(mequeobject *meque, Py_ssize_t n)
{
    Py_ssize_t input_size = Py_SIZE(meque);
    if (input_size == 0 || n == 1) {
        return Py_NewRef(meque);
    }

    if (n < 1) {
        meque_clear((PyObject *)meque);
        return Py_NewRef(meque);
    }

    if (input_size > PY_SSIZE_T_MAX / n) {
        PyErr_NoMemory();
        return NULL;
    }
    Py_ssize_t output_size = input_size * n;

    // First resize the meque to accommodate the repeated elements
    if (meque_grow_ensure(meque, output_size) < 0) {
        return NULL;
    }

    // Copy the elements to their new positions
    PyObject **items = meque->ob_item;
    Py_ssize_t first = meque->first_element;
    Py_ssize_t allocated = meque->allocated;
    Py_ssize_t mask = allocated - 1;  // Since allocated is a power of 2

    // First increment refcounts for all elements
    for (Py_ssize_t j = 0; j < input_size; j++) {
        Py_ssize_t idx = (first + j) & mask;
        _Py_RefcntAdd(items[idx], n-1);
    }

    // Check if original sequence wraps around
    Py_ssize_t original_end = (first + input_size) & mask;
    if (original_end <= first) {
        // Original sequence wraps around - create a clean copy first
        Py_ssize_t first_part = allocated - first;
        Py_ssize_t second_part = input_size - first_part;

        // Copy first part to the first copy position
        memcpy(&items[first & mask], &items[first], first_part * sizeof(PyObject *));
        // Copy second part
        memcpy(&items[(first & mask) + first_part], items, second_part * sizeof(PyObject *));

        // Now we can copy from this clean copy for all remaining copies
        for (Py_ssize_t i = 1; i < n; i++) {
            memcpy(&items[(first + i * input_size) & mask],
                   &items[first & mask],
                   input_size * sizeof(PyObject *));
        }
    } else {
        // Original sequence doesn't wrap - copy each sequence, handling wrap if it occurs
        for (Py_ssize_t i = 1; i < n; i++) {
            Py_ssize_t copy_start = (first + i * input_size) & mask;
            Py_ssize_t copy_end = (copy_start + input_size) & mask;

            if (copy_end < copy_start) {
                // This copy wraps - handle it in two parts
                Py_ssize_t first_part = allocated - copy_start;
                Py_ssize_t second_part = input_size - first_part;
                memcpy(&items[copy_start], &items[first & mask], first_part * sizeof(PyObject *));
                memcpy(items, &items[(first & mask) + first_part], second_part * sizeof(PyObject *));
            } else {
                // This copy doesn't wrap - do it in one go
                memcpy(&items[copy_start], &items[first & mask], input_size * sizeof(PyObject *));
            }
        }
    }
    Py_SET_SIZE(meque, output_size);
    return Py_NewRef(meque);
}

static PyObject *
meque_inplace_repeat(PyObject *self, Py_ssize_t n)
{
    mequeobject *meque = mequeobject_CAST(self);
    PyObject *result;
    Py_BEGIN_CRITICAL_SECTION(meque);
    result = meque_inplace_repeat_lock_held(meque, n);
    Py_END_CRITICAL_SECTION();
    return result;
}

static PyObject *
meque_repeat(PyObject *self, Py_ssize_t n)
{
    mequeobject *meque = mequeobject_CAST(self);
    mequeobject *new_meque;
    PyObject *rv;

    Py_BEGIN_CRITICAL_SECTION(meque);
    new_meque = (mequeobject *)meque_copy_impl(meque);
    Py_END_CRITICAL_SECTION();
    if (new_meque == NULL)
        return NULL;
    // It's safe to not acquire the per-object lock for new_deque; it's
    // invisible to other threads.
    rv = meque_inplace_repeat_lock_held(new_meque, n);
    Py_DECREF(new_meque);
    return rv;
}

static int
_meque_rotate(mequeobject *meque, Py_ssize_t n)
{
    Py_ssize_t len = Py_SIZE(meque);
    if (len <= 1)
        return 0;

    // Normalize n to be in range [0, len)
    n = n & (len - 1);  // Since len is a power of 2
    if (n == 0)
        return 0;

    // If n is more than half the length, rotate in the opposite direction
    if (n > (len >> 1)) {
        n = n - len;
    }

    PyObject **items = meque->ob_item;
    Py_ssize_t first = meque->first_element;
    Py_ssize_t allocated = meque->allocated;
    Py_ssize_t mask = allocated - 1;  // Since allocated is a power of 2

    // For positive rotation, we move elements from end to beginning
    if (n > 0) {
        Py_ssize_t move_size = len - n;
        Py_ssize_t src_start = (first + n) & mask;
        Py_ssize_t dst_start = first;

        // Check if we need to handle wrap-around
        if (src_start + move_size > allocated) {
            // First part: copy from src_start to end of buffer
            Py_ssize_t first_part = allocated - src_start;
            memmove(&items[dst_start], &items[src_start], first_part * sizeof(PyObject *));

            // Second part: copy from start of buffer
            memmove(&items[dst_start + first_part], items, (move_size - first_part) * sizeof(PyObject *));
        } else {
            // No wrap-around, single memmove
            memmove(&items[dst_start], &items[src_start], move_size * sizeof(PyObject *));
        }

        meque->first_element = (first + n) & mask;
    }
    // For negative rotation, we move elements from beginning to end
    else {
        Py_ssize_t move_size = len + n;  // n is negative here
        Py_ssize_t src_start = first;
        Py_ssize_t dst_start = (first + n) & mask;

        // Check if we need to handle wrap-around
        if (dst_start + move_size > allocated) {
            // First part: copy from src_start to end of buffer
            Py_ssize_t first_part = allocated - dst_start;
            memmove(&items[dst_start], &items[src_start], first_part * sizeof(PyObject *));

            // Second part: copy from start of buffer
            memmove(items, &items[src_start + first_part], (move_size - first_part) * sizeof(PyObject *));
        } else {
            // No wrap-around, single memmove
            memmove(&items[dst_start], &items[src_start], move_size * sizeof(PyObject *));
        }

        meque->first_element = (first + n) & mask;
    }

    meque->state++;
    return 0;
}

/*[clinic input]
@critical_section
_collections.meque.rotate as meque_rotate

    meque: mequeobject
    n: Py_ssize_t = 1
    /

Rotate the deque n steps to the right.  If n is negative, rotates left.
[clinic start generated code]*/

static PyObject *
meque_rotate_impl(mequeobject *meque, Py_ssize_t n)
/*[clinic end generated code: output=52a1cc399c5ebb4c input=3620ef39b86db333]*/
{
    if (!_meque_rotate(meque, n))
        Py_RETURN_NONE;
    return NULL;
}

/*[clinic input]
@critical_section
_collections.meque.reverse as meque_reverse

    meque: mequeobject

Reverse *IN PLACE*.
[clinic start generated code]*/

static PyObject *
meque_reverse_impl(mequeobject *meque)
/*[clinic end generated code: output=00c980c2260afe80 input=a7e7a017ad96ebe5]*/
{
    Py_ssize_t size = Py_SIZE(meque);
    if (size <= 1) {
        Py_RETURN_NONE;
    }

    Py_ssize_t first = meque->first_element;
    Py_ssize_t allocated = meque->allocated;
    Py_ssize_t mask = allocated - 1;
    PyObject **items = meque->ob_item;

    // Calculate the last element's position
    Py_ssize_t last = (first + size - 1) & mask;

    // Swap elements from both ends until we meet in the middle
    Py_ssize_t count = size / 2;
    while (count--) {
        // Swap elements
        PyObject *temp = items[first];
        items[first] = items[last];
        items[last] = temp;

        // Move indices, handling wrap-around
        first = (first + 1) & mask;
        last = (last - 1) & mask;
    }

    meque->state++;
    Py_RETURN_NONE;
}

/*[clinic input]
@critical_section
_collections.meque.count as meque_count

    meque: mequeobject
    value as v: object
    /

Return number of occurrences of value.
[clinic start generated code]*/

static PyObject *
meque_count_impl(mequeobject *meque, PyObject *v)
/*[clinic end generated code: output=ac507a140ec341d2 input=c30e8101d20eeded]*/
{
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t count = 0;
    size_t start_state = meque->state;
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        item = Py_NewRef(items[first]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp < 0)
            return NULL;
        count += cmp;

        if (start_state != meque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "meque mutated during iteration");
            return NULL;
        }

        /* Advance to next element, handling wrap-around */
        first = (first + 1) & mask;
    }
    return PyLong_FromSsize_t(count);
}

static int
meque_contains_lock_held(mequeobject *meque, PyObject *v)
{
    Py_ssize_t n = Py_SIZE(meque);
    size_t start_state = meque->state;
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        item = Py_NewRef(items[first]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp) {
            return cmp;
        }
        if (start_state != meque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "meque mutated during iteration");
            return -1;
        }
        first = (first + 1) & mask;
    }
    return 0;
}

static int
meque_contains(PyObject *self, PyObject *v)
{
    mequeobject *meque = mequeobject_CAST(self);
    int result;
    Py_BEGIN_CRITICAL_SECTION(meque);
    result = meque_contains_lock_held(meque, v);
    Py_END_CRITICAL_SECTION();
    return result;
}

static Py_ssize_t
meque_len(PyObject *self)
{
    PyVarObject *deque = _PyVarObject_CAST(self);
    return FT_ATOMIC_LOAD_SSIZE(deque->ob_size);
}

/*[clinic input]
@critical_section
@text_signature "($self, value, [start, [stop]])"
_collections.meque.index as meque_index

    meque: mequeobject
    value as v: object
    start: object(converter='_PyEval_SliceIndexNotNone', type='Py_ssize_t', c_default='0') = NULL
    stop: object(converter='_PyEval_SliceIndexNotNone', type='Py_ssize_t', c_default='Py_SIZE(meque)') = NULL
    /

Return first index of value.

Raises ValueError if the value is not present.
[clinic start generated code]*/

static PyObject *
meque_index_impl(mequeobject *meque, PyObject *v, Py_ssize_t start,
                 Py_ssize_t stop)
/*[clinic end generated code: output=1a3b286ab205e455 input=df4c4403279a0cc6]*/
{
    Py_ssize_t n;
    PyObject *item;
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;
    size_t start_state = meque->state;
    int cmp;

    if (start < 0) {
        start += Py_SIZE(meque);
        if (start < 0)
            start = 0;
    }
    if (stop < 0) {
        stop += Py_SIZE(meque);
        if (stop < 0)
            stop = 0;
    }
    if (stop > Py_SIZE(meque))
        stop = Py_SIZE(meque);
    if (start > stop)
        start = stop;
    assert(0 <= start && start <= stop && stop <= Py_SIZE(meque));

    // Calculate the current position in the ring buffer
    Py_ssize_t current_item = (first + start) & mask;

    n = stop - start;
    while (--n >= 0) {
        item = Py_NewRef(items[current_item]);
        cmp = PyObject_RichCompareBool(item, v, Py_EQ);
        Py_DECREF(item);
        if (cmp > 0)
            return PyLong_FromSsize_t(stop - n - 1);
        if (cmp < 0)
            return NULL;
        if (start_state != meque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "meque mutated during iteration");
            return NULL;
        }
        current_item = (current_item + 1) & mask;
    }
    PyErr_SetString(PyExc_ValueError, "meque.index(x): x not in meque");
    return NULL;
}

/*[clinic input]
@critical_section
_collections.meque.insert as meque_insert

    meque: mequeobject
    index: Py_ssize_t
    value: object
    /

Insert value before index.
[clinic start generated code]*/

static PyObject *
meque_insert_impl(mequeobject *meque, Py_ssize_t index, PyObject *value)
/*[clinic end generated code: output=d13d06f5fb3524ba input=789a37c70b173309]*/
{
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;

    // Handle negative indices
    if (index < 0) {
        index += n;
        if (index < 0)
            index = 0;
    }
    if (index > n)
        index = n;

    // Check if we need to grow the meque
    if (n == meque->allocated) {
        if (meque_grow_ensure(meque, Py_SIZE(meque)+1) < 0)
            return NULL;
        items = meque->ob_item;  // Update items pointer after potential realloc
    }

    // Calculate the insertion point in the ring buffer
    Py_ssize_t insert_pos = (first + index) & mask;

    // Determine direction based on which end is closer
    Py_ssize_t distance = index - (n >> 1);

    // If distance is negative, we're closer to the left end
    if (distance < 0) {
        // Check if we need to handle wrap-around
        if (first > insert_pos) {
            // Two memmoves needed due to wrap-around
            memmove(&items[0], &items[1], insert_pos * sizeof(PyObject *));
            // Copy the element at the boundary
            items[0] = items[meque->allocated - 1];
            memmove(&items[first], &items[first + 1], (meque->allocated - first - 1) * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[first], &items[first + 1], (insert_pos - first) * sizeof(PyObject *));
        }
        meque->first_element = (first - 1) & mask;
    }
    // If distance is positive or zero, we're closer to the right end
    else {
        // Check if we need to handle wrap-around
        Py_ssize_t last = (first + n - 1) & mask;
        if (insert_pos > last) {
            // Two memmoves needed due to wrap-around
            memmove(&items[insert_pos + 1], &items[insert_pos], (meque->allocated - insert_pos - 1) * sizeof(PyObject *));
            // Copy the element at the boundary
            items[meque->allocated - 1] = items[0];
            memmove(&items[1], &items[0], last * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[insert_pos + 1], &items[insert_pos], (last - insert_pos) * sizeof(PyObject *));
        }
    }

    // Insert the new value
    items[insert_pos] = Py_NewRef(value);
    Py_SET_SIZE(meque, n + 1);
    meque->state++;
    Py_RETURN_NONE;
}


static PyObject *
meque_item_lock_held(mequeobject *meque, Py_ssize_t i)
{
    // TODO(Matthias): check whether we should support negative indices the usual way?
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "meque index out of range");
        return NULL;
    }

    // Calculate the actual position in the ring buffer
    Py_ssize_t pos = (first + i) & mask;
    return Py_NewRef(items[pos]);
}

static PyObject *
meque_item(PyObject *self, Py_ssize_t i)
{
    mequeobject *meque = mequeobject_CAST(self);
    PyObject *result;
    result = meque_item_lock_held(meque, i);
    return result;
}

static int
meque_del_item(mequeobject *meque, Py_ssize_t i)
{
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "meque index out of range");
        return -1;
    }

    // Calculate the actual position in the ring buffer
    Py_ssize_t pos = (first + i) & mask;

    // Determine direction based on which end is closer
    Py_ssize_t distance = i - (n >> 1);

    // If distance is negative, we're closer to the left end
    if (distance < 0) {
        // Check if we need to handle wrap-around
        if (first > pos) {
            // Two memmoves needed due to wrap-around
            memmove(&items[1], &items[0], pos * sizeof(PyObject *));
            // Copy the element at the boundary
            items[0] = items[meque->allocated - 1];
            memmove(&items[first + 1], &items[first], (meque->allocated - first - 1) * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[first + 1], &items[first], (pos - first) * sizeof(PyObject *));
        }
        meque->first_element = (first + 1) & mask;
    }
    // If distance is positive or zero, we're closer to the right end
    else {
        // Check if we need to handle wrap-around
        Py_ssize_t last = (first + n - 1) & mask;
        if (pos > last) {
            // Two memmoves needed due to wrap-around
            memmove(&items[pos], &items[pos + 1], (meque->allocated - pos - 1) * sizeof(PyObject *));
            // Copy the element at the boundary
            items[meque->allocated - 1] = items[0];
            memmove(&items[0], &items[1], last * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[pos], &items[pos + 1], (last - pos) * sizeof(PyObject *));
        }
    }

    // Clear the last element and update size
    items[(first + n - 1) & mask] = NULL;
    Py_SET_SIZE(meque, n - 1);
    meque->state++;
    return 0;
}

/*[clinic input]
@critical_section
_collections.meque.remove as meque_remove

    meque: mequeobject
    value: object
    /

Remove first occurrence of value.
[clinic start generated code]*/

static PyObject *
meque_remove_impl(mequeobject *meque, PyObject *value)
/*[clinic end generated code: output=dcd984454051c535 input=3f0ab03deeda57a0]*/
{
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;
    Py_ssize_t i;
    int cmp;

    for (i = 0; i < n; i++) {
        PyObject *item = items[(first + i) & mask];
        cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        if (cmp > 0) {
            if (meque_del_item(meque, i) < 0)
                return NULL;
            Py_RETURN_NONE;
        }
        if (cmp < 0)
            return NULL;
    }
    PyErr_SetString(PyExc_ValueError, "meque.remove(x): x not in meque");
    return NULL;
}

static int
meque_ass_item_lock_held(mequeobject *meque, Py_ssize_t i, PyObject *v)
{
    Py_ssize_t n = Py_SIZE(meque);
    Py_ssize_t first = meque->first_element;
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = meque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "meque index out of range");
        return -1;
    }
    if (v == NULL) {
        // Delete item
        return meque_del_item(meque, i);
    }
    // Set item
    Py_ssize_t pos = (first + i) & mask;
    Py_SETREF(items[pos], Py_NewRef(v));
    return 0;
}

static int
meque_ass_item(PyObject *self, Py_ssize_t i, PyObject *v)
{
    mequeobject *meque = mequeobject_CAST(self);
    int result;
    Py_BEGIN_CRITICAL_SECTION(meque);
    result = meque_ass_item_lock_held(meque, i, v);
    Py_END_CRITICAL_SECTION();
    return result;
}

static void
meque_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);      /* stop GC from revisiting us   */
    meque_clear(self);              /* drop elements (once only)    */
    PyMem_Free(mequeobject_CAST(self)->ob_item);      /* free the ring buffer         */
    Py_TYPE(self)->tp_free(self);
}

static int
meque_traverse(PyObject *self, visitproc visit, void *arg)
{
    mequeobject *meque = mequeobject_CAST(self);

    Py_ssize_t n = Py_SIZE(meque);

    PyObject **buf = meque->ob_item;
    Py_ssize_t mask = meque->allocated - 1;
    Py_ssize_t i = meque->first_element;
    for (Py_ssize_t k = 0; k < n; k++) {
        Py_VISIT(buf[i]);
        i = (i + 1) & mask;
    }
    return 0;
}

/*[clinic input]
_collections.meque.__reduce__ as meque___reduce__

    meque: mequeobject

Return state information for pickling.
[clinic start generated code]*/

static PyObject *
meque___reduce___impl(mequeobject *meque)
/*[clinic end generated code: output=8042f666273ce10c input=0f6b8eccd4efa664]*/
{
    PyObject *state, *it;

    state = _PyObject_GetState((PyObject *)meque);
    if (state == NULL) {
        return NULL;
    }

    it = PyObject_GetIter((PyObject *)meque);
    if (it == NULL) {
        Py_DECREF(state);
        return NULL;
    }

    // It's safe to access meque->maxlen here without holding the per object
    // lock; meque->maxlen is only assigned during construction.
    if (meque->maxlen < 0) {
        return Py_BuildValue("O()NN", Py_TYPE(meque), state, it);
    }
    else {
        return Py_BuildValue("O(()n)NN", Py_TYPE(meque), meque->maxlen, state, it);
    }
}

PyDoc_STRVAR(meque_reduce_doc, "Return state information for pickling.");


static PyObject *
meque_repr(PyObject *meque)
{
    PyObject *aslist, *result;
    int i;

    i = Py_ReprEnter(meque);
    if (i != 0) {
        if (i < 0)
            return NULL;
        return PyUnicode_FromString("[...]");
    }

    aslist = PySequence_List(meque);
    if (aslist == NULL) {
        Py_ReprLeave(meque);
        return NULL;
    }

    Py_ssize_t first      = mequeobject_CAST(meque)->first_element;
    Py_ssize_t size       = Py_SIZE(meque);
    Py_ssize_t allocated  = mequeobject_CAST(meque)->allocated;
    PyObject   *prefix    = PyUnicode_FromFormat("<%zd, %zd, %zd>",
                                                 first, size, allocated);

    Py_ssize_t maxlen = mequeobject_CAST(meque)->maxlen;
    if (maxlen >= 0)
        result = PyUnicode_FromFormat("%s%U(%R, maxlen=%zd)",
                                    _PyType_Name(Py_TYPE(meque)), prefix, aslist,
                                    maxlen);
    else
        result = PyUnicode_FromFormat("%s%U(%R)",
                                    _PyType_Name(Py_TYPE(meque)), prefix, aslist);
    Py_ReprLeave(meque);
    Py_DECREF(aslist);
    Py_DECREF(prefix);
    return result;
}

static PyObject *
meque_richcompare(PyObject *v, PyObject *w, int op)
{
    mequeobject *meque1 = mequeobject_CAST(v);
    mequeobject *meque2 = mequeobject_CAST(w);
    PyObject *result;
    Py_ssize_t i, len1, len2;
    PyObject *item1, *item2;

    if (!PyObject_TypeCheck(w, Py_TYPE(v))) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    len1 = Py_SIZE(meque1);
    len2 = Py_SIZE(meque2);

    if (len1 != len2 && (op == Py_EQ || op == Py_NE)) {
        /* Shortcut: if the lengths differ, the deques are different */
        if (op == Py_EQ)
            Py_RETURN_FALSE;
        else
            Py_RETURN_TRUE;
    }
    /* Compare elements one by one, taking wrap-around into account */
    for (i = 0; i < len1 && i < len2; i++) {
        /* Get items using wrap-around indices */
        item1 = meque_item_lock_held(meque1, i);
        if (item1 == NULL) {
            return NULL;
        }
        item2 = meque_item_lock_held(meque2, i);
        if (item2 == NULL) {
            Py_DECREF(item1);
            return NULL;
        }

        result = PyObject_RichCompare(item1, item2, op);
        Py_DECREF(item1);
        Py_DECREF(item2);
        if (result == NULL) {
            return NULL;
        }
        if (result != Py_True) {
            return result;
        }
        Py_DECREF(result);
    }

    /* We reached the end of one deque */
    switch (op) {
    case Py_LT: return PyBool_FromLong(len1 < len2);
    case Py_LE: return PyBool_FromLong(len1 <= len2);
    case Py_EQ: return PyBool_FromLong(len1 == len2);
    case Py_NE: return PyBool_FromLong(len1 != len2);
    case Py_GT: return PyBool_FromLong(len1 > len2);
    case Py_GE: return PyBool_FromLong(len1 >= len2);
    default:
        Py_UNREACHABLE();
    }
}

/*[clinic input]
@critical_section
@text_signature "([iterable[, maxlen]])"
_collections.meque.__init__ as meque_init

    meque: mequeobject
    iterable: object = NULL
    maxlen as maxlenobj: object = NULL

A list-like sequence optimized for data accesses near its endpoints.
[clinic start generated code]*/

static int
meque_init_impl(mequeobject *meque, PyObject *iterable, PyObject *maxlenobj)
/*[clinic end generated code: output=5f992db11a7a9efb input=5db0d29e62358ce6]*/
{
    Py_ssize_t maxlen = -1;
    PyObject *it = NULL;
    PyObject *item;

    if (maxlenobj != NULL && maxlenobj != Py_None) {
        maxlen = PyLong_AsSsize_t(maxlenobj);
        if (maxlen == -1 && PyErr_Occurred())
            return -1;
        if (maxlen < 0) {
            PyErr_SetString(PyExc_ValueError, "maxlen must be non-negative");
            return -1;
        }
    }
    meque->maxlen = maxlen;

    /* Initialize the ring buffer */
    // TODO(Matthias): ok, here's where we set up to start with 8!!!
    meque->allocated = 0;  // Start with a power of 2
    meque->ob_item = NULL;
    // if (meque->ob_item == NULL) {
    //     PyErr_NoMemory();
    //     return -1;
    // }
    meque->first_element = 0;
    Py_SET_SIZE(meque, 0);

    if (iterable != NULL) {
        it = PyObject_GetIter(iterable);
        if (it == NULL)
            return -1;

        while ((item = PyIter_Next(it)) != NULL) {
            if (meque_append_lock_held(meque, item, maxlen) < 0) {
                Py_DECREF(item);
                Py_DECREF(it);
                return -1;
            }
            Py_DECREF(item);
        }
        Py_DECREF(it);
        if (PyErr_Occurred())
            return -1;
    }
    return 0;
}

/*[clinic input]
@critical_section
_collections.meque.__sizeof__ as meque___sizeof__

    meque: mequeobject

Return the size of the deque in memory, in bytes.
[clinic start generated code]*/

static PyObject *
meque___sizeof___impl(mequeobject *meque)
/*[clinic end generated code: output=7b0bb2d9fca03e84 input=62bbff6edf9fff7c]*/
{
    size_t res = _PyObject_SIZE(Py_TYPE(meque));
    res += meque->allocated * sizeof(PyObject *);
    return PyLong_FromSize_t(res);
}

static PyObject *
meque_get_maxlen(PyObject *self, void *Py_UNUSED(closure))
{
    mequeobject *meque = mequeobject_CAST(self);
    return PyLong_FromSsize_t(meque->maxlen);
}

static PyObject *meque_reviter(mequeobject *meque);

/*[clinic input]
_collections.meque.__reversed__ as meque___reversed__

    meque: mequeobject

Return a reverse iterator over the deque.
[clinic start generated code]*/

static PyObject *
meque___reversed___impl(mequeobject *meque)
/*[clinic end generated code: output=7e103da707cdfc4a input=57517d54f065671a]*/
{
    return meque_reviter(meque);
}

/* meque object ********************************************************/

static PyGetSetDef meque_getset[] = {
    {"maxlen", meque_get_maxlen, NULL,
     "maximum size of a deque or None if unbounded"},
    {0}
};


static PyObject *meque_iter(PyObject *deque);

static PyMethodDef meque_methods[] = {
    MEQUE_APPEND_METHODDEF
    MEQUE_APPENDLEFT_METHODDEF
    MEQUE_CLEARMETHOD_METHODDEF
    MEQUE___COPY___METHODDEF
    MEQUE_COPY_METHODDEF
    MEQUE_COUNT_METHODDEF
    MEQUE_EXTEND_METHODDEF
    MEQUE_EXTENDLEFT_METHODDEF
    MEQUE_INDEX_METHODDEF
    MEQUE_INSERT_METHODDEF
    MEQUE_POP_METHODDEF
    MEQUE_POPLEFT_METHODDEF
    MEQUE___REDUCE___METHODDEF
    MEQUE_REMOVE_METHODDEF
    MEQUE___REVERSED___METHODDEF
    MEQUE_REVERSE_METHODDEF
    MEQUE_ROTATE_METHODDEF
    MEQUE___SIZEOF___METHODDEF
    {"__class_getitem__",       Py_GenericAlias,
        METH_O|METH_CLASS,       PyDoc_STR("See PEP <placeholder>")},
    {NULL,              NULL}   /* sentinel */
};

static PyType_Slot meque_slots[] = {
    {Py_tp_dealloc, meque_dealloc},
    {Py_tp_repr, meque_repr},
    {Py_tp_hash, PyObject_HashNotImplemented},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_doc, (void *)meque_init__doc__},
    {Py_tp_traverse, meque_traverse},
    {Py_tp_clear, meque_clear},
    {Py_tp_richcompare, meque_richcompare},
    {Py_tp_iter, meque_iter},
    {Py_tp_getset, meque_getset},
    {Py_tp_init, meque_init},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, meque_new},
    {Py_tp_free, PyObject_GC_Del},
    {Py_tp_methods, meque_methods},
    // {Py_tp_members, meque_members},
    /* No members needed */

    // Sequence protocol
    {Py_sq_length, meque_len},
    {Py_sq_concat, meque_concat},
    {Py_sq_repeat, meque_repeat},
    {Py_sq_item, meque_item},
    {Py_sq_ass_item, meque_ass_item},
    {Py_sq_contains, meque_contains},
    {Py_sq_inplace_concat, meque_inplace_concat},
    {Py_sq_inplace_repeat, meque_inplace_repeat},
    {0, NULL},
};



static PyType_Spec meque_spec = {
    .name = "collections.meque",
    .basicsize = sizeof(mequeobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
              Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_SEQUENCE |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = meque_slots,
};

/*********************** Meque Iterator **************************/

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    mequeobject *meque;
    size_t state;          /* state when the iterator is created */
} mequeiterobject;

#define mequeiterobject_CAST(op)    ((mequeiterobject *)(op))

static PyObject *
meque_iter(PyObject *self)
{
    mequeiterobject *it;
    mequeobject *meque = mequeobject_CAST(self);

    collections_state *state = find_module_state_by_def(Py_TYPE(meque));
    it = PyObject_GC_New(mequeiterobject, state->mequeiter_type);
    if (it == NULL)
        return NULL;
    Py_BEGIN_CRITICAL_SECTION(meque);
    it->index = 0;
    it->meque = (mequeobject*)Py_NewRef(meque);
    it->state = meque->state;
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static int
mequeiter_traverse(PyObject *op, visitproc visit, void *arg)
{
    mequeiterobject *mio = mequeiterobject_CAST(op);
    Py_VISIT(Py_TYPE(mio));
    Py_VISIT(mio->meque);
    return 0;
}

static int
mequeiter_clear(PyObject *op)
{
    mequeiterobject *mio = mequeiterobject_CAST(op);
    Py_CLEAR(mio->meque);
    return 0;
}

static void
mequeiter_dealloc(PyObject *mio)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyTypeObject *tp = Py_TYPE(mio);
    PyObject_GC_UnTrack(mio);
    (void)mequeiter_clear(mio);
    PyObject_GC_Del(mio);
    Py_DECREF(tp);
}

static PyObject *
mequeiter_next_lock_held(mequeiterobject *it, mequeobject *meque)
{
    PyObject *item;
    if (it->meque->ob_item == NULL) {
        return NULL;
    }

    if (it->meque->state != it->state) {
        PyErr_SetString(PyExc_RuntimeError,
                        "meque mutated during iteration");
        return NULL;
    }
    Py_ssize_t allocated = it->meque->allocated;
    Py_ssize_t mask = allocated - 1;  // Since allocated is a power of 2
    if (it->index >= Py_SIZE(meque)) {
        return NULL;
    }

    item = meque->ob_item[(it->index + it->meque->first_element) & mask];
    it->index++;
    return Py_NewRef(item);
}

static PyObject *
mequeiter_next(PyObject *op)
{
    PyObject *result;
    mequeiterobject *it = mequeiterobject_CAST(op);
    Py_BEGIN_CRITICAL_SECTION2(it, it->meque);
    result = mequeiter_next_lock_held(it, it->meque);
    Py_END_CRITICAL_SECTION2();
    // TODO(Matthias): is this needed?
    // if (result == NULL) {
    //     Py_DECREF(it);
    // }
    return result;
}

static PyObject *
mequeiter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    Py_ssize_t i, index=0;
    PyObject *meque;
    mequeiterobject *it;
    collections_state *state = get_module_state_by_cls(type);
    if (!PyArg_ParseTuple(args, "O!|n", state->meque_type, &meque, &index))
        return NULL;
    assert(type == state->mequeiter_type);

    it = (mequeiterobject*)meque_iter(meque);
    if (!it)
        return NULL;
    /* consume items from the queue */
    for(i=0; i<index; i++) {
        PyObject *item = mequeiter_next((PyObject *)it);
        if (item) {
            Py_DECREF(item);
        } else {
            /*
             * It's safe to read directly from it without acquiring the
             * per-object lock; the iterator isn't visible to any other threads
             * yet.
             */
            if (it->index < mequeobject_CAST(meque)->allocated) {
                Py_DECREF(it);
                return NULL;
            } else
                break;
        }
    }
    return (PyObject*)it;
}

static PyObject *
mequeiter_len(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    mequeiterobject *it = mequeiterobject_CAST(op);
    return PyLong_FromSsize_t(it->meque->allocated - it->index);
}

PyDoc_STRVAR(mequeiter_len_doc, "Private method returning an estimate of len(list(it)).");

static PyObject *
mequeiter_reduce(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    mequeiterobject *it = mequeiterobject_CAST(op);
    PyTypeObject *ty = Py_TYPE(it);
    // It's safe to access it->meque without holding the per-object lock for it
    // here; it->meque is only assigned during construction of it.
    mequeobject *meque = it->meque;
    Py_ssize_t allocated, index;
    Py_BEGIN_CRITICAL_SECTION2(it, meque);
    allocated = meque->allocated;
    index = it->index;
    Py_END_CRITICAL_SECTION2();
    return Py_BuildValue("O(On)", ty, meque, allocated - index);
}

static PyMethodDef mequeiter_methods[] = {
    {"__length_hint__", mequeiter_len, METH_NOARGS, mequeiter_len_doc},
    {"__reduce__", mequeiter_reduce, METH_NOARGS, meque_reduce_doc},
    {NULL,              NULL}           /* sentinel */
};


static PyType_Slot mequeiter_slots[] = {
    {Py_tp_dealloc, mequeiter_dealloc},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_traverse, mequeiter_traverse},
    {Py_tp_clear, mequeiter_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, mequeiter_next},
    {Py_tp_methods, mequeiter_methods},
    {Py_tp_new, mequeiter_new},
    {0, NULL},
};

static PyType_Spec mequeiter_spec = {
    .name = "collections._meque_iterator",
    .basicsize = sizeof(mequeiterobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = mequeiter_slots,
};

/*********************** Meque Reverse Iterator **************************/

static PyObject *
meque_reviter(mequeobject *meque)
{
    mequeiterobject *it;
    collections_state *state = find_module_state_by_def(Py_TYPE(meque));

    it = PyObject_GC_New(mequeiterobject, state->mequereviter_type);
    if (it == NULL)
        return NULL;
    Py_BEGIN_CRITICAL_SECTION(meque);
    it->index = 0;
    it->meque = (mequeobject*)Py_NewRef(meque);
    it->state = meque->state;
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static PyObject *
mequereviter_next_lock_held(mequeiterobject *it, mequeobject *meque)
{
    Py_ssize_t mask = meque->allocated - 1;  // Since allocated is a power of 2
    PyObject *item;
    if (it->meque->ob_item == NULL) {
        return NULL;
    }

    if (it->index == Py_SIZE(it->meque))
        return NULL;

    if (it->meque->state != it->state) {
        PyErr_SetString(PyExc_RuntimeError,
                        "meque mutated during iteration");
        return NULL;
    }

    it->index++;
    item = it->meque->ob_item[(it->meque->first_element + Py_SIZE(it->meque) - it->index) & mask];
    return Py_NewRef(item);


}

static PyObject *
mequereviter_next(PyObject *self)
{
    PyObject *result;
    mequeiterobject *it = mequeiterobject_CAST(self);
    Py_BEGIN_CRITICAL_SECTION2(it, it->meque);
    result = mequereviter_next_lock_held(it, it->meque);
    Py_END_CRITICAL_SECTION2();
    // if (result == NULL) {
    //     Py_DECREF(it);
    // }
    return result;
}

static PyObject *
mequereviter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // TODO(Matthias): check the implementation carefully.
    // It's just cargo-culted from the deque version, but I don't know exactly what it's trying to do.
    Py_ssize_t i, index;
    PyObject *meque;
    mequeiterobject *it;
    collections_state *state = get_module_state_by_cls(type);
    if (!PyArg_ParseTuple(args, "O!|n", state->meque_type, &meque, &index))
        return NULL;
    assert(type == state->mequereviter_type);

    it = (mequeiterobject *)meque_reviter((mequeobject *)meque);
    if (!it)
        return NULL;
    /* consume items from the queue */
    for (i=0; i < index; i++) {
        PyObject *item = mequereviter_next((PyObject *)it);
        if (item) {
            Py_DECREF(item);
        } else {
            /*
             * It's safe to read directly from it without acquiring the
             * per-object lock; the iterator isn't visible to any other threads
             * yet.
             */
            if (it->index >= Py_SIZE(meque)) {
                Py_DECREF(it);
                return NULL;
            } else
                break;
        }
    }
    return (PyObject *)it;
}

static PyType_Slot mequereviter_slots[] = {
    {Py_tp_dealloc, mequeiter_dealloc},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_traverse, mequeiter_traverse},
    {Py_tp_clear, mequeiter_clear},
    {Py_tp_iter, PyObject_SelfIter},
    {Py_tp_iternext, mequereviter_next},
    {Py_tp_methods, mequeiter_methods},
    {Py_tp_new, mequereviter_new},
    {0, NULL},
};

static PyType_Spec mequereviter_spec = {
    .name = "collections._meque_reverse_iterator",
    .basicsize = sizeof(mequeiterobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = mequereviter_slots,
};

/* defaultdict type *********************************************************/

typedef struct {
    PyDictObject dict;
    PyObject *default_factory;
} defdictobject;

#define defdictobject_CAST(op)  ((defdictobject *)(op))

static PyType_Spec defdict_spec;

PyDoc_STRVAR(defdict_missing_doc,
"__missing__(key) # Called by __getitem__ for missing key; pseudo-code:\n\
  if self.default_factory is None: raise KeyError((key,))\n\
  self[key] = value = self.default_factory()\n\
  return value\n\
");

static PyObject *
defdict_missing(PyObject *op, PyObject *key)
{
    defdictobject *dd = defdictobject_CAST(op);
    PyObject *factory = dd->default_factory;
    PyObject *value;
    if (factory == NULL || factory == Py_None) {
        /* XXX Call dict.__missing__(key) */
        PyObject *tup;
        tup = PyTuple_Pack(1, key);
        if (!tup) return NULL;
        PyErr_SetObject(PyExc_KeyError, tup);
        Py_DECREF(tup);
        return NULL;
    }
    value = _PyObject_CallNoArgs(factory);
    if (value == NULL)
        return value;
    if (PyObject_SetItem(op, key, value) < 0) {
        Py_DECREF(value);
        return NULL;
    }
    return value;
}

static inline PyObject*
new_defdict(PyObject *op, PyObject *arg)
{
    defdictobject *dd = defdictobject_CAST(op);
    return PyObject_CallFunctionObjArgs((PyObject*)Py_TYPE(dd),
        dd->default_factory ? dd->default_factory : Py_None, arg, NULL);
}

PyDoc_STRVAR(defdict_copy_doc, "D.copy() -> a shallow copy of D.");

static PyObject *
defdict_copy(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    /* This calls the object's class.  That only works for subclasses
       whose class constructor has the same signature.  Subclasses that
       define a different constructor signature must override copy().
    */
    return new_defdict(op, op);
}

static PyObject *
defdict_reduce(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    /* __reduce__ must return a 5-tuple as follows:

       - factory function
       - tuple of args for the factory function
       - additional state (here None)
       - sequence iterator (here None)
       - dictionary iterator (yielding successive (key, value) pairs

       This API is used by pickle.py and copy.py.

       For this to be useful with pickle.py, the default_factory
       must be picklable; e.g., None, a built-in, or a global
       function in a module or package.

       Both shallow and deep copying are supported, but for deep
       copying, the default_factory must be deep-copyable; e.g. None,
       or a built-in (functions are not copyable at this time).

       This only works for subclasses as long as their constructor
       signature is compatible; the first argument must be the
       optional default_factory, defaulting to None.
    */
    PyObject *args;
    PyObject *items;
    PyObject *iter;
    PyObject *result;
    defdictobject *dd = defdictobject_CAST(op);

    if (dd->default_factory == NULL || dd->default_factory == Py_None)
        args = PyTuple_New(0);
    else
        args = PyTuple_Pack(1, dd->default_factory);
    if (args == NULL)
        return NULL;
    items = PyObject_CallMethodNoArgs(op, &_Py_ID(items));
    if (items == NULL) {
        Py_DECREF(args);
        return NULL;
    }
    iter = PyObject_GetIter(items);
    if (iter == NULL) {
        Py_DECREF(items);
        Py_DECREF(args);
        return NULL;
    }
    result = PyTuple_Pack(5, Py_TYPE(dd), args,
                          Py_None, Py_None, iter);
    Py_DECREF(iter);
    Py_DECREF(items);
    Py_DECREF(args);
    return result;
}

static PyMethodDef defdict_methods[] = {
    {"__missing__", defdict_missing, METH_O,
     defdict_missing_doc},
    {"copy", defdict_copy, METH_NOARGS,
     defdict_copy_doc},
    {"__copy__", defdict_copy, METH_NOARGS,
     defdict_copy_doc},
    {"__reduce__", defdict_reduce, METH_NOARGS,
     reduce_doc},
    {"__class_getitem__", Py_GenericAlias, METH_O|METH_CLASS,
     PyDoc_STR("See PEP 585")},
    {NULL}
};

static PyMemberDef defdict_members[] = {
    {"default_factory", _Py_T_OBJECT,
     offsetof(defdictobject, default_factory), 0,
     PyDoc_STR("Factory for default value called by __missing__().")},
    {NULL}
};

static void
defdict_dealloc(PyObject *op)
{
    defdictobject *dd = defdictobject_CAST(op);
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyTypeObject *tp = Py_TYPE(dd);
    PyObject_GC_UnTrack(dd);
    Py_CLEAR(dd->default_factory);
    PyDict_Type.tp_dealloc(op);
    Py_DECREF(tp);
}

static PyObject *
defdict_repr(PyObject *op)
{
    defdictobject *dd = defdictobject_CAST(op);
    PyObject *baserepr;
    PyObject *defrepr;
    PyObject *result;
    baserepr = PyDict_Type.tp_repr(op);
    if (baserepr == NULL)
        return NULL;
    if (dd->default_factory == NULL)
        defrepr = PyUnicode_FromString("None");
    else
    {
        int status = Py_ReprEnter(dd->default_factory);
        if (status != 0) {
            if (status < 0) {
                Py_DECREF(baserepr);
                return NULL;
            }
            defrepr = PyUnicode_FromString("...");
        }
        else
            defrepr = PyObject_Repr(dd->default_factory);
        Py_ReprLeave(dd->default_factory);
    }
    if (defrepr == NULL) {
        Py_DECREF(baserepr);
        return NULL;
    }
    result = PyUnicode_FromFormat("%s(%U, %U)",
                                  _PyType_Name(Py_TYPE(dd)),
                                  defrepr, baserepr);
    Py_DECREF(defrepr);
    Py_DECREF(baserepr);
    return result;
}

static PyObject*
defdict_or(PyObject* left, PyObject* right)
{
    PyObject *self, *other;

    int ret = PyType_GetBaseByToken(Py_TYPE(left), &defdict_spec, NULL);
    if (ret < 0) {
        return NULL;
    }
    if (ret) {
        self = left;
        other = right;
    }
    else {
        assert(PyType_GetBaseByToken(Py_TYPE(right), &defdict_spec, NULL) == 1);
        self = right;
        other = left;
    }
    if (!PyDict_Check(other)) {
        Py_RETURN_NOTIMPLEMENTED;
    }
    // Like copy(), this calls the object's class.
    // Override __or__/__ror__ for subclasses with different constructors.
    PyObject *new = new_defdict(self, left);
    if (!new) {
        return NULL;
    }
    if (PyDict_Update(new, right)) {
        Py_DECREF(new);
        return NULL;
    }
    return new;
}

static int
defdict_traverse(PyObject *op, visitproc visit, void *arg)
{
    defdictobject *self = defdictobject_CAST(op);
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->default_factory);
    return PyDict_Type.tp_traverse(op, visit, arg);
}

static int
defdict_tp_clear(PyObject *op)
{
    defdictobject *dd = defdictobject_CAST(op);
    Py_CLEAR(dd->default_factory);
    return PyDict_Type.tp_clear(op);
}

static int
defdict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    defdictobject *dd = defdictobject_CAST(self);
    PyObject *olddefault = dd->default_factory;
    PyObject *newdefault = NULL;
    PyObject *newargs;
    int result;
    if (args == NULL || !PyTuple_Check(args))
        newargs = PyTuple_New(0);
    else {
        Py_ssize_t n = PyTuple_GET_SIZE(args);
        if (n > 0) {
            newdefault = PyTuple_GET_ITEM(args, 0);
            if (!PyCallable_Check(newdefault) && newdefault != Py_None) {
                PyErr_SetString(PyExc_TypeError,
                    "first argument must be callable or None");
                return -1;
            }
        }
        newargs = PySequence_GetSlice(args, 1, n);
    }
    if (newargs == NULL)
        return -1;
    dd->default_factory = Py_XNewRef(newdefault);
    result = PyDict_Type.tp_init(self, newargs, kwds);
    Py_DECREF(newargs);
    Py_XDECREF(olddefault);
    return result;
}

PyDoc_STRVAR(defdict_doc,
"defaultdict(default_factory=None, /, [...]) --> dict with default factory\n\
\n\
The default factory is called without arguments to produce\n\
a new value when a key is not present, in __getitem__ only.\n\
A defaultdict compares equal to a dict with the same items.\n\
All remaining arguments are treated the same as if they were\n\
passed to the dict constructor, including keyword arguments.\n\
");

/* See comment in xxsubtype.c */
#define DEFERRED_ADDRESS(ADDR) 0

static PyType_Slot defdict_slots[] = {
    {Py_tp_token, Py_TP_USE_SPEC},
    {Py_tp_dealloc, defdict_dealloc},
    {Py_tp_repr, defdict_repr},
    {Py_nb_or, defdict_or},
    {Py_tp_getattro, PyObject_GenericGetAttr},
    {Py_tp_doc, (void *)defdict_doc},
    {Py_tp_traverse, defdict_traverse},
    {Py_tp_clear, defdict_tp_clear},
    {Py_tp_methods, defdict_methods},
    {Py_tp_members, defdict_members},
    {Py_tp_init, defdict_init},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_free, PyObject_GC_Del},
    {0, NULL},
};

static PyType_Spec defdict_spec = {
    .name = "collections.defaultdict",
    .basicsize = sizeof(defdictobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = defdict_slots,
};

/* helper function for Counter  *********************************************/

/*[clinic input]
_collections._count_elements

    mapping: object
    iterable: object
    /

Count elements in the iterable, updating the mapping
[clinic start generated code]*/

static PyObject *
_collections__count_elements_impl(PyObject *module, PyObject *mapping,
                                  PyObject *iterable)
/*[clinic end generated code: output=7e0c1789636b3d8f input=e79fad04534a0b45]*/
{
    PyObject *it, *oldval;
    PyObject *newval = NULL;
    PyObject *key = NULL;
    PyObject *bound_get = NULL;
    PyObject *mapping_get;
    PyObject *dict_get;
    PyObject *mapping_setitem;
    PyObject *dict_setitem;
    PyObject *one = _PyLong_GetOne();  // borrowed reference

    it = PyObject_GetIter(iterable);
    if (it == NULL)
        return NULL;

    /* Only take the fast path when get() and __setitem__()
     * have not been overridden.
     */
    mapping_get = _PyType_LookupRef(Py_TYPE(mapping), &_Py_ID(get));
    dict_get = _PyType_Lookup(&PyDict_Type, &_Py_ID(get));
    mapping_setitem = _PyType_LookupRef(Py_TYPE(mapping), &_Py_ID(__setitem__));
    dict_setitem = _PyType_Lookup(&PyDict_Type, &_Py_ID(__setitem__));

    if (mapping_get != NULL && mapping_get == dict_get &&
        mapping_setitem != NULL && mapping_setitem == dict_setitem &&
        PyDict_Check(mapping))
    {
        while (1) {
            /* Fast path advantages:
                   1. Eliminate double hashing
                      (by re-using the same hash for both the get and set)
                   2. Avoid argument overhead of PyObject_CallFunctionObjArgs
                      (argument tuple creation and parsing)
                   3. Avoid indirection through a bound method object
                      (creates another argument tuple)
                   4. Avoid initial increment from zero
                      (reuse an existing one-object instead)
            */
            Py_hash_t hash;

            key = PyIter_Next(it);
            if (key == NULL)
                break;

            hash = _PyObject_HashFast(key);
            if (hash == -1) {
                goto done;
            }

            oldval = _PyDict_GetItem_KnownHash(mapping, key, hash);
            if (oldval == NULL) {
                if (PyErr_Occurred())
                    goto done;
                if (_PyDict_SetItem_KnownHash(mapping, key, one, hash) < 0)
                    goto done;
            } else {
                newval = PyNumber_Add(oldval, one);
                if (newval == NULL)
                    goto done;
                if (_PyDict_SetItem_KnownHash(mapping, key, newval, hash) < 0)
                    goto done;
                Py_CLEAR(newval);
            }
            Py_DECREF(key);
        }
    }
    else {
        bound_get = PyObject_GetAttr(mapping, &_Py_ID(get));
        if (bound_get == NULL)
            goto done;

        PyObject *zero = _PyLong_GetZero();  // borrowed reference
        while (1) {
            key = PyIter_Next(it);
            if (key == NULL)
                break;
            oldval = PyObject_CallFunctionObjArgs(bound_get, key, zero, NULL);
            if (oldval == NULL)
                break;
            if (oldval == zero) {
                newval = Py_NewRef(one);
            } else {
                newval = PyNumber_Add(oldval, one);
            }
            Py_DECREF(oldval);
            if (newval == NULL)
                break;
            if (PyObject_SetItem(mapping, key, newval) < 0)
                break;
            Py_CLEAR(newval);
            Py_DECREF(key);
        }
    }

done:
    Py_XDECREF(mapping_get);
    Py_XDECREF(mapping_setitem);
    Py_DECREF(it);
    Py_XDECREF(key);
    Py_XDECREF(newval);
    Py_XDECREF(bound_get);
    if (PyErr_Occurred())
                return NULL;
            Py_RETURN_NONE;
        }

/* Helper function for namedtuple() ************************************/

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    PyObject* doc;
} _tuplegetterobject;

#define tuplegetterobject_CAST(op)  ((_tuplegetterobject *)(op))

/*[clinic input]
@classmethod
_tuplegetter.__new__ as tuplegetter_new

    index: Py_ssize_t
    doc: object
    /
[clinic start generated code]*/

static PyObject *
tuplegetter_new_impl(PyTypeObject *type, Py_ssize_t index, PyObject *doc)
/*[clinic end generated code: output=014be444ad80263f input=87c576a5bdbc0bbb]*/
{
    _tuplegetterobject* self;
    self = (_tuplegetterobject *)type->tp_alloc(type, 0);
    if (self == NULL) {
            return NULL;
    }
    self->index = index;
    self->doc = Py_NewRef(doc);
    return (PyObject *)self;
}

static PyObject *
tuplegetter_descr_get(PyObject *self, PyObject *obj, PyObject *type)
{
    Py_ssize_t index = tuplegetterobject_CAST(self)->index;
    PyObject *result;

    if (obj == NULL) {
        return Py_NewRef(self);
    }
    if (!PyTuple_Check(obj)) {
        if (obj == Py_None) {
            return Py_NewRef(self);
        }
        PyErr_Format(PyExc_TypeError,
                     "descriptor for index '%zd' for tuple subclasses "
                     "doesn't apply to '%s' object",
                     index,
                     Py_TYPE(obj)->tp_name);
    return NULL;
}

    if (!valid_index(index, PyTuple_GET_SIZE(obj))) {
        PyErr_SetString(PyExc_IndexError, "tuple index out of range");
        return NULL;
    }

    result = PyTuple_GET_ITEM(obj, index);
    return Py_NewRef(result);
}

static int
tuplegetter_descr_set(PyObject *self, PyObject *obj, PyObject *value)
{
    if (value == NULL) {
        PyErr_SetString(PyExc_AttributeError, "can't delete attribute");
    } else {
        PyErr_SetString(PyExc_AttributeError, "can't set attribute");
    }
    return -1;
}

static int
tuplegetter_traverse(PyObject *self, visitproc visit, void *arg)
{
    _tuplegetterobject *tuplegetter = tuplegetterobject_CAST(self);
    Py_VISIT(Py_TYPE(tuplegetter));
    Py_VISIT(tuplegetter->doc);
    return 0;
}

static int
tuplegetter_clear(PyObject *self)
{
    _tuplegetterobject *tuplegetter = tuplegetterobject_CAST(self);
    Py_CLEAR(tuplegetter->doc);
    return 0;
}

static void
tuplegetter_dealloc(PyObject *self)
{
    PyTypeObject *tp = Py_TYPE(self);
    PyObject_GC_UnTrack(self);
    (void)tuplegetter_clear(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static PyObject*
tuplegetter_reduce(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    _tuplegetterobject *self = tuplegetterobject_CAST(op);
    return Py_BuildValue("(O(nO))", (PyObject *)Py_TYPE(self),
                         self->index, self->doc);
}

static PyObject*
tuplegetter_repr(PyObject *op)
{
    _tuplegetterobject *self = tuplegetterobject_CAST(op);
    return PyUnicode_FromFormat("%s(%zd, %R)",
                                _PyType_Name(Py_TYPE(self)),
                                self->index, self->doc);
}


static PyMemberDef tuplegetter_members[] = {
    {"__doc__",  _Py_T_OBJECT, offsetof(_tuplegetterobject, doc), 0},
    {0}
};

static PyMethodDef tuplegetter_methods[] = {
    {"__reduce__", tuplegetter_reduce, METH_NOARGS, NULL},
    {NULL},
};

static PyType_Slot tuplegetter_slots[] = {
    {Py_tp_dealloc, tuplegetter_dealloc},
    {Py_tp_repr, tuplegetter_repr},
    {Py_tp_traverse, tuplegetter_traverse},
    {Py_tp_clear, tuplegetter_clear},
    {Py_tp_methods, tuplegetter_methods},
    {Py_tp_members, tuplegetter_members},
    {Py_tp_descr_get, tuplegetter_descr_get},
    {Py_tp_descr_set, tuplegetter_descr_set},
    {Py_tp_new, tuplegetter_new},
    {0, NULL},
};

static PyType_Spec tuplegetter_spec = {
    .name = "collections._tuplegetter",
    .basicsize = sizeof(_tuplegetterobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = tuplegetter_slots,
};


/* module level code ********************************************************/

static int
collections_traverse(PyObject *mod, visitproc visit, void *arg)
{
    collections_state *state = get_module_state(mod);
    Py_VISIT(state->deque_type);
    Py_VISIT(state->meque_type);
    Py_VISIT(state->defdict_type);
    Py_VISIT(state->dequeiter_type);
    Py_VISIT(state->dequereviter_type);
    Py_VISIT(state->tuplegetter_type);
    return 0;
}

static int
collections_clear(PyObject *mod)
{
    collections_state *state = get_module_state(mod);
    Py_CLEAR(state->deque_type);
    Py_CLEAR(state->meque_type);
    Py_CLEAR(state->defdict_type);
    Py_CLEAR(state->dequeiter_type);
    Py_CLEAR(state->dequereviter_type);
    Py_CLEAR(state->tuplegetter_type);
    return 0;
}

static void
collections_free(void *module)
{
    (void)collections_clear((PyObject *)module);
}

PyDoc_STRVAR(collections_doc,
"High performance data structures.\n\
- deque:        ordered collection accessible from endpoints only\n\
- meque:        ordered collection, with fast insert and remove from both ends, fast random access\n\
- defaultdict:  dict subclass with a default value factory\n\
");

static struct PyMethodDef collections_methods[] = {
    _COLLECTIONS__COUNT_ELEMENTS_METHODDEF
    {NULL,       NULL}          /* sentinel */
};

#define ADD_TYPE(MOD, SPEC, TYPE, BASE) do {                        \
    TYPE = (PyTypeObject *)PyType_FromMetaclass(NULL, MOD, SPEC,    \
                                                (PyObject *)BASE);  \
    if (TYPE == NULL) {                                             \
        return -1;                                                  \
    }                                                               \
    if (PyModule_AddType(MOD, TYPE) < 0) {                          \
        return -1;                                                  \
    }                                                               \
} while (0)

static int
collections_exec(PyObject *module) {
    collections_state *state = get_module_state(module);
    ADD_TYPE(module, &deque_spec, state->deque_type, NULL);
    ADD_TYPE(module, &meque_spec, state->meque_type, NULL);

    ADD_TYPE(module, &defdict_spec, state->defdict_type, &PyDict_Type);

    ADD_TYPE(module, &dequeiter_spec, state->dequeiter_type, NULL);
    ADD_TYPE(module, &mequeiter_spec, state->mequeiter_type, NULL);

    ADD_TYPE(module, &dequereviter_spec, state->dequereviter_type, NULL);
    ADD_TYPE(module, &mequereviter_spec, state->mequereviter_type, NULL);

    ADD_TYPE(module, &tuplegetter_spec, state->tuplegetter_type, NULL);

    if (PyModule_AddType(module, &PyODict_Type) < 0) {
        return -1;
    }

    return 0;
}

#undef ADD_TYPE

static struct PyModuleDef_Slot collections_slots[] = {
    {Py_mod_exec, collections_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef _collectionsmodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_collections",
    .m_doc = collections_doc,
    .m_size = sizeof(collections_state),
    .m_methods = collections_methods,
    .m_slots = collections_slots,
    .m_traverse = collections_traverse,
    .m_clear = collections_clear,
    .m_free = collections_free,
};

PyMODINIT_FUNC
PyInit__collections(void)
{
    return PyModuleDef_Init(&_collectionsmodule);
}
