#ifndef FONT_CACHE_HOST_RTDBG_H
#define FONT_CACHE_HOST_RTDBG_H
#include <stdio.h>
#define LOG_E(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif
