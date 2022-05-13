# include "Python.h"
# include "structmember.h"

# define PY_ARRAY_UNIQUE_SYMBOL AK_ARRAY_API
# define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

# include "numpy/arrayobject.h"
# include "numpy/arrayscalars.h"
# include "numpy/halffloat.h"

//------------------------------------------------------------------------------
// Macros
//------------------------------------------------------------------------------

// Bug in NumPy < 1.16 (https://github.com/numpy/numpy/pull/12131):
# undef PyDataType_ISBOOL
# define PyDataType_ISBOOL(obj) \
    PyTypeNum_ISBOOL(((PyArray_Descr*)(obj))->type_num)

// Given a PyObject, raise if not an array.
# define AK_CHECK_NUMPY_ARRAY(O)                                              \
    if (!PyArray_Check(O)) {                                                  \
        return PyErr_Format(PyExc_TypeError, "expected numpy array (got %s)", \
                            Py_TYPE(O)->tp_name);                             \
    }

// Given a PyObject, raise if not an array or is not one or two dimensional.
# define AK_CHECK_NUMPY_ARRAY_1D_2D(O) \
    do {\
        AK_CHECK_NUMPY_ARRAY(O)\
        int ndim = PyArray_NDIM((PyArrayObject *)O);\
        if (ndim != 1 && ndim != 2) {\
            return PyErr_Format(PyExc_NotImplementedError,\
                    "expected 1D or 2D array (got %i)",\
                    ndim);\
        }\
    } while (0)

// Placeholder of not implemented pathways / debugging.
# define AK_NOT_IMPLEMENTED(msg)\
    do {\
        PyErr_SetString(PyExc_NotImplementedError, msg);\
        return NULL;\
    } while (0)

# define _AK_DEBUG_BEGIN() \
    do {                   \
        fprintf(stderr, "XXX %s:%i:%s: ", __FILE__, __LINE__, __FUNCTION__);

# define _AK_DEBUG_END()       \
        fprintf(stderr, "\n"); \
        fflush(stderr);        \
    } while (0)

