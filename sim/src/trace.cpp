#include "trace.hpp"

void trace(const char *__restrict format, ...) {
    va_list args;
    va_start (args, format);
    vprintf(format, args);
    va_end (args);
    fflush(stdout);
}
