// Horizon self-test: the libc, POSIX, libstdc++ and libnx calls the headless game makes on its way from NRO start into
// a level, run on the console against the SD card, each reported as one line over nxlink (and to
// sdmc:/switch/black/selftest.log):
//   PASS name: values          behaves as on Linux, or as the runtime needs
//   FAIL name: values (expect) does not
//   INFO name: values          a fact of this console and firmware the runtime adapts to; nothing is expected
// Works in sdmc:/switch/black/selftest (removed at the end) and reads sdmc:/switch/black/game when it is there.
// Run with: nxlink -s black-selftest.nro      Returns to hbmenu when done.
#include <switch.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "kernel/memory.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

extern "C" {
// as switch/headless/newlib_hooks.cpp: libnx leaves out the detach hook, std::thread::detach needs it
int __syscall_thread_detach(struct __pthread_t*) { return 0; }
void userAppInit(void) {
    socketInitializeDefault();
    if (nxlinkStdio() < 0) consoleDebugInit(debugDevice_SVC);
}
void userAppExit(void) { socketExit(); }
}

namespace {

constexpr const char* kRoot = "sdmc:/switch/black";
constexpr const char* kWork = "sdmc:/switch/black/selftest";
FILE* g_log = nullptr;
int g_pass = 0, g_fail = 0, g_info = 0;

void Emit(const char* tag, const char* name, const char* fmt, va_list ap) {
    char body[900];
    std::vsnprintf(body, sizeof body, fmt, ap);
    char line[1024];
    int n = std::snprintf(line, sizeof line, "%s %s: %s\n", tag, name, body);
    if (n < 0) return;
    std::fwrite(line, 1, std::strlen(line), stdout);
    std::fflush(stdout);
    if (g_log) {
        std::fwrite(line, 1, std::strlen(line), g_log);
        std::fflush(g_log);
    }
}

__attribute__((format(printf, 3, 4))) bool Check(bool ok, const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Emit(ok ? "PASS" : "FAIL", name, fmt, ap);
    va_end(ap);
    (ok ? g_pass : g_fail)++;
    return ok;
}

__attribute__((format(printf, 2, 3))) void Info(const char* name, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Emit("INFO", name, fmt, ap);
    va_end(ap);
    g_info++;
}

void RemoveTree(const std::string& root);

std::string W(const std::string& rel) { return std::string(kWork) + "/" + rel; }

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

bool WriteFile(const std::string& p, size_t size, uint8_t seed = 1) {
    FILE* f = std::fopen(p.c_str(), "wb");
    if (!f) return false;
    std::vector<uint8_t> b(size);
    for (size_t i = 0; i < size; i++) b[i] = uint8_t(i * 7 + seed);
    bool ok = std::fwrite(b.data(), 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}

// errno and the fs service result of the last failed fsdev call
std::string Err() {
    char b[96];
    std::snprintf(b, sizeof b, "errno %d (%s), fs result 0x%X", errno, std::strerror(errno), fsdevGetLastResult());
    return b;
}

std::string Ec(const std::error_code& ec) {
    return ec ? std::to_string(ec.value()) + " (" + ec.message() + ")" : std::string("none");
}

// a guest memory block as the runtime makes it (kernel/memory.cpp MapRun): heap pages behind a code memory object,
// the owner view mapped read-write in reserved address space; undone at the end so hbmenu gets its heap back
struct CodeMem {
    void* src = nullptr;
    Handle handle = 0;
    uint8_t* at = nullptr;
    VirtmemReservation* rv = nullptr;
    size_t len = 0;
    Result rc = 0;
};

CodeMem MapCode(size_t len) {
    CodeMem c;
    c.len = len;
    c.src = aligned_alloc(0x1000, len);
    virtmemLock();
    void* at = virtmemFindAslr(len, 0x10000);
    if (at) c.rv = virtmemAddReservation(at, len);
    virtmemUnlock();
    if (!c.src || !c.rv) {
        c.rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        return c;
    }
    c.rc = svcCreateCodeMemory(&c.handle, c.src, len);
    if (R_SUCCEEDED(c.rc)) c.rc = svcControlCodeMemory(c.handle, CodeMapOperation_MapOwner, at, len, Perm_Rw);
    if (R_SUCCEEDED(c.rc)) {
        c.at = static_cast<uint8_t*>(at);
        std::memset(c.at, 0, len);
    }
    return c;
}

void UnmapCode(CodeMem& c) {
    if (c.at) svcControlCodeMemory(c.handle, CodeMapOperation_UnmapOwner, c.at, c.len, 0);
    if (c.handle) svcCloseHandle(c.handle);
    if (c.rv) {
        virtmemLock();
        virtmemRemoveReservation(c.rv);
        virtmemUnlock();
    }
    std::free(c.src);
    c = {};
}

// ---------------------------------------------------------------------------------------------------------------

void TestProcess() {
    u64 total = 0, used = 0, cores = 0, prios = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&prios, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0);
    Info("process", "applet type %d (0 application/title takeover, 2 library applet/album), memory total %llu MB, "
         "used %llu MB, core mask %llX, priority mask %016llX", int(appletGetAppletType()),
         (unsigned long long)(total >> 20), (unsigned long long)(used >> 20), (unsigned long long)cores,
         (unsigned long long)prios);
    // the runtime's placement: priorities 0x20 (interrupt level) to 0x3B, cores 1 and the others
    bool all = true;
    for (int p : {0x20, 0x21, 0x2C, 0x34, 0x3B}) all = all && (prios >> p & 1);
    Check(all, "process.priorities", "0x20, 0x21, 0x2C, 0x34, 0x3B allowed: %s (expect yes, threads.cpp sets them)",
          all ? "yes" : "no");
    Check((cores & 0x7) == 0x7, "process.cores", "core mask %llX (expect cores 0-2: guest core 1, others 0 and 2)",
          (unsigned long long)cores);
    Check(envIsSyscallHinted(0x4B) && envIsSyscallHinted(0x4C), "process.svc_code_memory",
          "CreateCodeMemory %d ControlCodeMemory %d (expect 1 1, memory.cpp needs both)", envIsSyscallHinted(0x4B),
          envIsSyscallHinted(0x4C));
    Info("process.svc_other", "MapProcessMemory 0x74 %d, MapPhysicalMemory 0x2C %d, SetThreadCoreMask 0x0F %d",
         envIsSyscallHinted(0x74), envIsSyscallHinted(0x2C), envIsSyscallHinted(0x0F));
    void* big = std::malloc(256u << 20);
    Check(big != nullptr, "heap.malloc_256mb", "%p (expect non-null; the game maps 64-128 MB of guest RAM)", big);
    std::free(big);
    void* al = aligned_alloc(0x1000, 64u << 20);
    Check(al && (uintptr_t(al) & 0xFFF) == 0, "heap.aligned_alloc_64mb", "%p (expect page aligned)", al);
    std::free(al);
    size_t mb = 0;
    std::vector<void*> blocks;
    while (void* b = std::malloc(64u << 20)) {
        blocks.push_back(b);
        mb += 64;
    }
    for (void* b : blocks) std::free(b);
    Info("heap.free", "about %zu MB more could be allocated in 64 MB blocks", mb);
}

void TestClocks() {
    timespec mono{}, real{}, res{};
    int rm = clock_gettime(CLOCK_MONOTONIC, &mono), rr = clock_gettime(CLOCK_REALTIME, &real);
    Check(rm == 0 && rr == 0, "clock.gettime", "monotonic rc %d %lld.%09ld, realtime rc %d %lld.%09ld (expect rc 0; libnx "
          "fails both with EIO when the time service gave no time at start)", rm, (long long)mono.tv_sec, mono.tv_nsec,
          rr, (long long)real.tv_sec, real.tv_nsec);
    clock_getres(CLOCK_MONOTONIC, &res);
    Info("clock.res", "%ld ns; monotonic and realtime differ by %lld s (libnx: both are boot time + ticks)", res.tv_nsec,
         (long long)(real.tv_sec - mono.tv_sec));
    auto s0 = Clock::now();
    Info("clock.steady_epoch", "steady_clock::now() = %.3f s since its epoch (Linux: since boot, libnx: since 1970)",
         std::chrono::duration<double>(s0.time_since_epoch()).count());
    bool mono_ok = true;
    auto prev = Clock::now();
    for (int i = 0; i < 200000; i++) {
        auto n = Clock::now();
        if (n < prev) mono_ok = false;
        prev = n;
    }
    Check(mono_ok, "clock.steady_monotonic", "200000 reads never went back: %s", mono_ok ? "yes" : "no");
    u64 t0 = armGetSystemTick();
    auto c0 = Clock::now();
    svcSleepThread(200000000);
    u64 t1 = armGetSystemTick();
    auto c1 = Clock::now();
    double tick_ms = double(armTicksToNs(t1 - t0)) / 1e6, steady_ms = Ms(c0, c1);
    Check(std::abs(tick_ms - steady_ms) < 2, "clock.steady_rate", "200 ms sleep: ticks %.3f ms, steady %.3f ms (expect "
          "within 2 ms)", tick_ms, steady_ms);
    auto sys = std::chrono::system_clock::now();
    time_t now = std::time(nullptr);
    long long sys_s = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    tm gm{}, lt{};
    gmtime_r(&now, &gm);
    localtime_r(&now, &lt);
    Check(std::llabs(sys_s - (long long)now) <= 1 && gm.tm_year + 1900 >= 2024, "clock.system",
          "system_clock %lld, time() %lld, UTC %04d-%02d-%02d %02d:%02d (expect equal, a current date)", sys_s,
          (long long)now, gm.tm_year + 1900, gm.tm_mon + 1, gm.tm_mday, gm.tm_hour, gm.tm_min);
    const char* tz = std::getenv("TZ");
    Info("clock.timezone", "TZ=%s, local %02d:%02d, UTC %02d:%02d", tz ? tz : "(unset)", lt.tm_hour, lt.tm_min,
         gm.tm_hour, gm.tm_min);
    // sleeps: the vertical blank thread sleeps to 16.667 ms marks, PaceFrame and KeDelayExecutionThread 1-2 ms
    for (auto want : {std::chrono::microseconds(100), std::chrono::microseconds(1000), std::chrono::microseconds(2000)}) {
        double worst = 0, sum = 0;
        for (int i = 0; i < 50; i++) {
            auto a = Clock::now();
            std::this_thread::sleep_for(want);
            double d = Ms(a, Clock::now());
            sum += d;
            worst = std::max(worst, d);
        }
        double w = double(want.count()) / 1000;
        char name[48];
        std::snprintf(name, sizeof name, "sleep.for_%lldus", (long long)want.count());
        Check(sum / 50 >= w && sum / 50 < w + 1.0, name, "mean %.3f ms, worst %.3f ms (expect mean within 1 ms over "
              "%.3f)", sum / 50, worst, w);
    }
    auto start = Clock::now();
    auto next = start;
    double late = 0;
    for (int i = 0; i < 30; i++) {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        late = std::max(late, Ms(next, Clock::now()));
    }
    Check(late < 2, "sleep.until_vblank", "30 marks 16.667 ms apart, latest wake %.3f ms after its mark (expect < 2)",
          late);
    auto y0 = Clock::now();
    for (int i = 0; i < 1000; i++) std::this_thread::yield();
    Info("sleep.yield", "1000 yields alone on the system: %.3f ms (sched_yield is svcSleepThread(-1))",
         Ms(y0, Clock::now()));
}

void TestCondvars() {
    std::mutex m;
    std::condition_variable cv;
    {
        std::unique_lock<std::mutex> lock(m);
        auto a = Clock::now();
        auto st = cv.wait_for(lock, std::chrono::milliseconds(10));
        double d = Ms(a, Clock::now());
        Check(st == std::cv_status::timeout && d >= 10 && d < 13, "condvar.wait_for", "%s after %.3f ms (expect timeout "
              "in 10-13 ms; objects.cpp timer thread)", st == std::cv_status::timeout ? "timeout" : "no timeout", d);
    }
    {
        std::unique_lock<std::mutex> lock(m);
        auto a = Clock::now();
        auto st = cv.wait_until(lock, a + std::chrono::milliseconds(20));
        double d = Ms(a, Clock::now());
        Check(st == std::cv_status::timeout && d >= 20 && d < 23, "condvar.wait_until_steady",
              "%s after %.3f ms (expect timeout in 20-23 ms; kernel Wait with a deadline)",
              st == std::cv_status::timeout ? "timeout" : "no timeout", d);
        a = Clock::now();
        st = cv.wait_until(lock, a - std::chrono::milliseconds(5));
        d = Ms(a, Clock::now());
        Check(st == std::cv_status::timeout && d < 1, "condvar.wait_until_past", "%.3f ms (expect an immediate timeout)",
              d);
    }
    {
        std::recursive_mutex rm;
        std::condition_variable_any any;
        std::unique_lock<std::recursive_mutex> lock(rm);
        auto a = Clock::now();
        any.wait_for(lock, std::chrono::milliseconds(50));
        double d = Ms(a, Clock::now());
        Check(d >= 50 && d < 54, "condvar.any_recursive", "%.3f ms (expect 50-54; Black's mixer pump)", d);
    }
    {
        // a wake from another thread, as a guest SetEvent wakes a waiter
        bool flag = false;
        Clock::time_point sent;
        std::thread t([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::lock_guard<std::mutex> l(m);
            flag = true;
            sent = Clock::now();
            cv.notify_one();
        });
        std::unique_lock<std::mutex> lock(m);
        bool got = cv.wait_for(lock, std::chrono::seconds(1), [&] { return flag; });
        double d = Ms(sent, Clock::now());
        lock.unlock();
        t.join();
        Check(got && d < 2, "condvar.notify", "woken %.3f ms after notify (expect < 2)", d);
    }
}

thread_local int t_value = 0;
std::atomic<int> g_tls_dtors{0};
struct TlsDtor {
    ~TlsDtor() { g_tls_dtors++; }
};
thread_local TlsDtor t_dtor;

struct StackProbe {
    u64 region = 0;
    u32 core = 99;
    s32 prio = 0;
    Result mask_rc = 1, prio_rc = 1;
    std::atomic<bool> done{false};
};

void TestThreads() {
    // kernel/threads.cpp Spawn: 2 MB stack, detached, placed by svcSetThreadCoreMask and svcSetThreadPriority
    StackProbe probe;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2u << 20);
    pthread_t th;
    int rc = pthread_create(&th, &attr, [](void* p) -> void* {
        auto& s = *static_cast<StackProbe*>(p);
        MemoryInfo mi{};
        u32 page = 0;
        int local = 0;
        svcQueryMemory(&mi, &page, reinterpret_cast<u64>(&local));
        s.region = mi.size;
        Handle h = threadGetCurHandle();
        s.mask_rc = svcSetThreadCoreMask(h, 1, 1u << 1);
        svcSleepThread(1000000);
        s.core = svcGetCurrentProcessorNumber();
        s.prio_rc = svcSetThreadPriority(h, 0x2C);
        svcGetThreadPriority(&s.prio, h);
        s.done = true;
        return nullptr;
    }, &probe);
    int drc = rc == 0 ? pthread_detach(th) : -1;
    pthread_attr_destroy(&attr);
    for (int i = 0; i < 200 && !probe.done; i++) svcSleepThread(5000000);
    Check(rc == 0 && drc == 0 && probe.done, "thread.spawn_2mb", "create %d, detach %d, ran %d (expect 0 0 1)", rc, drc,
          int(probe.done.load()));
    Check(probe.region >= (2u << 20), "thread.stack_2mb", "stack region %llu KB (expect >= 2048)",
          (unsigned long long)(probe.region >> 10));
    Check(R_SUCCEEDED(probe.mask_rc) && probe.core == 1, "thread.core_mask", "svcSetThreadCoreMask(1, 0x2) %X, then on "
          "core %u (expect 0 and core 1)", probe.mask_rc, probe.core);
    Check(R_SUCCEEDED(probe.prio_rc) && probe.prio == 0x2C, "thread.priority", "svcSetThreadPriority(0x2C) %X, read "
          "back 0x%X (expect 0, 0x2C)", probe.prio_rc, probe.prio);

