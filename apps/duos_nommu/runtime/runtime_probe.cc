// Linux NOMMU C++ runtime smoke test; ABI selected by build, used as PID 1.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#ifdef DUOS_HARDFLOAT
#define DUOS_ABI_LABEL "LP64D hardware FP"
#else
#define DUOS_ABI_LABEL "LP64 software FP"
#endif
int main()
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd < 0) return 1;
    dprintf(fd, "<6>duos-cxx: begin C++ runtime ABI=%s\n",DUOS_ABI_LABEL);
    bool ok = true;
    std::vector<unsigned> values(1024);
    for (unsigned i = 0; i < values.size(); ++i) values[i] = i * 37;
    for (unsigned i = 0; i < values.size(); ++i) ok &= values[i] == i * 37;
    std::string text = "C++ allocation";
    volatile double input = 2.0;
    double root = std::sqrt(input);
    ok &= root > 1.414 && root < 1.415;
    try { throw std::runtime_error("exception probe"); }
    catch (const std::exception& e) { ok &= std::string(e.what()) == "exception probe"; }
    timespec now{};
    ok &= clock_gettime(CLOCK_MONOTONIC, &now) == 0;
    dprintf(fd, "<6>duos-cxx: %s pid=%ld %s math=%.6f seconds=%ld\n",
            ok ? "PASS" : "FAIL", (long)getpid(), text.c_str(), root, now.tv_sec);
    // PID 1 must not exit. Keep diagnostic heartbeat slow enough for /dev/kmsg.
    for (;;) { sleep(5); dprintf(fd, "<6>duos-cxx: alive runtime=%s\n", ok ? "PASS" : "FAIL"); }
}
