/* Single boot CPU, interrupts masked throughout. No other SCM client exists. */
#ifndef GPU_PROBE_LOCKS_H
#define GPU_PROBE_LOCKS_H
#define LCK_GRP_DECLARE(name, description) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name, group) int name
static inline void lck_mtx_lock(int *lock) { (void)lock; }
static inline void lck_mtx_unlock(int *lock) { (void)lock; }
#endif
