// The logging PrxDecrypter.cpp refers to: off.
#include "Common/Common.h"
#include "Common/Log.h"
static bool off = false;
bool *g_bLogEnabledSetting = &off;
LogChannel g_log[(size_t)Log::NUMBER_OF_LOGS];
void GenericLog(Log type, LogLevel level, const char *file, int line, const char *fmt, ...) {}
