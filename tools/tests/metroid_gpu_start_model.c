/* SPDX-License-Identifier: GPL-2.0-only */
#include "metroid_gpu_start.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIT(n) (1u << (n))
static _Alignas(4096) unsigned char tables[MG_PT_MAX_PAGES * 4096];
static uint32_t regs[0x40000 / 4], gpu[0x28000];
struct model {
    unsigned writes, reads, fail_read, fail_write, publications, scm_calls;
    unsigned fail_scm, released, prepared;
    int prerequisites, prepare_error;
    bool stuck_tlb, stuck_cm3, stuck_hfi, unavailable;
    bool injected_mask_read;
    bool ignore_release;
    uint32_t sctlr_raz;
    uint32_t ao_mask_raz;
};
static uint32_t sr(void *p, uint32_t off)
{
    struct model *m = p; assert(off < sizeof(regs) && !(off & 3));
    if (++m->reads == m->fail_read) return UINT32_MAX;
    if (m->stuck_tlb && off == 0x207f4) return 1;
    return regs[off / 4];
}
static void sw(void *p, uint32_t off, uint32_t value)
{
    struct model *m = p; assert(off < sizeof(regs) && !(off & 3));
    if (++m->writes == m->fail_write) return;
    if (off >= 0x10000 && (off & 4095) == 0x58) {
        regs[off/4] &= ~value; return;
    }
    if(off>=0x10000 && !(off & 4095))value &= ~m->sctlr_raz;
    regs[off / 4] = value;
}
static void sw64(void *p, uint32_t off, uint64_t v)
{ sw(p,off,(uint32_t)v); sw(p,off + 4,(uint32_t)(v >> 32)); }
static void wait_us(void *p, unsigned us) { (void)p; assert(us <= 10); }
static void publish(void *p, const void *ptr, size_t n)
{ struct model *m = p; assert(ptr == tables && n >= 12288); m->publications++; }
static struct mg_smmu_io sio(struct model *m)
{ return (struct mg_smmu_io){m,sr,sw,sw64,wait_us,publish}; }
static void smmu_reset(struct model *m)
{
    *m = (struct model){0}; memset(regs,0,sizeof(regs));
    regs[0x20/4] = BIT(30) | BIT(27) | 8;
    regs[0x24/4] = (3u << 28) | 8; /* GR0/1 4K; CBs start at 64K */
    regs[0x28/4] = BIT(12) | 5; regs[0] = BIT(10);
    /* Leave an unrelated inherited stream and enabled context untouched. */
    regs[0x800/4] = BIT(31) | 1;
    regs[0xc00/4] = 2;
    regs[(0x10000 + 2 * 4096)/4] = 1;
    regs[(0x10000 + 2 * 4096 + 0x24)/4] = 1u << 16;
}
static uint64_t descriptor(const struct mg_page_tables *t, uint32_t va)
{
    const uint64_t *p = t->memory;
    const unsigned shift[] = {30,21,12};
    for (unsigned level = 0; level < 3; level++) {
        uint64_t d = p[(va >> shift[level]) & 511];
        if (!(d & 1)) return 0;
        uint64_t pa = d & 0x0000fffffffff000ull;
        if (level == 2) return d;
        assert(pa >= t->pa && pa - t->pa < t->used * 4096u);
        p = (const uint64_t *)((const char *)t->memory + pa - t->pa);
    }
    abort();
}
/* ARM AArch64 stage-1 permission rule: EL0-writable pages are implicitly
 * PXN for a privileged instruction fetch, independently of descriptor PXN. */
