// msvcr71.dll for NFSC.exe: printf/scanf formatting, FILE streams, low-level file descriptors, directory search and
// paths. Files go through the kernel32 shims (their paths map onto the data folder, see lib/file.h); stdout and stderr
// go to the log, line by line.

#include <winapi/msvcr71.h>
#include "crt.h"

#include <lib/file.h>
#include <winapi/kernel32.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace win32 { namespace crt
{

namespace
{

// One conversion of a printf format: its host spec (flags, width, precision) and the guest arguments it reads.
template< typename Ch >
struct Formatter
{
    std::basic_string<Ch> out;
    reg32 args;
    bool wide;      // wprintf rules: %s and %c are wide, %S and %C narrow

    reg32 next32()
    {
        const reg32 v = ld32(args);
        args += 4;
        return v;
    }

    uint64_t next64()
    {
        const uint64_t lo = next32();
        return lo | uint64_t(next32()) << 32;
    }

    double nextDouble()
    {
        const double d = ld64f(args);
        args += 8;
        return d;
    }

    void append(const char* s, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            out.push_back(Ch(static_cast<unsigned char>(s[i])));
    }

    void pad(const std::basic_string<Ch>& text, int width, bool left)
    {
        const int fill = width > int(text.size()) ? width - int(text.size()) : 0;
        if (!left)
            out.append(size_t(fill), Ch(' '));
        out += text;
        if (left)
            out.append(size_t(fill), Ch(' '));
    }

    void run(const Ch* f)
    {
        while (*f)
        {
            if (*f != '%')
            {
                out.push_back(*f++);
                continue;
            }
            ++f;
            if (*f == '%')
            {
                out.push_back(*f++);
                continue;
            }
            std::string spec = "%";
            bool left = false;
            while (*f == '-' || *f == '+' || *f == ' ' || *f == '#' || *f == '0')
            {
                left = left || *f == '-';
                spec += char(*f++);
            }
            int width = -1, precision = -1;
            if (*f == '*')
            {
                width = int32_t(next32());
                if (width < 0)
                {
                    left = true;
                    spec += '-';
                    width = -width;
                }
                ++f;
            }
            else
                for (width = 0; *f >= '0' && *f <= '9'; ++f)
                    width = width * 10 + (*f - '0');
            if (*f == '.')
            {
                ++f;
                if (*f == '*')
                {
                    precision = int32_t(next32());
                    ++f;
                }
                else
                    for (precision = 0; *f >= '0' && *f <= '9'; ++f)
                        precision = precision * 10 + (*f - '0');
            }
            if (width > 0)
                spec += std::to_string(width);
            if (precision >= 0)
                spec += "." + std::to_string(precision);
            // Sizes: h, l, ll, L, I64, I32, I (pointer size).
            bool longLong = false, shortInt = false, wideArg = wide, narrowArg = !wide;
            for (;;)
            {
                if (*f == 'h') { shortInt = true; narrowArg = true; wideArg = false; ++f; }
                else if (*f == 'l')
                {
                    ++f;
                    if (*f == 'l') { longLong = true; ++f; }
                    else { wideArg = true; narrowArg = false; }
                }
                else if (*f == 'L') { ++f; }
                else if (*f == 'w') { wideArg = true; narrowArg = false; ++f; }
                else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { longLong = true; f += 3; }
                else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { f += 3; }
                else if (*f == 'I') { ++f; }
                else break;
            }
            const char type = char(*f);
            if (!*f)
                break;
            ++f;
            char buffer[512];
            int n = 0;
            switch (type)
            {
            case 'd': case 'i':
                if (longLong)
                    n = snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), (long long)int64_t(next64()));
                else
                {
                    int32_t v = int32_t(next32());
                    if (shortInt)
                        v = int16_t(v);
                    n = snprintf(buffer, sizeof(buffer), (spec + "ld").c_str(), long(v));
                }
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (longLong)
                    n = snprintf(buffer, sizeof(buffer), (spec + "ll" + type).c_str(), (unsigned long long)next64());
                else
                {
                    uint32_t v = next32();
                    if (shortInt)
                        v = uint16_t(v);
                    n = snprintf(buffer, sizeof(buffer), (spec + "l" + type).c_str(), (unsigned long)v);
                }
                break;
            case 'p':
                n = snprintf(buffer, sizeof(buffer), "%08lX", (unsigned long)next32());
                break;
            case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A':
                n = snprintf(buffer, sizeof(buffer), (spec + type).c_str(), nextDouble());
                break;
            case 'c': case 'C':
            {
                const bool w = type == 'C' ? !wide || wideArg : wideArg && !narrowArg;
                const reg32 c = next32();
                std::basic_string<Ch> text(1, Ch(w ? (c & 0xffff) : (c & 0xff)));
                pad(text, width, left);
                continue;
            }
            case 's': case 'S':
            {
                const bool w = type == 'S' ? !wide : wideArg && !narrowArg;
                const reg32 s = next32();
                std::basic_string<Ch> text;
                const size_t limit = precision >= 0 ? size_t(precision) : size_t(-1);
                if (!s)
                    for (const char* p = w ? "(null)" : "(null)"; *p && text.size() < limit; ++p)
                        text.push_back(Ch(*p));
                else if (w)
                    for (reg32 i = 0; text.size() < limit; ++i)
                    {
                        x86::reg16 c;
                        memcpy(&c, g_guestBase + s + 2 * i, 2);
                        if (!c)
                            break;
                        text.push_back(Ch(c));
                    }
                else
                    for (const char* p = ptr(s); *p && text.size() < limit; ++p)
                        text.push_back(Ch(static_cast<unsigned char>(*p)));
                pad(text, width, left);
                continue;
            }
            case 'n':
            {
                const reg32 at = next32();
                if (at)
                    st32(at, reg32(out.size()));
                continue;
            }
            default:
                out.push_back(Ch('%'));
                out.push_back(Ch(type));
                continue;
            }
            append(buffer, size_t(std::max(0, std::min<int>(n, int(sizeof(buffer)) - 1))));
        }
    }
};

}

