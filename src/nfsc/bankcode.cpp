// Machine code inside the game's EA audio banks (SOUND/**/*.abk, magic "ABKC"). Each bank entry carries an x86 routine
// that the audio library calls through a pointer once the bank is loaded (run 29: a call to 0x01afd564 = bank
// 0x01afd4c0 + 0xa4 when the title screen started its sounds). That code is not part of NFSC.exe, so it is
// interpreted here: the dynamic_call resolver finds the bank by its header below the address and runs the routine
// instruction by instruction; its calls (rel32, patched by the loader to NFSC.exe's sound library) go back through
// dynamic_call.
//
// Derived from UG1VITA's src/nfsu/bankcode.cpp (MIT). Differences: the bank is found by scanning down for its "ABKC"
// header (Carbon's loaded-bank list is not mapped), entry offsets may be bank-relative or absolute, no build-time
// translations, no shadow compare, and the instruction set adds sub r32, r/m32 and the imm32 group 1 forms. A survey
// of all 285 banks of the v1.4 game (389 routines, 207 distinct) found only these instructions before each routine's
// ret: push / pop r32, mov r/m32 <-> r32, mov m8 imm8, add / sub / cmp r32, r/m32, add / cmp r/m32 imm8,
// imul r32, r/m32, jl / jg rel8, call rel32, ret. Anything else traps with its address and bytes.

#include <banknative.h>
#include <lib/foreigncode.h>
#include <lib/memmap.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <recomp.h>
#include <x86.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfsc { namespace bankcode {

void unsupported(x86::reg32 ip, const char* what)
{
    pf::logError("bank code: %s at 0x%08x (%02x %02x %02x %02x)", what, unsigned(ip), rc::ld8(ip), rc::ld8(ip + 1),
                 rc::ld8(ip + 2), rc::ld8(ip + 3));
    x86::guestTrap(ip, "bank code");
}