# define AK_DEBUG_OBJ(obj)              \
    _AK_DEBUG_BEGIN();                  \
        fprintf(stderr, #obj " = ");    \
        PyObject_Print(obj, stderr, 0); \
    _AK_DEBUG_END()

# define AK_DEBUG(msg)          \
    _AK_DEBUG_BEGIN();          \
        fprintf(stderr, #msg);  \
    _AK_DEBUG_END()

# if defined __GNUC__ || defined __clang__
# define AK_LIKELY(X) __builtin_expect(!!(X), 1)
# define AK_UNLIKELY(X) __builtin_expect(!!(X), 0)
# else
# define AK_LIKELY(X) (!!(X))
# define AK_UNLIKELY(X) (!!(X))
# endif

//------------------------------------------------------------------------------
// C-level utility functions
//------------------------------------------------------------------------------

// Takes and returns a PyArrayObject, optionally copying a mutable array and setting it as immutable
PyArrayObject *
AK_ImmutableFilter(PyArrayObject *a)
{
    // https://numpy.org/devdocs/reference/c-api/array.html#array-flags
    if (PyArray_FLAGS(a) & NPY_ARRAY_WRITEABLE) {
        if ((a = (PyArrayObject *)PyArray_NewCopy(a, NPY_ANYORDER))) {
            PyArray_CLEARFLAGS(a, NPY_ARRAY_WRITEABLE);
        }
        return a;
    }
    Py_INCREF(a);
    return a;
}

PyArray_Descr*
AK_ResolveDTypes(PyArray_Descr *d1, PyArray_Descr *d2)
{
    if (PyArray_EquivTypes(d1, d2)) {
        Py_INCREF(d1);
        return d1;
    }
    if (PyDataType_ISOBJECT(d1) || PyDataType_ISOBJECT(d2)
        || PyDataType_ISBOOL(d1) || PyDataType_ISBOOL(d2)
        || (PyDataType_ISSTRING(d1) != PyDataType_ISSTRING(d2))
        || ((PyDataType_ISDATETIME(d1) || PyDataType_ISDATETIME(d2))
            // PyDataType_ISDATETIME matches NPY_DATETIME or NPY_TIMEDELTA, so
            // we need to make sure we didn't get one of each:
            && !PyArray_EquivTypenums(d1->type_num, d2->type_num)))
    {
        return PyArray_DescrFromType(NPY_OBJECT);
    }
    PyArray_Descr *d = PyArray_PromoteTypes(d1, d2);
    if (!d) {
        PyErr_Clear();
        return PyArray_DescrFromType(NPY_OBJECT);
    }
    return d;
}

PyArray_Descr*
AK_ResolveDTypeIter(PyObject *dtypes)
{
    PyObject *iterator = PyObject_GetIter(dtypes);
    if (iterator == NULL) {
        // No need to set exception here. GetIter already sets TypeError
        return NULL;
    }
    PyArray_Descr *resolved = NULL;
    PyArray_Descr *dtype;
    while ((dtype = (PyArray_Descr*) PyIter_Next(iterator))) {
        if (!PyArray_DescrCheck(dtype)) {
            PyErr_Format(
                    PyExc_TypeError, "argument must be an iterable over %s, not %s",
                    ((PyTypeObject *) &PyArrayDescr_Type)->tp_name,
                    Py_TYPE(dtype)->tp_name
            );
            Py_DECREF(iterator);
            Py_DECREF(dtype);
            Py_XDECREF(resolved);
            return NULL;
        }
        if (!resolved) {
            resolved = dtype;
            continue;
        }
        Py_SETREF(resolved, AK_ResolveDTypes(resolved, dtype));
        Py_DECREF(dtype);
        if (!resolved || PyDataType_ISOBJECT(resolved)) {
            break;
        }
    }
    Py_DECREF(iterator);
    if (!resolved) {
        // this could happen if this function gets an empty tuple
        PyErr_SetString(PyExc_ValueError, "iterable passed to resolve dtypes is empty");
    }
    return resolved;
}

// Perform a deepcopy on an array, using an optional memo dictionary, and specialized to depend on immutable arrays. This depends on the module object to get the deepcopy method.
PyObject*
AK_ArrayDeepCopy(PyObject* m, PyArrayObject *array, PyObject *memo)
{
    PyObject *id = PyLong_FromVoidPtr((PyObject*)array);
    if (!id) {
        return NULL;
    }

    if (memo) {
        PyObject *found = PyDict_GetItemWithError(memo, id);
        if (found) { // found will be NULL if not in dict
            Py_INCREF(found); // got a borrowed ref, increment first
            Py_DECREF(id);
            return found;
        }
        else if (PyErr_Occurred()) {
            goto error;
        }
    }

    // if dtype is object, call deepcopy with memo
    PyObject *array_new;
    PyArray_Descr *dtype = PyArray_DESCR(array); // borrowed ref

    if (PyDataType_ISOBJECT(dtype)) {
        PyObject *deepcopy = PyObject_GetAttrString(m, "deepcopy");
        if (!deepcopy) {
            goto error;
        }
        array_new = PyObject_CallFunctionObjArgs(deepcopy, array, memo, NULL);
        Py_DECREF(deepcopy);
        if (!array_new) {
            goto error;
        }
    }
    else {
        // if not a n object dtype, we will force a copy (even if this is an immutable array) so as to not hold on to any references
        Py_INCREF(dtype); // PyArray_FromArray steals a reference
        array_new = PyArray_FromArray(
                array,
                dtype,
                NPY_ARRAY_ENSURECOPY);
        if (!array_new) {
            goto error;
        }
        if (memo && PyDict_SetItem(memo, id, array_new)) {
            Py_DECREF(array_new);
            goto error;
        }
    }
    // set immutable
    PyArray_CLEARFLAGS((PyArrayObject *)array_new, NPY_ARRAY_WRITEABLE);
    Py_DECREF(id);
    return array_new;
error:
    Py_DECREF(id);
    return NULL;
}


//------------------------------------------------------------------------------
// AK module public methods
//------------------------------------------------------------------------------

// Return the integer version of the pointer to underlying data-buffer of array.
static PyObject *
mloc(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY(a);
    return PyLong_FromVoidPtr(PyArray_DATA((PyArrayObject *)a));
}

//------------------------------------------------------------------------------
// filter functions

static PyObject *
immutable_filter(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY(a);
    return (PyObject *)AK_ImmutableFilter((PyArrayObject *)a);
}


static PyObject *
name_filter(PyObject *Py_UNUSED(m), PyObject *n)
{
    if (AK_UNLIKELY(PyObject_Hash(n) == -1)) {
        return PyErr_Format(PyExc_TypeError, "unhashable name (type '%s')",
                            Py_TYPE(n)->tp_name);
    }
    Py_INCREF(n);
    return n;
}

// Represent a 1D array as a 2D array with length as rows of a single-column array.
// https://stackoverflow.com/questions/56182259/how-does-one-acces-numpy-multidimensionnal-array-in-c-extensions
static PyObject *
shape_filter(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY_1D_2D(a);
    PyArrayObject *array = (PyArrayObject *)a;

    int size0 = PyArray_DIM(array, 0);
    // If 1D array, set size for axis 1 at 1, else use 2D array to get the size of axis 1
    int size1 = PyArray_NDIM(array) == 1 ? 1 : PyArray_DIM(array, 1);
    return Py_BuildValue("ii", size0, size1);
}

// Reshape if necessary a flat ndim 1 array into a 2D array with one columns and rows of length.
// related example: https://github.com/RhysU/ar/blob/master/ar-python.cpp
static PyObject *
column_2d_filter(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY_1D_2D(a);
    PyArrayObject *array = (PyArrayObject *)a;

    if (PyArray_NDIM(array) == 1) {
        // https://numpy.org/doc/stable/reference/c-api/types-and-structures.html#c.PyArray_Dims
        npy_intp dim[2] = {PyArray_DIM(array, 0), 1};
        PyArray_Dims shape = {dim, 2};
        // PyArray_Newshape might return NULL and set PyErr, so no handling to do here
        return PyArray_Newshape(array, &shape, NPY_ANYORDER); // already a PyObject*
    }
    Py_INCREF(a); // returning borrowed ref, must increment
    return a;
}

// Reshape if necessary a column that might be 2D or 1D is returned as a 1D array.
static PyObject *
column_1d_filter(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY_1D_2D(a);
    PyArrayObject *array = (PyArrayObject *)a;

    if (PyArray_NDIM(array) == 2) {
        npy_intp dim[1] = {PyArray_DIM(array, 0)};
        PyArray_Dims shape = {dim, 1};
        // NOTE: this will set PyErr if shape is not compatible
        return PyArray_Newshape(array, &shape, NPY_ANYORDER);
    }
    Py_INCREF(a);
    return a;
}

// Reshape if necessary a row that might be 2D or 1D is returned as a 1D array.
static PyObject *
row_1d_filter(PyObject *Py_UNUSED(m), PyObject *a)
{
    AK_CHECK_NUMPY_ARRAY_1D_2D(a);
    PyArrayObject *array = (PyArrayObject *)a;

    if (PyArray_NDIM(array) == 2) {
        npy_intp dim[1] = {PyArray_DIM(array, 1)};
        PyArray_Dims shape = {dim, 1};
        // NOTE: this will set PyErr if shape is not compatible
        return PyArray_Newshape(array, &shape, NPY_ANYORDER);
    }
    Py_INCREF(a);
    return a;
}

//------------------------------------------------------------------------------
// array utility

static char *array_deepcopy_kwarg_names[] = {
    "array",
    "memo",
    NULL
};

// Specialized array deepcopy that stores immutable arrays in an optional memo dict that can be provided with kwargs.
static PyObject *
array_deepcopy(PyObject *m, PyObject *args, PyObject *kwargs)
{
    PyObject *array;
    PyObject *memo = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
            "O|O!:array_deepcopy", array_deepcopy_kwarg_names,
            &array,
            &PyDict_Type, &memo)) {
        return NULL;
    }
    AK_CHECK_NUMPY_ARRAY(array);
    return AK_ArrayDeepCopy(m, (PyArrayObject*)array, memo);
}

