// The one translation unit that compiles the vendored miniz 3.1.2 source
// (third_party\miniz\miniz.c, unchanged), the way third_party\dxbc_hash is
// compiled in: as C++ with the project's flags, its warnings silenced here
// rather than in the vendored file. The macros come from miniz_config.h.
#include "miniz_config.h"

#pragma warning(push, 0)
#include "../../third_party/miniz/miniz.c"
#pragma warning(pop)
