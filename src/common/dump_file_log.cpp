// The graphics DLL's half of dump_file: its log lines go to the EDVR log. Kept apart from dump_file.cpp
// so the rigs that link the writer need no logger.
#include "dump_file.h"
#include "log.h"

namespace {
void sinkToLog(const char* line) { edvr::Log::get().note("%s", line); }
// Constant-initialised pointer in dump_file.cpp; this runs at DLL load, before any dump.
const bool g_installed = (dump_file::setLogSink(&sinkToLog), true);
}  // namespace
