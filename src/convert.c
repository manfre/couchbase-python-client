/**
 *     Copyright 2013 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 **/

#include "pycbc.h"
/**
 * Conversion functions
 */

/**
 * This is only called if 'o' is not bytes
 */
static PyObject*
convert_to_bytesobj(PyObject *o)
{
    PyObject *bytesobj = NULL;
    pycbc_assert(!PyBytes_Check(o));

    if (PyUnicode_Check(o)) {
        bytesobj = PyUnicode_AsUTF8String(o);
    }

    if (!bytesobj) {
        PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING,
                           0, "Couldn't convert object to bytes",
                           o);
    }
    return bytesobj;
}

enum {
    CONVERT_MODE_UTF8_FIRST,
    CONVERT_MODE_UTF8_ONLY,
    CONVERT_MODE_BYTES_ONLY
};

static PyObject *
convert_to_string(const char *buf, size_t nbuf, int mode)
{
    PyObject *ret = NULL;

    if (mode == CONVERT_MODE_BYTES_ONLY) {
        goto GT_BYTES;
    }

    ret = PyUnicode_DecodeUTF8(buf, nbuf, "strict");

    if (ret) {
        return ret;
    }

    if (mode == CONVERT_MODE_UTF8_ONLY) {
        PYCBC_EXC_WRAP(PYCBC_EXC_ENCODING, 0, "Couldn't decode as UTF-8");
        return NULL;
    }

    PyErr_Clear();
    GT_BYTES:

    return PyBytes_FromStringAndSize(buf, nbuf);
}

static int
encode_common(PyObject **o, void **buf, size_t *nbuf, lcb_uint32_t flags)
{
    PyObject *bytesobj;
    Py_ssize_t plen;
    int rv;

    if ((flags & PYCBC_FMT_UTF8) == PYCBC_FMT_UTF8) {
#if PY_MAJOR_VERSION == 2
        if (PyString_Check(*o)) {
#else
        if (0) {
#endif
            bytesobj = *o;
            Py_INCREF(*o);
        } else {
            if (!PyUnicode_Check(*o)) {
                PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING,
                                   0, "Must be unicode or string", *o);
                return -1;
            }
            bytesobj = PyUnicode_AsUTF8String(*o);
        }

    } else if ((flags & PYCBC_FMT_BYTES) == PYCBC_FMT_BYTES) {
        if (PyBytes_Check(*o) || PyByteArray_Check(*o)) {
            bytesobj = *o;
            Py_INCREF(*o);

        } else {
            PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING, 0,
                               "Must be bytes or bytearray", *o);
            return -1;
        }

    } else {
        PyObject *args = NULL;
        PyObject *helper;

        if ((flags & PYCBC_FMT_PICKLE) == PYCBC_FMT_PICKLE) {
            helper = pycbc_helpers.pickle_encode;

        } else if ((flags & PYCBC_FMT_JSON) == PYCBC_FMT_JSON) {
            helper = pycbc_helpers.json_encode;

        } else {
            PYCBC_EXC_WRAP(PYCBC_EXC_ARGUMENTS, 0, "Unrecognized format");
            return -1;
        }

        args = PyTuple_Pack(1, *o);
        bytesobj = PyObject_CallObject(helper, args);
        Py_DECREF(args);

        if (!bytesobj) {
            PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING,
                               0, "Couldn't encode value", *o);
            return -1;
        }

        if (!PyBytes_Check(bytesobj)) {
            PyObject *old = bytesobj;
            bytesobj = convert_to_bytesobj(old);
            Py_DECREF(old);
            if (!bytesobj) {
                return -1;
            }
        }
    }

    if (PyByteArray_Check(bytesobj)) {
        *buf = PyByteArray_AS_STRING(bytesobj);
        plen = PyByteArray_GET_SIZE(bytesobj);
        rv = 0;

    } else {
        rv = PyBytes_AsStringAndSize(bytesobj, (char**)buf, &plen);
    }

    if (rv < 0) {
        Py_DECREF(bytesobj);
        PYCBC_EXC_WRAP(PYCBC_EXC_ENCODING, 0, "Couldn't encode value");
        return -1;
    }

    *nbuf = plen;
    *o = bytesobj;
    return 0;
}

