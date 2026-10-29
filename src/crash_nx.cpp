// Switch only: write the crash location (and a frame-pointer backtrace) next to the save data so field crashes
// can be symbolized against the ELF with addr2line, then let libnx raise the normal error.
#ifdef __SWITCH__
#include <switch.h>
#include <stdio.h>
#include <string>

extern std::string g_logPath;

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
    std::string path = g_logPath.empty() ? std::string("sdmc:/switch/superhexagon/log.txt") : g_logPath;
    FILE* f = fopen((path + ".crash").c_str(), "w");
    if (!f) {
        return;
    }
    MemoryInfo mi;
    u32 pi;
    svcQueryMemory(&mi, &pi, ctx->pc.x);
    fprintf(f, "exception type=%u pc=%llx (region base %llx, offset %llx) lr=%llx sp=%llx fp=%llx fault_addr=%llx\n",
            ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)mi.addr,
            (unsigned long long)(ctx->pc.x - mi.addr), (unsigned long long)ctx->lr.x, (unsigned long long)ctx->sp.x,
            (unsigned long long)ctx->fp.x, (unsigned long long)ctx->far.x);
    svcQueryMemory(&mi, &pi, ctx->lr.x);
    fprintf(f, "lr region base %llx offset %llx\n", (unsigned long long)mi.addr, (unsigned long long)(ctx->lr.x - mi.addr));
    for (int i = 0; i < 29; ++i) {
        fprintf(f, "x%d=%llx ", i, (unsigned long long)ctx->cpu_gprs[i].x);
    }
    fprintf(f, "\n");
    u64 fp = ctx->fp.x;
    for (int i = 0; i < 20 && fp && !(fp & 7); ++i) {
        u64* fr = (u64*)fp;
        u64 ret = fr[1];
        svcQueryMemory(&mi, &pi, ret);
        fprintf(f, "  bt %d: %llx (base %llx + %llx)\n", i, (unsigned long long)ret, (unsigned long long)mi.addr,
                (unsigned long long)(ret - mi.addr));
        fp = fr[0];
    }
    fclose(f);
}
#endif