//------------------------------------------------------------------------------
// type resolution

static PyObject *
resolve_dtype(PyObject *Py_UNUSED(m), PyObject *args)
{
    PyArray_Descr *d1, *d2;
    if (!PyArg_ParseTuple(args, "O!O!:resolve_dtype",
                          &PyArrayDescr_Type, &d1, &PyArrayDescr_Type, &d2))
    {
        return NULL;
    }
    return (PyObject *)AK_ResolveDTypes(d1, d2);
}

static PyObject *
resolve_dtype_iter(PyObject *Py_UNUSED(m), PyObject *arg)
{
    return (PyObject *)AK_ResolveDTypeIter(arg);
}

//------------------------------------------------------------------------------
// general utility

static PyObject *
dtype_from_element(PyObject *Py_UNUSED(m), PyObject *arg)
{
    // -------------------------------------------------------------------------
    // 1. Handle fast, exact type checks first.

    // None
    if (arg == Py_None) {
        return (PyObject*)PyArray_DescrFromType(NPY_OBJECT);
    }

    // Float
    if (PyFloat_CheckExact(arg)) {
        return (PyObject*)PyArray_DescrFromType(NPY_DOUBLE);
    }

    // Integers
    if (PyLong_CheckExact(arg)) {
        return (PyObject*)PyArray_DescrFromType(NPY_LONG);
    }

    // Bool
    if (PyBool_Check(arg)) {
        return (PyObject*)PyArray_DescrFromType(NPY_BOOL);
    }

    PyObject* dtype = NULL;

    // String
    if (PyUnicode_CheckExact(arg)) {
        PyArray_Descr* descr = PyArray_DescrFromType(NPY_UNICODE);
        if (descr == NULL) {
            return NULL;
        }
        dtype = (PyObject*)PyArray_DescrFromObject(arg, descr);
        Py_DECREF(descr);
        return dtype;
    }

    // Bytes
    if (PyBytes_CheckExact(arg)) {
        PyArray_Descr* descr = PyArray_DescrFromType(NPY_STRING);
        if (descr == NULL) {
            return NULL;
        }
        dtype = (PyObject*)PyArray_DescrFromObject(arg, descr);
        Py_DECREF(descr);
        return dtype;
    }

    // -------------------------------------------------------------------------
    // 2. Construct dtype (slightly more complicated)

    // Already known
    dtype = PyObject_GetAttrString(arg, "dtype");
    if (dtype) {
        return dtype;
    }
    PyErr_Clear();

    // -------------------------------------------------------------------------
    // 3. Handles everything else.
    return (PyObject*)PyArray_DescrFromType(NPY_OBJECT);
}