namespace {

const x86::reg32 BankMagic   = 0x434B4241;  // "ABKC"
const x86::reg32 MaxBankScan = 0x400000;    // bytes below the address searched for the header
const uint32_t   MaxEntries  = 256;

// Bytes [begin, end) of one entry's routine in the bank at bank.
struct Range
{
    x86::reg32 begin;
    x86::reg32 end;
    x86::reg32 bank;
};

x86::reg32 absolute(x86::reg32 value, x86::reg32 bank)
{
    return value < bank ? bank + value : value;
}

// Header: entry count (u16 at +0x0A), first entry (offset at +0x1C). Entry: routine at +0x28, its end at +0x2C; the
// next entry follows after 0x3C + 4 * (byte +0x24 + byte +0x27) bytes (as in Underground's loader).
bool findRoutine(x86::reg32 address, Range& range)
{
    if (address < 0x1000 || !win32::MemMap::isGuestRange(address, 4))
        return false;
    for (x86::reg32 at = address & ~3u, scanned = 0; at >= 0x1000 && scanned < MaxBankScan; at -= 4, scanned += 4)
    {
        if (rc::ld32(at) != BankMagic)
            continue;
        const x86::reg32 bank = at;
        const x86::reg32 count = rc::ld16(bank + 0x0A);
        // Entry ranges overlap in Carbon's banks (roadnoise_00_mb.abk: entry 0 0xe4-0x6d0, entry 1 0x1f0-0x86c), so
        // an entry that starts at the address wins over one that merely contains it (run 70: calls to entry 1 ran
        // as entry 0 entered in the middle, never native).
        x86::reg32 entry = bank + rc::ld32(bank + 0x1C);
        bool found = false;
        for (x86::reg32 i = 0; i < count && i < MaxEntries; ++i)
        {
            const x86::reg32 begin = absolute(rc::ld32(entry + 0x28), bank);
            const x86::reg32 end = absolute(rc::ld32(entry + 0x2C), bank);
            if (address == begin)
            {
                range = Range{ begin, end, bank };
                return true;
            }
            if (!found && address > begin && address < end)
            {
                range = Range{ begin, end, bank };
                found = true;
            }
            entry += 0x3C + 4 * (x86::reg32(rc::ld8(entry + 0x24)) + rc::ld8(entry + 0x27));
        }
        return found;   // the nearest header owns the address or nothing does
    }
    return false;
}

const uint8_t NoRegister = 0xff;
const unsigned MaxLength = 12;

enum Op : uint8_t
{
    OpPush,         // push r32
    OpPop,          // pop r32
    OpMovStore,     // mov r/m32, r32
    OpMovLoad,      // mov r32, r/m32
    OpAdd,          // add r32, r/m32
    OpSub,          // sub r32, r/m32
    OpCmp,          // cmp r32, r/m32
    OpAddImm,       // add r/m32, imm
    OpSubImm,       // sub r/m32, imm
    OpCmpImm,       // cmp r/m32, imm
    OpStoreByte,    // mov m8, imm8
    OpImul,         // imul r32, r/m32
    OpJl,           // jl rel8
    OpJg,           // jg rel8
    OpCall,         // call rel32
    OpRet,          // ret
};

struct Instruction
{
    uint32_t   word[MaxLength / 4];     // the instruction's bytes, zero after length
    uint32_t   mask[MaxLength / 4];     // 0xff for each byte of the instruction
    x86::reg32 start;
    uint32_t   next;        // index + 1 of the instruction at start + length, 0 while not linked
    uint8_t    length;
    uint8_t    op;
    uint8_t    reg;         // ModRM reg field, or the register of push / pop
    bool       memory;      // the ModRM operand is in memory
    uint8_t    rm;          // the operand register when !memory
    uint8_t    base;
    uint8_t    index;
    uint8_t    scale;
    x86::reg32 displacement;
    x86::reg32 immediate;   // sign-extended immediate, or the jump or call target
};

// Decoded instructions of one routine: a slot per routine byte holds an index into instructions plus 1.
struct Routine
{
    std::vector<uint32_t>    slots;
    std::vector<Instruction> instructions;
    std::atomic<int>         running{0};
    uint32_t                 runs = 0;          // since the last report
    uint32_t                 executed = 0;      // instructions, since the last report
    x86::reg32               begin = 0, end = 0, bank = 0;
    // The build-time translation (banknative.h) whose bytes, relocated for begin and bank (expected), the routine
    // had at its last run; seen: the routine bytes of the last run that found none.
    const Translation*       native = nullptr;
    std::vector<uint8_t>     expected;
    std::vector<uint8_t>     seen;
    uint32_t                 nativeRuns = 0;    // since the last report
    uint32_t                 verifiedUs = 0;    // last full compare of the bytes against expected (perf::now)
};

// The audio thread runs bank code without the execution lock (bank_unlocked, default on), so the cache, its
// counters and the translation lookup are guarded by s_cacheLock. It is never held while guest code runs (routines
// call back into the game). A routine that is running is never dropped.
std::mutex s_cacheLock;
std::unordered_map<uint64_t, std::unique_ptr<Routine>> s_routines;
const size_t MaxRoutines = 512;
const size_t MaxRoutineBytes = 0x10000;

Routine* routine(const Range& range)
{
    const x86::reg32 size = range.end - range.begin;
    if (size > MaxRoutineBytes)
        return nullptr;
    const uint64_t key = (uint64_t(range.begin) << 32) | range.end;
    auto found = s_routines.find(key);
    if (found != s_routines.end())
        return found->second.get();
    if (s_routines.size() >= MaxRoutines)
    {
        for (auto it = s_routines.begin(); it != s_routines.end();)
            it = it->second->running ? std::next(it) : s_routines.erase(it);
    }
    std::unique_ptr<Routine> created(new Routine);
    created->slots.assign(size, 0);
    created->begin = range.begin;
    created->end = range.end;
    created->bank = range.bank;
    Routine* result = created.get();
    s_routines.emplace(key, std::move(created));
    return result;
}

class Interpreter
{
public:
    Interpreter(x86::CPU& cpu, const Range& range, Routine& routine)
        :   m_cpu(cpu), m_range(range), m_routine(routine)
    {
        ++m_routine.running;
    }
    ~Interpreter()
    {
        --m_routine.running;
    }

