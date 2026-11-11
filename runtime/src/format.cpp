// printf-family implementation over guest arguments.
#include "kernel.h"

#include <string>

namespace
{
    // Source of variadic arguments: registers/stack (sprintf) or a va_list
    // (vsprintf). Both use 64-bit slots; 32-bit values are in the low word.
    struct ArgSource
    {
        PPCContext* ctx = nullptr;  // register mode
        int next = 0;
        uint32_t va = 0;            // va_list mode

        uint64_t Next()
        {
            if (ctx)
                return GetArg(*ctx, next++);
            uint64_t v = Load64(va);
            va += 8;
            return v;
        }
    };

    std::string FormatGuest(const char* fmt, ArgSource& args)
    {
        std::string out;
        if (!fmt)
            return out;
        char tmp[512];

        for (const char* p = fmt; *p; p++)
        {
            if (*p != '%')
            {
                out.push_back(*p);
                continue;
            }
            const char* specStart = p++;
            if (*p == '%')
            {
                out.push_back('%');
                continue;
            }

            std::string spec = "%";
            while (*p && strchr("-+ #0", *p))
                spec.push_back(*p++);
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                p++;
            }
            else
                while (*p >= '0' && *p <= '9')
                    spec.push_back(*p++);
            if (*p == '.')
            {
                spec.push_back(*p++);
                if (*p == '*')
                {
                    spec += std::to_string(int32_t(args.Next()));
                    p++;
                }
                else
                    while (*p >= '0' && *p <= '9')
                        spec.push_back(*p++);
            }

            // Length modifiers
            int longness = 0;   // 0 = int, 1 = long (32-bit on the console), 2 = 64-bit
            bool shortArg = false, wide = false;
            for (;;)
            {
                if (*p == 'h') { shortArg = true; p++; }
                else if (*p == 'l') { longness++; wide = true; p++; }
                else if (*p == 'w') { wide = true; p++; }
                else if (*p == 'L' || *p == 'q' || *p == 'j') { longness = 2; p++; }
                else if (*p == 'z' || *p == 't') { p++; }
                else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { longness = 2; p += 3; }
                else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
                else break;
            }
            bool is64 = longness >= 2;
            char conv = *p;
            if (!conv)
                break;

            switch (conv)
            {
            case 'd': case 'i':
            {
                uint64_t raw = args.Next();
                long long v = is64 ? (long long)int64_t(raw) : shortArg ? (long long)int16_t(raw) : (long long)int32_t(raw);
                snprintf(tmp, sizeof(tmp), (spec + "lld").c_str(), v);
                out += tmp;
                break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                uint64_t raw = args.Next();
                unsigned long long v = is64 ? raw : shortArg ? uint16_t(raw) : uint32_t(raw);
                snprintf(tmp, sizeof(tmp), (spec + "ll" + conv).c_str(), v);
                out += tmp;
                break;
            }
            case 'p':
                snprintf(tmp, sizeof(tmp), "%08X", uint32_t(args.Next()));
                out += tmp;
                break;
            case 'c': case 'C':
            {
                uint32_t c = uint32_t(args.Next());
                out.push_back(char(c < 0x100 ? c : '?'));
                break;
            }
            case 's': case 'S':
            {
                uint32_t ptr = uint32_t(args.Next());
                std::string s;
                bool w = (conv == 'S') ? !shortArg : wide;
                if (!ptr)
                    s = "(null)";
                else if (w)
                    s = GuestWideToAscii(ptr);
                else
                    s = GuestStr(ptr);
                snprintf(tmp, sizeof(tmp), (spec + "s").c_str(), s.c_str());
                out += tmp;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                uint64_t raw = args.Next();
                double d;
                memcpy(&d, &raw, 8);
                snprintf(tmp, sizeof(tmp), (spec + conv).c_str(), d);
                out += tmp;
                break;
            }
            case 'n':
                args.Next();
                break;
            default:
                out.append(specStart, p + 1);
                break;
            }
        }
        return out;
    }

    uint32_t WriteResult(uint32_t buffer, const std::string& s)
    {
        memcpy(GuestToHost(buffer), s.c_str(), s.size() + 1);
        return uint32_t(s.size());
    }
}

GUEST_IMPORT(sprintf)
{
    ArgSource args{ &ctx, 2, 0 };
    RETURN(WriteResult(ARG32(0), FormatGuest(GuestStr(ARG32(1)), args)));
}

GUEST_IMPORT(vsprintf)
{
    ArgSource args{ nullptr, 0, ARG32(2) };
    RETURN(WriteResult(ARG32(0), FormatGuest(GuestStr(ARG32(1)), args)));
}

GUEST_IMPORT(_vsnprintf)
{
    // (buffer, count, format, va_list); MS semantics: -1 when truncated
    uint32_t buffer = ARG32(0), count = ARG32(1);
    ArgSource args{ nullptr, 0, ARG32(3) };
    std::string s = FormatGuest(GuestStr(ARG32(2)), args);
    if (s.size() > count)
    {
        memcpy(GuestToHost(buffer), s.data(), count);
        RETURN(int64_t(-1));
    }
    memcpy(GuestToHost(buffer), s.data(), s.size());
    if (s.size() < count)
        Store8(buffer + uint32_t(s.size()), 0);
    RETURN(s.size());
}

GUEST_IMPORT(DbgPrint)
{
    ArgSource args{ &ctx, 1, 0 };
    std::string s = FormatGuest(GuestStr(ARG32(0)), args);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    LOG("[DbgPrint] %s", s.c_str());
    RETURN(STATUS_SUCCESS);
}
