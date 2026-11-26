/* SPDX-License-Identifier: GPL-2.0-only
 * End-to-end ownership test using real power, SMMU, loader and start cores.
 * MMIO models hardware side effects; no callback pretends the scheduler ran.
 */
#include "metroid_gpu_live.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIT(n) (1u << (n))
static struct model {
    uint32_t cc[0x9800/4], gcc[0x80000/4], smmu[MG_SMMU_SIZE/4], gpu[0x28000];
    unsigned allocations, frees, votes, fabric_calls, configure_calls, publications, gpu_writes;
    unsigned fail_allocate, fail_fabric_check, fabric_checks;
    int fabric_error, configure_error;
    bool fabric, bad_power, bad_smmu, bad_hfi, bad_tcm, stuck_cm3;
    uint64_t next;
    struct mg_live *owner;
} m;
static void reset(struct mg_live *s)
{
    memset(&m, 0, sizeof(m)); memset(s, 0, sizeof(*s)); m.owner = s;
    m.next = 0x100004000ull;
    m.cc[0x9080/4] = 1;
    m.gcc[0] = BIT(31); m.gcc[0x10/4] = 31; m.gcc[0x14/4] = 0x4000;
    m.smmu[0x20/4] = BIT(30) | BIT(27) | 8;
    m.smmu[0x24/4] = (3u << 28) | 8;
    m.smmu[0x28/4] = BIT(12) | 5; m.smmu[0] = BIT(10);
}
static int allocate(void *p, size_t bytes, struct mg_memory_block *b)
{
    (void)p;
    if (++m.allocations == m.fail_allocate) return MG_LIMIT;
    void *cpu = NULL; assert(!posix_memalign(&cpu, MG_MEMORY_PAGE, bytes));
    memset(cpu, 0, bytes);
    *b = (struct mg_memory_block){cpu, cpu, m.next, bytes}; m.next += bytes;
    return MG_OK;
}
static void release(void *p, struct mg_memory_block *b)
{ (void)p; assert(!m.owner->memory.exposed); free(b->cpu); memset(b,0,sizeof(*b)); m.frees++; }
static int fabric(void *p)
{ (void)p; assert(!m.votes); m.fabric_calls++; m.fabric = !m.fabric_error; return m.fabric_error; }
static bool fabric_ready(void *p)
{ (void)p; return m.fabric && ++m.fabric_checks != m.fail_fabric_check; }
static int rails(void *p)
{ (void)p; assert(m.fabric && !m.gpu_writes); m.votes++; return MG_OK; }
static uint32_t power_read(void *p, enum mg_bus bus, uint32_t offset)
{
    (void)p;
    if (m.bad_power) return UINT32_MAX;
    return (bus == MG_CC ? m.cc : m.gcc)[offset/4];
}
static void power_write(void *p, enum mg_bus bus, uint32_t offset, uint32_t value)
{
    (void)p; assert(m.fabric && m.votes);
    (bus == MG_CC ? m.cc : m.gcc)[offset/4] = value;
    if(bus==MG_GCC && offset==0x71150)m.gcc[offset/4] |= BIT(31);
    if (bus != MG_CC) return;
    if (offset == 0x9080) m.cc[0x9094/4] = BIT(31);
    if (offset == 4 && value == 1) m.cc[0] = (m.cc[0] | BIT(31)) & ~BIT(8);
    if (offset == 0x93ec || offset == 0x9318) m.cc[offset/4] &= ~1u;
}
static void wait_us(void *p, unsigned us) { (void)p; assert(us <= 10); }
static void publish(void *p, const void *memory, size_t bytes)
{ (void)p; assert(memory && bytes && m.fabric); m.publications++; }
static uint32_t smmu_read(void *p, uint32_t offset)
{ (void)p; assert(m.configure_calls); return m.bad_smmu ? UINT32_MAX : m.smmu[offset/4]; }
static void smmu_write(void *p, uint32_t offset, uint32_t value)
{
    (void)p; assert(m.owner->memory.exposed);
    if (offset >= 0x10000 && (offset & 4095) == 0x58) m.smmu[offset/4] &= ~value;
    else m.smmu[offset/4] = offset>=0x10000 && !(offset & 4095) ? value & ~BIT(12) : value;
}
static void smmu_write64(void *p, uint32_t offset, uint64_t value)
{ smmu_write(p,offset,(uint32_t)value); smmu_write(p,offset+4,(uint32_t)(value>>32)); }
static int configure(void *p)
{
    (void)p; assert(m.owner->power.stage == MG_POWER_READY);
    m.configure_calls++; m.gpu[0x1f800] = 1;
    return m.configure_error;
}
static uint32_t gpu_read(void *p, uint32_t reg)
{ (void)p; assert(m.configure_calls && m.owner->domain.attached); return m.gpu[reg] ^ (m.bad_tcm && reg == 0x1b400 ? 1u : 0u); }
/* Independent architectural permission check on the actual GMU page tables.
 * Previously the mock signalled ready on reset release without a fetch. */