    void begin(x86::reg32 ip)
    {
        load();
        m_current = at(ip);
    }

    // Runs to the next call (true: registers and flags in cpu, cpu.ip the target, the return address pushed) or to
    // the return (false).
    bool segment()
    {
        uint32_t current = m_current;
        for (;;)
        {
            ++m_executed;
            if (!matches(m_routine.instructions[current]))
                redecode(current);
            const Instruction& in = m_routine.instructions[current];
            switch (in.op)
            {
            case OpPush:
            {
                const x86::reg32 value = m_r[in.reg];
                m_r[4] -= 4;
                rc::st32(m_r[4], value);
                break;
            }
            case OpPop:
            {
                const x86::reg32 value = rc::ld32(m_r[4]);
                m_r[4] += 4;
                m_r[in.reg] = value;
                break;
            }
            case OpMovStore:
                write32(in, address(in), m_r[in.reg]);
                break;
            case OpMovLoad:
                m_r[in.reg] = read32(in, address(in));
                break;
            case OpAdd:
                m_r[in.reg] = add(m_r[in.reg], read32(in, address(in)));
                break;
            case OpSub:
                m_r[in.reg] = sub(m_r[in.reg], read32(in, address(in)));
                break;
            case OpCmp:
                sub(m_r[in.reg], read32(in, address(in)));
                break;
            case OpAddImm:
            {
                const x86::reg32 at = address(in);
                write32(in, at, add(read32(in, at), in.immediate));
                break;
            }
            case OpSubImm:
            {
                const x86::reg32 at = address(in);
                write32(in, at, sub(read32(in, at), in.immediate));
                break;
            }
            case OpCmpImm:
                sub(read32(in, address(in)), in.immediate);
                break;
            case OpStoreByte:
                rc::st8(address(in), x86::reg8(in.immediate));
                break;
            case OpImul:
            {
                const int64_t product = int64_t(x86::sreg32(m_r[in.reg])) * x86::sreg32(read32(in, address(in)));
                const x86::reg32 low = x86::reg32(product);
                materialize();
                m_cpu.flags.cf = m_cpu.flags.of = (product != int64_t(x86::sreg32(low)));
                m_r[in.reg] = low;
                break;
            }
            case OpJl:
            case OpJg:
            {
                materialize();
                const bool less = m_cpu.flags.sf != m_cpu.flags.of;
                if (in.op == OpJl ? less : (!less && !m_cpu.flags.zf))
                {
                    current = at(in.immediate);
                    continue;
                }
                break;
            }
            case OpCall:
                m_r[4] -= 4;
                rc::st32(m_r[4], in.start + in.length);
                store();
                m_cpu.ip = in.immediate;
                m_current = current;
                return true;
            case OpRet:
                m_r[4] += 4;
                store();
                m_current = current;
                return false;
            }
            current = following(current);
        }
    }

    // After the call returned (the callee popped the return address): registers from cpu, then the next instruction.
    void resume()
    {
        load();
        m_current = following(m_current);
    }

    uint32_t executed() const { return m_executed; }
    uint32_t decoded() const { return m_decoded; }

private:
    enum Pending : uint8_t { PendingNone, PendingAdd, PendingSub };

    void load()
    {
        m_r[0] = m_cpu.eax; m_r[1] = m_cpu.ecx; m_r[2] = m_cpu.edx; m_r[3] = m_cpu.ebx;
        m_r[4] = m_cpu.esp; m_r[5] = m_cpu.ebp; m_r[6] = m_cpu.esi; m_r[7] = m_cpu.edi;
    }

