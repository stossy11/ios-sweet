/* SPDX-License-Identifier: GPL-2.0-only */
#include "metroid_gpu_scheduler.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static _Alignas(4096) uint8_t hfi[16384];
static struct model {
    struct mg_scheduler scheduler;
    struct mg_hfi_queue command, message, debug;
    uint32_t start, last_request, sequence;
    unsigned commands, allocations, boots, delay, reads, fail_command;
    unsigned inject;
    bool ready, omit_ack;
} m;
static bool ready(void *p) { (void)p; return m.ready; }
static uint32_t read_reg(void *p, uint32_t reg)
{
    (void)p; assert(m.ready); m.reads++;
    if (m.inject == 8 && reg == 0x23b05) return 8;
    if (reg == 0x1f980) return 1;
    if (reg == 0x1f81c) return 0x100;
    return 0;
}
static void enqueue(struct mg_hfi_queue *q, uint32_t *p, unsigned count, unsigned id, unsigned type)
{ assert(!mg_hfi_message(id,type,++m.sequence,count,p)); assert(!mg_hfi_write(q,p,count)); }
static void ack(uint32_t request)
{
    if (m.omit_ack) return;
    uint32_t p[] = {0,request,m.commands == m.fail_command ? UINT32_MAX : 0};
    if (m.inject == 1) p[1] ^= 1u << 20;
    enqueue(&m.message,p,3,request & 255,1);
    if (m.inject == 2) { uint32_t fault[] = {0,9}; enqueue(&m.debug,fault,2,100,0); }
}
static void write_reg(void *p, uint32_t reg, uint32_t value)
{
    (void)p;
    if (reg == 0x1f991) { assert(value == 1); return; }
    assert(reg == 0x1f994 && value == 1);
    uint32_t packet[255]; int words = mg_hfi_read(&m.command,packet,255); assert(words > 0);
    unsigned id = packet[0] & 255, type = (packet[0] >> 16) & 15;
    if (id == 20 && type == 1) {
        assert(words == 11 && packet[1] == m.last_request);
        assert(packet[2] == 0x4000 && packet[3] == 1 && packet[6] == 7 && packet[7] == 7);
        assert(packet[8] == 0x60020000 && packet[9] == 16384);
        ack(m.start); return;
    }
    assert(type == 0); m.commands++;
    if (id == 10) {
        m.start = packet[0];
        uint32_t request[] = {0,1,0,0,0x3f,0,0,7,0,16384,14};
        if (m.inject == 3) request[4] |= 1u << 12;
        if (m.inject == 4) request[10] = 31;
        if (m.inject == 5) request[5] = 11;
        enqueue(&m.message,request,11,20,0); m.last_request = request[0];
    } else {
        if (id == 131) assert(m.scheduler.booting_gpu && words == 3 && packet[1] == 0x70000000);
        ack(packet[0]);
    }
}
static void delay_us(void *p, unsigned us) { (void)p; m.delay += us; }
static int allocate(void *p, struct mg_sched_allocation *a)
{
    (void)p; assert(a->bytes == 16384 && a->flags == 0x3f && a->firmware_handle == 7);
    m.allocations++;
    if (m.inject == 6) return MG_LIMIT;
    a->gpu = 0x100004000ull; a->gmu = m.inject == 7 ? 1 : 0x60020000;
    return MG_OK;
}
static int boot_gpu(void *p)
{
    (void)p; m.boots++;
    const uint32_t pm4[] = {0x70000000,0};
    return mg_scheduler_raw(&m.scheduler,pm4,2);
}
static const struct mg_sched_io io = {NULL,ready,read_reg,write_reg,delay_us,allocate,boot_gpu};
static const uint32_t perf[] = {15,0,1,1,3,0x1234,0,300000,1,2,0x4567,500000};
static const uint32_t bw[160] = {3,1,1,1,1,1,0x50020,[24]=0x50024};
static const struct mg_sched_config config = {hfi,sizeof(hfi),0x60000000,0x20070001,
    {perf,sizeof(perf)/4},{bw,sizeof(bw)/4}};
static void reset(void)
{
    memset(&m,0,sizeof(m)); m.ready = true;
    assert(!mg_hfi_init(hfi,sizeof(hfi),0x60000000));
    assert(!mg_hfi_bind(hfi,sizeof(hfi),0x60000000,0,&m.command));
    assert(!mg_hfi_bind(hfi,sizeof(hfi),0x60000000,1,&m.message));
    assert(!mg_hfi_bind(hfi,sizeof(hfi),0x60000000,2,&m.debug));
}
int main(void)
{
    reset();
    assert(mg_scheduler_boot(&m.scheduler,&io,&config) == MG_OK);
    assert(m.scheduler.stage == MG_SCHED_GPU && m.allocations == 1 && m.boots == 1);
    assert(!m.scheduler.quarantined && m.scheduler.allocation_count == 1 && m.commands == 12);
    assert(m.scheduler.sequence == 13 && m.scheduler.allocations[0].host_handle == 7);
    assert(mg_scheduler_boot(&m.scheduler,&io,&config) == MG_BUSY);
    uint32_t pm4[] = {0}; assert(mg_scheduler_raw(&m.scheduler,pm4,1) == MG_INVALID);
    for (unsigned inject = 1; inject <= 8; inject++) {
        reset(); m.inject = inject;
        assert(mg_scheduler_boot(&m.scheduler,&io,&config) != MG_OK);
        assert(m.scheduler.quarantined && m.scheduler.error && m.scheduler.stage != MG_SCHED_GPU);
        if (inject >= 3 && inject <= 5) {
            assert(m.scheduler.error == MG_UNSUPPORTED && m.scheduler.stage == MG_SCHED_CORE);
            assert(m.scheduler.allocation_request_words == 11);
            assert(m.scheduler.allocation_request[0] == m.last_request);
            assert(m.scheduler.allocation_request[7] == 7 && m.scheduler.allocation_request[9] == 16384);
            if (inject == 3) assert(m.scheduler.allocation_request[4] == (0x3f | (1u << 12)));
            if (inject == 4) assert(m.scheduler.allocation_request[10] == 31);
            if (inject == 5) assert(m.scheduler.allocation_request[5] == 11);
        }
        if (inject <= 5 || inject == 8) assert(!m.allocations);
        if (inject == 8) assert(!m.commands);
    }
    for (unsigned failure = 1; failure <= 12; failure++) {
        reset(); m.fail_command = failure;
        assert(mg_scheduler_boot(&m.scheduler,&io,&config) == MG_IO && m.scheduler.quarantined);
    }
    reset(); m.omit_ack = true;
    assert(mg_scheduler_boot(&m.scheduler,&io,&config) == MG_TIMEOUT && m.delay == 1000000);
    reset(); m.ready = false;
    assert(mg_scheduler_boot(&m.scheduler,&io,&config) == MG_UNBACKED && !m.reads && !m.commands);
    reset(); struct mg_sched_config bad = config; bad.version = 0;
    assert(mg_scheduler_boot(&m.scheduler,&io,&bad) == MG_INVALID && !m.scheduler.attempted);
    puts("PASS: Gen8 scheduler table/feature/START wire, memory requests, raw CP commands, ACK/fault/timeout quarantine");
    return 0;
}
