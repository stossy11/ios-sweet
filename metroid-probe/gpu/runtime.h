#ifndef GPU_PROBE_RUNTIME_H
#define GPU_PROBE_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <libkern/libkern.h>
typedef uintptr_t vm_offset_t;
enum { EIO=5, ENOMEM=12, EBUSY=16, ENODEV=19, EINVAL=22, ETIMEDOUT=60 };
int printf(const char *, ...);
void delay(unsigned);
static inline vm_offset_t ml_io_map(uint64_t pa, size_t bytes)
{ (void)bytes; return (uintptr_t)pa; }
#endif
