#include "cxf/base.h"

const char *cxf_result_str(cxf_result r) {
    switch (r) {
    case CXF_OK:            return "ok";
    case CXF_ERR_NOMEM:     return "nomem";
    case CXF_ERR_FULL:      return "full";
    case CXF_ERR_EMPTY:     return "empty";
    case CXF_ERR_BAD_HANDLE:return "bad_handle";
    case CXF_ERR_RANGE:     return "range";
    case CXF_ERR_DEP:       return "dep";
    case CXF_ERR_FORMAT:    return "format";
    case CXF_ERR_IO:        return "io";
    }
    return "unknown";
}
