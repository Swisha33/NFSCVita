#include <platform/log.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pf
{

// s_logPath is read and cleared under s_logMutex, which stays until the process exits so that no logger can use a
// deleted mutex. The path is copied: callers may pass a temporary.
static char        s_logPathBuffer[256];
static const char* s_logPath = nullptr;
static SceUID      s_logMutex = -1;
static bool        s_console = false;

// The log stays open from logInit as a kernel descriptor (sceIo, outside newlib's descriptor table): an open per line
// failed silently once the game had used up the table (run 15: the log stopped while the game ran on). sceIoWrite is
// unbuffered, so nothing is lost if the process dies.
static SceUID s_logFd = -1;
static int    s_prevResult = 0;     // sceIoRename of the previous log
static bool   s_prevCopied = false;

static void writeOut(const char* text, unsigned length)
{
    if (s_logFd >= 0)
    {
        sceIoWrite(s_logFd, text, length);
        return;
    }
    // Fallback when the kernel descriptor could not be opened: one open per line.
    int file = open(s_logPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (file >= 0)
    {
        write(file, text, length);
        close(file);
    }
}

// Lines are written by a low-priority thread (log_writer) from a pending buffer: a write to ux0 waits behind the
// card's other transfers, and a 6 MB track section read takes ~400 ms there, so one line logged meanwhile stalled the
// logging thread that long (the game thread, or the EA audio thread holding the sound lock). Errors and fatals are
// still written and synced at once by the thread that logs them, after everything pending.
// s_ioMutex orders the writes: a drain takes the pending lines and writes them with it held. Lock order: s_ioMutex,
// then s_logMutex.
static const unsigned PendingBytes = 64 * 1024;
static char     s_pending[PendingBytes];
static char     s_writing[PendingBytes];
static unsigned s_pendingLength = 0;
static SceUID   s_ioMutex = -1;
static SceUID   s_writer = -1;

// s_logMutex not held by the caller.
static void drain()
{
    if (s_ioMutex >= 0)
        sceKernelLockMutex(s_ioMutex, 1, nullptr);
    sceKernelLockMutex(s_logMutex, 1, nullptr);
    const unsigned length = s_pendingLength;
    if (length)
        memcpy(s_writing, s_pending, length);
    s_pendingLength = 0;
    const bool open = s_logPath != nullptr;
    sceKernelUnlockMutex(s_logMutex, 1);
    if (length && open)
        writeOut(s_writing, length);
    if (s_ioMutex >= 0)
        sceKernelUnlockMutex(s_ioMutex, 1);
}

static int writerEntry(SceSize, void*)
{
    for (;;)
    {
        sceKernelDelayThread(100 * 1000);
        if (s_pendingLength)
            drain();
    }
    return 0;
}

// s_logMutex held (released and taken again when the buffer is full).
static void append(const char* text, unsigned length)
{
    if (s_writer < 0)
    {
        writeOut(text, length);
        return;
    }
    if (s_pendingLength + length > PendingBytes)
    {
        sceKernelUnlockMutex(s_logMutex, 1);
        drain();
        sceKernelLockMutex(s_logMutex, 1, nullptr);
        if (length > PendingBytes)
        {
            // A block larger than the buffer: written directly, in order after what was pending.
            sceKernelUnlockMutex(s_logMutex, 1);
            drain();
            sceKernelLockMutex(s_ioMutex, 1, nullptr);
            if (s_logPath)
                writeOut(text, length);
            sceKernelUnlockMutex(s_ioMutex, 1);
            sceKernelLockMutex(s_logMutex, 1, nullptr);
            return;
        }
        if (s_pendingLength + length > PendingBytes)
        {
            // Other threads filled it again meanwhile: wait for the writer instead of reordering.
            while (s_pendingLength + length > PendingBytes)
            {
                sceKernelUnlockMutex(s_logMutex, 1);
                drain();
                sceKernelLockMutex(s_logMutex, 1, nullptr);
            }
        }
    }
    memcpy(s_pending + s_pendingLength, text, length);
    s_pendingLength += length;
}

static void writeLine(const char* level, const char* fmt, va_list args)
{
    char buffer[1024];
    int prefix = snprintf(buffer, sizeof(buffer), "[%s] ", level);
    int len = vsnprintf(buffer + prefix, sizeof(buffer) - prefix - 1, fmt, args);
    if (len < 0)
        return;
    len += prefix;
    if (len > int(sizeof(buffer)) - 2)
        len = int(sizeof(buffer)) - 2;
    buffer[len++] = '\n';
    buffer[len] = 0;

    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    if (s_console)
        sceClibPrintf("%s", buffer);
    if (s_logPath)
        append(buffer, unsigned(len));
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
    // Errors often come right before a stop: write and sync them at once.
    if (s_logFd >= 0 && (level[0] == 'e' || level[0] == 'f'))
        logSync();
}

void logInit(const char* logFilePath)
{
    s_logMutex = sceKernelCreateMutex("log",0, 0, nullptr);
    if (logFilePath)
    {
        snprintf(s_logPathBuffer, sizeof(s_logPathBuffer), "%s", logFilePath);
        s_logPath = s_logPathBuffer;
    }
    if (s_logPath)
    {
        // The previous run's log stays as <name>.prev.log: a crash log survives the restart after it.
        char previous[sizeof(s_logPathBuffer) + 8];
        const char* dot = strrchr(s_logPath, '.');
        const int stem = dot ? int(dot - s_logPath) : int(strlen(s_logPath));
        snprintf(previous, sizeof(previous), "%.*s.prev%s", stem, s_logPath, dot ? dot : "");
        sceIoRemove(previous);
        // sceIoRename did not keep it on the user's Vita (no .prev.log ever appeared, 2026-11-24): copy instead.
        s_prevResult = sceIoRename(s_logPath, previous);
        if (s_prevResult < 0)
        {
            const SceUID from = sceIoOpen(s_logPath, SCE_O_RDONLY, 0);
            if (from >= 0)
            {
                const SceUID to = sceIoOpen(previous, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
                if (to >= 0)
                {
                    static char chunk[16384];
                    int n;
                    while ((n = sceIoRead(from, chunk, sizeof(chunk))) > 0)
                        sceIoWrite(to, chunk, n);
                    sceIoClose(to);
                    s_prevCopied = true;
                }
                sceIoClose(from);
            }
        }
        // Only this descriptor writes the file, so its position is always the end: no SCE_O_APPEND.
        s_logFd = sceIoOpen(s_logPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (s_logFd < 0)
        {
            int file = open(s_logPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (file >= 0)
                close(file);
            char line[96];
            const int length = snprintf(line, sizeof(line), "[error] log: sceIoOpen failed (0x%08x), one open per line\n",
                                        unsigned(s_logFd));
            append(line, unsigned(length));
        }
        if (s_prevResult < 0)
        {
            char line[128];
            const int length = snprintf(line, sizeof(line), "[info] log: previous log %s (rename 0x%08x)\n",
                                        s_prevCopied ? "copied to .prev.log" : "not kept", unsigned(s_prevResult));
            append(line, unsigned(length));
        }
        if (s_logFd >= 0)
        {
            s_ioMutex = sceKernelCreateMutex("log_io", 0, 0, nullptr);
            // Below the game's threads; any core.
            const SceUID writer = sceKernelCreateThread("log_writer", &writerEntry, 180, 16 * 1024, 0, 0,
                                                        nullptr);
            if (s_ioMutex >= 0 && writer >= 0 && sceKernelStartThread(writer, 0, nullptr) >= 0)
                s_writer = writer;
        }
    }
}

// The file size others see (FTP) follows the writes only after a sync while the descriptor stays open.
void logSync()
{
    if (s_logMutex < 0)
        return;
    drain();
    if (s_ioMutex >= 0)
        sceKernelLockMutex(s_ioMutex, 1, nullptr);
    if (s_logFd >= 0)
        sceIoSyncByFd(s_logFd, 0);
    if (s_ioMutex >= 0)
        sceKernelUnlockMutex(s_ioMutex, 1);
}

void logSetConsole(bool enabled)
{
    s_console = enabled;
}

void logShutdown()
{
    if (s_logMutex >= 0)
        drain();
    if (s_ioMutex >= 0)
        sceKernelLockMutex(s_ioMutex, 1, nullptr);
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    s_logPath = nullptr;
    if (s_logFd >= 0)
        sceIoClose(s_logFd);
    s_logFd = -1;
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
    if (s_ioMutex >= 0)
        sceKernelUnlockMutex(s_ioMutex, 1);
}

void logDebug(const char* fmt, ...)
{
#ifdef NFSU_LOG_DEBUG
    va_list args;
    va_start(args, fmt);
    writeLine("debug", fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}

void logInfo(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("info", fmt, args);
    va_end(args);
}

void logBlock(const char* text, unsigned length)
{
    if (!length)
        return;
    if (s_logMutex >= 0)
        sceKernelLockMutex(s_logMutex, 1, nullptr);
    if (s_console)
        sceClibPrintf("%.*s", int(length), text);
    if (s_logPath)
        append(text, length);
    if (s_logMutex >= 0)
        sceKernelUnlockMutex(s_logMutex, 1);
}

void logError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("error", fmt, args);
    va_end(args);
}

void fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    writeLine("fatal", fmt, args);
    va_end(args);
    logShutdown();
    sceKernelExitProcess(1);
    for (;;) {}
}

}