static PyObject *
isna_element(PyObject *Py_UNUSED(m), PyObject *arg)
{
    // None
    if (arg == Py_None) {
        Py_RETURN_TRUE;
    }

    // NaN
    if (PyFloat_Check(arg)) {
        return PyBool_FromLong(isnan(PyFloat_AS_DOUBLE(arg)));
    }
    if (PyArray_IsScalar(arg, Half)) {
        return PyBool_FromLong(npy_half_isnan(PyArrayScalar_VAL(arg, Half)));
    }
    if (PyArray_IsScalar(arg, Float32)) {
        return PyBool_FromLong(isnan(PyArrayScalar_VAL(arg, Float32)));
    }
    if (PyArray_IsScalar(arg, Float64)) {
        return PyBool_FromLong(isnan(PyArrayScalar_VAL(arg, Float64)));
    }
    # ifdef PyFloat128ArrType_Type
    if (PyArray_IsScalar(arg, Float128)) {
        return PyBool_FromLong(isnan(PyArrayScalar_VAL(arg, Float128)));
    }
    # endif

    // Complex NaN
    if (PyComplex_Check(arg)) {
        Py_complex val = ((PyComplexObject*)arg)->cval;
        return PyBool_FromLong(isnan(val.real) || isnan(val.imag));
    }
    if (PyArray_IsScalar(arg, Complex64)) {
        npy_cfloat val = PyArrayScalar_VAL(arg, Complex64);
        return PyBool_FromLong(isnan(val.real) || isnan(val.imag));
    }
    if (PyArray_IsScalar(arg, Complex128)) {
        npy_cdouble val = PyArrayScalar_VAL(arg, Complex128);
        return PyBool_FromLong(isnan(val.real) || isnan(val.imag));
    }
    # ifdef PyComplex256ArrType_Type
    if (PyArray_IsScalar(arg, Complex256)) {
        npy_clongdouble val = PyArrayScalar_VAL(arg, Complex256);
        return PyBool_FromLong(isnan(val.real) || isnan(val.imag));
    }
    # endif

    // NaT - Datetime
    if (PyArray_IsScalar(arg, Datetime)) {
        return PyBool_FromLong(PyArrayScalar_VAL(arg, Datetime) == NPY_DATETIME_NAT);
    }

    // NaT - Timedelta
    if (PyArray_IsScalar(arg, Timedelta)) {
        return PyBool_FromLong(PyArrayScalar_VAL(arg, Timedelta) == NPY_DATETIME_NAT);
    }

    Py_RETURN_FALSE;
}

//------------------------------------------------------------------------------
// array utility

