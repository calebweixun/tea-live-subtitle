/* obs_log()/plugin-support stand-in for the OBS-free end-to-end client test.
 * Log lines go to stderr so they never mix with the driver's JSON stdout. */
#include "plugin-support.h"

const char *PLUGIN_NAME = "tea-live-subtitle-e2e";
const char *PLUGIN_VERSION = "test";

void obs_log(int log_level, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[obs_log %d] ", log_level);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}
