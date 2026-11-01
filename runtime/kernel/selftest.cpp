// FD_SELFTEST=save: exercises the save-game path the way the game drives it
// (XamContentCreateEx -> NtCreateFile/NtWriteFile -> NtClose -> XamContentClose,
// then reopen with "open existing" and read back), without playing to a save
// point. Uses a separate content name so the real save is untouched.

#include <cstring>
#include <filesystem>

#include "kernel.h"
#include "vfs.h"

PPC_EXTERN_FUNC(__imp__XamContentCreateEx);
PPC_EXTERN_FUNC(__imp__XamContentClose);
PPC_EXTERN_FUNC(__imp__NtCreateFile);
PPC_EXTERN_FUNC(__imp__NtOpenFile);
PPC_EXTERN_FUNC(__imp__NtWriteFile);
PPC_EXTERN_FUNC(__imp__NtReadFile);
PPC_EXTERN_FUNC(__imp__NtClose);

namespace kernel::selftest
{
namespace
{
constexpr uint32_t FILE_OPEN = 1, FILE_OVERWRITE_IF = 5;

struct Guest
{
    PPCContext ctx{};
    uint32_t scratch = mem::AllocVirtual(0x10000);
    uint32_t next = 0x2000;
    Guest() { ctx.r1.u64 = scratch + 0x1000; } // stack: STACK_ARG reads r1+0x54...

    uint32_t Alloc(uint32_t size)
    {
        const uint32_t address = scratch + next;
        next += (size + 15) & ~15u;
        memset(mem::Translate(address), 0, size);
        return address;
    }
    uint32_t String(const char* text)
    {
        const uint32_t address = Alloc(uint32_t(strlen(text)) + 1);
        strcpy(mem::Ptr<char>(address), text);
        return address;
    }
    // OBJECT_ATTRIBUTES { root, ANSI_STRING* name, attributes }
    uint32_t ObjectAttributes(const char* path)
    {
        const uint32_t name = Alloc(8), attributes = Alloc(12);
        *mem::Ptr<be<uint16_t>>(name) = uint16_t(strlen(path));
        *mem::Ptr<be<uint16_t>>(name + 2) = uint16_t(strlen(path) + 1);
        mem::Write32(name + 4, String(path));
        mem::Write32(attributes + 4, name);
        return attributes;
    }
    uint32_t Call(void (*fn)(PPCContext&, uint8_t*), std::initializer_list<uint32_t> args, uint32_t stack0 = 0)
    {
        PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
        uint32_t i = 0;
        for (uint32_t a : args) regs[i++]->u64 = a;
        mem::Write32(ctx.r1.u32 + 0x54, stack0);
        fn(ctx, mem::g_base);
        return ctx.r3.u32;
    }
};

bool Open(Guest& g, uint32_t content, uint32_t flags)
{
    const uint32_t r = g.Call(__imp__XamContentCreateEx, {0, g.String("selftest"), content, flags, g.Alloc(4), g.Alloc(4), 0, 0});
    return r == 0;
}
}

bool RunSave()
{
    Guest g;
    // XCONTENT_DATA: device 1, type 1 (saved game), file name at 0x108.
    const uint32_t content = g.Alloc(0x134);
    mem::Write32(content, 1);
    mem::Write32(content + 4, 1);
    strcpy(mem::Ptr<char>(content + 0x108), "FDSELFTEST");
    const char payload[] = "From Dust save self-test 0123456789";
    const uint32_t buffer = g.String(payload), iosb = g.Alloc(8), handle = g.Alloc(4);

    bool ok = Open(g, content, 2 /* create always */);
    ok = ok && g.Call(__imp__NtCreateFile, {handle, 0x40000000 | 0x80000000, g.ObjectAttributes("selftest:\\slot0.sav"),
        iosb, 0, 0x80, 0, FILE_OVERWRITE_IF}, 0) == 0;
    ok = ok && g.Call(__imp__NtWriteFile, {mem::Read32(handle), 0, 0, 0, iosb, buffer, sizeof(payload), 0}) == 0;
    if (ok) g.Call(__imp__NtClose, {mem::Read32(handle)});
    g.Call(__imp__XamContentClose, {g.String("selftest"), 0});
    Log("selftest save: write %s", ok ? "ok" : "FAILED");

    const uint32_t readBack = g.Alloc(sizeof(payload));
    bool read = ok && Open(g, content, 3 /* open existing */);
    read = read && g.Call(__imp__NtOpenFile, {handle, 0x80000000, g.ObjectAttributes("selftest:\\slot0.sav"), iosb, 7, 0}) == 0;
    read = read && g.Call(__imp__NtReadFile, {mem::Read32(handle), 0, 0, 0, iosb, readBack, sizeof(payload), 0}) == 0;
    read = read && memcmp(mem::Translate(readBack), payload, sizeof(payload)) == 0;
    if (read) g.Call(__imp__NtClose, {mem::Read32(handle)});
    g.Call(__imp__XamContentClose, {g.String("selftest"), 0});
    Log("selftest save: reopen and read back %s", read ? "ok" : "FAILED");

    std::error_code ec;
    std::filesystem::remove_all(vfs::UserDirectory() / "content" / "00000001" / "FDSELFTEST", ec);
    return ok && read;
}
}