static PyObject *
AK_get_unique_array_positions(PyArrayObject *array)
{
    /*
        # Equivalent Python code:

        _, positions = np.unique(array, return_index=True)
        return positions

        # OR

        positions = argsort_array(array)                 # 1

        array = array[positions]                         # 2

        mask = np.empty(array.shape, dtype=bool)         # 3
        mask[0] = True                                   # 4
        mask[1:] = array[1:] != array[:-1]               # 5

        return positions[mask]                           # 6
    */

    // Quicksort is safe since array just contains integers
    PyArrayObject* positions = (PyArrayObject*)PyArray_ArgSort(array, 0, NPY_QUICKSORT);
    if (positions == NULL) {
        return NULL;
    }

    PyArrayObject *sorted_array = PyObject_GetItem((PyObject*)array, (PyObject*)positions);
    array = NULL; // We don't need this anymore.
    if (sorted_array == NULL) {
        Py_DECREF(positions);
        return NULL;
    }

    // Build up the mask by comparing each element in the sorted array to the
    // previous element.
    PyArrayObject *mask = (PyArrayObject*)PyArray_Empty(
            1,                                // ndim
            PyArray_DIMS(sorted_array),       // shape
            PyArray_DescrFromType(NPY_BOOL),  // dtype
            0                                 // fortran
            );
    if (mask == NULL) {
        Py_DECREF(sorted_array);
        Py_DECREF(positions);
        return NULL;
    }

    npy_bool *mask_values = (npy_bool*)PyArray_DATA(mask);
    npy_int64 *sorted_array_values = (npy_int64*)PyArray_DATA(sorted_array);

    mask_values[0] = 1;

    for (size_t i = 1; i < PyArray_SIZE(sorted_array); ++i) {
        mask_values[i] = sorted_array_values[i] != sorted_array_values[i-1];
    }

    // Apply the mask to indexers to provide the unique positions
    PyObject * result = PyObject_GetItem((PyObject*)positions, (PyObject*)mask);
    Py_DECREF(sorted_array);
    Py_DECREF(positions);
    Py_DECREF(mask);
    return result;
}


