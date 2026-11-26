/* Standalone, one-shot Gen8.6.0 owner. No storage or XNU services. */
#include "runtime.h"
#include "metroid_gpu_accel.h"
#include "screen.h"
#include "../../metroid-loader/memory_layout.h"

#define ARENA_BYTES (128u*1024u*1024u)
#define ZAP_PA 0x94e9a000ull
#define REGISTER_BYTES 0xa0000u
extern unsigned char _head[], _image_end[];
extern const unsigned char fw_gmu[], fw_gmu_end[], fw_sqe[], fw_sqe_end[];
extern const unsigned char fw_aqe[], fw_aqe_end[], fw_zap[], fw_zap_end[];
extern const char *metroid_check_fdt(const void *);
static struct mg_live live;
static struct mg_accel accelerator;
static struct mg_gen8_job pixel_job;
static size_t arena_used;
static uint64_t last_address;
static unsigned exception_active, frames;
static bool console_ready;

static uint64_t counter(void)
{ uint64_t t; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t)); return t; }
static uint32_t read32(uint64_t pa)
{
    last_address=pa;
    uint32_t value=*(volatile uint32_t *)(uintptr_t)pa;
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void write32(uint64_t pa, uint32_t value)
{
    last_address=pa;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)pa=value;
    __asm__ volatile("dsb osh" ::: "memory");
}
static uint32_t gpu_read(void *p, uint32_t reg)
{ (void)p; return reg<REGISTER_BYTES/4 ? read32(MG_GPU_PA+4*reg) : UINT32_MAX; }
static void gpu_write(void *p, uint32_t reg, uint32_t value)
{ (void)p; if(reg<REGISTER_BYTES/4) write32(MG_GPU_PA+4*reg,value); }
static uint32_t power_read(void *p, enum mg_bus bus, uint32_t off)
{ (void)p; return read32((bus==MG_CC ? MG_GPUCC_PA : 0x100000)+off); }
static void power_write(void *p, enum mg_bus bus, uint32_t off, uint32_t value)
{ (void)p; write32((bus==MG_CC ? MG_GPUCC_PA : 0x100000)+off,value); }
static uint32_t smmu_read(void *p, uint32_t off)
{ (void)p; return off<=MG_SMMU_SIZE-4 ? read32(MG_SMMU_PA+off) : UINT32_MAX; }
static void smmu_write(void *p, uint32_t off, uint32_t value)
{ (void)p; if(off<=MG_SMMU_SIZE-4) write32(MG_SMMU_PA+off,value); }
static void smmu_write64(void *p, uint32_t off, uint64_t value)
{
    (void)p;
    if(off>MG_SMMU_SIZE-8 || (off&7)) return;
    last_address=MG_SMMU_PA+off;
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint64_t *)(uintptr_t)last_address=value;
    __asm__ volatile("dsb osh" ::: "memory");
}
static void wait_us(void *p, unsigned us) { (void)p; delay(us); }
static void publish(void *p, const void *cpu, size_t bytes)
{
    /* Entry requires MMU and D-cache off and never enables either. All
     * allocations have one physical CPU alias and remain pinned to reboot. */
    (void)p; (void)cpu; (void)bytes;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int allocate(void *p, size_t bytes, struct mg_memory_block *out)
{
    (void)p;
    if(!out || !bytes || bytes>ARENA_BYTES || (bytes&(MG_MEMORY_PAGE-1)) ||
        bytes>ARENA_BYTES-arena_used) return MG_LIMIT;
    uint64_t pa=METROID_RAM_BASE+arena_used;
    arena_used+=bytes;
    memset((void *)(uintptr_t)pa,0,bytes);
    *out=(struct mg_memory_block){(void *)(uintptr_t)pa,(void *)(uintptr_t)pa,pa,bytes};
    return MG_OK;
}
static void release(void *p, struct mg_memory_block *block)
{ (void)p; *block=(struct mg_memory_block){0}; /* Never recycle backing. */ }
static int rails(void *p) { (void)p; return metroid_gpu_rpmh_enable() ? MG_IO : MG_OK; }
static int fabric(void *p) { (void)p; return metroid_gpu_rpmh_fabric_enable() ? MG_IO : MG_OK; }
static bool fabric_ready(void *p) { (void)p; return metroid_gpu_rpmh_fabric_ready(); }
static int configure(void *p)
{
    (void)p;
    uint32_t reset=power_read(NULL,MG_CC,0x9314), cm3=gpu_read(NULL,0x1f800);
    if(reset==UINT32_MAX || cm3==UINT32_MAX) return MG_IO;
    if((reset&1) || cm3!=1) return MG_STALE;
    uint32_t vote=power_read(NULL,MG_CC,0x7000);
    if(vote==UINT32_MAX) return MG_IO;
    power_write(NULL,MG_CC,0x7000,vote|1);
    unsigned us;
    for(us=0;us<1000;us++) {
        vote=power_read(NULL,MG_CC,0x7000);
        if(vote==UINT32_MAX) return MG_IO;
        if(mg_clock_running(vote)) break;
        delay(1);
    }
    if(us==1000) return MG_TIMEOUT;
    for(uint32_t reg=0x27810;reg<=0x27814;reg++) {
        gpu_write(NULL,reg,0x1b13);
        if(gpu_read(NULL,reg)!=0x1b13) return MG_IO;
    }
    uint64_t skew=0;
    for(unsigned i=0;i<16;i++) {
        uint64_t a=counter(), b=counter();
        gpu_write(NULL,0x27880,(uint32_t)b); gpu_write(NULL,0x27881,(uint32_t)(b>>32));
        uint64_t c=counter();
        if(c-b>b-a) skew+=(c-b)-(b-a);
    }
    uint64_t seed=counter()+(skew+8)/16;
    gpu_write(NULL,0x27880,(uint32_t)seed); gpu_write(NULL,0x27881,(uint32_t)(seed>>32));
    return MG_OK;
}
static bool powered(void *p)
{
    (void)p;
    if(live.stage!=MG_LIVE_HFI || live.quarantined) return false;
    uint32_t cx=power_read(NULL,MG_CC,0x9094);
    return cx!=UINT32_MAX && (cx&0x80000000) &&
        mg_clock_running(power_read(NULL,MG_CC,0x90d4)) &&
        mg_clock_running(power_read(NULL,MG_CC,0x7000));
}
static int synchronize(void *p, enum mg_gpu_sync direction, const void *cpu, size_t bytes)
{
    (void)p;
    uintptr_t pa=(uintptr_t)cpu;
    if(!powered(NULL) || direction>MG_GPU_TO_CPU || !bytes || pa<METROID_RAM_BASE ||
        pa-METROID_RAM_BASE>=arena_used || bytes>arena_used-(pa-METROID_RAM_BASE)) return MG_UNBACKED;
    __asm__ volatile("dsb sy" ::: "memory");
    return MG_OK;
}
static int scheduler_tables(void *p, uint32_t *perf, uint32_t *bw)
{ (void)p; return metroid_gpu_rpmh_scheduler_tables(perf,bw) ? MG_IO : MG_OK; }
static void trace(void *p, const char *phase)
{
    (void)p;
    printf("GPU: %s\n",phase);
    gpu_screen_present();
}
__attribute__((noreturn)) static void stop(void)
{ for(;;) __asm__ volatile("wfe"); }
void probe_exception(uint64_t slot)
{
    if(exception_active++ || !console_ready) stop();
    uint64_t esr,far,elr;
    __asm__ volatile("mrs %0, esr_el1; mrs %1, far_el1; mrs %2, elr_el1" : "=r"(esr),"=r"(far),"=r"(elr));
    printf("EXCEPTION slot=%llu\n",slot);
    printf("ESR=%#llx FAR=%#llx\n",esr,far);
    printf("ELR=%#llx last-mmio=%#llx\n",elr,last_address);
    gpu_screen_present(); stop();
}
static void report(int result)
{
    if(live.start.firmware_mismatch)
        printf("TCM expected=%#x\n",live.start.firmware_expected);
    printf("HFI=%#x log=%#x\n",live.memory.config.hfi,live.memory.config.log);
    printf("tables PA=%#llx CB=%u\n",(unsigned long long)live.memory.tables.pa,live.domain.cb);
    if(live.start.snapshot_valid) {
        printf("GMU snapshot BEFORE reset\n");
        for(unsigned i=0;i<MG_START_SNAPSHOT_WORDS;i++)
            printf("gmu[%#x]=%#x\n",mg_start_snapshot_registers[i],live.start.snapshot[i]);
    }
    if(live.fault_snapshot_valid) {
        const char *names[]={"GFSR","GFSYNR0","GFSYNR1","FSR","FARlo","FARhi","FSYNR0","FSYNR1"};
        printf("SMMU faults AFTER reset\n");
        for(unsigned i=0;i<8;i++) printf("%s=%#x\n",names[i],live.fault_snapshot[i]);
    }
    if(live.start.released) for(unsigned i=0;i<live.memory.count;i++) {
        const struct mg_gmu_backing *b=&live.memory.backing[i];
        if(b->iova!=live.memory.config.log || b->bytes<32) continue;
        const volatile uint32_t *log=b->cpu;
        printf("GMU log raw first 8 words\n");
        for(unsigned j=0;j<8;j++) printf("log[%u]=%#x\n",j,log[j]);
    }
    printf("RESULT err=%d live=%u power=%u start=%u\n",result,live.stage,live.power.stage,live.start.stage);
    printf("power-reg=%#x/%#x\n",live.power.offset,live.power.value);
    printf("SMMU=%#x/%#x start-reg=%#x/%#x\n",live.domain.offset,live.domain.value,live.start.reg,live.start.value);
    printf("accel=%u sched=%u hw=%u zap=%u\n",accelerator.stage,accelerator.scheduler.stage,accelerator.hardware.stage,accelerator.zap.stage);
    printf("hw-reg=%#x/%#x submit-reg=%#x/%#x\n",accelerator.hardware.reg,accelerator.hardware.value,accelerator.backend.fault_register,accelerator.backend.fault_value);
    printf("sched-reg=%#x/%#x\n",accelerator.scheduler.fault_register,accelerator.scheduler.fault_value);
    printf("TCM verified words=%u\n",live.start.firmware_words);
    printf("poll reads=%u first=%#x\n",live.start.poll_reads,live.start.poll_first);
    printf("pixels verified frames=%u\n",frames);
    printf("Held until manual restart. No storage accessed.\n");
    gpu_screen_present();
}
static unsigned packet7(unsigned opcode, unsigned count)
{
    unsigned cp=1,op=1;
    for(unsigned n=count;n;n>>=1) cp^=n&1;
    for(unsigned n=opcode;n;n>>=1) op^=n&1;
    return 0x70000000u|count|(cp<<15)|(opcode<<16)|(op<<23);
}
static uint32_t color(unsigned pixel, unsigned frame)
{
    const uint32_t colors[]={0xff0000ff,0xff00ff00,0xffff0000,0xffffffff};
    return colors[((pixel%32)/8+(pixel/32)/8+frame)&3];
}
static void display_pixels(const uint32_t *pixels)
{
    gpu_screen_pixels=pixels;
    gpu_screen_frames=frames;
    gpu_screen_present();
}
static int pixels_test(void)
{
    struct mg_render *r=&accelerator.render;
    struct mg_accel_block *cmd=accelerator.commands, *out=accelerator.output;
    uint32_t *commands=cmd->memory.cpu, *pixels=out->memory.cpu;
    uint32_t command_address=0,output_address=0;
    for(unsigned i=0;i<MG_RENDER_BUFFERS;i++) {
        if(r->mappings[i].backing.cpu==commands) command_address=r->mappings[i].address;
        if(r->mappings[i].backing.cpu==pixels) output_address=r->mappings[i].address;
    }
    if(!r->verified || !command_address || !output_address || cmd->memory.bytes<4112 || out->memory.bytes<4096) return MG_UNBACKED;
    for(unsigned frame=0;frame<16;frame++) {
        memset(pixels,0,4096);
        commands[0]=packet7(0x3d,1026); commands[1]=output_address; commands[2]=0;
        for(unsigned i=0;i<1024;i++) commands[3+i]=color(i,frame);
        commands[1027]=packet7(0x26,0);
        pixel_job=(struct mg_gen8_job){.buffer_count=2,.ib_count=1};
        pixel_job.buffers[0]=(struct mg_gen8_buffer){commands,command_address,cmd->memory.bytes,MG_GPU_READ|MG_GPU_EXEC};
        pixel_job.buffers[1]=(struct mg_gen8_buffer){pixels,output_address,out->memory.bytes,MG_GPU_READ|MG_GPU_WRITE};
        pixel_job.ibs[0]=(struct mg_gen8_ib){0,0,4112};
        uint64_t fence=0;
        int error=r->ops.submit(r->ops.context,&pixel_job,&fence);
        if(!error) error=r->ops.wait(r->ops.context,fence,MG_QUEUE_MAX_WAIT_US);
        if(error) return error;
        for(unsigned i=0;i<1024;i++) if(pixels[i]!=color(i,frame)) return MG_IO;
        frames++; display_pixels(pixels);
    }
    return MG_OK;
}
void probe_main(uint64_t dtb, uint64_t entry_el, uint64_t sctlr)
{
    printf("METROID GPU PROBE v6\n");
    printf("Shared driver core; no XNU or storage\n");
    console_ready=true;
    if(!gpu_screen_present()) stop();
    trace(NULL,"entry system registers");
    uint64_t current,hz;
    __asm__ volatile("mrs %0, CurrentEL; mrs %1, cntfrq_el0" : "=r"(current),"=r"(hz));
    if(current!=4 || (sctlr&5) || !hz || hz>1000000000ull) {
        printf("STOP: entry EL/MMU/cache/timer invalid\n"); gpu_screen_present(); stop();
    }
    (void)entry_el;
    trace(NULL,"entry checks passed");
    /* Before any arena write, reuse loader's complete live FDT RAM and
     * reservation checks, including initrd and FDT overlap refusal. */
    printf("FDT address=%#llx\n",dtb);
    trace(NULL,"validate live FDT / private RAM");
    const char *error=metroid_check_fdt((const void *)(uintptr_t)dtb);
    uint64_t start=(uintptr_t)_head,end=(uintptr_t)_image_end;
    if(error || end<start || (start<METROID_RAM_BASE+METROID_RAM_SIZE && METROID_RAM_BASE<end) ||
        (start<ZAP_PA+0x2000 && ZAP_PA<end)) {
        printf("STOP: memory preflight %s\n",error ? error : "Image overlaps owned RAM/ZAP");
        gpu_screen_present(); stop();
    }
    trace(NULL,"validate ZAP metadata");
    struct mg_zap_plan zap;
    if(mg_zap_parse(fw_zap,fw_zap_end-fw_zap,&zap) || zap.span>0x2000) {
        printf("STOP: ZAP firmware/carveout invalid\n"); gpu_screen_present(); stop();
    }
    const struct mg_live_io io={
        .memory={NULL,allocate,release},.power={NULL,power_read,power_write,wait_us,rails},
        .smmu={NULL,smmu_read,smmu_write,smmu_write64,wait_us,publish},
        .retain_fabric=fabric,.fabric_ready=fabric_ready,.configure=configure,
        .read=gpu_read,.write=gpu_write,.delay=wait_us,.publish=publish,.scm=mg_scm_native,.trace=trace,
    };
    int result=mg_live_boot(&live,&io,fw_gmu,fw_gmu_end-fw_gmu);
    if(!result) {
        const struct mg_accel_io aio={NULL,powered,synchronize,scheduler_tables,(void *)(uintptr_t)ZAP_PA,ZAP_PA,0x2000,mg_scm_pas_native};
        const struct mg_accel_images images={fw_sqe,fw_aqe,fw_zap,fw_sqe_end-fw_sqe,fw_aqe_end-fw_aqe,fw_zap_end-fw_zap};
        result=mg_accel_boot(&accelerator,&live,&aio,&images);
    }
    if(!result) { trace(NULL,"Fenced pixel pattern (not a shader test)"); result=pixels_test(); }
    report(result); stop();
}
