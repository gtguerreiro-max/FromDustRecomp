#include "guest_printf.h"

#include <cstdio>
#include <cstring>

namespace kernel
{
    GuestArgs GuestArgs::FromRegisters(const PPCContext& ctx, int firstReg)
    {
        GuestArgs args;
        args.ctx_ = &ctx;
        args.reg_ = firstReg;
        args.stack_ = ctx.r1.u32 + 0x50;
        return args;
    }

    GuestArgs GuestArgs::FromVaList(uint32_t vaList)
    {
        GuestArgs args;
        args.stack_ = vaList;
        return args;
    }

    uint64_t GuestArgs::Next()
    {
        if (ctx_ && reg_ <= 10)
        {
            const PPCRegister* regs[] = { &ctx_->r3, &ctx_->r4, &ctx_->r5, &ctx_->r6, &ctx_->r7, &ctx_->r8, &ctx_->r9, &ctx_->r10 };
            return regs[reg_++ - 3]->u64;
        }
        const uint64_t value = *mem::Ptr<be<uint64_t>>(stack_);
        stack_ += 8;
        return value;
    }

    std::string FormatGuest(const char* format, GuestArgs& args)
    {
        std::string out;
        char spec[32], buffer[512];
        for (const char* p = format; *p; ++p)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }
            if (p[1] == '%')
            {
                out += '%';
                ++p;
                continue;
            }

            // %[flags][width][.precision][length]conversion
            const char* start = p++;
            while (*p && strchr("-+ #0", *p))
                ++p;
            auto number = [&](int& value) {
                if (*p == '*')
                {
                    value = int32_t(args.Next());
                    ++p;
                    return true;
                }
                bool any = false;
                while (*p >= '0' && *p <= '9')
                {
                    any = true;
                    ++p;
                }
                return any;
            };
            int width = -1, precision = -1;
            const bool starWidth = *p == '*';
            number(width);
            bool starPrecision = false;
            if (*p == '.')
            {
                ++p;
                starPrecision = *p == '*';
                number(precision);
            }

            bool wide = false, is64 = false;
            if (*p == 'h') { ++p; if (*p == 'h') ++p; }
            else if (*p == 'l') { ++p; wide = true; if (*p == 'l') { ++p; is64 = true; } }
            else if (*p == 'L' || *p == 'q' || *p == 'j' || *p == 'z' || *p == 't') { is64 = *p != 'z' && *p != 't'; ++p; }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { p += 3; is64 = true; }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
            const char conv = *p;
            if (!conv)
                break;

            // Rebuild a host spec without the length modifier.
            std::string flags(start, p);
            for (const char* m : { "I64", "I32", "ll", "hh" })
            {
                for (size_t pos; (pos = flags.find(m)) != std::string::npos;)
                    flags.erase(pos, strlen(m));
            }
            for (char m : { 'l', 'h', 'L', 'q', 'j', 'z', 't' })
            {
                for (size_t pos; (pos = flags.find(m)) != std::string::npos;)
                    flags.erase(pos, 1);
            }
            // Width/precision given as '*' were already consumed; pass them through.
            auto emit = [&](auto value) {
                snprintf(spec, sizeof(spec), "%s%c", flags.c_str(), conv);
                if (starWidth && starPrecision)
                    snprintf(buffer, sizeof(buffer), spec, width, precision, value);
                else if (starWidth)
                    snprintf(buffer, sizeof(buffer), spec, width, value);
                else if (starPrecision)
                    snprintf(buffer, sizeof(buffer), spec, precision, value);
                else
                    snprintf(buffer, sizeof(buffer), spec, value);
                out += buffer;
            };

            switch (conv)
            {
            case 'd': case 'i':
                if (is64) emit((long long)args.Next()); else emit(int32_t(args.Next()));
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (is64) emit((unsigned long long)args.Next()); else emit(uint32_t(args.Next()));
                break;
            case 'c':
                emit(int(uint8_t(args.Next())));
                break;
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
                out += buffer;
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                const uint64_t bits = args.Next();
                double d;
                memcpy(&d, &bits, 8);
                emit(d);
                break;
            }
            case 's': case 'S':
            {
                const uint32_t ptr = uint32_t(args.Next());
                std::string s;
                if (!ptr)
                    s = "(null)";
                else if (conv == 'S' || wide)
                {
                    for (uint32_t a = ptr; uint16_t c = *mem::Ptr<be<uint16_t>>(a); a += 2)
                        s += c < 0x80 ? char(c) : '?';
                }
                else
                    s = mem::Ptr<char>(ptr);
                emit(s.c_str());
                break;
            }
            case 'n':
                args.Next();
                break;
            default:
                out.append(start, p + 1);
                break;
            }
        }
        return out;
    }
}