static PyObject *
new_indexers_from_indexer_subset(PyObject *Py_UNUSED(m), PyObject *args, PyObject *kwargs)
{
    /*
        # Equivalent Python code:

        num_unique = len(positions)                                    # 1
        order_found = np.full(num_unique, num_unique, dtype=np.int64)  # 2
        new_indexers = np.empty(len(array), dtype=np.int64)            # 3

        num_found = 0                                                  # 4

        for i, element in enumerate(array):                            # 5
            if order_found[element] == num_unique:                     # 6
                order_found[element] = num_found                       # 7
                num_found += 1                                         # 8

            if num_found == num_unique:                                # 9
                return positions, array                                # 10

            new_indexers[i] = order_found[element]                     # 11

       order_found = order_found[order_found != num_unique]            # 12
       _, positions = np.unique(order_found, return_index=True)        # 13

       return positions[:num_found], new_indexers                      # 14
    */
    PyArrayObject *array;
    PyArrayObject *positions;

    static char *kwlist[] = {"array", "positions", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O!O!:new_indexers_from_indexer_subset", kwlist,
                &PyArray_Type, &array,
                &PyArray_Type, &positions
        ))
    {
        return NULL;
    }

    size_t num_unique = PyArray_SIZE(positions);

    npy_intp dims = {num_unique};
    PyArrayObject *order_found = (PyArrayObject*)PyArray_Empty(
            1,                                // ndim
            &dims,                            // shape
            PyArray_DescrFromType(NPY_INT64), // dtype
            0                                 // fortran
            );
    if (order_found == NULL) {
        return NULL;
    }

    PyObject *num_unique_scalar = PyLong_FromLong(num_unique);
    if (num_unique_scalar == NULL) {
        Py_DECREF(order_found);
        return NULL;
    }

    // We use ``num_unique`` to signal that we haven't found the element yet
    // This works, because each element must be 0 < num_unique.
    if (PyArray_FillWithScalar(order_found, num_unique_scalar))
    {
        Py_DECREF(order_found);
        Py_DECREF(num_unique_scalar);
        return NULL;
    }

    PyArrayObject *new_indexers = (PyArrayObject*)PyArray_Empty(
            1,                                // ndim
            PyArray_DIMS(array),              // shape
            PyArray_DescrFromType(NPY_INT64), // dtype
            0                                 // fortran
            );
    if (new_indexers == NULL) {
        Py_DECREF(order_found);
        Py_DECREF(num_unique_scalar);
        return NULL;
    }

    // We know that our incoming dtypes are all int64! This is a safe cast.
    // Plus, it's easier (and less error prone) to work with native C-arrays
    // over using numpy's iteration APIs.
    npy_int64 *array_values = (npy_int64*)PyArray_DATA(array);
    npy_int64 *order_found_values = (npy_int64*)PyArray_DATA(order_found);
    npy_int64 *new_indexers_values = (npy_int64*)PyArray_DATA(new_indexers);

    size_t num_found = 0;

    for (size_t i = 0; i < PyArray_SIZE(array); ++i) {

        npy_int64 element = array_values[i];

        if (order_found_values[element] == num_unique) {
            order_found_values[element] = num_found;
            ++num_found;

            if (num_found == num_unique) {
                // This insight is core to the performance of the algorithm.
                // If we have found every possible indexer, we can simply return
                // back the inputs! Essentially, we can observe on <= single pass
                // that we have the opportunity for re-use
                Py_DECREF(order_found);
                Py_DECREF(new_indexers);
                Py_DECREF(num_unique_scalar);
                return PyTuple_Pack(2, positions, array);
            }
        }

        new_indexers_values[i] = order_found_values[element];
    }

    // 12.   order_found = order_found[order_found != num_unique]
    PyObject *order_not_found_mask = PyObject_RichCompare(
            (PyObject*)order_found,
            num_unique_scalar,
            Py_NE
            );
    Py_DECREF(num_unique_scalar);
    if (order_not_found_mask == NULL) {
        Py_DECREF(order_found);
        Py_DECREF(new_indexers);
        return NULL;
    }

    PyArrayObject *order_found_filtered = PyObject_GetItem(
            (PyObject*)order_found,
            order_not_found_mask
            );
    Py_DECREF(order_found);
    Py_DECREF(order_not_found_mask);
    if (order_found_filtered == NULL) {
        Py_DECREF(new_indexers);
        return NULL;
    }

    // 13. _, positions = np.unique(order_found, return_index=True)
    positions = AK_get_unique_array_positions(order_found_filtered);
    Py_DECREF(order_found_filtered);

    if (positions == NULL) {
        Py_DECREF(new_indexers);
        return NULL;
    }

    // Is it okay to use this?
    PyObject *slice = _PySlice_FromIndices(0, num_found);
    if (slice == NULL) {
        Py_DECREF(new_indexers);
        return NULL;
    }

    PyObject *relevant_positions = PyObject_GetItem((PyObject*)positions, slice);
    Py_DECREF(positions);
    Py_DECREF(slice);
    if (relevant_positions == NULL) {
        Py_DECREF(new_indexers);
        return NULL;
    }

    // 14. return positions[:num_found], new_indexers
    return PyTuple_Pack(2, relevant_positions, new_indexers);
}


//------------------------------------------------------------------------------
// ArrayGO
//------------------------------------------------------------------------------

typedef struct {
    PyObject_VAR_HEAD
    PyObject *array;
    PyObject *list;
} ArrayGOObject;

static PyTypeObject ArrayGOType;

PyDoc_STRVAR(
    ArrayGO_doc,
    "\n"
    "A grow only, one-dimensional, object type array, "
    "specifically for usage in IndexHierarchy IndexLevel objects.\n"
    "\n"
    "Args:\n"
    "    own_iterable: flag iterable as ownable by this instance.\n"
);

PyDoc_STRVAR(
    ArrayGO_copy_doc,
    "Return a new ArrayGO with an immutable array from this ArrayGO\n"
);

PyDoc_STRVAR(ArrayGO_values_doc, "Return the immutable labels array\n");

//------------------------------------------------------------------------------
// ArrayGO utility functions

static int
update_array_cache(ArrayGOObject *self)
{
    if (self->list) {
        if (self->array) {
            PyObject *container = PyTuple_Pack(2, self->array, self->list);
            if (!container) {
                return -1;
            }
            Py_SETREF(self->array, PyArray_Concatenate(container, 0));
            Py_DECREF(container);
        }
        else {
            self->array = PyArray_FROM_OT(self->list, NPY_OBJECT);
        }
        PyArray_CLEARFLAGS((PyArrayObject *)self->array, NPY_ARRAY_WRITEABLE);
        Py_CLEAR(self->list);
    }
    return 0;
}