    void store()
    {
        m_cpu.eax = m_r[0]; m_cpu.ecx = m_r[1]; m_cpu.edx = m_r[2]; m_cpu.ebx = m_r[3];
        m_cpu.esp = m_r[4]; m_cpu.ebp = m_r[5]; m_cpu.esi = m_r[6]; m_cpu.edi = m_r[7];
        materialize();
    }

    void materialize()
    {
        if (m_pending == PendingNone)
            return;
        const x86::reg32 a = m_flagA, b = m_flagB, r = m_flagResult;
        if (m_pending == PendingAdd)
        {
            m_cpu.flags.cf = r < a;
            m_cpu.flags.of = ((a ^ r) & (b ^ r)) >> 31;
        }
        else
        {
            m_cpu.flags.cf = a < b;
            m_cpu.flags.of = ((a ^ b) & (a ^ r)) >> 31;
        }
        m_cpu.flags.zf = r == 0;
        m_cpu.flags.sf = r >> 31;
        m_pending = PendingNone;
    }

    uint32_t at(x86::reg32 address)
    {
        if (address < m_range.begin || address >= m_range.end)
        {
            store();
            unsupported(address, "jump out of the routine");
        }
        uint32_t& slot = m_routine.slots[address - m_range.begin];
        if (!slot)
        {
            Instruction in;
            decode(address, in);
            m_routine.instructions.push_back(in);
            slot = uint32_t(m_routine.instructions.size());
        }
        return slot - 1;
    }

    uint32_t following(uint32_t current)
    {
        const uint32_t next = m_routine.instructions[current].next;
        if (next)
            return next - 1;
        const Instruction& in = m_routine.instructions[current];
        const uint32_t index = at(in.start + in.length);
        m_routine.instructions[current].next = index + 1;
        return index;
    }

    // The loader can rewrite a routine (another bank loaded at the same address): cached decodes are checked, with
    // one masked compare of 12 bytes (run 66: a memcmp call per instruction was 18% of the audio thread). decode()
    // made sure the 12 bytes are guest memory.
    bool matches(const Instruction& in) const
    {
        uint32_t word[MaxLength / 4];
        memcpy(word, rc::host(in.start), sizeof(word));
        return (((word[0] ^ in.word[0]) & in.mask[0]) | ((word[1] ^ in.word[1]) & in.mask[1])
                | ((word[2] ^ in.word[2]) & in.mask[2])) == 0;
    }

    void redecode(uint32_t current)
    {
        Instruction in;
        decode(m_routine.instructions[current].start, in);
        in.next = 0;
        m_routine.instructions[current] = in;
    }

    void decode(x86::reg32 start, Instruction& in)
    {
        ++m_decoded;
        m_start = start;
        m_ip = start;
        memset(&in, 0, sizeof(in));
        in.start = start;
        in.base = NoRegister;
        in.index = NoRegister;
        const x86::reg8 op = fetch8();
        if (op >= 0x50 && op <= 0x5F)
        {
            in.op = op <= 0x57 ? OpPush : OpPop;
            in.reg = op & 7;
        }
        else
        {
            switch (op)
            {
            case 0x89: in.op = OpMovStore; modrm(in); break;
            case 0x8B: in.op = OpMovLoad; modrm(in); break;
            case 0x03: in.op = OpAdd; modrm(in); break;
            case 0x2B: in.op = OpSub; modrm(in); break;
            case 0x3B: in.op = OpCmp; modrm(in); break;
            case 0x81:
            case 0x83:
                modrm(in);
                in.immediate = op == 0x83 ? x86::reg32(x86::sreg32(x86::sreg8(fetch8()))) : fetch32();
                if (in.reg == 0)
                    in.op = OpAddImm;
                else if (in.reg == 5)
                    in.op = OpSubImm;
                else if (in.reg == 7)
                    in.op = OpCmpImm;
                else
                    unsupported(m_start, "group 1 operation");
                break;
            case 0xC6:
                modrm(in);
                if (in.reg != 0 || !in.memory)
                    unsupported(m_start, "C6 form");
                in.immediate = fetch8();
                in.op = OpStoreByte;
                break;
            case 0x0F:
                if (fetch8() != 0xAF)
                    unsupported(m_start, "two-byte opcode");
                modrm(in);
                in.op = OpImul;
                break;
            case 0x7C:
            case 0x7F:
            {
                const x86::reg32 rel = x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
                in.immediate = m_ip + rel;
                in.op = op == 0x7C ? OpJl : OpJg;
                break;
            }
            case 0xE8:
            {
                const x86::reg32 rel = fetch32();
                in.immediate = m_ip + rel;
                in.op = OpCall;
                break;
            }
            case 0xC3:
                in.op = OpRet;
                break;
            default:
                unsupported(m_start, "instruction");
            }
        }
        in.length = uint8_t(m_ip - m_start);
        if (in.length > MaxLength || !win32::MemMap::isGuestRange(m_start, MaxLength))
            unsupported(m_start, "instruction at the end of guest memory");
        uint8_t* bytes = reinterpret_cast<uint8_t*>(in.word);
        uint8_t* mask = reinterpret_cast<uint8_t*>(in.mask);
        memcpy(bytes, rc::host(m_start), in.length);
        memset(mask, 0xff, in.length);
    }

