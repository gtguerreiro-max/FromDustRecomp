// Rtl string, memory and time helpers.

#include <cstring>
#include <cwctype>

#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace kernel;

// ANSI_STRING / UNICODE_STRING: { u16 Length, u16 MaximumLength, u32 Buffer }

KFUNC(RtlInitAnsiString)
{
    const uint32_t dst = ARG0, src = ARG1;
    const uint16_t length = src ? uint16_t(strlen(mem::Ptr<char>(src))) : 0;
    *mem::Ptr<be<uint16_t>>(dst) = length;
    *mem::Ptr<be<uint16_t>>(dst + 2) = uint16_t(src ? length + 1 : 0);
    mem::Write32(dst + 4, src);
    RETURN(0);
}

KFUNC(RtlInitUnicodeString)
{
    const uint32_t dst = ARG0, src = ARG1;
    uint16_t length = 0;
    if (src)
    {
        while (*mem::Ptr<be<uint16_t>>(src + length))
            length += 2;
    }
    *mem::Ptr<be<uint16_t>>(dst) = length;
    *mem::Ptr<be<uint16_t>>(dst + 2) = uint16_t(src ? length + 2 : 0);
    mem::Write32(dst + 4, src);
    RETURN(0);
}

KFUNC(RtlFreeAnsiString)
{
    mem::Free(mem::Read32(ARG0 + 4));
    mem::Write32(ARG0 + 4, 0);
    RETURN(0);
}

KFUNC(RtlUnicodeStringToAnsiString)
{
    // (dst ANSI_STRING*, src UNICODE_STRING*, allocate)
    const uint32_t dst = ARG0, src = ARG1;
    const uint16_t chars = *mem::Ptr<be<uint16_t>>(src) / 2;
    const uint32_t srcBuffer = mem::Read32(src + 4);
    uint32_t buffer = mem::Read32(dst + 4);
    if (ARG2)
    {
        buffer = mem::AllocVirtual(chars + 1u, 16);
        mem::Write32(dst + 4, buffer);
        *mem::Ptr<be<uint16_t>>(dst + 2) = uint16_t(chars + 1);
    }
    else if (*mem::Ptr<be<uint16_t>>(dst + 2) < chars + 1)
    {
        RETURN(0x80000005); // STATUS_BUFFER_OVERFLOW
    }
    for (uint16_t i = 0; i < chars; i++)
    {
        const uint16_t c = *mem::Ptr<be<uint16_t>>(srcBuffer + i * 2u);
        mem::Ptr<char>(buffer)[i] = c < 0x100 ? char(c) : '?';
    }
    mem::Ptr<char>(buffer)[chars] = 0;
    *mem::Ptr<be<uint16_t>>(dst) = chars;
    RETURN(STATUS_SUCCESS);
}

KFUNC(RtlMultiByteToUnicodeN)
{
    // (dst, dst_bytes, written_ptr, src, src_bytes)
    const uint32_t count = ARG4 < ARG1 / 2 ? ARG4 : ARG1 / 2;
    for (uint32_t i = 0; i < count; i++)
        *mem::Ptr<be<uint16_t>>(ARG0 + i * 2) = uint16_t(uint8_t(mem::Ptr<char>(ARG3)[i]));
    if (ARG2)
        mem::Write32(ARG2, count * 2);
    RETURN(STATUS_SUCCESS);
}

KFUNC(RtlUnicodeToMultiByteN)
{
    // (dst, dst_bytes, written_ptr, src, src_bytes)
    const uint32_t count = ARG4 / 2 < ARG1 ? ARG4 / 2 : ARG1;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint16_t c = *mem::Ptr<be<uint16_t>>(ARG3 + i * 2);
        mem::Ptr<char>(ARG0)[i] = c < 0x100 ? char(c) : '?';
    }
    if (ARG2)
        mem::Write32(ARG2, count);
    RETURN(STATUS_SUCCESS);
}

KFUNC(RtlUpcaseUnicodeChar) { RETURN(uint16_t(towupper(wchar_t(ARG0 & 0xFFFF)))); }

KFUNC(RtlCompareStringN)
{
    // (s1, len1, s2, len2, case_insensitive)
    const char* a = mem::Ptr<char>(ARG0);
    const char* b = mem::Ptr<char>(ARG2);
    const uint32_t n = ARG1 < ARG3 ? ARG1 : ARG3;
    const int r = ARG4 ? _strnicmp(a, b, n) : strncmp(a, b, n);
    RETURN(uint32_t(r != 0 ? r : int(ARG1) - int(ARG3)));
}

KFUNC(RtlCompareMemory)
{
    const uint8_t* a = mem::Ptr<uint8_t>(ARG0);
    const uint8_t* b = mem::Ptr<uint8_t>(ARG1);
    uint32_t i = 0;
    while (i < ARG2 && a[i] == b[i])
        ++i;
    RETURN(i);
}

KFUNC(RtlCompareMemoryUlong)
{
    // (src, length, pattern) -> bytes matching
    uint32_t i = 0;
    while (i + 4 <= ARG1 && mem::Read32(ARG0 + i) == ARG2)
        i += 4;
    RETURN(i);
}

KFUNC(RtlFillMemoryUlong)
{
    for (uint32_t i = 0; i + 4 <= ARG1; i += 4)
        mem::Write32(ARG0 + i, ARG2);
    RETURN(0);
}

// TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (u16)
KFUNC(RtlTimeToTimeFields)
{
    const uint64_t time = *mem::Ptr<be<uint64_t>>(ARG0);
    FILETIME ft{ DWORD(time), DWORD(time >> 32) };
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    const uint16_t fields[] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, st.wDayOfWeek };
    for (int i = 0; i < 8; i++)
        *mem::Ptr<be<uint16_t>>(ARG1 + i * 2) = fields[i];
    RETURN(0);
}

KFUNC(RtlTimeFieldsToTime)
{
    SYSTEMTIME st{};
    uint16_t* f[] = { &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond, &st.wMilliseconds };
    for (int i = 0; i < 7; i++)
        *f[i] = *mem::Ptr<be<uint16_t>>(ARG0 + i * 2);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        RETURN(0);
    *mem::Ptr<be<uint64_t>>(ARG1) = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    RETURN(1);
}
