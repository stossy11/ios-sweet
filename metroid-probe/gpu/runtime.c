#include "runtime.h"
#include "screen.h"
#include <stdarg.h>

int strcmp(const char *a, const char *b)
{
    while(*a && *a==*b) { a++; b++; }
    return (unsigned char)*a-(unsigned char)*b;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const volatile unsigned char *x=a, *y=b;
    for (size_t i=0; i<n; i++) if (x[i]!=y[i]) return x[i]<y[i] ? -1 : 1;
    return 0;
}
static void number(uint64_t value, unsigned base)
{
    char digits[24]; unsigned n=0;
    do { digits[n++]="0123456789abcdef"[value%base]; value/=base; } while(value);
    while(n) gpu_screen_putc(digits[--n]);
}
int printf(const char *format, ...)
{
    va_list ap; va_start(ap,format);
    while (*format) {
        if (*format!='%') { gpu_screen_putc(*format++); continue; }
        format++;
        if (*format=='#') { gpu_screen_string("0x"); format++; }
        unsigned longs=0;
        while (*format=='l' || *format=='z') { longs++; format++; }
        char type=*format;
        if (!type) break;
        format++;
        if (type=='s') { gpu_screen_string(va_arg(ap,const char *)); continue; }
        if (type=='%') { gpu_screen_putc('%'); continue; }
        uint64_t value;
        if (type=='d' || type=='i') {
            int64_t v=longs ? va_arg(ap,int64_t) : va_arg(ap,int);
            value=(uint64_t)v;
            if(v<0) { gpu_screen_putc('-'); value=0-value; }
        } else value=longs ? va_arg(ap,uint64_t) : va_arg(ap,unsigned);
        number(value,type=='x' || type=='X' ? 16 : 10);
    }
    va_end(ap);
    return 0;
}
void delay(unsigned us)
{
    uint64_t hz,start,now;
    __asm__ volatile("mrs %0, cntfrq_el0; isb; mrs %1, cntpct_el0" : "=r"(hz),"=r"(start));
    uint64_t duration=(hz*us+999999)/1000000;
    do { __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now)); } while(now-start<duration);
}
