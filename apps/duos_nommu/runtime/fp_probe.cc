// LP64 soft ABI with explicit F/D instructions: isolate hardware and kernel state.
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <cstdint>
static bool arithmetic()
{
    uint64_t d;
    uint32_t f;
    const uint64_t a=UINT64_C(0x3ff8000000000000), b=UINT64_C(0x4000000000000000);
    asm volatile("fmv.d.x ft0,%1; fmv.d.x ft1,%2; fadd.d ft2,ft0,ft1; fmv.x.d %0,ft2"
                 : "=r"(d) : "r"(a),"r"(b) : "ft0","ft1","ft2");
    if(d!=UINT64_C(0x400c000000000000)) return false; // 1.5+2.0=3.5
    asm volatile("fmv.w.x ft0,%1; fmv.w.x ft1,%2; fmul.s ft2,ft0,ft1; fmv.x.w %0,ft2"
                 : "=r"(f) : "r"(0x3fc00000u),"r"(0x40000000u) : "ft0","ft1","ft2");
    return f==0x40400000u; // 1.5*2.0=3.0
}
int main()
{
    int fd=open("/dev/kmsg",O_WRONLY);
    if(fd<0) return 1;
    auto caps=getauxval(AT_HWCAP);
    dprintf(fd,"<6>duos-fp: begin LP64 explicit FP32/FP64 hwcap=%lx\n",caps);
    if(!(caps&(1ul<<('f'-'a'))) || !(caps&(1ul<<('d'-'a')))) {
        dprintf(fd,"<3>duos-fp: FAIL kernel does not advertise F and D\n");
        for(;;) sleep(10);
    }
    bool ok=arithmetic();
    dprintf(fd,"<6>duos-fp: arithmetic %s\n",ok?"PASS":"FAIL");
    for(unsigned i=0;i<100 && ok;++i) {
        // Change values every time so stale restoration cannot pass the check.
        uint64_t want64=UINT64_C(0x3ff8000000000000)+i, got64;
        uint32_t want32=0x40100000u+i, got32, csr;
        asm volatile("fmv.d.x fs0,%0; fmv.w.x fs1,%1; fscsr %2"
                     :: "r"(want64),"r"(want32),"r"(64u) : "fs0","fs1","memory"); // round down
        usleep(20000); // force scheduler switches to other kernel tasks
        asm volatile("fmv.x.d %0,fs0; fmv.x.w %1,fs1; frcsr %2"
                     : "=r"(got64),"=r"(got32),"=r"(csr) :: "memory");
        ok=got64==want64 && got32==want32 && csr==64u;
        asm volatile("fscsr zero" ::: "memory");
        if(!ok) dprintf(fd,"<3>duos-fp: FAIL context iteration=%u\n",i);
    }
    dprintf(fd,"<6>duos-fp: DONE %s arithmetic and 100 sleep/state checks\n",ok?"PASS":"FAIL");
    for(;;) { sleep(10); dprintf(fd,"<6>duos-fp: alive result=%s\n",ok?"PASS":"FAIL"); }
}