static int
decode_common(PyObject **vp, const char *buf, size_t nbuf, lcb_uint32_t flags)
{
    PyObject *decoded = NULL;

    if ((flags & PYCBC_FMT_UTF8) == PYCBC_FMT_UTF8) {
        decoded = convert_to_string(buf, nbuf, CONVERT_MODE_UTF8_ONLY);
        if (!decoded) {
            return -1;
        }

    } else if ((flags & PYCBC_FMT_BYTES) == PYCBC_FMT_BYTES) {
        GT_BYTES:
        decoded = convert_to_string(buf, nbuf, CONVERT_MODE_BYTES_ONLY);
        pycbc_assert(decoded);

    } else {
        PyObject *converter = NULL;
        PyObject *args = NULL;
        PyObject *first_arg = NULL;

        if ((flags & PYCBC_FMT_PICKLE) == PYCBC_FMT_PICKLE) {
            converter = pycbc_helpers.pickle_decode;
            first_arg = convert_to_string(buf, nbuf, CONVERT_MODE_BYTES_ONLY);
            pycbc_assert(first_arg);

        } else if ((flags & PYCBC_FMT_JSON) == PYCBC_FMT_JSON) {
            converter = pycbc_helpers.json_decode;
            first_arg = convert_to_string(buf, nbuf, CONVERT_MODE_UTF8_ONLY);

            if (!first_arg) {
                return -1;
            }

        } else {
            PyErr_Warn(PyExc_UserWarning, "Unrecognized flags. Forcing bytes");
            goto GT_BYTES;
        }

        pycbc_assert(first_arg);
        args = PyTuple_Pack(1, first_arg);
        decoded = PyObject_CallObject(converter, args);

        Py_DECREF(args);
        Py_DECREF(first_arg);
    }

    if (!decoded) {
        PyObject *bytes_tmp = PyBytes_FromStringAndSize(buf, nbuf);
        PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING, 0, "Failed to decode bytes",
                           bytes_tmp);
        Py_XDECREF(bytes_tmp);
        return -1;
    }

    *vp = decoded;
    return 0;
}

int
pycbc_tc_simple_encode(PyObject **p,
                       void *buf,
                       size_t *nbuf,
                       lcb_uint32_t flags)
{
    return encode_common(p, buf, nbuf, flags);
}

int
pycbc_tc_simple_decode(PyObject **vp,
                       const char *buf,
                       size_t nbuf,
                       lcb_uint32_t flags)
{
    return decode_common(vp, buf, nbuf, flags);
}

enum {
    ENCODE_KEY = 1,
    ENCODE_VALUE,
    DECODE_KEY,
    DECODE_VALUE
};
static int
do_call_tc(pycbc_Connection *conn,
          PyObject *obj,
          PyObject *flags,
          PyObject **result,
          int mode)
{
    PyObject *meth = NULL;
    PyObject *args = NULL;
    PyObject *strlookup = NULL;
    int ret = -1;

    switch (mode) {
    case ENCODE_KEY:
        strlookup = pycbc_helpers.tcname_encode_key;
        args = PyTuple_Pack(1, obj);
        break;
    case DECODE_KEY:
        strlookup = pycbc_helpers.tcname_decode_key;
        args = PyTuple_Pack(1, obj);
        break;

    case ENCODE_VALUE:
        strlookup = pycbc_helpers.tcname_encode_value;
        args = PyTuple_Pack(2, obj, flags);
        break;

    case DECODE_VALUE:
        strlookup = pycbc_helpers.tcname_decode_value;
        args = PyTuple_Pack(2, obj, flags);
        break;
    }
    if (args == NULL) {
        PYCBC_EXC_WRAP(PYCBC_EXC_INTERNAL, 0, "Couldn't build arguments");
        goto GT_DONE;
    }

    meth = PyObject_GetAttr(conn->tc, strlookup);
    if (!meth) {
        PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ENCODING, 0,
                           "Couldn't find transcoder method",
                           conn->tc);
        goto GT_DONE;
    }
    *result = PyObject_Call(meth, args, NULL);
    if (*result) {
        ret = 0;
    }

    GT_DONE:
    Py_XDECREF(meth);
    Py_XDECREF(args);
    return ret;
}