static bool gmu_fetch_allowed(uint32_t va)
{
    const struct mg_page_tables *t = &m.owner->memory.tables;
    const uint64_t *table = t->memory;
    const unsigned shifts[] = {30,21,12};
    for (unsigned level = 0; level < 3; level++) {
        uint64_t pte = table[(va >> shifts[level]) & 511];
        if ((pte & 3) != 3) return false;
        if (level == 2) {
            bool user_write = (pte & BIT(6)) && !(pte & BIT(7));
            return (pte & BIT(10)) && !(pte & (1ull << 53)) && !user_write;
        }
        uint64_t pa = pte & 0x0000fffffffff000ull;
        assert(pa >= t->pa && pa - t->pa < t->used * 4096u);
        table = (const uint64_t *)((const char *)t->memory + pa - t->pa);
    }
    return false;
}
static void gpu_write(void *p, uint32_t reg, uint32_t value)
{
    (void)p; assert(m.owner->memory.exposed && m.owner->domain.attached);
    m.gpu_writes++; m.gpu[reg] = reg == 0x23b06 ? value & 0x7fu : value;
    if (reg == 0x1f800 && value == 0) {
        assert(m.publications >= m.owner->memory.count + 1);
        if (!m.stuck_cm3 && gmu_fetch_allowed(0x4000)) m.gpu[0x1f81c] = 0x100;
        else {
            unsigned cb = m.owner->domain.cb_base + (m.owner->domain.cb << m.owner->domain.page_shift);
            bool permission = !gmu_fetch_allowed(0x4000);
            m.smmu[(cb+0x58)/4] = permission ? 0x408 : 2;
            m.smmu[(cb+0x60)/4] = permission ? 0x4000 : 0x4010;
            m.smmu[(cb+0x68)/4] = permission ? 0x1a0063 : 0x123;
        }
    }
    if (reg == 0x1f986 && value == 1 && !m.bad_hfi) m.gpu[0x1f980] = 1;
}
static struct mg_scm_result scm(void *p, uint32_t fn, uint32_t info, uint64_t arg)
{ (void)p; (void)fn; (void)arg; assert(info == 1 && m.gpu[0x1f800] == 1); return (struct mg_scm_result){0,1}; }
static const struct mg_live_io io = {
    .memory = {NULL,allocate,release}, .power = {NULL,power_read,power_write,wait_us,rails},
    .smmu = {NULL,smmu_read,smmu_write,smmu_write64,wait_us,publish},
    .retain_fabric = fabric, .fabric_ready = fabric_ready, .configure = configure,
    .read = gpu_read, .write = gpu_write, .delay = wait_us, .publish = publish, .scm = scm,
};
/* External instruction cache and actual version words loaded into mock TCM. */
static const uint32_t fw[] = {
    0x4000,0,1,4096, 0x4000,4,0,0,0x12345678,
    0,4,0,0,0x87654321,
    0x10007fe0,4,0,0,0x50040001, 0x10007ff0,4,0,0,0x20070001,
};
static void cleanup(struct mg_live *s)
{
    /* Only the host model has no DMA. This escape hatch is NOT production. */
    s->memory.exposed = false;
    assert(!mg_gmu_memory_release(&s->memory,&io.memory));
}
int main(int argc, char **argv)
{
    struct mg_live *s = calloc(1,sizeof(*s)); assert(s);
    reset(s);
    assert(mg_live_boot(s,&io,fw,sizeof(fw)) == MG_OK && s->stage == MG_LIVE_HFI);
    assert(s->memory.exposed && !s->quarantined && !m.frees && m.votes == 1);
    assert(m.gpu[0x1b400] == 0x87654321);
    const struct mg_gmu_backing b = s->memory.backing[s->memory.firmware_count];
    struct mg_hfi_queue q;
    assert(mg_hfi_bind(b.cpu,b.bytes,b.iova,0,&q) == MG_OK);
    assert(*(uint32_t *)s->memory.backing[s->memory.count-1].cpu == 1024);
    assert(mg_live_gmu_mapped(s,b.cpu,b.iova,b.bytes));
    assert(mg_live_gmu_mapped(s,(char *)b.cpu+4,b.iova+4,b.bytes-4));
    assert(!mg_live_gmu_mapped(s,(char *)b.cpu+4,b.iova,b.bytes-4));
    assert(!mg_live_gmu_mapped(s,b.cpu,b.iova,b.bytes+1));
    assert(!mg_live_gmu_mapped(s,b.cpu,UINT32_MAX,SIZE_MAX));
    assert(!mg_live_gmu_mapped(s,b.cpu,b.iova,0));
    m.fabric = false; assert(!mg_live_gmu_mapped(s,b.cpu,b.iova,4)); m.fabric = true;
    assert(mg_live_boot(s,&io,fw,sizeof(fw)) == MG_BUSY);
    cleanup(s);
    for (unsigned failure = 0; failure < 8; failure++) {
        reset(s);
        if (failure == 0) m.fail_allocate = 2;
        if (failure == 1) m.fabric_error = MG_TIMEOUT;
        if (failure == 2) m.fail_fabric_check = 1;
        if (failure == 3) m.bad_power = true;
        if (failure == 4) m.configure_error = MG_IO;
        if (failure == 5) m.fail_fabric_check = 2;
        if (failure == 6) m.bad_smmu = true;
        if (failure == 7) m.bad_hfi = true;
        assert(mg_live_boot(s,&io,fw,sizeof(fw)) != MG_OK);
        assert(s->error && s->stage != MG_LIVE_HFI);
        assert(mg_live_boot(s,&io,fw,sizeof(fw)) == MG_BUSY);
        if (failure < 6) {
            assert(!s->memory.exposed && !m.gpu_writes);
            assert(m.frees == m.allocations - (failure == 0));
        } else {
            assert(s->memory.exposed && s->quarantined && !m.frees);
            assert(mg_gmu_memory_release(&s->memory,&io.memory) == MG_BUSY);
        }
        if (failure == 7) assert(s->start.quarantined && m.gpu[0x1f800] == 1);
        cleanup(s);
    }
    reset(s); m.bad_tcm = true;
    assert(mg_live_boot(s,&io,fw,sizeof(fw)) == MG_IO);
    assert(s->start.reg == 0x1b400 && s->start.value == 0x87654320);
    assert(s->start.firmware_expected == 0x87654321 && !s->start.firmware_words);
    assert(!s->start.released && !m.gpu[0x1f986] && !m.frees && s->quarantined);
    cleanup(s);
    reset(s); m.stuck_cm3 = true;
    assert(mg_live_boot(s,&io,fw,sizeof(fw)) == MG_TIMEOUT);
    assert(s->stage == MG_LIVE_SMMU && s->power.stage == MG_POWER_READY);
    assert(s->start.stage == MG_START_CONFIG && s->start.reg == 0x1f81c && !s->start.value);
    assert(s->start.firmware_words == 3 && s->start.poll_reads == 10000);
    assert(s->start.snapshot_valid && !s->start.snapshot[0] && m.gpu[0x1f800] == 1);
    assert(s->fault_snapshot_valid && s->fault_snapshot[3] == 2);
    assert(s->fault_snapshot[4] == 0x4010 && s->fault_snapshot[6] == 0x123);
    assert(!m.gpu[0x1f986] && s->quarantined && !m.frees);
    cleanup(s);
    reset(s); struct mg_live_io missing = io; missing.retain_fabric = NULL;
    assert(mg_live_boot(s,&missing,fw,sizeof(fw)) == MG_INVALID && !m.allocations && !m.votes);
    assert(argc <= 2);
    if (argc == 2) {
        FILE *file = fopen(argv[1],"rb"); assert(file);
        assert(!fseek(file,0,SEEK_END)); long size = ftell(file);
        assert(size > 0 && size <= MG_FW_MAX_BYTES && !fseek(file,0,SEEK_SET));
        void *image = malloc((size_t)size); assert(image);
        assert(fread(image,1,(size_t)size,file) == (size_t)size); fclose(file);
        reset(s);
        assert(mg_live_boot(s,&io,image,(size_t)size) == MG_OK && s->stage == MG_LIVE_HFI);
        const struct mg_gmu_backing *hfi = &s->memory.backing[s->memory.firmware_count];
        assert(mg_hfi_bind(hfi->cpu,hfi->bytes,hfi->iova,0,&q) == MG_OK);
        assert(s->memory.exposed && !m.frees);
        cleanup(s); free(image);
        puts("PASS: real reserved firmware through integrated GMU boot MMIO model");
    }
    free(s);
    puts("PASS: integrated GMU ownership/startup, mapping identity, failure unwind and DMA quarantine");
    return 0;
}
