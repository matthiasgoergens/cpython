#include "Python.h"
#include "pycore_call.h"          // _PyObject_CallNoArgs()
#include "pycore_dict.h"          // _PyDict_GetItem_KnownHash()
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_moduleobject.h"  // _PyModule_GetState()
#include "pycore_pyatomic_ft_wrappers.h"
#include "pycore_typeobject.h"    // _PyType_GetModuleState()
#include "pycore_weakref.h"       // FT_CLEAR_WEAKREFS()

#include <stddef.h>

typedef struct {
    PyTypeObject *deque_type;
    PyTypeObject *defdict_type;
    PyTypeObject *dequeiter_type;
    PyTypeObject *dequereviter_type;
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
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=d178039753446059]*/

typedef struct dequeobject dequeobject;

/* We can safely assume type to be the defining class,
 * since tuplegetter is not a base type */
#define clinic_state() (get_module_state_by_cls(type))
#include "clinic/_collectionsmodule.c.h"
#undef clinic_state

/* collections module implementation of a deque() datatype
   Written and maintained by Raymond D. Hettinger <python@rcn.com> et al
*/

/* deque type as growable ring buffer *********************************************************/

/*[python input]
class dequeobject_converter(self_converter):
    type = "dequeobject *"
[python start generated code]*/
/*[python end generated code: output=da39a3ee5e6b4b0d input=76e5c03d44497c3a]*/

struct dequeobject {
    PyObject_VAR_HEAD
    /* Vector of pointers to list elements.  list[0] is ob_item[0], etc. */
    PyObject **ob_item;
    //  allocated needs to be a power of two
    Py_ssize_t allocated;
    Py_ssize_t first_element;
    Py_ssize_t maxlen;

    size_t state;               /* incremented whenever the indices move, to eg detect mutations during iteration */
};

#define dequeobject_CAST(op)    ((dequeobject *)(op))


static PyObject *
deque_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    dequeobject *deque;

    /* create dequeobject structure */
    deque = (dequeobject *)type->tp_alloc(type, 0);
    if (deque == NULL)
        return NULL;

    Py_SET_SIZE(deque, 0);
    deque->ob_item = NULL;
    deque->allocated = 0;
    deque->first_element = 0;
    deque->maxlen = -1;
    deque->state = 0;

    return (PyObject *)deque;
}

static size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

// We want zero to behave like +infinity, hence the -1 and unsigned overflow.
static Py_ssize_t min3_special(Py_ssize_t a, Py_ssize_t b, Py_ssize_t c) {
    return min(a -1, min(b -1, c-1)) + 1;
}

// TODO(Matthias): use this for delete as well.  And for middle insert?
static void circular_mem_move(PyObject **items, Py_ssize_t allocated, Py_ssize_t dst_start, Py_ssize_t src_start, Py_ssize_t m) {
    assert(m <= allocated);
    Py_ssize_t mask = allocated - 1;

    // move backwards:
    if (((src_start - dst_start) & mask) < m) {
        Py_ssize_t remaining = m;
        while (remaining > 0) {
            Py_ssize_t src_until_wrap = (- src_start) & mask;
            Py_ssize_t dst_until_wrap = (- dst_start) & mask;
            Py_ssize_t step = min3_special(src_until_wrap, dst_until_wrap, remaining);
            memmove(&items[dst_start], &items[src_start], step * sizeof(PyObject *));
            remaining -= step;
            assert(remaining >= 0);
            assert(step > 0);
            src_start = (src_start + step) & mask;
            dst_start = (dst_start + step) & mask;
        }
    }
    // move forwards:
    else {
        Py_ssize_t remaining = m;
        Py_ssize_t src_end = (src_start + remaining) & mask;
        Py_ssize_t dst_end = (dst_start + remaining) & mask;
        while (remaining > 0) {
            Py_ssize_t step = min3_special(src_end, dst_end, remaining);
            memmove(&items[(dst_end-step) & mask], &items[(src_end-step) & mask], step * sizeof(PyObject *));
            remaining -= step;
            assert(remaining >= 0);
            assert(step > 0);
            src_end = (src_end - step) & mask;
            dst_end = (dst_end - step) & mask;
        }
    }
}

