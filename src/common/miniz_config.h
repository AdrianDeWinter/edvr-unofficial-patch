// miniz 3.1.2 (third_party\miniz, unchanged) as the EDVR dump container uses it:
// zlib-format deflate and inflate only, no stdio, no time, no archive code, and
// no zlib-compatible names (nothing else in the process is zlib). Every
// translation unit that includes miniz must see the same macros, so they live
// here. The vendored header's own warnings stay out of the project's /W4.
#pragma once

#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES

#pragma warning(push, 0)
#include "../../third_party/miniz/miniz.h"
#pragma warning(pop)