//------------------------------------------------------------------------------
// ArrayGO Methods:

static PyObject *
ArrayGO_new(PyTypeObject *cls, PyObject *args, PyObject *kwargs)
{
    char* argnames[] = {"iterable", "own_iterable", NULL};
    PyObject *iterable;
    int own_iterable = 0;
    int parsed = PyArg_ParseTupleAndKeywords(
        args, kwargs, "O|$p:ArrayGO", argnames, &iterable, &own_iterable
    );
    if (!parsed) {
        return NULL;
    }
    ArrayGOObject *self = (ArrayGOObject *)cls->tp_alloc(cls, 0);
    if (!self) {
        return NULL;
    }

    if (PyArray_Check(iterable)) {
        if (!PyDataType_ISOBJECT(PyArray_DESCR((PyArrayObject *)iterable))) {
            PyErr_SetString(PyExc_NotImplementedError,
                            "only object arrays are supported");
            Py_DECREF(self);
            return NULL;
        }
        if (own_iterable) {
            // ArrayGO(np.array(...), own_iterable=True)
            PyArray_CLEARFLAGS((PyArrayObject *)iterable, NPY_ARRAY_WRITEABLE);
            self->array = iterable;
            Py_INCREF(iterable);
            return (PyObject *)self;
        }
        // ArrayGO(np.array(...))
        self->array = (PyObject *)AK_ImmutableFilter((PyArrayObject *)iterable);
        if (!self->array) {
            Py_CLEAR(self);
        }
        return (PyObject *)self;
    }
    if (PyList_CheckExact(iterable) && own_iterable) {
        // ArrayGO([...], own_iterable=True)
        self->list = iterable;
        Py_INCREF(iterable);
        return (PyObject *)self;
    }
    // ArrayGO([...])
    self->list = PySequence_List(iterable);
    if (!self->list) {
        Py_CLEAR(self);
    }
    return (PyObject *)self;
}


static PyObject *
ArrayGO_append(ArrayGOObject *self, PyObject *value)
{
    if (!self->list) {
        self->list = PyList_New(1);
        if (!self->list) {
            return NULL;
        }
        Py_INCREF(value);
        PyList_SET_ITEM(self->list, 0, value);
    }
    else if (PyList_Append(self->list, value)) {
        return NULL;
    }
    Py_RETURN_NONE;
}