    x86::reg8 fetch8()
    {
        return rc::ld8(m_ip++);
    }

    x86::reg32 fetch32()
    {
        const x86::reg32 value = rc::ld32(m_ip);
        m_ip += 4;
        return value;
    }

    void modrm(Instruction& in)
    {
        const x86::reg8 byte = fetch8();
        const int mod = byte >> 6;
        const int rm = byte & 7;
        in.reg = (byte >> 3) & 7;
        if (mod == 3)
        {
            in.memory = false;
            in.rm = uint8_t(rm);
            return;
        }
        in.memory = true;
        if (rm == 4)
        {
            const x86::reg8 sib = fetch8();
            const int base = sib & 7;
            const int index = (sib >> 3) & 7;
            if (base == 5 && mod == 0)
                in.displacement = fetch32();
            else
                in.base = uint8_t(base);
            if (index != 4)
            {
                in.index = uint8_t(index);
                in.scale = uint8_t(sib >> 6);
            }
        }
        else if (rm == 5 && mod == 0)
        {
            in.displacement = fetch32();
        }
        else
        {
            in.base = uint8_t(rm);
        }
        if (mod == 1)
            in.displacement += x86::reg32(x86::sreg32(x86::sreg8(fetch8())));
        else if (mod == 2)
            in.displacement += fetch32();
    }

    x86::reg32 address(const Instruction& in) const
    {
        x86::reg32 address = in.base != NoRegister ? m_r[in.base] : 0;
        if (in.index != NoRegister)
            address += m_r[in.index] << in.scale;
        return address + in.displacement;
    }

    x86::reg32 read32(const Instruction& in, x86::reg32 address) const
    {
        return in.memory ? rc::ld32(address) : m_r[in.rm];
    }

    void write32(const Instruction& in, x86::reg32 address, x86::reg32 value)
    {
        if (in.memory)
            rc::st32(address, value);
        else
            m_r[in.rm] = value;
    }

    x86::reg32 add(x86::reg32 a, x86::reg32 b)
    {
        const x86::reg32 r = a + b;
        m_pending = PendingAdd;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = r;
        return r;
    }

    x86::reg32 sub(x86::reg32 a, x86::reg32 b)
    {
        const x86::reg32 r = a - b;
        m_pending = PendingSub;
        m_flagA = a;
        m_flagB = b;
        m_flagResult = r;
        return r;
    }