int
pycbc_tc_encode_key(pycbc_Connection *conn,
                    PyObject **key,
                    void **buf,
                    size_t *nbuf)
{
    int rv;
    Py_ssize_t plen;

    PyObject *orig_key;
    PyObject *new_key = NULL;

    if (!conn->tc) {
        return encode_common(key, buf, nbuf, PYCBC_FMT_UTF8);
    }

    orig_key = *key;
    pycbc_assert(orig_key);

    rv = do_call_tc(conn, orig_key, NULL, &new_key, ENCODE_KEY);

    if (new_key == NULL || rv < 0) {
        return -1;
    }

    rv = PyBytes_AsStringAndSize(new_key, (char**)buf, &plen);

    if (rv == -1) {
        PYCBC_EXC_WRAP_KEY(PYCBC_EXC_ENCODING,
                           0,
                           "Couldn't convert encoded key to bytes. It is "
                           "possible that the Transcoder.encode_key method "
                           "returned an unexpected value",
                           new_key);

        Py_XDECREF(new_key);
        return -1;
    }

    if (plen == 0) {
        PYCBC_EXC_WRAP_KEY(PYCBC_EXC_ENCODING,
                           0,
                           "Transcoder.encode_key returned an empty string",
                           new_key);
        Py_XDECREF(new_key);
        return -1;
    }

    *nbuf = plen;
    *key = new_key;
    return 0;
}

int
pycbc_tc_decode_key(pycbc_Connection *conn,
                     const void *key,
                     size_t nkey,
                     PyObject **pobj)
{
    PyObject *bobj;
    int rv = 0;
    if (conn->data_passthrough) {
        bobj = PyBytes_FromStringAndSize(key, nkey);
        *pobj = bobj;

    } else if (!conn->tc) {
        return decode_common(pobj, key, nkey, PYCBC_FMT_UTF8);

    } else {
        bobj = PyBytes_FromStringAndSize(key, nkey);
        if (bobj) {
            rv = do_call_tc(conn, bobj, NULL, pobj, DECODE_KEY);
            Py_XDECREF(bobj);

        } else {
            rv = -1;
        }

        if (rv < 0) {
            return -1;
        }
    }

    if (*pobj == NULL) {
        return -1;
    }

    if (PyObject_Hash(*pobj) == -1) {
        PYCBC_EXC_WRAP_KEY(PYCBC_EXC_ENCODING, 0,
                           "Transcoder.decode_key must return a hashable object",
                           *pobj);
        Py_XDECREF(*pobj);
        return -1;
    }

    return 0;
}

PyObject *
pycbc_tc_determine_format(PyObject *value)
{
    if (PyUnicode_Check(value)) {
        return pycbc_helpers.fmt_utf8_flags;

    } else if (PyBytes_Check(value) || PyByteArray_Check(value)) {
        return pycbc_helpers.fmt_bytes_flags;

    } else if (PyList_Check(value) ||
            PyTuple_Check(value) ||
            PyDict_Check(value) ||
            value == Py_True ||
            value == Py_False ||
            value == Py_None) {
        return pycbc_helpers.fmt_json_flags;

    } else {
        return pycbc_helpers.fmt_pickle_flags;
    }
}