    // the interrupt level priority of the runtime's vertical blank, DPC and sound threads, and the bounds
    std::thread([&] {
        Handle h = threadGetCurHandle();
        for (int p : {0x20, 0x21, 0x34, 0x3B}) {
            Result r = svcSetThreadPriority(h, u32(p));
            s32 got = 0;
            svcGetThreadPriority(&got, h);
            char name[40];
            std::snprintf(name, sizeof name, "thread.priority_%02X", p);
            Check(R_SUCCEEDED(r) && got == p, name, "rc %X, read back 0x%X", r, got);
        }
        Result r = svcSetThreadCoreMask(h, -1, 0x5);
        Check(R_SUCCEEDED(r), "thread.core_mask_others", "svcSetThreadCoreMask(-1, 0x5) %X (expect 0; split mode)", r);
    }).join();

    // std::thread: the runtime's timer, clock, input poller and Black's audio pump threads take its default stack
    u64 std_region = 0;
    std::thread([&] {
        MemoryInfo mi{};
        u32 page = 0;
        int local = 0;
        svcQueryMemory(&mi, &page, reinterpret_cast<u64>(&local));
        std_region = mi.size;
    }).join();
    Info("thread.std_stack", "std::thread stack %llu KB (libnx default for a pthread without a size; Linux 8192)",
         (unsigned long long)(std_region >> 10));
    bool detached = false;
    try {
        std::thread([] {}).detach();
        detached = true;
    } catch (const std::exception& e) {
        Info("thread.detach_error", "%s", e.what());
    }
    Check(detached, "thread.std_detach", "detach %s (expect ok with the __syscall_thread_detach hook)",
          detached ? "ok" : "threw");