    x86::CPU&   m_cpu;
    Range       m_range;
    Routine&    m_routine;
    x86::reg32  m_r[8];         // eax ecx edx ebx esp ebp esi edi; in cpu at calls and on return
    Pending     m_pending = PendingNone;
    x86::reg32  m_flagA = 0, m_flagB = 0, m_flagResult = 0;
    uint32_t    m_current = 0;
    x86::reg32  m_ip = 0, m_start = 0;
    uint32_t    m_executed = 0, m_decoded = 0;
};

uint32_t s_runs = 0;
thread_local int s_depth = 0;   // routines running on this thread; only the outermost one is timed
const uint32_t ReportRuns = 50000;
uint32_t s_reportTime = 0;

// Every ReportRuns runs: the five routines run most often since the last report.
void report()
{
    const uint32_t now = pf::perf::now();
    Routine* top[5] = {};
    uint64_t total = 0;
    for (auto& entry : s_routines)
    {
        Routine* r = entry.second.get();
        total += r->executed;
        for (int i = 0; i < 5; ++i)
        {
            if (!top[i] || r->runs > top[i]->runs)
            {
                for (int j = 4; j > i; --j)
                    top[j] = top[j - 1];
                top[i] = r;
                break;
            }
        }
    }
    pf::logInfo("bank code: %u runs in %u ms, %llu instructions, %u routines cached", unsigned(ReportRuns),
                unsigned((now - s_reportTime) / 1000), (unsigned long long)total, unsigned(s_routines.size()));
    for (Routine* r : top)
    {
        if (!r || !r->runs)
            break;
        const uint32_t interpreted = r->runs - r->nativeRuns;
        pf::logInfo("bank code:   0x%08x +0x%x len %u (bank 0x%08x): %u runs (%u native%s%s), %u instructions "
                    "interpreted (%u per run)", unsigned(r->begin), unsigned(r->begin - r->bank),
                    unsigned(r->end - r->begin), unsigned(r->bank), unsigned(r->runs), unsigned(r->nativeRuns),
                    r->native ? ", " : "", r->native ? r->native->name : "", unsigned(r->executed),
                    unsigned(interpreted ? r->executed / interpreted : 0));
    }
    for (auto& entry : s_routines)
        entry.second->runs = entry.second->executed = entry.second->nativeRuns = 0;
    s_reportTime = now;
}

// The range resolve found for the address dynamic_call is about to run.
// Handed from resolve() to runRoutine() within one dynamic_call on the same thread.
thread_local x86::reg32 s_resolvedAddress = 0;
thread_local Range      s_resolvedRange = { 0, 0, 0 };

// ---- build-time translations (tools/recompiler/bankcode.py, nfsc_bank.cpp; as in NFSUG2_VITA) ----

// The translation's bytes as the loader leaves them for a routine at begin in the bank at bank.
void relocate(const Translation& t, x86::reg32 begin, x86::reg32 bank, std::vector<uint8_t>& out)
{
    out.assign(t.bytes, t.bytes + t.byteCount);
    for (uint32_t i = 0; i < t.relocationCount; ++i)
    {
        const Relocation& r = t.relocations[i];
        const x86::reg32 value = r.kind == CallTarget ? r.value - (begin + r.offset) - 4 : r.value + bank;
        memcpy(&out[r.offset], &value, sizeof(value));
    }
}

bool spansMatch(const Translation& t, x86::reg32 begin, const std::vector<uint8_t>& expected)
{
    for (uint32_t i = 0; i < t.spanCount; ++i)
    {
        const Span& s = t.spans[i];
        if (memcmp(rc::host(begin + s.offset), &expected[s.offset], s.length) != 0)
            return false;
    }
    return true;
}

std::unordered_set<uint32_t> s_untranslated;   // content hashes of routines logged as having no translation

// The translation of the routine's current bytes, or null. Carbon's bank loader is not hooked, so the bytes are
// compared on every run (the hot skid routine: 11 KB, ~385 runs/s; cheap next to interpreting it).
// Run 218: the full compare on every run was ~6 % of the audio thread (memcmp), and the audio thread holds EA's SND
// lock while it runs, which the game thread waits for. Between full compares (every FullVerifyUs per routine) only
// the first and last QuickBytes are compared: a bank is replaced by a load, not patched in place.
const uint32_t FullVerifyUs = 250000;
const uint32_t QuickBytes = 32;

// The first QuickBytes of the first span and the last QuickBytes of the last span (the instructions the
// translation runs; bytes outside the spans may be data the routine changes).
bool quickMatch(const Translation& t, const Routine& routine, const Range& range)
{
    if (!t.spanCount)
        return true;
    const Span& first = t.spans[0];
    const Span& last = t.spans[t.spanCount - 1];
    const uint32_t a = std::min(QuickBytes, first.length);
    const uint32_t b = std::min(QuickBytes, last.length);
    const uint32_t lastAt = last.offset + last.length - b;
    return memcmp(rc::host(range.begin + first.offset), &routine.expected[first.offset], a) == 0
        && memcmp(rc::host(range.begin + lastAt), &routine.expected[lastAt], b) == 0;
}

const Translation* translation(Routine& routine, const Range& range)
{
    if (routine.native)
    {
        const uint32_t now = pf::perf::now();
        if (now - routine.verifiedUs < FullVerifyUs && quickMatch(*routine.native, routine, range))
            return routine.native;
        if (spansMatch(*routine.native, range.begin, routine.expected))
        {
            routine.verifiedUs = now;
            return routine.native;
        }
    }
    routine.native = nullptr;
    const x86::reg32 size = range.end - range.begin;
    if (!routine.seen.empty() && memcmp(rc::host(range.begin), routine.seen.data(), size) == 0)
        return nullptr;
    for (uint32_t i = 0; i < g_translationCount; ++i)
    {
        const Translation& t = g_translations[i];
        if (t.length != size)
            continue;
        relocate(t, range.begin, range.bank, routine.expected);
        if (spansMatch(t, range.begin, routine.expected))
        {
            routine.native = &t;
            routine.verifiedUs = pf::perf::now();
            routine.seen.clear();
            pf::logInfo("bank code: routine 0x%08x-0x%08x runs translated (%s)", unsigned(range.begin),
                        unsigned(range.end), t.name);
            return &t;
        }
    }
    routine.seen.assign(rc::host(range.begin), rc::host(range.begin) + size);
    uint32_t hash = 2166136261u;
    for (x86::reg32 i = 0; i < size; ++i)
        hash = (hash ^ rc::ld8(range.begin + i)) * 16777619u;
    if (s_untranslated.size() < 256 && s_untranslated.insert(hash).second)
        pf::logInfo("bank code: routine 0x%08x-0x%08x (%u bytes, FNV-1a %08x) has no translation, interpreted",
                    unsigned(range.begin), unsigned(range.end), unsigned(size), unsigned(hash));
    return nullptr;
}

void runNative(win32::WinApplication* app, x86::CPU& cpu, const Translation& t, const Range& range)
{
    Direct memory;
    uint32_t resume = 0;
    for (;;)
    {
        resume = t.run(cpu, memory, range.begin, range.bank, resume);
        if (resume == Returned)
            break;
        app->dynamic_call(cpu.ip, cpu);
        if (cpu.terminate)
            break;
    }
}

bool s_native = true;   // setting bank_native (default on)
bool s_unlocked = true; // setting bank_unlocked (default on)

// Entered like a translated function: cpu.ip is the routine, [cpu.esp] the return address.
void runRoutine(win32::WinApplication* app, x86::CPU& cpu)
{
    Range range;
    if (s_resolvedAddress == cpu.ip && cpu.ip)
        range = s_resolvedRange;
    else if (!findRoutine(cpu.ip, range))
        unsupported(cpu.ip, "no loaded bank routine");
    s_resolvedAddress = 0;
    std::unique_lock<std::mutex> cacheGuard(s_cacheLock);
    if (++s_runs == 1)
        pf::logInfo("bank code: first routine 0x%08x-0x%08x in the bank at 0x%08x (%u translations, bank_native %s)",
                    unsigned(range.begin), unsigned(range.end), unsigned(range.bank), unsigned(g_translationCount),
                    s_native ? "on" : "off");
    Routine* cached = routine(range);
    const bool timed = pf::perf::enabled() && s_depth == 0;
    const uint32_t start = timed ? pf::perf::now() : 0;
    if (cpu.ip != range.begin)
    {
        // Evidence for inner entry points (run 69: roadnoise never native, wind native in half its runs).
        static std::unordered_set<uint64_t> s_innerLogged;
        const uint64_t key = (uint64_t(range.begin) << 32) | (cpu.ip - range.begin);
        if (s_innerLogged.size() < 256 && s_innerLogged.insert(key).second)
            pf::logInfo("bank code: routine 0x%08x-0x%08x (bank 0x%08x, +0x%x) entered at +0x%x, return 0x%08x",
                        unsigned(range.begin), unsigned(range.end), unsigned(range.bank),
                        unsigned(range.begin - range.bank), unsigned(cpu.ip - range.begin), unsigned(rc::ld32(cpu.esp)));
    }
    const Translation* native = cached && s_native && cpu.ip == range.begin ? translation(*cached, range) : nullptr;
    if (native)
    {
        ++cached->running;
        cacheGuard.unlock();
        ++s_depth;
        runNative(app, cpu, *native, range);
        --s_depth;
        cacheGuard.lock();
        --cached->running;
        ++cached->runs;
        ++cached->nativeRuns;
        if (s_runs % ReportRuns == 0)
            report();
        cacheGuard.unlock();
        if (pf::perf::enabled())
            pf::perf::audio(pf::perf::AudioBankNative, 1);
        if (timed)
        {
            pf::perf::audio(pf::perf::AudioBankRuns, 1);
            pf::perf::audio(pf::perf::AudioBankUs, pf::perf::now() - start);
        }
        return;
    }
    Routine uncached;
    if (!cached)
    {
        uncached.slots.assign(range.end - range.begin, 0);
        cached = &uncached;
    }
    ++cached->running;
    cacheGuard.unlock();
    // The interpreter's decode slots are shared per routine: interpreted runs keep the execution lock.
    const bool took = app->takeContext(cpu);
    ++s_depth;
    Interpreter interpreter(cpu, range, *cached);
    interpreter.begin(cpu.ip);
    while (interpreter.segment())
    {
        app->dynamic_call(cpu.ip, cpu);
        if (cpu.terminate)
            break;
        interpreter.resume();
    }
    --s_depth;
    if (took)
        app->releaseContext(cpu);
    cacheGuard.lock();
    --cached->running;
    ++cached->runs;
    cached->executed += interpreter.executed();
    if (s_runs % ReportRuns == 0)
        report();
    cacheGuard.unlock();
    if (pf::perf::enabled())
    {
        pf::perf::audio(pf::perf::AudioBankInterpreted, 1);
        pf::perf::audio(pf::perf::AudioBankInstructions, interpreter.executed());
        pf::perf::audio(pf::perf::AudioBankDecodes, interpreter.decoded());
    }
    if (timed)
    {
        pf::perf::audio(pf::perf::AudioBankRuns, 1);
        pf::perf::audio(pf::perf::AudioBankUs, pf::perf::now() - start);
    }
}

win32::MethodPtr resolve(x86::reg32 address)
{
    Range range;
    if (!findRoutine(address, range))
        return nullptr;
    s_resolvedAddress = address;
    s_resolvedRange = range;
    return &runRoutine;
}

}

win32::WinApplication* g_bankApp = nullptr;

void install(win32::WinApplication& app)
{
    g_bankApp = &app;
    s_native = pf::Config::getBool("bank_native", true);
    s_unlocked = pf::Config::getBool("bank_unlocked", true);
    win32::setForeignCodeResolver(&resolve);
    if (s_unlocked)
        app.setLockFreeMethod(&runRoutine);
}

}}