std::string format(const char* fmt, reg32 args)
{
    Formatter<char> f{std::string(), args, false};
    if (fmt)
        f.run(fmt);
    return f.out;
}

std::u16string formatWide(const char16_t* fmt, reg32 args)
{
    Formatter<char16_t> f{std::u16string(), args, true};
    if (fmt)
        f.run(fmt);
    return f.out;
}

int scan(const char* input, const char* fmt, reg32 args)
{
    const char* in = input;
    int assigned = 0;
    bool converted = false;
    auto next = [&]() { const reg32 a = ld32(args); args += 4; return a; };
    while (*fmt)
    {
        if (isspace(static_cast<unsigned char>(*fmt)))
        {
            while (isspace(static_cast<unsigned char>(*in)))
                ++in;
            ++fmt;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%')
        {
            if (*fmt == '%')
                ++fmt;
            if (!*in)
                return converted ? assigned : -1;
            if (*in != *fmt)
                return assigned;
            ++in;
            ++fmt;
            continue;
        }
        ++fmt;
        const bool suppress = *fmt == '*';
        if (suppress)
            ++fmt;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        bool isShort = false, isLong = false, isLongLong = false;
        for (;;)
        {
            if (*fmt == 'h') { isShort = true; ++fmt; }
            else if (*fmt == 'l') { ++fmt; if (*fmt == 'l') { isLongLong = true; ++fmt; } else isLong = true; }
            else if (*fmt == 'L') { isLong = true; ++fmt; }
            else if (fmt[0] == 'I' && fmt[1] == '6' && fmt[2] == '4') { isLongLong = true; fmt += 3; }
            else break;
        }
        const char type = *fmt++;
        if (type == 'n')
        {
            if (!suppress)
                st32(next(), reg32(in - input));
            continue;
        }
        if (type != 'c' && type != '[')
            while (isspace(static_cast<unsigned char>(*in)))
                ++in;
        if (!*in)
            return converted ? assigned : -1;
        // The field: at most width characters of the input.
        std::string field(in, width > 0 ? std::min<size_t>(size_t(width), strlen(in)) : strlen(in));
        const char* begin = field.c_str();
        char* end = const_cast<char*>(begin);
        switch (type)
        {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
        {
            const int base = type == 'd' || type == 'u' ? 10 : type == 'i' ? 0 : type == 'o' ? 8 : 16;
            const long long v = type == 'u' || type == 'x' || type == 'X' || type == 'o'
                ? (long long)strtoull(begin, &end, base) : strtoll(begin, &end, base);
            if (end == begin)
                return assigned;
            if (!suppress)
            {
                const reg32 at = next();
                if (isLongLong)
                {
                    st32(at, reg32(v));
                    st32(at + 4, reg32(uint64_t(v) >> 32));
                }
                else if (isShort)
                    memcpy(g_guestBase + at, &v, 2);
                else
                    st32(at, reg32(v));
                ++assigned;
            }
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G':
        {
            const double v = strtod(begin, &end);
            if (end == begin)
                return assigned;
            if (!suppress)
            {
                const reg32 at = next();
                if (isLong)
                    memcpy(g_guestBase + at, &v, 8);
                else
                {
                    const float f = float(v);
                    memcpy(g_guestBase + at, &f, 4);
                }
                ++assigned;
            }
            break;
        }
        case 's':
        {
            while (*end && !isspace(static_cast<unsigned char>(*end)))
                ++end;
            if (!suppress)
            {
                char* to = ptr(next());
                memcpy(to, begin, size_t(end - begin));
                to[end - begin] = 0;
                ++assigned;
            }
            break;
        }
        case 'c':
        {
            const size_t n = std::min<size_t>(size_t(width > 0 ? width : 1), field.size());
            end += n;
            if (!suppress)
            {
                memcpy(ptr(next()), begin, n);
                ++assigned;
            }
            break;
        }
        case '[':
        {
            const bool negate = *fmt == '^';
            if (negate)
                ++fmt;
            std::string set;
            if (*fmt == ']')
                set += *fmt++;
            while (*fmt && *fmt != ']')
                set += *fmt++;
            if (*fmt == ']')
                ++fmt;
            auto member = [&](char c) {
                for (size_t i = 0; i < set.size(); ++i)
                {
                    if (i + 2 < set.size() && set[i + 1] == '-' ? c >= set[i] && c <= set[i + 2] : c == set[i])
                        return true;
                    if (i + 2 < set.size() && set[i + 1] == '-')
                        i += 2;
                }
                return false;
            };
            while (*end && member(*end) != negate)
                ++end;
            if (end == begin)
                return assigned;
            if (!suppress)
            {
                char* to = ptr(next());
                memcpy(to, begin, size_t(end - begin));
                to[end - begin] = 0;
                ++assigned;
            }
            break;
        }
        default:
            pf::logError("msvcr71: sscanf conversion %%%c not supported", type);
            return assigned;
        }
        converted = true;
        in += end - begin;
    }
    return assigned;
}

}}