    // thread_local: per thread, destructors at thread end
    t_value = 1;
    int other = -1;
    int before = g_tls_dtors;
    std::thread([&] {
        other = t_value;
        t_value = 2;
        (void)&t_dtor;
    }).join();
    Check(other == 0 && t_value == 1, "thread.thread_local", "new thread saw %d, main keeps %d (expect 0, 1)", other,
          t_value);
    Info("thread.tls_destructor", "thread_local destructors run at thread end: %d", g_tls_dtors - before);

    // pthread_exit, as ExitThread ends a guest thread
    pthread_t pe;
    pthread_create(&pe, nullptr, [](void*) -> void* {
        pthread_exit(reinterpret_cast<void*>(42));
        return nullptr;
    }, nullptr);
    void* ret = nullptr;
    pthread_join(pe, &ret);
    Check(ret == reinterpret_cast<void*>(42), "thread.pthread_exit", "joined with %p (expect 0x2a)", ret);
    Info("thread.hardware_concurrency", "%u", std::thread::hardware_concurrency());
}

void TestCodeMemory(const std::string& data_file, size_t data_size) {
    CodeMem c = MapCode(8u << 20);  // the data file lands at +1 MB
    if (!Check(R_SUCCEEDED(c.rc) && c.at, "codemem.map", "rc %X at %p (expect 0)", c.rc, static_cast<void*>(c.at))) {
        UnmapCode(c);
        return;
    }
    // locked operations on guest memory from threads on two cores, as the guest's lock cmpxchg / xadd
    auto* counter = reinterpret_cast<std::atomic<uint32_t>*>(c.at + 0x100);
    auto* wide = reinterpret_cast<std::atomic<uint64_t>*>(c.at + 0x200);
    auto work = [&](int core) {
        svcSetThreadCoreMask(threadGetCurHandle(), core, 1u << core);
        for (int i = 0; i < 1000000; i++) {
            counter->fetch_add(1);
            uint64_t v = wide->load();
            while (!wide->compare_exchange_weak(v, v + 3)) {
            }
        }
    };
    std::thread a(work, 0), b(work, 2);
    a.join();
    b.join();
    Check(counter->load() == 2000000 && wide->load() == 6000000, "codemem.atomics",
          "fetch_add total %u, cas total %llu (expect 2000000, 6000000)", counter->load(),
          (unsigned long long)wide->load());

    // reads straight into guest memory, unbuffered so the stream does not stage them
    FILE* f = std::fopen(data_file.c_str(), "rb");
    if (f) {
        std::setvbuf(f, nullptr, _IONBF, 0);
        errno = 0;
        size_t got = std::fread(c.at + 0x1000, 1, 0x10000, f);
        Info("codemem.direct_fread", "fread of 64 KB into code memory: %zu bytes, %s (libnx falls back to its own "
             "buffer only on 0xD401; memory.cpp bounces every read)", got, Err().c_str());
        std::fclose(f);
    }
    int fd = open(data_file.c_str(), O_RDONLY);
    if (fd >= 0) {
        errno = 0;
        ssize_t got = read(fd, c.at + 0x20000, 0x10000);
        Info("codemem.direct_read", "read(2) of 64 KB into code memory: %zd, %s (disc.cpp's ISO reads go through the "
             "bounce buffer)", got, Err().c_str());
        close(fd);
    }
    // the runtime's own path: mem::FileRead / mem::FileWrite through the bounce buffer
    f = std::fopen(data_file.c_str(), "rb");
    std::vector<uint8_t> want(data_size);
    size_t n = f ? std::fread(want.data(), 1, data_size, f) : 0;
    if (f) std::fseek(f, 0, SEEK_SET);
    size_t got = f ? mem::FileRead(c.at + 0x100000, data_size, f) : 0;
    if (f) std::fclose(f);
    Check(n == data_size && got == data_size && std::memcmp(c.at + 0x100000, want.data(), data_size) == 0,
          "codemem.bounce_read", "mem::FileRead %zu of %zu bytes, contents %s (expect all, equal)", got, data_size,
          std::memcmp(c.at + 0x100000, want.data(), data_size) == 0 ? "equal" : "differ");
    std::string out = W("from_guest.bin");
    f = std::fopen(out.c_str(), "wb");
    size_t put = f ? mem::FileWrite(c.at + 0x100000, data_size, f) : 0;
    if (f) std::fclose(f);
    struct stat st {};
    stat(out.c_str(), &st);
    Check(put == data_size && size_t(st.st_size) == data_size, "codemem.bounce_write",
          "mem::FileWrite %zu bytes, file %lld bytes (expect %zu)", put, (long long)st.st_size, data_size);
    f = std::fopen(out.c_str(), "wb");
    errno = 0;
    put = f ? std::fwrite(c.at + 0x100000, 1, 0x10000, f) : 0;
    if (f) std::fclose(f);
    Info("codemem.direct_fwrite", "fwrite of 64 KB from code memory: %zu, %s (SavePersistedDisplay now goes through "
         "mem::FileWrite)", put, Err().c_str());

    // sockets with guest buffers (xnet.cpp send/recv, unused by Black)
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(28999);
    if (s >= 0 && bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) {
        std::memcpy(c.at + 0x700000, "guest", 6);
        errno = 0;
        ssize_t sent = sendto(s, c.at + 0x700000, 6, 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        int se = errno;
        timeval tv{0, 200000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        errno = 0;
        ssize_t recvd = recv(s, c.at + 0x700100, 64, 0);
        Info("codemem.socket", "sendto from code memory %zd (errno %d), recv into code memory %zd (errno %d)", sent,
             se, recvd, errno);
    } else {
        Info("codemem.socket", "no loopback socket (errno %d)", errno);
    }
    if (s >= 0) close(s);
    UnmapCode(c);
    Check(true, "codemem.unmap", "the owner view unmapped and the heap block freed");
}

void TestStdio() {
    std::string p = W("stdio.bin");
    Check(WriteFile(p, 100), "stdio.create_wb", "100 bytes");
    FILE* f = std::fopen(p.c_str(), "r+b");
    if (!Check(f != nullptr, "stdio.open_rplus", "%s", f ? "ok" : Err().c_str())) return;
    std::fseek(f, 0, SEEK_END);
    long long end = ftello(f);
    Check(end == 100, "stdio.seek_end", "ftello at the end %lld (expect 100)", end);
    char b[64];
    std::fseek(f, 0, SEEK_END);
    size_t at_eof = std::fread(b, 1, 10, f);
    Check(at_eof == 0 && std::feof(f), "stdio.read_at_eof", "%zu bytes, feof %d, ferror %d (expect 0, 1, 0)", at_eof,
          std::feof(f), std::ferror(f));
    int sk = fseeko(f, 150, SEEK_SET);
    size_t past = std::fread(b, 1, 10, f);
    Info("stdio.read_past_eof", "fseeko(150) %d, ftello %lld, fread %zu, feof %d, ferror %d (Transfer reports end of "
         "file on 0 bytes)", sk, (long long)ftello(f), past, std::feof(f), std::ferror(f));
    std::clearerr(f);
    fseeko(f, 10, SEEK_SET);
    size_t again = std::fread(b, 1, 10, f);
    Check(again == 10 && uint8_t(b[0]) == uint8_t(10 * 7 + 1), "stdio.read_after_eof", "%zu bytes from 10, first %u "
          "(expect 10, 71)", again, uint8_t(b[0]));
    // the runtime seeks before every transfer; a write past the end
    fseeko(f, 120, SEEK_SET);
    size_t w = std::fwrite("x", 1, 1, f);
    std::fflush(f);
    struct stat st {};
    fstat(fileno(f), &st);
    Check(w == 1 && st.st_size == 121, "stdio.write_past_eof", "wrote %zu at 120, size %lld (expect 1, 121)", w,
          (long long)st.st_size);
    int big = fseeko(f, 3ll << 30, SEEK_SET);
    Check(big == 0 && ftello(f) == (3ll << 30), "stdio.seek_3gb", "fseeko(3 GB) %d, ftello %lld (expect 0, 3221225472; "
          "nothing written there)", big, (long long)ftello(f));
    int tr = ftruncate(fileno(f), 40);
    fstat(fileno(f), &st);
    Check(tr == 0 && st.st_size == 40, "stdio.ftruncate_open", "ftruncate(40) %d, size %lld (expect 0, 40; "
          "NtSetInformationFile end of file)", tr, (long long)st.st_size);
    std::fclose(f);
    f = std::fopen(p.c_str(), "w+b");
    fstat(fileno(f), &st);
    Check(st.st_size == 0, "stdio.w_plus_truncates", "size %lld (expect 0)", (long long)st.st_size);
    std::fwrite("abc", 1, 3, f);
    std::fclose(f);
    f = std::fopen(p.c_str(), "ab");
    std::fwrite("de", 1, 2, f);
    std::fclose(f);
    f = std::fopen(p.c_str(), "rb");
    size_t n = f ? std::fread(b, 1, sizeof b, f) : 0;
    if (f) std::fclose(f);
    Check(n == 5 && std::memcmp(b, "abcde", 5) == 0, "stdio.append", "%zu bytes \"%.*s\" (expect abcde)", n, int(n), b);
    Check(truncate(p.c_str(), 2) == 0 && stat(p.c_str(), &st) == 0 && st.st_size == 2, "stdio.truncate_closed",
          "truncate by path on a closed file: size %lld (expect 2)", (long long)st.st_size);

    // buffering: newlib sizes the buffer from st_blksize, which fsdev leaves 0 (1 KB, one fs request each)
    std::string big_file = W("big.bin");
    WriteFile(big_file, 8u << 20);
    std::vector<uint8_t> buf(8u << 20);
    for (int mode : {0, 1}) {
        f = std::fopen(big_file.c_str(), "rb");
        if (mode) std::setvbuf(f, nullptr, _IONBF, 0);
        auto a = Clock::now();
        size_t total = 0;
        for (size_t at = 0; at < buf.size(); at += 0x10000) total += std::fread(buf.data() + at, 1, 0x10000, f);
        double ms = Ms(a, Clock::now());
        std::fclose(f);
        Info(mode ? "stdio.speed_unbuffered" : "stdio.speed_buffered", "8 MB in 64 KB freads: %zu bytes in %.1f ms, "
             "%.1f MB/s", total, ms, 8000.0 / ms);
    }
    f = std::fopen(big_file.c_str(), "rb");
    struct stat bs {};
    fstat(fileno(f), &bs);
    std::fclose(f);
    Info("stdio.blksize", "st_blksize %ld, BUFSIZ %d", long(bs.st_blksize), BUFSIZ);
    std::remove(big_file.c_str());

    // iostreams
    {
        std::ofstream o(W("stream.txt"));
        o << "line one\nline two\n";
    }
    std::ifstream in(W("stream.txt"));
    std::string l1, l2;
    std::getline(in, l1);
    std::getline(in, l2);
    Check(l1 == "line one" && l2 == "line two", "stdio.iostream", "\"%s\" \"%s\"", l1.c_str(), l2.c_str());
}

// Horizon's fs service and a file open for writing: what kernel/files.cpp met before it opened read only files read
// only (the game's NtCreateFile with FILE_OPEN asks for read access)
void TestLocks(const std::string& game_file) {
    std::string p = W("Lock.bin");
    WriteFile(p, 4096);
    struct stat st {};
    std::error_code ec;

    FILE* rw = std::fopen(p.c_str(), "r+b");
    errno = 0;
    int src = stat(p.c_str(), &st);
    std::string serr = Err();
    uintmax_t size = fs::file_size(p, ec);
    std::string size_ec = Ec(ec);
    bool exists = fs::exists(p, ec);
    std::string exists_ec = Ec(ec);
    auto t = fs::last_write_time(p, ec);
    (void)t;
    std::string time_ec = Ec(ec);
    Info("lock.rplus_then_stat", "open r+b, then stat by path rc %d %s; fs::file_size %lld ec %s; fs::exists %d ec "
         "%s; fs::last_write_time ec %s (Linux: 0, 4096, 1, none)", src, src ? serr.c_str() : "", (long long)size,
         size_ec.c_str(), int(exists), exists_ec.c_str(), time_ec.c_str());
    int frc = rw ? fstat(fileno(rw), &st) : -1;
    Check(frc == 0 && st.st_size == 4096, "lock.rplus_fstat", "fstat of the open r+b stream rc %d size %lld (expect 0, "
          "4096; files.cpp describes open files this way)", frc, (long long)st.st_size);
    errno = 0;
    FILE* second = std::fopen(p.c_str(), "rb");
    Info("lock.rplus_then_open_rb", "a second open (rb) %s %s (Linux: ok)", second ? "ok" : "failed",
         second ? "" : Err().c_str());
    if (second) std::fclose(second);
    errno = 0;
    int ren = rename(p.c_str(), W("Lock2.bin").c_str());
    Info("lock.rplus_then_rename", "rename of the open file rc %d %s (Linux: 0)", ren, ren ? Err().c_str() : "");
    if (ren == 0) rename(W("Lock2.bin").c_str(), p.c_str());
    errno = 0;
    int trc = truncate(p.c_str(), 100);
    Info("lock.rplus_then_truncate", "truncate by path of the open file rc %d %s (Linux: 0)", trc,
         trc ? Err().c_str() : "");
    if (rw) std::fclose(rw);

    FILE* r1 = std::fopen(p.c_str(), "rb");
    FILE* r2 = std::fopen(p.c_str(), "rb");
    errno = 0;
    src = stat(p.c_str(), &st);
    Check(r1 && r2 && src == 0, "lock.rb_twice_and_stat", "two rb opens %d %d, stat by path rc %d %s (expect 1 1 0; "
          "the game opens some files twice, D:\\levels\\level_00\\DESTRUCT.bks)", r1 != nullptr, r2 != nullptr, src,
          src ? Err().c_str() : "");
    errno = 0;
    FILE* w = std::fopen(p.c_str(), "r+b");
    Info("lock.rb_then_rplus", "r+b while two rb are open: %s %s (Linux: ok)", w ? "ok" : "failed",
         w ? "" : Err().c_str());
    if (w) std::fclose(w);
    if (r1) std::fclose(r1);
    if (r2) std::fclose(r2);

    // the console crash: NtCreateFile opened Sound\DSP\BkXboxFx.bin r+b, then sized it by path
    if (!game_file.empty()) {
        FILE* g = std::fopen(game_file.c_str(), "rb");
        errno = 0;
        src = stat(game_file.c_str(), &st);
        Check(g && src == 0 && st.st_size > 0, "lock.game_file_rb", "%s open rb, stat by path rc %d size %lld %s "
              "(expect 0 and its size)", game_file.c_str(), src, (long long)st.st_size, src ? Err().c_str() : "");
        if (g) std::fclose(g);
        g = std::fopen(game_file.c_str(), "r+b");
        errno = 0;
        src = stat(game_file.c_str(), &st);
        Info("lock.game_file_rplus", "open r+b (%s), stat by path rc %d %s (what the old runtime did)",
             g ? "ok" : "failed", src, src ? Err().c_str() : "");
        if (g) std::fclose(g);
    }
}

void TestRenameRemove() {
    std::string a = W("RenA.txt"), b = W("RenB.txt");
    WriteFile(a, 10, 1);
    WriteFile(b, 20, 2);
    errno = 0;
    int r = rename(a.c_str(), W("RenC.txt").c_str());
    Check(r == 0, "rename.file", "rc %d %s", r, r ? Err().c_str() : "");
    errno = 0;
    r = rename(W("RenC.txt").c_str(), b.c_str());
    struct stat st {};
    stat(b.c_str(), &st);
    Info("rename.over_existing", "rename onto an existing file rc %d %s, target now %lld bytes (POSIX: 0, 10; files.cpp "
         "removes the target first on Horizon)", r, r ? Err().c_str() : "", (long long)st.st_size);
    std::error_code ec;
    fs::rename(W("RenC.txt"), b, ec);
    Info("rename.fs_over_existing", "std::filesystem::rename onto an existing file ec %s", Ec(ec).c_str());
    fs::create_directories(W("RenDir/Inner"), ec);
    r = rename(W("RenDir").c_str(), W("RenDir2").c_str());
    Check(r == 0 && fs::is_directory(W("RenDir2/Inner")), "rename.dir", "rc %d (expect 0, contents moved)", r);
    WriteFile(W("Case.txt"), 5);
    errno = 0;
    r = rename(W("Case.txt").c_str(), W("CASE.TXT").c_str());
    std::string listed;
    for (auto& e : fs::directory_iterator(kWork, ec))
        if (strcasecmp(e.path().filename().c_str(), "case.txt") == 0) listed = e.path().filename().string();
    Info("rename.case_only", "Case.txt -> CASE.TXT rc %d %s, listed as \"%s\"", r, r ? Err().c_str() : "",
         listed.c_str());

    Check(std::remove(b.c_str()) == 0, "remove.file", "%s", Err().c_str());
    fs::create_directories(W("Empty"), ec);
    Check(std::remove(W("Empty").c_str()) == 0, "remove.empty_dir", "remove(3) of a folder (newlib: unlink, then rmdir)");
    bool removed = fs::remove(W("RenDir2"), ec);
    Check(!removed && ec, "remove.nonempty_dir", "fs::remove %d ec %s (expect 0 and an error)", int(removed),
          Ec(ec).c_str());
    std::error_code ec2;
    auto count = fs::remove_all(W("RenDir2"), ec2);
    Info("remove.remove_all", "fs::remove_all %lld ec %s (lstat is ENOSYS on sdmc:, remove_all uses it; unused by the "
         "runtime)", (long long)(count == static_cast<std::uintmax_t>(-1) ? -1 : (long long)count), Ec(ec2).c_str());
    if (fs::exists(W("RenDir2"))) {
        rmdir(W("RenDir2/Inner").c_str());
        rmdir(W("RenDir2").c_str());
    }
    std::error_code ec3;
    bool missing = fs::remove(W("NotThere.txt"), ec3);
    Check(!missing && !ec3, "remove.missing", "fs::remove of a missing file %d ec %s (expect 0, none)", int(missing),
          Ec(ec3).c_str());
}

void TestFilesystem() {
    std::error_code ec;
    bool made = fs::create_directories(W("MixedCase/Sub/Deep"), ec);
    Check(made && !ec, "fs.create_directories", "%d ec %s (expect 1, none; MountDefaults, log Init)", int(made),
          Ec(ec).c_str());
    made = fs::create_directories(W("MixedCase/Sub/Deep"), ec);
    Check(!made && !ec, "fs.create_directories_again", "%d ec %s (expect 0, none)", int(made), Ec(ec).c_str());
    made = fs::create_directory(W("MixedCase"), ec);
    Check(!made && !ec, "fs.create_directory_existing", "%d ec %s (expect 0, none)", int(made), Ec(ec).c_str());
    std::string file = W("MixedCase/Data.BIN");
    WriteFile(file, 1234);
    time_t written = std::time(nullptr);

    Check(fs::exists(file, ec) && fs::is_regular_file(file, ec) && !fs::is_directory(file, ec), "fs.file_kind",
          "exists, regular, not a folder");
    Check(fs::is_directory(W("MixedCase/Sub"), ec) && !fs::is_regular_file(W("MixedCase/Sub"), ec), "fs.dir_kind",
          "a folder");
    uintmax_t size = fs::file_size(file, ec);
    Check(size == 1234 && !ec, "fs.file_size", "%llu ec %s (expect 1234)", (unsigned long long)size, Ec(ec).c_str());
    size = fs::file_size(W("MixedCase/Sub"), ec);
    Info("fs.file_size_dir", "of a folder %lld ec %s", (long long)size, Ec(ec).c_str());
    auto st = fs::status(file, ec);
    Check(st.type() == fs::file_type::regular, "fs.status", "type %d (expect %d regular)", int(st.type()),
          int(fs::file_type::regular));
    auto sst = fs::symlink_status(file, ec);
    Info("fs.symlink_status", "type %d ec %s (lstat on sdmc:)", int(sst.type()), Ec(ec).c_str());
    std::error_code tec;
    auto ft = fs::last_write_time(file, tec);
    auto sys = std::chrono::file_clock::to_sys(ft);
    long long secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
    Check(!tec && std::llabs(secs - (long long)written) <= 5, "fs.last_write_time", "%lld, written at %lld, %+lld s ec "
          "%s (expect within 5 s; an hour multiple is the time zone)", secs, (long long)written,
          secs - (long long)written, Ec(tec).c_str());
    struct stat sb {};
    stat(file.c_str(), &sb);
    Check(S_ISREG(sb.st_mode) && sb.st_size == 1234, "stat.file", "mode %o size %lld mtime %lld (expect 100xxx, 1234)",
          unsigned(sb.st_mode), (long long)sb.st_size, (long long)sb.st_mtime);
    stat(W("MixedCase").c_str(), &sb);
    Check(S_ISDIR(sb.st_mode), "stat.dir", "mode %o mtime %lld (expect 040xxx)", unsigned(sb.st_mode),
          (long long)sb.st_mtime);
    int trail = stat((W("MixedCase") + "/").c_str(), &sb);
    Info("stat.trailing_slash", "stat of \"MixedCase/\" rc %d %s", trail, trail ? Err().c_str() : "");

    // case: the runtime's MatchCase looks a name up as given, then by a case-blind scan of the folder
    std::string wrong = W("mixedcase/SUB");
    Info("fs.case_blind_dir", "exists(\"mixedcase/SUB\") %d", int(fs::exists(wrong, ec)));
    FILE* f = std::fopen(W("MIXEDCASE/data.bin").c_str(), "rb");
    Info("fs.case_blind_file", "fopen(\"MIXEDCASE/data.bin\") %s", f ? "opens" : "fails");
    if (f) std::fclose(f);

    // listing: names in creation order c, A, b, Zdir; NtQueryDirectoryFile sorts them
    fs::create_directories(W("List"), ec);
    for (const char* n : {"c.TXT", "A.txt", "b.txt"}) WriteFile(W(std::string("List/") + n), 3);
    fs::create_directories(W("List/Zdir"), ec);
    std::string order;
    int files = 0, dirs = 0;
    bool threw = false;
    try {
        for (auto& e : fs::directory_iterator(W("List"))) {
            order += e.path().filename().string() + " ";
            (e.is_directory() ? dirs : files)++;
        }
    } catch (const std::exception& e) {
        threw = true;
        order += std::string("threw ") + e.what();
    }
    Check(!threw && files == 3 && dirs == 1, "fs.directory_iterator", "%s(expect 3 files, 1 folder, no throw; the "
          "range-for increment throws on a readdir error)", order.c_str());
    Info("fs.listing_order", "%s", order.c_str());
    int all = 0;
    for (auto it = fs::recursive_directory_iterator(W("MixedCase"), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        all++;
    Info("fs.recursive_iterator", "%d entries under MixedCase ec %s (expect 3)", all, Ec(ec).c_str());
    std::error_code mec;
    fs::directory_iterator missing(W("NoSuchDir"), mec);
    Check(mec && missing == fs::directory_iterator(), "fs.iterator_missing", "ec %s (expect an error, no throw)",
          Ec(mec).c_str());
    DIR* d = opendir(W("List").c_str());
    std::string types;
    while (dirent* e = d ? readdir(d) : nullptr) types += std::string(e->d_name) + "=" + std::to_string(e->d_type) + " ";
    if (d) closedir(d);
    Info("dirent.d_type", "%s(DT_REG 8, DT_DIR 4)", types.c_str());

    std::error_code rec;
    fs::resize_file(file, 100, rec);
    Check(!rec && fs::file_size(file, ec) == 100, "fs.resize_file_closed", "ec %s size %llu (expect none, 100)",
          Ec(rec).c_str(), (unsigned long long)fs::file_size(file, ec));
    std::error_code cec;
    bool copied = fs::copy_file(file, W("MixedCase/Copy.bin"), cec);
    Info("fs.copy_file", "%d ec %s (unused by the runtime)", int(copied), Ec(cec).c_str());
    fs::space_info sp = fs::space(kRoot, ec);
    Info("fs.space", "capacity %llu MB, free %llu MB ec %s", (unsigned long long)(sp.capacity >> 20),
         (unsigned long long)(sp.free >> 20), Ec(ec).c_str());

    // paths: libstdc++ sees "sdmc:" as a plain first component, not a root name
    fs::path sd("sdmc:/switch/black/game");
    Info("path.sdmc", "is_absolute %d, root_name \"%s\", parent \"%s\", filename \"%s\", first element \"%s\"",
         int(sd.is_absolute()), sd.root_name().c_str(), sd.parent_path().c_str(), sd.filename().c_str(),
         sd.begin()->c_str());
    Info("path.join_sdmc", "path(\"sdmc:/a\") / \"sdmc:/b\" = \"%s\" (an absolute right side would replace)",
         (fs::path("sdmc:/a") / "sdmc:/b").c_str());
    std::error_code aec;
    Info("path.absolute", "fs::absolute(\"sdmc:/x\") = \"%s\" ec %s", fs::absolute("sdmc:/x", aec).c_str(),
         Ec(aec).c_str());
    std::error_code wec;
    Info("path.weakly_canonical", "\"%s\" ec %s", fs::weakly_canonical(W("MixedCase/../MixedCase"), wec).c_str(),
         Ec(wec).c_str());
    char cwd[512] = {};
    getcwd(cwd, sizeof cwd);
    std::error_code pec;
    Check(std::string(cwd).find("switch/black") != std::string::npos, "path.cwd",
          "getcwd \"%s\", current_path \"%s\" ec %s (expect sdmc:/switch/black after chdir; app::ExeDir)", cwd,
          fs::current_path(pec).c_str(), Ec(pec).c_str());
    f = std::fopen("selftest/MixedCase/Data.BIN", "rb");
    Check(f != nullptr, "path.relative_open", "fopen of a path relative to the working folder %s (expect opens; "
          "pad_script's cmd file)", f ? "opens" : "fails");
    if (f) std::fclose(f);
}

void TestLibc() {
    setenv("SELFTEST_A", "1", 1);
    static char put[] = "SELFTEST_B=two";
    putenv(put);
    const char* a = std::getenv("SELFTEST_A");
    const char* b = std::getenv("SELFTEST_B");
    Check(a && b && std::strcmp(a, "1") == 0 && std::strcmp(b, "two") == 0, "env.set_get", "A=%s B=%s", a ? a : "-",
          b ? b : "-");
    Check(std::toupper('a') == 'A' && std::tolower('Q') == 'q', "ctype.ascii", "toupper/tolower");
    Info("ctype.latin1", "toupper(0xE9) 0x%X, isalpha(0xE9) %d (Linux C locale: 0xE9, 0)", std::toupper(0xE9),
         std::isalpha(0xE9));
    char b2[64];
    std::snprintf(b2, sizeof b2, "%.3f %lld %ls %zu", 1.5, -3ll, L"wide", size_t(7));
    Check(std::strcmp(b2, "1.500 -3 wide 7") == 0, "printf.formats", "\"%s\" (expect \"1.500 -3 wide 7\")", b2);
    Check(std::strtod("2.25", nullptr) == 2.25, "strtod", "2.25");
    Info("rand", "rand() %d %d", std::rand(), std::rand());
    try {
        std::random_device rd;
        Info("random_device", "%u", rd());
    } catch (const std::exception& e) {
        Info("random_device", "threw %s", e.what());
    }
    bool caught = false;
    try {
        throw std::runtime_error("x");
    } catch (const std::runtime_error&) {
        caught = true;
    }
    Check(caught, "exception.catch", "a thrown runtime_error caught");
    caught = false;
    try {
        (void)fs::file_size(W("Missing.bin"));
    } catch (const fs::filesystem_error& e) {
        caught = true;
        Info("exception.filesystem_error", "%s", e.what());
    }
    Check(caught, "exception.filesystem", "fs::file_size of a missing file throws filesystem_error");
    Check(sizeof(time_t) == 8 && sizeof(off_t) == 8, "types", "time_t %zu off_t %zu bytes (expect 8 8)", sizeof(time_t),
          sizeof(off_t));
}

// ---------------------------------------------------------------------------------------------------------------
// round 2: what kernel/files.cpp and kernel/memory.cpp now rely on, and what the game meets past the first level

// kernel/files.cpp HostKind: the entry type through fsdev's path translation, one fs request that opens nothing
int EntryType(const std::string& p) {
    FsFileSystem* device = nullptr;
    char inner[FS_MAX_PATH] = {};
    FsDirEntryType type;
    if (fsdevTranslatePath(p.c_str(), &device, inner) == -1) return -1;
    Result rc = fsFsGetEntryType(device, inner, &type);
    if (R_FAILED(rc)) return -2;
    return type == FsDirEntryType_Dir ? 2 : 1;
}

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> b(size);
    for (size_t i = 0; i < size; i++) b[i] = uint8_t(i * 7 + seed);
    return b;
}

// two handles on one file as NtCreateFile now opens them: read only unless a write right is asked
void TestHandles() {
    std::string p = W("Handles.bin");
    WriteFile(p, 8192, 3);
    // the entry type while a writer holds the file: what NtCreateFile, NtOpenFile and MatchCase now ask
    FILE* rw = std::fopen(p.c_str(), "r+b");
    int kind = EntryType(p);
    Check(rw && kind == 1, "handles.entry_type_while_writing", "r+b %s, entry type %d (expect opened, 1 file; files.cpp "
          "HostKind)", rw ? "opened" : "failed", kind);
    Check(EntryType(kWork) == 2 && EntryType(W("NotThere.bin")) == -2, "handles.entry_type_kinds", "folder %d, missing "
          "%d (expect 2, -2)", EntryType(kWork), EntryType(W("NotThere.bin")));
    errno = 0;
    FILE* rw2 = std::fopen(p.c_str(), "r+b");
    Info("handles.two_writers", "a second r+b while one is open: %s %s (Linux: ok; Horizon: refused, NtCreateFile then "
         "gives STATUS_ACCESS_DENIED)", rw2 ? "ok" : "failed", rw2 ? "" : Err().c_str());
    if (rw2) std::fclose(rw2);
    errno = 0;
    FILE* trunc = std::fopen(p.c_str(), "w+b");
    Info("handles.overwrite_while_writing", "w+b while r+b is open: %s %s (refused must leave the file whole)",
         trunc ? "ok" : "failed", trunc ? "" : Err().c_str());
    if (trunc) std::fclose(trunc);
    struct stat st {};
    int frc = rw ? fstat(fileno(rw), &st) : -1;
    Check(frc == 0 && (trunc || st.st_size == 8192), "handles.refused_open_keeps_data", "size by the writer %lld (expect "
          "8192 when the w+b was refused)", (long long)st.st_size);
    if (rw) std::fclose(rw);

    // a reader open first, then a writer: the game's streams stay open while it opens others
    FILE* r = std::fopen(p.c_str(), "rb");
    errno = 0;
    FILE* w = std::fopen(p.c_str(), "r+b");
    Info("handles.reader_then_writer", "r+b while an rb is open: %s %s", w ? "ok" : "failed", w ? "" : Err().c_str());
    if (w) {
        std::setvbuf(w, nullptr, _IONBF, 0);
        std::fseek(w, 0, SEEK_SET);
        std::fwrite("WXYZ", 1, 4, w);
        char got[4] = {};
        std::setvbuf(r, nullptr, _IONBF, 0);
        std::fseek(r, 0, SEEK_SET);
        size_t n = std::fread(got, 1, 4, r);
        Check(n == 4 && std::memcmp(got, "WXYZ", 4) == 0, "handles.write_seen_by_reader", "the rb handle reads \"%.4s\" "
              "after the r+b wrote WXYZ unbuffered (expect WXYZ)", got);
        std::fclose(w);
    }
    if (r) std::fclose(r);

    // the same file read on two threads through two handles, into code memory through mem::FileRead (each thread
    // its own bounce buffer), as an audio stream reads while a level loads
    const size_t size = 3u << 20;
    std::string big = W("Shared.bin");
    WriteFile(big, size, 9);
    std::vector<uint8_t> want = Pattern(size, 9);
    CodeMem c = MapCode(8u << 20);
    if (!c.at) {
        Check(false, "handles.concurrent_readers", "no code memory (rc %X)", c.rc);
    } else {
        std::atomic<int> bad{0}, reads{0};
        auto reader = [&](int id) {
            FILE* f = std::fopen(big.c_str(), "rb");
            if (!f) {
                bad++;
                return;
            }
            std::setvbuf(f, nullptr, _IONBF, 0);
            std::mt19937 rng(id);
            uint8_t* dst = c.at + id * (4u << 20);
            for (int i = 0; i < 200; i++) {
                size_t len = 0x800 + rng() % 0x60000, at = rng() % (size - len);
                fseeko(f, off_t(at), SEEK_SET);
                size_t got = mem::FileRead(dst, len, f);
                if (got != len || std::memcmp(dst, want.data() + at, len) != 0) bad++;
                reads++;
            }
            std::fclose(f);
        };
        auto t0 = Clock::now();
        std::thread a(reader, 0), b(reader, 1);
        a.join();
        b.join();
        Check(bad == 0, "handles.concurrent_readers", "%d reads on two threads, %d wrong, %.1f ms (expect 0 wrong)",
              reads.load(), bad.load(), Ms(t0, Clock::now()));
        UnmapCode(c);
    }
    std::remove(big.c_str());

    // the runtime's rename onto an existing file on Horizon (NtSetInformationFile class 10): the file in the way moved
    // aside, the rename, then the one aside removed, or put back when the rename fails
    std::string from = W("SaveTmp.bin"), to = W("Save.bin"), aside = to + ".~replaced";
    WriteFile(from, 100, 1);
    WriteFile(to, 200, 2);
    int moved = std::rename(to.c_str(), aside.c_str());
    int ren = std::rename(from.c_str(), to.c_str());
    if (ren == 0) std::remove(aside.c_str());
    else std::rename(aside.c_str(), to.c_str());
    stat(to.c_str(), &st);
    Check(moved == 0 && ren == 0 && st.st_size == 100 && EntryType(aside) == -2, "rename.replace_by_moving_aside",
          "aside %d, rename %d, Save.bin %lld bytes, aside left %d (expect 0 0 100 -2)", moved, ren, (long long)st.st_size,
          EntryType(aside));
    // the same while a reader holds the old file: nothing may be lost
    WriteFile(from, 100, 1);
    FILE* held = std::fopen(to.c_str(), "rb");
    errno = 0;
    moved = std::rename(to.c_str(), aside.c_str());
    std::string merr = moved ? Err() : "";
    ren = moved == 0 ? std::rename(from.c_str(), to.c_str()) : -1;
    if (moved == 0 && ren != 0) std::rename(aside.c_str(), to.c_str());
    if (held) std::fclose(held);
    if (moved == 0 && ren == 0) std::remove(aside.c_str());
    st = {};
    int left = stat(to.c_str(), &st);
    Check(left == 0 && (st.st_size == 100 || st.st_size == 200), "rename.replace_while_open", "aside of an open file %d "
          "%s, rename %d, Save.bin rc %d %lld bytes (expect a whole file either way: 100 renamed, 200 the old one kept)",
          moved, merr.c_str(), ren, left, (long long)st.st_size);
    std::remove(from.c_str());
    std::remove(to.c_str());
}

// the hard disk tree the game's XAPI walks: UDATA\<title>\<save>\SaveMeta.xbx and the save's files
void TestSaves() {
    std::error_code ec;
    std::string title = W("hdd/E/UDATA/45410083");
    fs::create_directories(title + "/0123456789AB", ec);
    fs::create_directories(title + "/BA9876543210", ec);
    WriteFile(title + "/TitleMeta.xbx", 64);
    WriteFile(title + "/0123456789AB/SaveMeta.xbx", 96);
    WriteFile(title + "/0123456789AB/Profile.bin", 20000);
    WriteFile(title + "/BA9876543210/SaveMeta.xbx", 96);
    // XFindFirstSaveGame: the title folder's folders, each with its SaveMeta.xbx; NtQueryDirectoryFile gives each
    // entry's size, times and attributes (files.cpp Describe by path)
    int saves = 0, metas = 0, files = 0;
    for (auto& e : fs::directory_iterator(title, ec)) {
        if (e.is_directory()) {
            saves++;
            struct stat st {};
            if (stat((e.path() / "SaveMeta.xbx").c_str(), &st) == 0 && st.st_size == 96) metas++;
        } else {
            files++;
        }
    }
    Check(saves == 2 && metas == 2 && files == 1, "saves.enumerate", "%d save folders, %d SaveMeta.xbx, %d other files "
          "(expect 2 2 1) ec %s", saves, metas, files, Ec(ec).c_str());
    // the profile being written while the folder is listed: stat by path of the held file, and the entry type
    std::string profile = title + "/0123456789AB/Profile.bin";
    FILE* w = std::fopen(profile.c_str(), "r+b");
    struct stat st {};
    errno = 0;
    int src = stat(profile.c_str(), &st);
    std::string serr = src ? Err() : "";
    Info("saves.stat_while_writing", "stat by path of a save being written rc %d %s (files.cpp then lists it with no "
         "size or time)", src, serr.c_str());
    Check(EntryType(profile) == 1, "saves.entry_type_while_writing", "%d (expect 1: NtQueryFullAttributesFile and the "
          "listing still find it)", EntryType(profile));
    if (w) {
        std::setvbuf(w, nullptr, _IONBF, 0);
        std::vector<uint8_t> b = Pattern(30000, 4);
        std::fseek(w, 0, SEEK_SET);
        std::fwrite(b.data(), 1, b.size(), w);
        int tr = ftruncate(fileno(w), 25000);
        fstat(fileno(w), &st);
        Check(tr == 0 && st.st_size == 25000, "saves.rewrite_and_cut", "grown to 30000 then cut %d to %lld (expect 0, "
              "25000)", tr, (long long)st.st_size);
        std::fclose(w);
    }
    // the free space XAPI asks for (files.cpp answers 8 GB of 16 without asking the host)
    struct statvfs vs {};
    int vrc = statvfs(kRoot, &vs);
    fs::space_info sp = fs::space(kRoot, ec);
    Check(vrc == 0 && vs.f_blocks > 0 && sp.capacity > 0 && !ec, "saves.free_space", "statvfs rc %d, %llu MB free of "
          "%llu MB; fs::space ec %s", vrc, (unsigned long long)(uint64_t(vs.f_bavail) * vs.f_frsize >> 20),
          (unsigned long long)(uint64_t(vs.f_blocks) * vs.f_frsize >> 20), Ec(ec).c_str());
    // file times: UTC seconds against time(); a handle's fstat keeps the time stamps read when it opened
    std::string stamp = title + "/0123456789AB/Stamp.bin";
    WriteFile(stamp, 10);
    time_t made = std::time(nullptr);
    stat(stamp.c_str(), &st);
    long long first = (long long)st.st_mtime;
    Check(std::llabs(first - (long long)made) <= 5, "saves.mtime_utc", "mtime %lld, time() %lld, %+lld s (expect within "
          "5 s: libnx gives UTC through the time zone rule)", first, (long long)made, first - (long long)made);
    svcSleepThread(2500000000ll);
    FILE* f = std::fopen(stamp.c_str(), "r+b");
    if (f) {
        std::setvbuf(f, nullptr, _IONBF, 0);
        std::fwrite("again", 1, 5, f);
        struct stat open_st {};
        fstat(fileno(f), &open_st);
        std::fclose(f);
        stat(stamp.c_str(), &st);
        Info("saves.mtime_after_write", "fstat while open %lld, stat after close %lld, first %lld (libnx: fstat gives "
             "the time of the open; NtQueryInformationFile on a written handle reports that)",
             (long long)open_st.st_mtime, (long long)st.st_mtime, first);
    }
    RemoveTree(W("hdd"));
}

// guest memory as MapChunks makes it: 1 MB chunks, a run of new chunks one code memory object, never unmapped
void TestGuestMemory() {
    // how far the heap goes in code memory objects of 4 MB (the game: 64 MB physical, the write combined view's 64 MB
    // when svcMapProcessMemory is not allowed, about 10 MB of virtual allocations)
    std::vector<CodeMem> runs;
    size_t mb = 0;
    while (mb < 1024) {
        CodeMem c = MapCode(4u << 20);
        if (!c.at) {
            Info("guestmem.limit_rc", "the run after %zu MB failed: rc %X", mb, c.rc);
            UnmapCode(c);
            break;
        }
        runs.push_back(c);
        mb += 4;
    }
    u64 used = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    Check(mb >= 160, "guestmem.capacity", "%zu MB in %zu code memory objects of 4 MB, process memory used %llu MB "
          "(expect >= 160: 64 physical + 64 write combined + virtual)", mb, runs.size(),
          (unsigned long long)(used >> 20));
    // accesses across two objects that sit next to each other in the guest's space are what MapChunks makes; here
    // each object has its own reservation, so the boundary is checked within one: unaligned scalars and NEON
    if (!runs.empty()) {
        uint8_t* m = runs[0].at;
        volatile uint32_t* u32p = reinterpret_cast<volatile uint32_t*>(m + 0xFFE);
        *u32p = 0x11223344;
        uint64_t v64 = 0x0102030405060708ull;
        std::memcpy(m + 0x1FFB, &v64, 8);
        uint64_t back = 0;
        std::memcpy(&back, m + 0x1FFB, 8);
        typedef uint8_t v16 __attribute__((vector_size(16), aligned(1)));
        v16 vec;
        for (int i = 0; i < 16; i++) vec[i] = uint8_t(i);
        *reinterpret_cast<v16*>(m + 0x2FF9) = vec;
        v16 vb = *reinterpret_cast<v16*>(m + 0x2FF9);
        bool same = true;
        for (int i = 0; i < 16; i++) same = same && vb[i] == i;
        Check(*u32p == 0x11223344 && back == v64 && same, "guestmem.unaligned", "4, 8 and 16 byte accesses across page "
              "boundaries at odd addresses in code memory (expect equal)");
        // the lock ops the generated code emits: xchg of a byte and a word, xadd, cmpxchg, all aligned
        uint8_t* b = m + 0x3001;
        uint8_t old8 = __atomic_exchange_n(b, uint8_t(5), __ATOMIC_SEQ_CST);
        auto* w = reinterpret_cast<uint32_t*>(m + 0x3004);
        uint32_t old32 = __atomic_exchange_n(w, 7u, __ATOMIC_SEQ_CST);
        uint32_t add = __atomic_fetch_add(w, 3u, __ATOMIC_SEQ_CST);
        uint32_t expect = 10;
        bool cas = __atomic_compare_exchange_n(w, &expect, 11u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        Check(old8 == 0 && *b == 5 && old32 == 0 && add == 7 && cas && *w == 11, "guestmem.lock_ops", "xchg8 %u->%u, "
              "xchg32 %u, xadd %u, cmpxchg %d -> %u (expect 0->5, 0, 7, 1 -> 11)", old8, *b, old32, add, int(cas), *w);
    }
    for (CodeMem& c : runs) UnmapCode(c);
    // FPCR as threads start: the generated code expects round to nearest and no flush to zero
    auto fpcr = [] {
        uint64_t v;
        asm volatile("mrs %0, fpcr" : "=r"(v));
        return v;
    };
    uint64_t main_fpcr = fpcr(), std_fpcr = ~0ull;
    std::thread([&] { std_fpcr = fpcr(); }).join();
    Check(main_fpcr == 0 && std_fpcr == 0, "cpu.fpcr", "main %llX, std::thread %llX (expect 0 0: nearest, no flush to "
          "zero, no default NaN)", (unsigned long long)main_fpcr, (unsigned long long)std_fpcr);
    Info("process.svc_exit", "ExitProcess 0x07 %d, OutputDebugString 0x27 %d (newlib_hooks.cpp ends the process with "
         "svcExitProcess; the crash handler writes with svcOutputDebugString)", envIsSyscallHinted(0x07),
         envIsSyscallHinted(0x27));
}

// a LOCK op on an address that is not a multiple of its size: x86 allows it, an A57 exclusive faults. Opt in, last:
// run when sdmc:/switch/black/selftest-misaligned exists; a fault ends the program here (no summary after it)
void TestMisalignedLock() {
    CodeMem c = MapCode(1u << 20);
    if (!c.at) return;
    auto* w = reinterpret_cast<uint32_t*>(c.at + 0x102);
    std::printf("INFO lock.misaligned: xadd on %p next; a crash here is the alignment fault\n", static_cast<void*>(w));
    std::fflush(stdout);
    uint32_t old = __atomic_fetch_add(w, 1u, __ATOMIC_SEQ_CST);
    Info("lock.misaligned", "xadd on an address 2 mod 4 returned %u, now %u (no fault on this console)", old, *w);
    UnmapCode(c);
}

std::string FindGameFile() {
    std::error_code ec;
    std::string game = std::string(kRoot) + "/game";
    if (!fs::is_directory(game, ec)) return {};
    // Sound\DSP\BkXboxFx.bin with the names as the disc has them
    std::string cur = game;
    for (const char* part : {"sound", "dsp", "bkxboxfx.bin"}) {
        std::string found;
        for (auto& e : fs::directory_iterator(cur, ec))
            if (strcasecmp(e.path().filename().c_str(), part) == 0) found = e.path().string();
        if (found.empty()) return {};
        cur = found;
    }
    return cur;
}

// a folder and all in it, by hand: fs::remove_all depends on lstat
void RemoveTree(const std::string& root) {
    std::error_code ec;
    std::vector<std::string> dirs;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_directory()) dirs.push_back(it->path().string());
        else std::remove(it->path().c_str());
    }
    for (auto d = dirs.rbegin(); d != dirs.rend(); ++d) rmdir(d->c_str());
    rmdir(root.c_str());
}

}  // namespace

int main() {
    chdir(kRoot);
    std::error_code ec;
    RemoveTree(kWork);
    fs::create_directories(kWork, ec);
    g_log = std::fopen((std::string(kRoot) + "/selftest.log").c_str(), "w");
    std::printf("black self-test: %s\n", kWork);
    std::fflush(stdout);

    std::string game_file = FindGameFile();
    Info("game", "%s", game_file.empty() ? "no sdmc:/switch/black/game/Sound/DSP/BkXboxFx.bin" : game_file.c_str());
    std::string data = W("data.bin");
    const size_t data_size = 3u << 20 | 0x123;
    WriteFile(data, data_size, 5);

    TestProcess();
    TestClocks();
    TestCondvars();
    TestThreads();
    TestCodeMemory(data, data_size);
    TestStdio();
    TestLocks(game_file);
    TestRenameRemove();
    TestFilesystem();
    TestLibc();
    TestHandles();
    TestSaves();
    TestGuestMemory();

    RemoveTree(kWork);
    // the detached test threads have ended: the loader unmaps this program once main returns
    svcSleepThread(200000000);
    char done[128];
    std::snprintf(done, sizeof done, "black self-test: %d pass, %d fail, %d info\n", g_pass, g_fail, g_info);
    std::fputs(done, stdout);
    if (g_log) {
        std::fputs(done, g_log);
        std::fclose(g_log);
    }
    std::fflush(stdout);
    struct stat opt {};
    if (stat((std::string(kRoot) + "/selftest-misaligned").c_str(), &opt) == 0) TestMisalignedLock();
    return 0;
}
