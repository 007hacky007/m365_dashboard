#include <stdint.h>
// Provide concrete definition of stdNumb font by mapping GLCDFONTDECL to a defining form once here.
#ifdef GLCDFONTDECL
#undef GLCDFONTDECL
#endif
#define GLCDFONTDECL(name) const uint8_t name[]
#include "../../M365/fonts/stdNumb.h"
