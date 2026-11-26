#ifndef GPU_PROBE_LIBKERN_H
#define GPU_PROBE_LIBKERN_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
int strcmp(const char *, const char *);
#endif