static int deque_maybe_shrink(dequeobject *deque)
{
    Py_ssize_t old_allocated = deque->allocated;

    Py_ssize_t new_allocated = old_allocated;
    // Ensure we don't shrink too much, we want to keep at least half the slots
    // free.  This is basically the same heuristic we use for growing, but in reverse.
    while (Py_SIZE(deque) <= new_allocated >> 2 && new_allocated > 0) {
        new_allocated >>= 1;
    }
    if(new_allocated >= deque->allocated) {
        return 0; // No need to shrink
    }

    if(deque->first_element + Py_SIZE(deque) > old_allocated) {
        // If there's wrap-around, then the bit from 0 to somewhere in the middle is already good.
        // But we need to copy the elements from deque->first_element to the end of the array, if any.
        Py_ssize_t dst_start = new_allocated - deque->first_element;
        circular_mem_move(
            deque->ob_item,
            old_allocated,
            dst_start,
            deque->first_element,
            old_allocated - deque->first_element);
        assert(deque->first_element < new_allocated);
        deque->first_element = dst_start;
    } else if (deque->first_element >= new_allocated) {
        // In this case, there's no wrap around, but we are past the end of the new array.
        circular_mem_move(deque->ob_item, new_allocated, 0, deque->first_element, Py_SIZE(deque));
        deque->first_element = 0;
    } else {
        // Here the items already fit completely in the smaller array.
    }
    assert(deque->first_element < new_allocated);
    assert(deque->first_element + Py_SIZE(deque) <= new_allocated);
    // TODO(Matthias): properly handle errors, when we can't shrink.
    PyObject **ob_item = (PyObject **)PyMem_Realloc(deque->ob_item, new_allocated * sizeof(PyObject *));
    if(ob_item == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    deque->ob_item = ob_item;
    deque->allocated = new_allocated;
    return 0;
}

/*[clinic input]
@critical_section
_collections.deque.pop as deque_pop

    deque: dequeobject

Remove and return the rightmost element.
[clinic start generated code]*/

static PyObject *
deque_pop_impl(dequeobject *deque)
/*[clinic end generated code: output=21dbf03cd7259ec9 input=4b3754030e7a5478]*/
{
    // TODO(Matthias): do we need to lock anything?
    PyObject *item;
    if (Py_SIZE(deque) == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    item = deque->ob_item[(deque->first_element + Py_SIZE(deque) - 1) & (deque->allocated - 1)];
    Py_SET_SIZE(deque, Py_SIZE(deque) - 1);
    deque_maybe_shrink(deque);
    deque->state++;
    // TODO(Matthias): consider shrinking the deque, if we are below some
    // threshold.  Perhaps half? Or perhaps only shrink on inserts?
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
/*[clinic end generated code: output=905cd2f73f5c42a6 input=398b4abbe7f594df]*/
{
    // TODO(Matthias): do we need to lock anything?
    PyObject *item;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    if (Py_SIZE(deque) == 0) {
        PyErr_SetString(PyExc_IndexError, "pop from an empty deque");
        return NULL;
    }
    item = deque->ob_item[deque->first_element];
    deque->first_element = (deque->first_element + 1) & mask;
    Py_SET_SIZE(deque, Py_SIZE(deque) - 1);
    deque_maybe_shrink(deque);
    deque->state++;
    // TODO(Matthias): consider shrinking the deque, if we are below some threshold.
    // Or perhaps only shrink on inserts?
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

static int deque_grow_ensure(dequeobject *deque, Py_ssize_t min_size)
{
    if (deque->allocated >= min_size) {
        return 0;
    }

    Py_ssize_t old_size = deque->allocated;
    Py_ssize_t old_mask = old_size - 1;
    // round up min_size to the next power of 2
    while(min_size > deque->allocated) {
        deque->allocated = (deque->allocated == 0) ? 1 : deque->allocated * 2;
    }

    PyObject **ob_item = (PyObject **)PyMem_Realloc(deque->ob_item, deque->allocated * sizeof(PyObject *));
    if(ob_item == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    deque->ob_item = ob_item;

    // Check if there's a wrap-around
    Py_ssize_t last_element = (deque->first_element + Py_SIZE(deque)) & old_mask;
    if (Py_SIZE(deque) > 1 && last_element <= deque->first_element) {
        // There's a wrap-around, need to move either the first or last part
        Py_ssize_t wrapped_part_size = last_element;
        Py_ssize_t start_part_size = Py_SIZE(deque) - wrapped_part_size;

        if (wrapped_part_size <= start_part_size) {
            // Move the first part to the new space
            circular_mem_move(deque->ob_item, deque->allocated, old_size, 0, wrapped_part_size);
        } else {
            // Move the last part to the new space
            Py_ssize_t dst_start = deque->allocated - start_part_size;
            circular_mem_move(deque->ob_item, deque->allocated, dst_start, deque->first_element, start_part_size);
            deque->first_element = dst_start;
        }
    }
    return 0;
}

static inline int
deque_append_lock_held(dequeobject *deque, PyObject *item, Py_ssize_t maxlen)
{
    if (Py_SIZE(deque) == deque->allocated) {
        int result = deque_grow_ensure(deque, Py_SIZE(deque)+1);
        if(result != 0) {
            return result;
        }
    }
    deque->ob_item[(deque->first_element + Py_SIZE(deque)) & (deque->allocated - 1)] = item;
    Py_SET_SIZE(deque, Py_SIZE(deque) + 1);
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
_collections.deque.append as deque_append

    deque: dequeobject
    item: object
    /

Add an element to the right side of the deque.
[clinic start generated code]*/

static PyObject *
deque_append_impl(dequeobject *deque, PyObject *item)
/*[clinic end generated code: output=a9bb7d97f92757c8 input=5dc707e82fab58b9]*/
{
    if (deque_append_lock_held(deque, Py_NewRef(item), deque->maxlen) < 0)
        return NULL;
    Py_RETURN_NONE;
}


static inline int
deque_appendleft_lock_held(dequeobject *deque, PyObject *item,
                           Py_ssize_t maxlen)
{
    if (Py_SIZE(deque) >= deque->allocated) {
        int result = deque_grow_ensure(deque, Py_SIZE(deque)+1);
        if(result != 0) {
            return result;
        }
    }
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    deque->first_element = (deque->first_element - 1) & mask;
    deque->ob_item[deque->first_element] = item;
    Py_SET_SIZE(deque, Py_SIZE(deque) + 1);
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
/*[clinic end generated code: output=6632ff0d636f208d input=d666903e8064ceb7]*/
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
/*[clinic end generated code: output=f89404334ceb151f input=14167af70bc2fbf4]*/
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
/*[clinic end generated code: output=b79929ea34c76705 input=aebb2457196e597c]*/
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
/*[clinic end generated code: output=99b41209bacdd683 input=2baf9f303bf5ef08]*/
{
    // TODO(Matthias): we can actually copy via memcpy, instead of using extend.
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
            PyObject *item = old_deque->ob_item[old_deque->first_element];
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
/*[clinic end generated code: output=c69b84a6d52873ea input=674df1beccbf86da]*/
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
    dequeobject *deque = dequeobject_CAST(self);
    if (deque->ob_item == NULL)
        return 0;                         /* already cleared */

    /* decref all elements */
    Py_ssize_t n    = Py_SIZE(deque);
    Py_ssize_t mask = deque->allocated - 1;
    Py_ssize_t idx  = deque->first_element;

    for (Py_ssize_t k = 0; k < n; k++, idx++) {
        Py_CLEAR(deque->ob_item[idx & mask]);      /* safe even if element NULL */
    }

    /* reset header */
    PyMem_Free(deque->ob_item);
    deque->ob_item       = NULL;
    deque->allocated     = 0;
    deque->first_element = 0;
    Py_SET_SIZE(deque, 0);
    deque->state++;

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
/*[clinic end generated code: output=7dcd0c6af389ba2d input=0614792bc7ccd074]*/
{
    (void)deque_clear((PyObject *)deque);
    Py_RETURN_NONE;
}

// TODO(Matthias): I don't think this works.
static PyObject *
deque_inplace_repeat_lock_held(dequeobject *deque, Py_ssize_t n)
{
    Py_ssize_t input_size = Py_SIZE(deque);
    if (input_size == 0 || n == 1) {
        return Py_NewRef(deque);
    }

    if (n < 1) {
        deque_clear((PyObject *)deque);
        return Py_NewRef(deque);
    }

    if (input_size > PY_SSIZE_T_MAX / n) {
        PyErr_NoMemory();
        return NULL;
    }
    Py_ssize_t output_size = input_size * n;

    // First resize the deque to accommodate the repeated elements
    if (deque_grow_ensure(deque, output_size) < 0) {
        return NULL;
    }

    // Copy the elements to their new positions
    PyObject **items = deque->ob_item;
    Py_ssize_t first = deque->first_element;
    Py_ssize_t allocated = deque->allocated;
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
    Py_SET_SIZE(deque, output_size);
    return Py_NewRef(deque);
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

// TODO(Matthias): Special case for length = allocated: we only need to move the first element, no memcpy.
static int
_deque_rotate(dequeobject *deque, Py_ssize_t n)
{
    Py_ssize_t len=Py_SIZE(deque), halflen=len>>1;
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
    if (n == 0) {
        return 0;
    }

    PyObject **items = deque->ob_item;
    Py_ssize_t first = deque->first_element;
    Py_ssize_t allocated = deque->allocated;
    Py_ssize_t mask = allocated - 1;  // Since allocated is a power of 2

    if (len == allocated) {
        // Special case: we only need to move the first element index.
        deque->first_element = (first - n) & mask;
        return 0;
    }

    // For positive rotation, we move elements from end to beginning
    if (n > 0) {
        Py_ssize_t src_start = (first + len - n) & mask;
        Py_ssize_t dst_start = (first - n) & mask;
        circular_mem_move(items, allocated, dst_start, src_start, n);
        deque->first_element = (first - n) & mask;
    }
    // For negative rotation, we move elements from beginning to end
    else {
        Py_ssize_t src_start = first;
        Py_ssize_t dst_start = (first + len) & mask;
        circular_mem_move(items, allocated, dst_start, src_start, -n);
        deque->first_element = (first - n) & mask;
    }

    deque->state++;
    return 0;
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
/*[clinic end generated code: output=52a1cc399c5ebb4c input=3620ef39b86db333]*/
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
/*[clinic end generated code: output=00c980c2260afe80 input=a7e7a017ad96ebe5]*/
{
    Py_ssize_t size = Py_SIZE(deque);
    if (size <= 1) {
        Py_RETURN_NONE;
    }

    Py_ssize_t first = deque->first_element;
    Py_ssize_t allocated = deque->allocated;
    Py_ssize_t mask = allocated - 1;
    PyObject **items = deque->ob_item;

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

    deque->state++;
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
/*[clinic end generated code: output=ac507a140ec341d2 input=c30e8101d20eeded]*/
{
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t count = 0;
    size_t start_state = deque->state;
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        item = Py_NewRef(items[first]);
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

        /* Advance to next element, handling wrap-around */
        first = (first + 1) & mask;
    }
    return PyLong_FromSsize_t(count);
}

static int
deque_contains_lock_held(dequeobject *deque, PyObject *v)
{
    Py_ssize_t n = Py_SIZE(deque);
    size_t start_state = deque->state;
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;
    PyObject *item;
    int cmp;

    while (--n >= 0) {
        item = Py_NewRef(items[first]);
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
        first = (first + 1) & mask;
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
/*[clinic end generated code: output=1a3b286ab205e455 input=df4c4403279a0cc6]*/
{
    Py_ssize_t n;
    PyObject *item;
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;
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
        if (start_state != deque->state) {
            PyErr_SetString(PyExc_RuntimeError,
                            "deque mutated during iteration");
            return NULL;
        }
        current_item = (current_item + 1) & mask;
    }
    PyErr_SetString(PyExc_ValueError, "deque.index(x): x not in deque");
    return NULL;
}

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
/*[clinic end generated code: output=d13d06f5fb3524ba input=789a37c70b173309]*/
{
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;

    // Handle negative indices
    if (index < 0) {
        index += n;
        if (index < 0)
            index = 0;
    }
    if (index > n)
        index = n;

    // TODO(Matthias): perhaps also shrink the deque if it's too empty?
    // Just like we resize dicts.
    // Check if we need to grow the deque
    if (n == deque->allocated) {
        if (deque_grow_ensure(deque, Py_SIZE(deque)+1) < 0)
            return NULL;
        items = deque->ob_item;  // Update items pointer after potential realloc
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
            items[0] = items[deque->allocated - 1];
            memmove(&items[first], &items[first + 1], (deque->allocated - first - 1) * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[first], &items[first + 1], (insert_pos - first) * sizeof(PyObject *));
        }
        deque->first_element = (first - 1) & mask;
    }
    // If distance is positive or zero, we're closer to the right end
    else {
        // Check if we need to handle wrap-around
        Py_ssize_t last = (first + n - 1) & mask;
        if (insert_pos > last) {
            // Two memmoves needed due to wrap-around
            memmove(&items[insert_pos + 1], &items[insert_pos], (deque->allocated - insert_pos - 1) * sizeof(PyObject *));
            // Copy the element at the boundary
            items[deque->allocated - 1] = items[0];
            memmove(&items[1], &items[0], last * sizeof(PyObject *));
        } else {
            // Single memmove possible
            memmove(&items[insert_pos + 1], &items[insert_pos], (last - insert_pos) * sizeof(PyObject *));
        }
    }

    // Insert the new value
    items[insert_pos] = Py_NewRef(value);
    Py_SET_SIZE(deque, n + 1);
    deque->state++;
    Py_RETURN_NONE;
}


static PyObject *
deque_item_lock_held(dequeobject *deque, Py_ssize_t i)
{
    // TODO(Matthias): check whether we should support negative indices the usual way?
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return NULL;
    }

    // Calculate the actual position in the ring buffer
    Py_ssize_t pos = (first + i) & mask;
    return Py_NewRef(items[pos]);
}

static PyObject *
deque_item(PyObject *self, Py_ssize_t i)
{
    dequeobject *deque = dequeobject_CAST(self);
    PyObject *result;
    result = deque_item_lock_held(deque, i);
    return result;
}

// TODO(Matthias): this one is probably broken.
// TODO(Matthias): optimise from which direction we shift to close the gap.
// TODO(Matthias): check whether we need to decrease the reference count of the deleted item, or if our callers do that?
static int
deque_del_item(dequeobject *deque, Py_ssize_t i)
{
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return -1;
    }

    // Calculate the actual position in the ring buffer
    Py_ssize_t pos = (first + i) & mask;
    PyObject *item = items[pos];
    assert (item != NULL);
    // Something is wrong with probably the rotation, so that we delete stuff twice?
    // Or something is wrong with the growth?
    Py_DECREF(item);

    // Determine direction based on which end is closer
    Py_ssize_t distance = i - (n >> 1);

    // If distance is negative, we're closer to the left end
    if (distance < 0) {
        // Move elements from first to pos-1 one position to the right
        circular_mem_move(items, deque->allocated, (first + 1) & mask, first, -distance);
        deque->first_element = (first + 1) & mask;
    }
    // If distance is positive or zero, we're closer to the right end
    else {
        // Move elements from pos+1 to last one position to the left
        circular_mem_move(items, deque->allocated, pos, (pos + 1) & mask, distance);
    }

    Py_SET_SIZE(deque, n - 1);
    deque->state++;
    return 0;
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
/*[clinic end generated code: output=dcd984454051c535 input=3f0ab03deeda57a0]*/
{
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;
    Py_ssize_t i;
    int cmp;

    for (i = 0; i < n; i++) {
        PyObject *item = items[(first + i) & mask];
        cmp = PyObject_RichCompareBool(item, value, Py_EQ);
        if (cmp > 0) {
            if (deque_del_item(deque, i) < 0)
                return NULL;
            Py_RETURN_NONE;
        }
        if (cmp < 0)
            return NULL;
    }
    PyErr_SetString(PyExc_ValueError, "deque.remove(x): x not in deque");
    return NULL;
}

static int
deque_ass_item_lock_held(dequeobject *deque, Py_ssize_t i, PyObject *v)
{
    Py_ssize_t n = Py_SIZE(deque);
    Py_ssize_t first = deque->first_element;
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject **items = deque->ob_item;

    if (i < 0 || i >= n) {
        PyErr_SetString(PyExc_IndexError, "deque index out of range");
        return -1;
    }
    if (v == NULL) {
        // Delete item
        return deque_del_item(deque, i);
    }
    // Set item
    Py_ssize_t pos = (first + i) & mask;
    Py_SETREF(items[pos], Py_NewRef(v));
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
    PyObject_GC_UnTrack(self);      /* stop GC from revisiting us   */
    deque_clear(self);              /* drop elements (once only)    */
    Py_TYPE(self)->tp_free(self);
}

static int
deque_traverse(PyObject *self, visitproc visit, void *arg)
{
    dequeobject *deque = dequeobject_CAST(self);

    Py_ssize_t n = Py_SIZE(deque);

    PyObject **buf = deque->ob_item;
    Py_ssize_t mask = deque->allocated - 1;
    Py_ssize_t i = deque->first_element;
    for (Py_ssize_t k = 0; k < n; k++) {
        Py_VISIT(buf[i]);
        i = (i + 1) & mask;
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
/*[clinic end generated code: output=8042f666273ce10c input=0f6b8eccd4efa664]*/
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
    // lock; deque->maxlen is only assigned during construction.
    if (deque->maxlen < 0) {
        return Py_BuildValue("O()NN", Py_TYPE(deque), state, it);
    }
    else {
        return Py_BuildValue("O(()n)NN", Py_TYPE(deque), deque->maxlen, state, it);
    }
}

PyDoc_STRVAR(deque_reduce_doc, "Return state information for pickling.");


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

    Py_ssize_t first      = dequeobject_CAST(deque)->first_element;
    Py_ssize_t size       = Py_SIZE(deque);
    Py_ssize_t allocated  = dequeobject_CAST(deque)->allocated;
    PyObject   *prefix    = PyUnicode_FromFormat("<%zd, %zd, %zd>",
                                                 first, size, allocated);

    Py_ssize_t maxlen = dequeobject_CAST(deque)->maxlen;
    if (maxlen >= 0)
        result = PyUnicode_FromFormat("%s%U(%R, maxlen=%zd)",
                                    _PyType_Name(Py_TYPE(deque)), prefix, aslist,
                                    maxlen);
    else
        result = PyUnicode_FromFormat("%s%U(%R)",
                                    _PyType_Name(Py_TYPE(deque)), prefix, aslist);
    Py_ReprLeave(deque);
    Py_DECREF(aslist);
    Py_DECREF(prefix);
    return result;
}

static PyObject *
deque_richcompare(PyObject *v, PyObject *w, int op)
{
    dequeobject *deque1 = dequeobject_CAST(v);
    dequeobject *deque2 = dequeobject_CAST(w);
    PyObject *result;
    Py_ssize_t i, len1, len2;
    PyObject *item1, *item2;

    if (!PyObject_TypeCheck(w, Py_TYPE(v))) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    len1 = Py_SIZE(deque1);
    len2 = Py_SIZE(deque2);

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
        item1 = deque_item_lock_held(deque1, i);
        if (item1 == NULL) {
            return NULL;
        }
        item2 = deque_item_lock_held(deque2, i);
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
_collections.deque.__init__ as deque_init

    deque: dequeobject
    iterable: object = NULL
    maxlen as maxlenobj: object = NULL

A list-like sequence optimized for data insertion and deletion near its endpoints.
[clinic start generated code]*/

static int
deque_init_impl(dequeobject *deque, PyObject *iterable, PyObject *maxlenobj)
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
    deque->maxlen = maxlen;

    /* Initialize the ring buffer */
    // TODO(Matthias): ok, here's where we set up to start with 8!!!
    deque->allocated = 0;  // Start with a power of 2
    deque->ob_item = NULL;
    // if (deque->ob_item == NULL) {
    //     PyErr_NoMemory();
    //     return -1;
    // }
    deque->first_element = 0;
    Py_SET_SIZE(deque, 0);

    if (iterable != NULL) {
        it = PyObject_GetIter(iterable);
        if (it == NULL)
            return -1;

        while ((item = PyIter_Next(it)) != NULL) {
            if (deque_append_lock_held(deque, item, maxlen) < 0) {
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
_collections.deque.__sizeof__ as deque___sizeof__

    deque: dequeobject

Return the size of the deque in memory, in bytes.
[clinic start generated code]*/

static PyObject *
deque___sizeof___impl(dequeobject *deque)
/*[clinic end generated code: output=7b0bb2d9fca03e84 input=62bbff6edf9fff7c]*/
{
    size_t res = _PyObject_SIZE(Py_TYPE(deque));
    res += deque->allocated * sizeof(PyObject *);
    return PyLong_FromSize_t(res);
}

static PyObject *
deque_get_maxlen(PyObject *self, void *Py_UNUSED(closure))
{
    dequeobject *deque = dequeobject_CAST(self);
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
/*[clinic end generated code: output=7e103da707cdfc4a input=57517d54f065671a]*/
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
        METH_O|METH_CLASS,       PyDoc_STR("See PEP <placeholder>")},
    {NULL,              NULL}   /* sentinel */
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
    // {Py_tp_members, deque_members},
    /* No members needed */

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

/*********************** deque Iterator **************************/

typedef struct {
    PyObject_HEAD
    Py_ssize_t index;
    dequeobject *deque;
    size_t state;          /* state when the iterator is created */
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
    it->index = 0;
    it->deque = (dequeobject*)Py_NewRef(deque);
    it->state = deque->state;
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static int
dequeiter_traverse(PyObject *op, visitproc visit, void *arg)
{
    dequeiterobject *mio = dequeiterobject_CAST(op);
    Py_VISIT(Py_TYPE(mio));
    Py_VISIT(mio->deque);
    return 0;
}

static int
dequeiter_clear(PyObject *op)
{
    dequeiterobject *mio = dequeiterobject_CAST(op);
    Py_CLEAR(mio->deque);
    return 0;
}

static void
dequeiter_dealloc(PyObject *mio)
{
    /* bpo-31095: UnTrack is needed before calling any callbacks */
    PyTypeObject *tp = Py_TYPE(mio);
    PyObject_GC_UnTrack(mio);
    (void)dequeiter_clear(mio);
    PyObject_GC_Del(mio);
    Py_DECREF(tp);
}

static PyObject *
dequeiter_next_lock_held(dequeiterobject *it, dequeobject *deque)
{
    PyObject *item;
    if (it->deque->ob_item == NULL) {
        return NULL;
    }

    if (it->deque->state != it->state) {
        PyErr_SetString(PyExc_RuntimeError,
                        "deque mutated during iteration");
        return NULL;
    }
    Py_ssize_t allocated = it->deque->allocated;
    Py_ssize_t mask = allocated - 1;  // Since allocated is a power of 2
    if (it->index >= Py_SIZE(deque)) {
        return NULL;
    }

    item = deque->ob_item[(it->index + it->deque->first_element) & mask];
    it->index++;
    return Py_NewRef(item);
}

static PyObject *
dequeiter_next(PyObject *op)
{
    PyObject *result;
    dequeiterobject *it = dequeiterobject_CAST(op);
    Py_BEGIN_CRITICAL_SECTION2(it, it->deque);
    result = dequeiter_next_lock_held(it, it->deque);
    Py_END_CRITICAL_SECTION2();
    // TODO(Matthias): is this needed?
    // if (result == NULL) {
    //     Py_DECREF(it);
    // }
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
            if (it->index < dequeobject_CAST(deque)->allocated) {
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
    return PyLong_FromSsize_t(it->deque->allocated - it->index);
}

PyDoc_STRVAR(dequeiter_len_doc, "Private method returning an estimate of len(list(it)).");

static PyObject *
dequeiter_reduce(PyObject *op, PyObject *Py_UNUSED(dummy))
{
    dequeiterobject *it = dequeiterobject_CAST(op);
    PyTypeObject *ty = Py_TYPE(it);
    // It's safe to access it->deque without holding the per-object lock for it
    // here; it->deque is only assigned during construction of it.
    dequeobject *deque = it->deque;
    Py_ssize_t allocated, index;
    Py_BEGIN_CRITICAL_SECTION2(it, deque);
    allocated = deque->allocated;
    index = it->index;
    Py_END_CRITICAL_SECTION2();
    return Py_BuildValue("O(On)", ty, deque, allocated - index);
}

static PyMethodDef dequeiter_methods[] = {
    {"__length_hint__", dequeiter_len, METH_NOARGS, dequeiter_len_doc},
    {"__reduce__", dequeiter_reduce, METH_NOARGS, deque_reduce_doc},
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

/*********************** deque Reverse Iterator **************************/

static PyObject *
deque_reviter(dequeobject *deque)
{
    dequeiterobject *it;
    collections_state *state = find_module_state_by_def(Py_TYPE(deque));

    it = PyObject_GC_New(dequeiterobject, state->dequereviter_type);
    if (it == NULL)
        return NULL;
    Py_BEGIN_CRITICAL_SECTION(deque);
    it->index = 0;
    it->deque = (dequeobject*)Py_NewRef(deque);
    it->state = deque->state;
    Py_END_CRITICAL_SECTION();
    PyObject_GC_Track(it);
    return (PyObject *)it;
}

static PyObject *
dequereviter_next_lock_held(dequeiterobject *it, dequeobject *deque)
{
    Py_ssize_t mask = deque->allocated - 1;  // Since allocated is a power of 2
    PyObject *item;
    if (it->deque->ob_item == NULL) {
        return NULL;
    }

    if (it->index == Py_SIZE(it->deque))
        return NULL;

    if (it->deque->state != it->state) {
        PyErr_SetString(PyExc_RuntimeError,
                        "deque mutated during iteration");
        return NULL;
    }

    it->index++;
    item = it->deque->ob_item[(it->deque->first_element + Py_SIZE(it->deque) - it->index) & mask];
    return Py_NewRef(item);


}

static PyObject *
dequereviter_next(PyObject *self)
{
    PyObject *result;
    dequeiterobject *it = dequeiterobject_CAST(self);
    Py_BEGIN_CRITICAL_SECTION2(it, it->deque);
    result = dequereviter_next_lock_held(it, it->deque);
    Py_END_CRITICAL_SECTION2();
    // if (result == NULL) {
    //     Py_DECREF(it);
    // }
    return result;
}

static PyObject *
dequereviter_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    // TODO(Matthias): check the implementation carefully.
    // It's just cargo-culted from the deque version, but I don't know exactly what it's trying to do.
    Py_ssize_t i, index;
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
    for (i=0; i < index; i++) {
        PyObject *item = dequereviter_next((PyObject *)it);
        if (item) {
            Py_DECREF(item);
        } else {
            /*
             * It's safe to read directly from it without acquiring the
             * per-object lock; the iterator isn't visible to any other threads
             * yet.
             */
            if (it->index >= Py_SIZE(deque)) {
                Py_DECREF(it);
                return NULL;
            } else
                break;
        }
    }
    return (PyObject *)it;
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

PyDoc_STRVAR(reduce_doc, "Return state information for pickling.");

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

static int
valid_index(Py_ssize_t i, Py_ssize_t limit)
{
    /* The cast to size_t lets us use just a single comparison
       to check whether i is in the range: 0 <= i < limit */
    return (size_t) i < (size_t) limit;
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
- deque:        ordered collection, with fast insert and remove from both ends, fast random access\n\
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

    ADD_TYPE(module, &defdict_spec, state->defdict_type, &PyDict_Type);

    ADD_TYPE(module, &dequeiter_spec, state->dequeiter_type, NULL);

    ADD_TYPE(module, &dequereviter_spec, state->dequereviter_type, NULL);

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