namespace win32 { namespace msvcr71
{

using namespace crt;

namespace
{

const reg32 InvalidHandle = 0xffffffff;

// A FILE of the guest: its kernel32 handle, or the log for stdout and stderr.
struct Stream
{
    HANDLE handle = InvalidHandle;
    bool log = false;
    bool eof = false;
    bool error = false;
    int pushed = -1;            // ungetc
    std::string line;           // log streams: text up to the next newline
};

pf::LwMutex s_streamMutex("msvcr71 stdio");
std::map<reg32, Stream> s_streams;          // guest FILE address -> stream
std::map<int, HANDLE> s_descriptors;        // low-level descriptor -> handle
int s_nextDescriptor = 3;

reg32 iob()
{
    return staticBlock("_iob", 20 * 32);
}

// The stream of a guest FILE*; stdin/stdout/stderr (&_iob[0..2]) are created on first use.
Stream* stream(reg32 file)
{
    const auto it = s_streams.find(file);
    if (it != s_streams.end())
        return &it->second;
    const reg32 base = iob();
    if (file >= base && file < base + 3 * 32 && (file - base) % 32 == 0)
    {
        Stream& s = s_streams[file];
        s.log = file != base;
        return &s;
    }
    return nullptr;
}

void writeLog(Stream& s, const char* data, size_t n)
{
    s.line.append(data, n);
    size_t at;
    while ((at = s.line.find('\n')) != std::string::npos)
    {
        pf::logInfo("guest: %s", s.line.substr(0, at).c_str());
        s.line.erase(0, at + 1);
    }
}

size_t writeStream(WinApplication* app, x86::CPU& cpu, Stream& s, const void* data, size_t n)
{
    if (s.log)
    {
        writeLog(s, static_cast<const char*>(data), n);
        return n;
    }
    DWORD written = 0;
    if (s.handle == InvalidHandle || !kernel32::WriteFile(app, cpu, s.handle, data, DWORD(n), &written, nullptr))
        s.error = true;
    return written;
}

size_t readStream(WinApplication* app, x86::CPU& cpu, Stream& s, char* data, size_t n)
{
    size_t done = 0;
    if (n && s.pushed >= 0)
    {
        data[done++] = char(s.pushed);
        s.pushed = -1;
    }
    if (done < n && !s.log && s.handle != InvalidHandle)
    {
        DWORD read = 0;
        if (!kernel32::ReadFile(app, cpu, s.handle, data + done, DWORD(n - done), &read, nullptr))
            s.error = true;
        done += read;
    }
    if (done < n)
        s.eof = true;
    return done;
}

// kernel32 access and creation of a C mode ("r", "wb", "a+", ...) or _open flags.
struct OpenMode
{
    DWORD access;
    DWORD creation;
    bool append;
};

OpenMode modeOf(const char* mode)
{
    const bool plus = strchr(mode, '+') != nullptr;
    switch (mode[0])
    {
    case 'w': return { GENERIC_WRITE | (plus ? GENERIC_READ : 0), CREATE_ALWAYS, false };
    case 'a': return { GENERIC_WRITE | (plus ? GENERIC_READ : 0), OPEN_ALWAYS, true };
    default:  return { GENERIC_READ | (plus ? GENERIC_WRITE : 0), OPEN_EXISTING, false };
    }
}

OpenMode modeOf(reg32 flags)
{
    const reg32 access = flags & 3;
    OpenMode m{ access == 1 ? GENERIC_WRITE : access == 2 ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                OPEN_EXISTING, (flags & 0x8) != 0 };
    if ((flags & 0x100) && (flags & 0x400))
        m.creation = CREATE_NEW;
    else if ((flags & 0x100) && (flags & 0x200))
        m.creation = CREATE_ALWAYS;
    else if (flags & 0x100)
        m.creation = OPEN_ALWAYS;
    else if (flags & 0x200)
        m.creation = TRUNCATE_EXISTING;
    return m;
}

HANDLE openFile(WinApplication* app, x86::CPU& cpu, const char* path, const OpenMode& mode)
{
    const HANDLE h = kernel32::CreateFileA(app, cpu, path, mode.access, FILE_SHARE_READ, nullptr, mode.creation,
                                           FILE_ATTRIBUTE_NORMAL, 0);
    if (h != InvalidHandle && mode.append)
        kernel32::SetFilePointer(app, cpu, h, 0, nullptr, 2);
    if (h == InvalidHandle)
        setErrno(2);
    return h;
}

HANDLE descriptor(int fd)
{
    pf::LwLockGuard guard(s_streamMutex);
    const auto it = s_descriptors.find(fd);
    return it == s_descriptors.end() ? InvalidHandle : it->second;
}

// FILETIME (100 ns since 1601) to a 32-bit time_t.
reg32 unixTime(const FILETIME& ft)
{
    const int64_t ticks = int64_t(uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    return ticks > 0 ? reg32(ticks / 10000000) : 0;
}

// struct _finddata_t (msvcr71, 32-bit time_t): attrib, time_create, time_access, time_write, size, name[260].
void storeFindData(reg32 at, const WIN32_FIND_DATAA& d)
{
    st32(at, d.dwFileAttributes);
    st32(at + 4, unixTime(d.ftCreationTime));
    st32(at + 8, unixTime(d.ftLastAccessTime));
    st32(at + 12, unixTime(d.ftLastWriteTime));
    st32(at + 16, d.nFileSizeLow);
    memcpy(ptr(at + 20), d.cFileName, MAX_PATH);
}

reg32 printTo(WinApplication* app, x86::CPU& cpu, reg32 file, const std::string& text)
{
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(file);
    if (!s)
        return reg32(-1);
    return reg32(writeStream(app, cpu, *s, text.data(), text.size()));
}

// Formatted output into a guest buffer of count characters (no limit: 0xffffffff), with the terminator if it fits;
// the _snprintf rule: -1 when the text does not fit.
reg32 printToBuffer(reg32 buffer, reg32 count, const std::string& text)
{
    if (text.size() > count || (text.size() == count && count == 0xffffffff))
        return reg32(-1);
    memcpy(ptr(buffer), text.data(), text.size());
    if (text.size() < count)
        ptr(buffer)[text.size()] = 0;
    return text.size() == count ? reg32(-1) : reg32(text.size());
}

}

// ---- formatted output ------------------------------------------------------------------------------------------------

x86::reg32 crt_sprintf(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const std::string text = format(ptr(arg(cpu, 1)), cpu.esp + 12);
    memcpy(ptr(arg(cpu, 0)), text.c_str(), text.size() + 1);
    traceText("sprintf", arg(cpu, 0), cpu);
    return reg32(text.size());
}

x86::reg32 crt_vsprintf(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const std::string text = format(ptr(arg(cpu, 1)), arg(cpu, 2));
    memcpy(ptr(arg(cpu, 0)), text.c_str(), text.size() + 1);
    traceText("vsprintf", arg(cpu, 0), cpu);
    return reg32(text.size());
}

x86::reg32 crt__snprintf(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 n = printToBuffer(arg(cpu, 0), arg(cpu, 1), format(ptr(arg(cpu, 2)), cpu.esp + 16));
    traceText("_snprintf", arg(cpu, 0), cpu);
    return n;
}

x86::reg32 crt__vsnprintf(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 n = printToBuffer(arg(cpu, 0), arg(cpu, 1), format(ptr(arg(cpu, 2)), arg(cpu, 3)));
    traceText("_vsnprintf", arg(cpu, 0), cpu);
    return n;
}

// int vswprintf(wchar_t*, size_t count, const wchar_t*, va_list) (the C++ overload with a count)
x86::reg32 crt_vswprintf_count(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const std::u16string text = formatWide(ptr<const char16_t>(arg(cpu, 2)), arg(cpu, 3));
    const reg32 count = arg(cpu, 1);
    if (!count)
        return reg32(-1);
    const size_t n = std::min<size_t>(text.size(), count - 1);
    memcpy(ptr(arg(cpu, 0)), text.data(), 2 * n);
    memset(ptr(arg(cpu, 0)) + 2 * n, 0, 2);
    traceWide("vswprintf", arg(cpu, 0), cpu);
    return n < text.size() ? reg32(-1) : reg32(n);
}

x86::reg32 crt_printf(WinApplication* app, x86::CPU& cpu)
{
    return printTo(app, cpu, iob() + 32, format(ptr(arg(cpu, 0)), cpu.esp + 8));
}

x86::reg32 crt_fprintf(WinApplication* app, x86::CPU& cpu)
{
    return printTo(app, cpu, arg(cpu, 0), format(ptr(arg(cpu, 1)), cpu.esp + 12));
}

x86::reg32 crt_vfprintf(WinApplication* app, x86::CPU& cpu)
{
    return printTo(app, cpu, arg(cpu, 0), format(ptr(arg(cpu, 1)), arg(cpu, 2)));
}

x86::reg32 crt_sscanf(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reg32(scan(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), cpu.esp + 12));
}

// ---- FILE streams ----------------------------------------------------------------------------------------------------

x86::reg32 crt_fopen(WinApplication* app, x86::CPU& cpu)
{
    const char* path = ptr(arg(cpu, 0));
    const char* mode = ptr(arg(cpu, 1));
    if (!path || !mode)
        return 0;
    const HANDLE h = openFile(app, cpu, path, modeOf(mode));
    if (h == InvalidHandle)
        return 0;
    const reg32 file = allocate(32, true);
    pf::LwLockGuard guard(s_streamMutex);
    s_streams[file].handle = h;
    return file;
}

x86::reg32 crt_fclose(WinApplication* app, x86::CPU& cpu)
{
    const reg32 file = arg(cpu, 0);
    HANDLE h = InvalidHandle;
    {
        pf::LwLockGuard guard(s_streamMutex);
        Stream* s = stream(file);
        if (!s)
            return reg32(-1);
        if (s->log)
            return 0;
        h = s->handle;
        s_streams.erase(file);
    }
    if (h != InvalidHandle)
        kernel32::CloseHandle(app, cpu, h);
    if (file < iob() || file >= iob() + 20 * 32)
        release(file);
    return 0;
}

x86::reg32 crt_fflush(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

// size_t fread(void*, size_t size, size_t count, FILE*)
x86::reg32 crt_fread(WinApplication* app, x86::CPU& cpu)
{
    const reg32 size = arg(cpu, 1), count = arg(cpu, 2);
    if (!size || !count)
        return 0;
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(arg(cpu, 3));
    if (!s)
        return 0;
    return reg32(readStream(app, cpu, *s, ptr(arg(cpu, 0)), size_t(size) * count) / size);
}

x86::reg32 crt_fwrite(WinApplication* app, x86::CPU& cpu)
{
    const reg32 size = arg(cpu, 1), count = arg(cpu, 2);
    if (!size || !count)
        return 0;
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(arg(cpu, 3));
    if (!s)
        return 0;
    return reg32(writeStream(app, cpu, *s, ptr(arg(cpu, 0)), size_t(size) * count) / size);
}

x86::reg32 crt_fgetc(WinApplication* app, x86::CPU& cpu)
{
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(arg(cpu, 0));
    char c;
    if (!s || readStream(app, cpu, *s, &c, 1) != 1)
        return reg32(-1);
    return static_cast<unsigned char>(c);
}

x86::reg32 crt_ungetc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int32_t(arg(cpu, 0));
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(arg(cpu, 1));
    if (!s || c < 0)
        return reg32(-1);
    s->pushed = c & 0xff;
    s->eof = false;
    return reg32(c & 0xff);
}

x86::reg32 crt_fputc(WinApplication* app, x86::CPU& cpu)
{
    const char c = char(arg(cpu, 0));
    pf::LwLockGuard guard(s_streamMutex);
    Stream* s = stream(arg(cpu, 1));
    if (!s || writeStream(app, cpu, *s, &c, 1) != 1)
        return reg32(-1);
    return static_cast<unsigned char>(c);
}

// ---- low-level descriptors ---------------------------------------------------------------------------------------------

// int _open(const char*, int flags, ...)
x86::reg32 crt__open(WinApplication* app, x86::CPU& cpu)
{
    const char* path = ptr(arg(cpu, 0));
    if (!path)
        return reg32(-1);
    const HANDLE h = openFile(app, cpu, path, modeOf(arg(cpu, 1)));
    if (h == InvalidHandle)
        return reg32(-1);
    pf::LwLockGuard guard(s_streamMutex);
    const int fd = s_nextDescriptor++;
    s_descriptors[fd] = h;
    return reg32(fd);
}

x86::reg32 crt__close(WinApplication* app, x86::CPU& cpu)
{
    const int fd = int32_t(arg(cpu, 0));
    HANDLE h = InvalidHandle;
    {
        pf::LwLockGuard guard(s_streamMutex);
        const auto it = s_descriptors.find(fd);
        if (it == s_descriptors.end())
            return reg32(-1);
        h = it->second;
        s_descriptors.erase(it);
    }
    kernel32::CloseHandle(app, cpu, h);
    return 0;
}

x86::reg32 crt__read(WinApplication* app, x86::CPU& cpu)
{
    const HANDLE h = descriptor(int32_t(arg(cpu, 0)));
    DWORD read = 0;
    if (h == InvalidHandle || !kernel32::ReadFile(app, cpu, h, ptr(arg(cpu, 1)), arg(cpu, 2), &read, nullptr))
        return reg32(-1);
    return read;
}

x86::reg32 crt__write(WinApplication* app, x86::CPU& cpu)
{
    const int fd = int32_t(arg(cpu, 0));
    if (fd == 1 || fd == 2)
        return printTo(app, cpu, iob() + 32 * reg32(fd), std::string(ptr(arg(cpu, 1)), arg(cpu, 2)));
    const HANDLE h = descriptor(fd);
    DWORD written = 0;
    if (h == InvalidHandle || !kernel32::WriteFile(app, cpu, h, ptr(arg(cpu, 1)), arg(cpu, 2), &written, nullptr))
        return reg32(-1);
    return written;
}

// long _lseek(int, long offset, int origin)
x86::reg32 crt__lseek(WinApplication* app, x86::CPU& cpu)
{
    const HANDLE h = descriptor(int32_t(arg(cpu, 0)));
    if (h == InvalidHandle)
        return reg32(-1);
    return kernel32::SetFilePointer(app, cpu, h, LONG(arg(cpu, 1)), nullptr, arg(cpu, 2));
}

x86::reg32 crt__filelength(WinApplication* app, x86::CPU& cpu)
{
    const HANDLE h = descriptor(int32_t(arg(cpu, 0)));
    if (h == InvalidHandle)
        return reg32(-1);
    return kernel32::GetFileSize(app, cpu, h, nullptr);
}

// ---- directory search ----------------------------------------------------------------------------------------------

// intptr_t _findfirst(const char* spec, struct _finddata_t*)
x86::reg32 crt__findfirst(WinApplication* app, x86::CPU& cpu)
{
    WIN32_FIND_DATAA data;
    memset(&data, 0, sizeof(data));
    const HANDLE h = kernel32::FindFirstFileA(app, cpu, ptr(arg(cpu, 0)), &data);
    if (h == InvalidHandle)
    {
        setErrno(2);
        return reg32(-1);
    }
    storeFindData(arg(cpu, 1), data);
    return h;
}

x86::reg32 crt__findnext(WinApplication* app, x86::CPU& cpu)
{
    WIN32_FIND_DATAA data;
    memset(&data, 0, sizeof(data));
    if (!kernel32::FindNextFileA(app, cpu, arg(cpu, 0), &data))
    {
        setErrno(2);
        return reg32(-1);
    }
    storeFindData(arg(cpu, 1), data);
    return 0;
}

x86::reg32 crt__findclose(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::FindClose(app, cpu, arg(cpu, 0)) ? 0 : reg32(-1);
}

// ---- directories and paths -------------------------------------------------------------------------------------------

x86::reg32 crt__mkdir(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::CreateDirectoryA(app, cpu, ptr(arg(cpu, 0)), nullptr) ? 0 : reg32(-1);
}

x86::reg32 crt__rmdir(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::RemoveDirectoryA(app, cpu, ptr(arg(cpu, 0))) ? 0 : reg32(-1);
}

x86::reg32 crt_remove(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::DeleteFileA(app, cpu, ptr(arg(cpu, 0))) ? 0 : reg32(-1);
}

// char* _getcwd(char* buffer, int size): a new block when buffer is null.
x86::reg32 crt__getcwd(WinApplication* app, x86::CPU& cpu)
{
    char path[MAX_PATH];
    const DWORD n = kernel32::GetCurrentDirectoryA(app, cpu, MAX_PATH, path);
    if (!n || n >= MAX_PATH)
        return 0;
    reg32 buffer = arg(cpu, 0);
    const reg32 size = arg(cpu, 1);
    if (!buffer)
        buffer = allocate(std::max<reg32>(size, n + 1));
    else if (n + 1 > size)
    {
        setErrno(34);
        return 0;
    }
    memcpy(ptr(buffer), path, n + 1);
    return buffer;
}

// char* _fullpath(char* absolute, const char* relative, size_t max)
x86::reg32 crt__fullpath(WinApplication* app, x86::CPU& cpu)
{
    char path[MAX_PATH];
    const DWORD n = kernel32::GetFullPathNameA(app, cpu, ptr(arg(cpu, 1)), MAX_PATH, path, nullptr);
    if (!n || n >= MAX_PATH)
        return 0;
    reg32 buffer = arg(cpu, 0);
    if (!buffer)
        buffer = allocate(n + 1);
    else if (n + 1 > arg(cpu, 2))
        return 0;
    memcpy(ptr(buffer), path, n + 1);
    return buffer;
}

// The data folder is drive C:.
x86::reg32 crt__getdrive(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 3;
}

// unsigned _getdiskfree(unsigned drive, struct _diskfree_t*): total_clusters, avail_clusters, sectors_per_cluster,
// bytes_per_sector.
x86::reg32 crt__getdiskfree(WinApplication* app, x86::CPU& cpu)
{
    DWORD sectors = 0, bytes = 0, freeClusters = 0, total = 0;
    if (!kernel32::GetDiskFreeSpaceA(app, cpu, "C:\\", &sectors, &bytes, &freeClusters, &total))
        return 15;
    const reg32 info = arg(cpu, 1);
    st32(info, total);
    st32(info + 4, freeClusters);
    st32(info + 8, sectors);
    st32(info + 12, bytes);
    return 0;
}

}}