static PyObject *
ArrayGO_extend(ArrayGOObject *self, PyObject *values)
{
    if (!self->list) {
        self->list = PySequence_List(values);
        if (!self->list) {
            return NULL;
        }
        Py_RETURN_NONE;
    }
    Py_ssize_t len = PyList_Size(self->list);
    if (len < 0 || PyList_SetSlice(self->list, len, len, values)) {
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
ArrayGO_getnewargs(ArrayGOObject *self, PyObject *Py_UNUSED(unused))
{
    if (self->list && update_array_cache(self)) {
        return NULL;
    }
    return PyTuple_Pack(1, self->array);
}

static PyObject *
ArrayGO_copy(ArrayGOObject *self, PyObject *Py_UNUSED(unused))
{
    ArrayGOObject *copy = PyObject_GC_New(ArrayGOObject, &ArrayGOType);
    copy->array = self->array;
    copy->list = self->list ? PySequence_List(self->list) : NULL;
    Py_XINCREF(copy->array);
    return (PyObject *)copy;
}

static PyObject *
ArrayGO_iter(ArrayGOObject *self)
{
    if (self->list && update_array_cache(self)) {
        return NULL;
    }
    return PyObject_GetIter(self->array);
}

static PyObject *
ArrayGO_mp_subscript(ArrayGOObject *self, PyObject *key)
{
    if (self->list && update_array_cache(self)) {
        return NULL;
    }
    return PyObject_GetItem(self->array, key);
}

static Py_ssize_t
ArrayGO_mp_length(ArrayGOObject *self)
{
    return ((self->array ? PyArray_SIZE((PyArrayObject *)self->array) : 0)
            + (self->list ? PyList_Size(self->list) : 0));
}

static PyObject *
ArrayGO_values_getter(ArrayGOObject *self, void* Py_UNUSED(closure))
{
    if (self->list && update_array_cache(self)) {
        return NULL;
    }
    Py_INCREF(self->array);
    return self->array;
}

static int
ArrayGO_traverse(ArrayGOObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->array);
    Py_VISIT(self->list);
    return 0;
}

static int
ArrayGO_clear(ArrayGOObject *self)
{
    Py_CLEAR(self->array);
    Py_CLEAR(self->list);
    return 0;
}

static void
ArrayGO_dealloc(ArrayGOObject *self)
{
    Py_XDECREF(self->array);
    Py_XDECREF(self->list);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

// ArrayGo method bundles

static struct PyGetSetDef ArrayGO_getset[] = {
    {"values", (getter)ArrayGO_values_getter, NULL, ArrayGO_values_doc, NULL},
    {NULL},
};

static PyMethodDef ArrayGO_methods[] = {
    {"append", (PyCFunction)ArrayGO_append, METH_O, NULL},
    {"copy", (PyCFunction)ArrayGO_copy, METH_NOARGS, ArrayGO_copy_doc},
    {"extend", (PyCFunction)ArrayGO_extend, METH_O, NULL},
    {"__getnewargs__", (PyCFunction)ArrayGO_getnewargs, METH_NOARGS, NULL},
    {NULL},
};

static PyMappingMethods ArrayGO_as_mapping = {
    .mp_length = (lenfunc)ArrayGO_mp_length,
    .mp_subscript = (binaryfunc) ArrayGO_mp_subscript,
};

//------------------------------------------------------------------------------
// ArrayGo PyTypeObject
// https://docs.python.org/3/c-api/typeobj.html

static PyTypeObject ArrayGOType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_as_mapping = &ArrayGO_as_mapping,
    .tp_basicsize = sizeof(ArrayGOObject),
    .tp_clear = (inquiry)ArrayGO_clear,
    .tp_dealloc = (destructor)ArrayGO_dealloc,
    .tp_doc = ArrayGO_doc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_getset = ArrayGO_getset,
    .tp_iter = (getiterfunc)ArrayGO_iter,
    .tp_methods = ArrayGO_methods,
    .tp_name = "arraykit.ArrayGO",
    .tp_new = ArrayGO_new,
    .tp_traverse = (traverseproc)ArrayGO_traverse,
};

//------------------------------------------------------------------------------
// ArrayKit module definition
//------------------------------------------------------------------------------

static PyMethodDef arraykit_methods[] =  {
    {"immutable_filter", immutable_filter, METH_O, NULL},
    {"mloc", mloc, METH_O, NULL},
    {"name_filter", name_filter, METH_O, NULL},
    {"shape_filter", shape_filter, METH_O, NULL},
    {"column_2d_filter", column_2d_filter, METH_O, NULL},
    {"column_1d_filter", column_1d_filter, METH_O, NULL},
    {"row_1d_filter", row_1d_filter, METH_O, NULL},
    {"array_deepcopy",
            (PyCFunction)array_deepcopy,
            METH_VARARGS | METH_KEYWORDS,
            NULL},
    {"resolve_dtype", resolve_dtype, METH_VARARGS, NULL},
    {"resolve_dtype_iter", resolve_dtype_iter, METH_O, NULL},
    {"isna_element", isna_element, METH_O, NULL},
    {"dtype_from_element", dtype_from_element, METH_O, NULL},
    {"new_indexers_from_indexer_subset",
            (PyCFunction)new_indexers_from_indexer_subset,
            METH_VARARGS | METH_KEYWORDS,
            NULL},
    {NULL},
};

static struct PyModuleDef arraykit_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_arraykit",
    .m_doc = NULL,
    .m_size = -1,
    .m_methods = arraykit_methods,
};

PyObject *
PyInit__arraykit(void)
{
    import_array();
    PyObject *m = PyModule_Create(&arraykit_module);

    PyObject *copy = PyImport_ImportModule("copy");
    if (!copy) {
        Py_XDECREF(m);
        return NULL;
    }
    PyObject *deepcopy = PyObject_GetAttrString(copy, "deepcopy");
    Py_DECREF(copy);
    if (!deepcopy) {
        Py_XDECREF(m);
        return NULL;
    }

    if (!m ||
        PyModule_AddStringConstant(m, "__version__", Py_STRINGIFY(AK_VERSION)) ||
        PyType_Ready(&ArrayGOType) ||
        PyModule_AddObject(m, "ArrayGO", (PyObject *) &ArrayGOType) ||
        PyModule_AddObject(m, "deepcopy", deepcopy))
    {
        Py_DECREF(deepcopy);
        Py_XDECREF(m);
        return NULL;
    }
    return m;
}