int
pycbc_tc_encode_value(pycbc_Connection *conn,
                       PyObject **value,
                       PyObject *flag_v,
                       void **buf,
                       size_t *nbuf,
                       lcb_uint32_t *flags)
{
    PyObject *flags_obj;
    PyObject *orig_value;
    PyObject *new_value = NULL;
    PyObject *result_tuple = NULL;
    unsigned long flags_stackval;
    int rv;
    Py_ssize_t plen;

    orig_value = *value;

    if (!flag_v) {
        flag_v = conn->dfl_fmt;
    }

    if (!conn->tc) {

        if (flag_v == pycbc_helpers.fmt_auto) {
            flag_v = pycbc_tc_determine_format(*value);
        }

        rv = pycbc_get_u32(flag_v, &flags_stackval);
        if (rv < 0) {
            PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_ARGUMENTS, 0,
                               "Bad value for flags",
                               flag_v);
            return -1;
        }

        *flags = flags_stackval & PYCBC_FMT_MASK;
        return encode_common(value, buf, nbuf, flags_stackval);
    }

    /**
     * Calling into Transcoder
     */

    rv = do_call_tc(conn, orig_value, flag_v, &result_tuple, ENCODE_VALUE);
    if (rv < 0) {
        return -1;
    }

    if (!PyTuple_Check(result_tuple) || PyTuple_GET_SIZE(result_tuple) != 2) {
        PYCBC_EXC_WRAP_EX(PYCBC_EXC_ENCODING, 0,
                          "Expected return of (bytes, flags)",
                          orig_value,
                          result_tuple);

        Py_XDECREF(result_tuple);
        return -1;

    }

    new_value = PyTuple_GET_ITEM(result_tuple, 0);
    flags_obj = PyTuple_GET_ITEM(result_tuple, 1);

    if (new_value == NULL || flags_obj == NULL) {
        PYCBC_EXC_WRAP_OBJ(PYCBC_EXC_INTERNAL, 0, "Tuple GET_ITEM had NULL",
                           result_tuple);

        Py_XDECREF(result_tuple);
        return -1;
    }

    rv = pycbc_get_u32(flags_obj, &flags_stackval);
    if (rv < 0) {
        Py_XDECREF(result_tuple);
        PYCBC_EXC_WRAP_VALUE(PYCBC_EXC_ENCODING, 0,
                             "Transcoder.encode_value() returned a bad "
                             "value for flags", orig_value);
        return -1;
    }

    *flags = flags_stackval;
    rv = PyBytes_AsStringAndSize(new_value, (char**)buf, &plen);
    if (rv == -1) {
        Py_XDECREF(result_tuple);

        PYCBC_EXC_WRAP_VALUE(PYCBC_EXC_ENCODING, 0,
                             "Value returned by Transcoder.encode_value() "
                             "could not be converted to bytes",
                orig_value);
        return -1;
    }

    *value = new_value;
    *nbuf = plen;

    Py_INCREF(new_value);
    Py_XDECREF(result_tuple);

    return 0;
}

int
pycbc_tc_decode_value(pycbc_Connection *conn,
                       const void *value,
                       size_t nvalue,
                       lcb_uint32_t flags,
                       PyObject **pobj)
{
    PyObject *result = NULL;
    PyObject *pint = NULL;
    PyObject *pbuf = NULL;
    int rv;

    if (conn->data_passthrough == 0 && conn->tc == NULL) {
        return decode_common(pobj, value, nvalue, flags);
    }

    if (conn->data_passthrough) {
        *pobj = PyBytes_FromStringAndSize(value, nvalue);
        if (*pobj) {
            return 0;
        }
        return -1;
    }

    pbuf = PyBytes_FromStringAndSize(value, nvalue);
    if (!pbuf) {
        pbuf = PyBytes_FromString("");
    }

    pint = pycbc_IntFromUL(flags);
    if (!pint) {
        PYCBC_EXC_WRAP(PYCBC_EXC_INTERNAL, 0, "Couldn't create flags object");
        rv = -1;
        goto GT_DONE;
    }

    rv = do_call_tc(conn, pbuf, pint, &result, DECODE_VALUE);

    GT_DONE:
    Py_XDECREF(pint);
    Py_XDECREF(pbuf);

    if (rv < 0) {
        return -1;
    }

    *pobj = result;
    return 0;
}