static bool privileged_fetch(uint64_t pte)
{
    bool user_write = (pte & BIT(6)) && !(pte & BIT(7));
    return (pte & 3) == 3 && (pte & BIT(10)) &&
        !(pte & (1ull << 53)) && !user_write;
}
static uint64_t translate(const struct mg_page_tables *t, uint32_t va)
{
    uint64_t pte = descriptor(t,va);
    return pte ? (pte & 0x0000fffffffff000ull) | (va & 4095) : UINT64_MAX;
}
static void test_tables(void)
{
    struct mg_page_tables t = {tables,0x90000000,sizeof(tables),0,false};
    struct mg_dma_range r[] = {{0x4000,0x8000,0x100000000,true},
        {0x60000000,0x10000,0xa0000000,false},
        {0xc01ff000,0x3000,0xb0000000,false}};
    assert(mg_smmu_tables(&t,r,3) == 0 && t.built);
    for (unsigned i = 0; i < 3; i++) {
        assert(translate(&t,r[i].iova - 1) == UINT64_MAX);
        assert(translate(&t,r[i].iova + r[i].bytes) == UINT64_MAX);
        for (unsigned o = 0; o < r[i].bytes; o += 4096)
        {
            assert(translate(&t,r[i].iova + o) == r[i].pa + o);
            uint64_t pte = descriptor(&t,r[i].iova + o);
            assert(!(pte & (BIT(6) | BIT(7)))); /* privileged RW */
            assert(privileged_fetch(pte) == r[i].executable);
            if (r[i].executable) assert(!privileged_fetch(pte | BIT(6)));
            else assert((pte & (3ull << 53)) == (3ull << 53));
        }
    }
    assert(translate(&t,0) == UINT64_MAX);
    assert(mg_smmu_tables(&t,r,3) == MG_BUSY);
    t.built = false; t.bytes = 4096;
    assert(mg_smmu_tables(&t,r,3) == MG_TOO_SMALL && !t.built);
    t.bytes = sizeof(tables); r[1].pa = r[0].pa;
    assert(mg_smmu_tables(&t,r,3) == MG_INVALID);
    r[1].pa = t.pa; assert(mg_smmu_tables(&t,r,3) == MG_INVALID);
    r[1].pa = 1ull << 48; assert(mg_smmu_tables(&t,r,3) == MG_INVALID);
    r[1].pa = 0xa0000000; r[1].iova = 0xfffff000; r[1].bytes = 8192;
    assert(mg_smmu_tables(&t,r,3) == MG_INVALID);
    r[1].bytes = 4096; assert(mg_smmu_tables(&t,r,3) == 0);
    assert(translate(&t,UINT32_MAX) == r[1].pa + 4095);
}
static void test_smmu(void)
{
    struct model m; struct mg_smmu_io io = sio(&m);
    struct mg_page_tables t = {tables,0x90000000,sizeof(tables),0,false};
    struct mg_dma_range r = {0x60000000,0x4000,0xa0000000,false};
    assert(mg_smmu_tables(&t,&r,1) == 0);
    smmu_reset(&m); struct mg_smmu_domain s = {0};
    uint32_t original[sizeof(regs)/sizeof(regs[0])]; memcpy(original,regs,sizeof(regs));
    assert(mg_smmu_attach(&io,&s,&t) == 0 && s.attached && s.cb == 0 && s.asid == 2);
    assert(regs[0] == BIT(10) && regs[0x800/4] == original[0x800/4]);
    assert(regs[0xc00/4] == original[0xc00/4] && regs[0x12000/4] == 1);
    assert(regs[(0x800+4*s.smr)/4] == (BIT(31)|5));
    assert(m.publications == 1 && mg_smmu_attach(&io,&s,&t) == MG_BUSY);
    unsigned writes = m.writes, reads = m.reads;
    smmu_reset(&m);m.sctlr_raz=BIT(12);regs[0x24/4]=(4u<<28)|8;
    s=(struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t)==MG_OK && s.attached);
    assert(s.cb_base==0x20000 && regs[0x20000/4]==0x27); // exact device readback
    const unsigned mandatory[]={0,1,2,5};
    for(unsigned i=0;i<4;i++) {
        smmu_reset(&m);m.sctlr_raz=BIT(12)|BIT(mandatory[i]);
        s=(struct mg_smmu_domain){0};
        assert(mg_smmu_attach(&io,&s,&t)==MG_IO && !s.attached && s.quarantined);
    }

    for (unsigned i = 1; i <= reads; i++) {
        smmu_reset(&m); m.fail_read = i; s = (struct mg_smmu_domain){0};
        assert(mg_smmu_attach(&io,&s,&t) != 0 && !s.attached);
        if (s.touched) assert(s.quarantined && regs[0x10000/4] == 0);
    }
    for (unsigned i = 1; i <= writes; i++) {
        smmu_reset(&m); m.fail_write = i; s = (struct mg_smmu_domain){0};
        int ret = mg_smmu_attach(&io,&s,&t);
        /* Zero/idempotent/maintenance writes may be unobservable in a model. */
        if (ret) assert(s.quarantined && !s.attached && regs[0x10000/4] == 0);
        assert(regs[0x800/4] == original[0x800/4] && regs[0xc00/4] == original[0xc00/4]);
    }
    smmu_reset(&m); regs[0xc00/4] = 1u << 16; regs[0x800/4] = BIT(31)|5;
    s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_BUSY && !m.writes);
    smmu_reset(&m); regs[0x800/4] = BIT(31)|(7u<<16); /* mask includes SID5 */
    s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_BUSY && !m.writes);
    smmu_reset(&m); regs[0] = 0; s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_BUSY && !m.writes);
    smmu_reset(&m); regs[0x24/4] = BIT(31)|8; s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_UNSUPPORTED && !m.writes);
    smmu_reset(&m); regs[0x800/4] = BIT(31)|5; regs[0xc00/4] = 2u << 16;
    s = (struct mg_smmu_domain){0}; assert(mg_smmu_attach(&io,&s,&t) == 0);
    smmu_reset(&m); regs[0x10058/4] = 0xc0000010;
    s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == 0 && regs[0x10058/4] == 0);
    smmu_reset(&m); regs[0x28/4] = BIT(12); r.pa = 0x100000000ull;
    t.built = false; assert(mg_smmu_tables(&t,&r,1) == 0);
    s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_INVALID && !m.writes);
    r.pa = 0xa0000000; t.built = false; assert(mg_smmu_tables(&t,&r,1) == 0);
    smmu_reset(&m); m.stuck_tlb = true;
    /* Actual selected CB is zero (base 0x10000). */
    regs[0x107f4/4] = 1; s = (struct mg_smmu_domain){0};
    assert(mg_smmu_attach(&io,&s,&t) == MG_TIMEOUT && s.quarantined);
}
static struct mg_scm_result scm(void *p, uint32_t fn, uint32_t info, uint64_t arg)
{
    struct model *m = p; assert(info == 1 && !m->released);
    m->scm_calls++;
    assert((fn == 0x42000601 && arg == 0x02002801) || (fn == 0x42002801 && arg == 0xb));
    if (m->scm_calls == m->fail_scm) return (struct mg_scm_result){-12,99};
    return (struct mg_scm_result){0,fn == 0x42000601 ? !m->unavailable : 123};
}
static uint32_t gr(void *p, uint32_t reg)
{
    struct model *m = p; assert(reg < sizeof(gpu)/sizeof(gpu[0]));
    if (++m->reads == m->fail_read) {
        m->injected_mask_read = reg == 0x1f993;
        return UINT32_MAX;
    }
    return gpu[reg];
}
static void gw(void *p, uint32_t reg, uint32_t value)
{
    struct model *m = p; assert(reg < sizeof(gpu)/sizeof(gpu[0]));
    if (++m->writes == m->fail_write) return;
    if (reg == 0x1f800 && value == 0 && m->ignore_release) return;
    gpu[reg] = value;
    if (reg == 0x23b06) gpu[reg] &= 0x7fu & ~m->ao_mask_raz;
    if (reg == 0x1f800 && value == 0) {
        assert(m->prepared && m->scm_calls == 2); m->released++;
        assert(gpu[0x1f9ca] == 0x08060000 && gpu[0x1f985] == 0x60000000);
        if (!m->stuck_cm3) gpu[0x1f81c] = 0x100;
    }
    if (reg == 0x1f986 && value == 1 && !m->stuck_hfi) gpu[0x1f980] = 1;
}
static int ready(void *p) { return ((struct model *)p)->prerequisites; }
static int prepare(void *p)
{
    struct model *m = p; assert(gpu[0x1f800] == 1 && !m->scm_calls);
    m->prepared++; return m->prepare_error;
}
static void gmu_reset(struct model *m)
{
    *m = (struct model){0}; memset(gpu,0,sizeof(gpu)); gpu[0x1f800] = 1;
    gpu[0x1c400+0xff8] = 0x50040001; gpu[0x1c400+0xffc] = 0x20070001;
}
static void test_start(void)
{
    struct model m; gmu_reset(&m);
    struct mg_start_io io = {&m,gr,gw,wait_us,ready,prepare,scm};
    struct mg_start_config c = {0x60000000,0x60004000,0x4000,0x60008000};
    struct mg_start_status s = {0};
    assert(mg_gmu_start(&io,&c,&s) == 0 && s.stage == MG_START_HFI && s.released);
    unsigned reads = m.reads;
    unsigned writes = m.writes;
    assert(mg_gmu_start(&io,&c,&s) == MG_BUSY);
    assert(gpu[0x23b06] == 0x7f); /* Exact device readback must reach HFI. */
    for (unsigned bit = 0; bit < 7; bit++) {
        gmu_reset(&m); m.ao_mask_raz = BIT(bit); s = (struct mg_start_status){0};
        assert(mg_gmu_start(&io,&c,&s) == MG_IO && !m.released && s.quarantined);
        assert(s.stage == MG_START_SECURE && s.reg == 0x23b06);
        assert(s.value == (0x7fu & ~BIT(bit)) && gpu[0x1f800] == 1);
    }
    gmu_reset(&m); m.ao_mask_raz = 0x7f; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_IO && !m.released && s.quarantined);
    assert(s.reg == 0x23b06 && s.value == 0);
    for (unsigned i = 1; i <= reads; i++) {
        gmu_reset(&m); m.fail_read = i; s = (struct mg_start_status){0};
        int ret = mg_gmu_start(&io,&c,&s);
        /* Only the full-width HFI mask accepts all-ones readback. */
        if (m.injected_mask_read) { assert(ret == 0); continue; }
        assert(ret != 0);
        if (s.stage >= MG_START_RESET) assert(s.quarantined && gpu[0x1f800] == 1);
    }
    for (unsigned i = 1; i <= writes; i++) {
        gmu_reset(&m); m.fail_write = i; s = (struct mg_start_status){0};
        int ret = mg_gmu_start(&io,&c,&s);
        if (ret) assert(s.quarantined && gpu[0x1f800] == 1);
    }
    for (unsigned i = 1; i <= 2; i++) {
        gmu_reset(&m); m.fail_scm = i; s = (struct mg_start_status){0};
        assert(mg_gmu_start(&io,&c,&s) == MG_IO && !m.released && s.quarantined);
    }
    gmu_reset(&m); m.unavailable = true; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_UNSUPPORTED && !m.released && m.scm_calls == 1);
    gmu_reset(&m); m.prerequisites = MG_UNBACKED; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_UNBACKED && !m.reads && !m.writes);
    gmu_reset(&m); m.prepare_error = MG_UNBACKED; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_UNBACKED && !m.scm_calls && s.quarantined);
    gmu_reset(&m); gpu[0x1f800] = 0; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_BUSY && !m.writes);
    gmu_reset(&m); gpu[0x1f980] = 1; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_STALE && !m.released);
    gmu_reset(&m); m.stuck_cm3 = true; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_TIMEOUT && s.quarantined && gpu[0x1f800] == 1);
    assert(s.stage == MG_START_CONFIG && s.reg == 0x1f81c && s.value == 0);
    assert(s.poll_reads == 10000 && s.poll_first == 0 && s.snapshot_valid);
    assert(s.snapshot[0] == 0 && s.snapshot[1] == 0); /* Before reset, not after. */
    assert(!gpu[0x1f986]); /* HFI must never start on the exact device failure. */
    gmu_reset(&m); m.ignore_release = true; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_IO && s.quarantined);
    assert(s.reg == 0x1f800 && s.value == 1 && !s.poll_reads && s.snapshot[0] == 1);
    assert(!gpu[0x1f986]);
    gmu_reset(&m); m.stuck_hfi = true; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_TIMEOUT && s.stage == MG_START_CM3 && s.quarantined);
    gmu_reset(&m); c.log = c.hfi; s = (struct mg_start_status){0};
    assert(mg_gmu_start(&io,&c,&s) == MG_INVALID && !m.reads && !m.writes);
}
static _Alignas(4096) uint8_t backing[0x10000];
static void load_tcm(void *p, uint32_t reg, uint32_t value)
{
    struct model *m = p;
    assert((reg >= 0x1b400 && reg < 0x1c400) || (reg >= 0x1c400 && reg < 0x1d400));
    m->writes++; gpu[reg] = value;
}
static void load_publish(void *p, const void *memory, size_t bytes)
{
    struct model *m = p; assert(memory == backing && bytes == sizeof(backing));
    m->publications++;
}
static void test_load(void)
{
    /* Reservation metadata, then external and both TCM payload records. */
    uint32_t image[] = {0x4000,0,1,0x10000, 0x4000,4,0,0,0xfeedface,
        0,4,0,0,0x12345678, 0x10004000,4,0,0,0xabcdef01};
    struct mg_fw_plan plan;
    struct model m = {0};
    struct mg_gmu_backing b = {0x4000,sizeof(backing),backing};
    struct mg_gmu_loader l = {&m,load_tcm,load_publish,&b,1};
    memset(backing,0xaa,sizeof(backing));
    assert(mg_gmu_load(image,sizeof(image),&plan,&l) == 0);
    assert(m.writes == 2 && m.publications == 1);
    assert(gpu[0x1b400] == 0x12345678 && gpu[0x1c400] == 0xabcdef01);
    uint32_t word; memcpy(&word,backing,4); assert(word == 0xfeedface);
    for (unsigned i = 4; i < sizeof(backing); i++) assert(backing[i] == 0);
    m = (struct model){0}; b.bytes = 4096; memset(backing,0xaa,sizeof(backing));
    assert(mg_gmu_load(image,sizeof(image),&plan,&l) == MG_UNBACKED);
    assert(!m.writes && !m.publications && backing[0] == 0xaa);
    b.bytes = sizeof(backing); b.iova = 0;
    assert(mg_gmu_load(image,sizeof(image),&plan,&l) == MG_INVALID && !m.writes);
    b.iova = 0x4000;
    assert(mg_gmu_load(image,sizeof(image)-1,&plan,&l) != 0 && !m.writes);
    assert(mg_gmu_load(&plan,sizeof(plan),&plan,&l) == MG_INVALID && !m.writes);
}
static void real_publish(void *p, const void *memory, size_t n)
{ (void)p; assert(memory && n && !(n & 4095)); }
static void test_real(const char *path)
{
    FILE *f = fopen(path,"rb"); assert(f);
    assert(fseek(f,0,SEEK_END) == 0); long n = ftell(f);
    assert(n > 0 && n <= MG_FW_MAX_BYTES && fseek(f,0,SEEK_SET) == 0);
    void *image = malloc((size_t)n); assert(image);
    assert(fread(image,1,(size_t)n,f) == (size_t)n); fclose(f);
    struct mg_fw_plan plan; assert(mg_fw_parse(image,(size_t)n,&plan) == 0);
    struct mg_gmu_backing buffers[MG_DMA_MAX_RANGES];
    struct mg_dma_range ranges[MG_DMA_MAX_RANGES];
    size_t count = 0;
    for (unsigned i = 0; i < plan.reservation_count; i++) {
        const struct mg_fw_reservation *r = &plan.reservations[i];
        if (r->region == MG_ITCM || r->region == MG_DTCM) continue;
        assert(count < MG_DMA_MAX_RANGES);
        uint32_t va = r->address & ~4095u;
        uint32_t bytes = (r->address - va + r->size + 4095) & ~4095u;
        void *memory = aligned_alloc(4096,bytes); assert(memory);
        buffers[count] = (struct mg_gmu_backing){va,bytes,memory};
        ranges[count] = (struct mg_dma_range){va,bytes,0x100000000ull + count * 0x100000,
            r->region == MG_CACHE};
        count++;
    }
    struct model m = {0};
    struct mg_gmu_loader loader = {&m,load_tcm,real_publish,buffers,count};
    assert(mg_gmu_load(image,(size_t)n,&plan,&loader) == 0);
    for (unsigned i = 0; i < plan.count; i++) {
        struct mg_fw_block *b = &plan.blocks[i];
        if (b->region == MG_ITCM || b->region == MG_DTCM) continue;
        bool matched = false;
        for (size_t j = 0; j < count; j++)
            if (b->address >= buffers[j].iova &&
                (uint64_t)b->address + b->size <= (uint64_t)buffers[j].iova + buffers[j].bytes) {
                assert(memcmp((uint8_t *)buffers[j].cpu + b->address - buffers[j].iova,
                    (uint8_t *)image + b->offset,b->size) == 0);
                matched = true;
            }
        assert(matched);
    }
    struct mg_page_tables t = {tables,0x90000000,sizeof(tables),0,false};
    assert(mg_smmu_tables(&t,ranges,count) == 0);
    uint64_t code = descriptor(&t,0x4000);
    assert(privileged_fetch(code));
    assert(!privileged_fetch(code | BIT(6))); /* exact old mapping fault */
    for (size_t i = 0; i < count; i++) {
        assert(translate(&t,ranges[i].iova) == ranges[i].pa);
        free(buffers[i].cpu);
    }
    free(image);
    printf("PASS: local GMU firmware loaded into mock TCM + %zu external buffers; %u table pages\n",count,t.used);
}
int main(int argc, char **argv)
{
    test_tables(); test_smmu(); test_start(); test_load();
    assert(argc <= 2); if (argc == 2) test_real(argv[1]);
    puts("PASS: GPU stream-5 tables/ownership/attach and secure CM3/HFI startup failure models");
    return 0;
}
