#ifndef Py_QDICTOBJECT_H
#define Py_QDICTOBJECT_H
#ifdef __cplusplus
extern "C" {
#endif


/* QOrderedDict */
/* This API is optional and mostly redundant. */

#ifndef Py_LIMITED_API

typedef struct _qdictobject PyQDictObject;

PyAPI_DATA(PyTypeObject) PyQDict_Type;
PyAPI_DATA(PyTypeObject) PyQDictIter_Type;
PyAPI_DATA(PyTypeObject) PyQDictKeys_Type;
PyAPI_DATA(PyTypeObject) PyQDictItems_Type;
PyAPI_DATA(PyTypeObject) PyQDictValues_Type;

#define PyQDict_Check(op) PyObject_TypeCheck((op), &PyQDict_Type)
#define PyQDict_CheckExact(op) Py_IS_TYPE((op), &PyQDict_Type)
#define PyQDict_SIZE(op) PyDict_GET_SIZE((op))

PyAPI_FUNC(PyObject *) PyQDict_New(void);
PyAPI_FUNC(int) PyQDict_SetItem(PyObject *od, PyObject *key, PyObject *item);
PyAPI_FUNC(int) PyQDict_DelItem(PyObject *od, PyObject *key);

/* wrappers around PyDict* functions */
#define PyQDict_GetItem(od, key) PyDict_GetItem(_PyObject_CAST(od), (key))
#define PyQDict_GetItemWithError(od, key) \
    PyDict_GetItemWithError(_PyObject_CAST(od), (key))
#define PyQDict_Contains(od, key) PyDict_Contains(_PyObject_CAST(od), (key))
#define PyQDict_Size(od) PyDict_Size(_PyObject_CAST(od))
#define PyQDict_GetItemString(od, key) \
    PyDict_GetItemString(_PyObject_CAST(od), (key))

#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_QDICTOBJECT_H */
