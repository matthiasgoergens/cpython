#ifndef PY_INTERNAL_TYPECACHE_H
#define PY_INTERNAL_TYPECACHE_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_stackref.h"


#define _Py_TYPECACHE_MINSIZE (1 << 3)
#define _Py_TYPECACHE_MAXSIZE (1 << 16)

struct type_cache_entry {
    PyObject *name;     // name of the attribute or method, interned string, NULL if the entry is empty
    PyObject *value;    // borrowed reference or NULL
};

// Per-type attribute lookup cache: speed up attribute and method lookups,
// see _PyTypeCache_Lookup().
struct type_cache {
    uint32_t mask;                                              // mask for indexing into hashtable, i.e. size of hashtable is mask + 1
    uint32_t available;                                         // number of available entries in hashtable
    uint32_t used;                                              // number of used entries in hashtable
    unsigned int version_tag;                                   // initialized from type->tp_version_tag
    struct type_cache_entry hashtable[_Py_TYPECACHE_MINSIZE];   // hashtable entries
};

struct _PyTypeCacheLookupResult {
    _PyStackRef value;      // value is a stack reference to the cached attribute or method, or NULL if not found
    int cache_hit;          // 1 if the cache entry is valid and matches the type's version tag, 0 otherwise
    unsigned int version_tag;   // version tag of the type when the value was cached
};


#ifndef Py_GIL_DISABLED
/* Inline lookup for the default (GIL) build, used on the hot path in
 * _PyType_LookupStackRefAndVersion().  With the GIL no lock-free reader
 * protocol is needed, so this is the same probe as _PyTypeCache_Lookup()
 * without the out-of-line call and struct return. */
static inline struct _PyTypeCacheLookupResult
_PyTypeCache_LookupInCache(struct type_cache *cache, PyTypeObject *type, PyObject *name)
{
    struct _PyTypeCacheLookupResult miss = {PyStackRef_NULL, 0, 0};
    if (cache == NULL) {
        return miss;
    }
    Py_hash_t hash = PyUnstable_Unicode_GET_CACHED_HASH(name);
    uint32_t index = (uint32_t)hash & cache->mask;
    for (;;) {
        PyObject *entry_name = cache->hashtable[index].name;
        if (entry_name == name) {
            break;
        }
        if (entry_name == NULL) {
            return miss;
        }
        index = (index + 1) & cache->mask;
    }
    if (cache->version_tag != type->tp_version_tag) {
        return miss;
    }
    PyObject *v = cache->hashtable[index].value;
    _PyStackRef out_ref = v ? PyStackRef_FromPyObjectNew(v) : PyStackRef_NULL;
    return (struct _PyTypeCacheLookupResult){out_ref, 1, cache->version_tag};
}
#endif

extern void _PyTypeCache_InitType(PyTypeObject *type);
extern void _PyTypeCache_Insert(PyTypeObject *type, PyObject *name, PyObject *value);
PyAPI_FUNC(struct _PyTypeCacheLookupResult) _PyTypeCache_Lookup(PyTypeObject *type, PyObject *name);
PyAPI_FUNC(void) _PyTypeCache_Invalidate(PyTypeObject *type);

#ifdef __cplusplus
}
#endif
#endif /* PY_INTERNAL_TYPECACHE_H */
