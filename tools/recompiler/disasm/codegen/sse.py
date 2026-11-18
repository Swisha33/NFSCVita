"""SSE instructions on cpu.xmm (include/cpu.h x86::regxmm): the scalar and packed float code of NFSC.exe's EA audio
library, which was compiled for SSE and runs it unconditionally (a static initializer at 0x9bdb70 reaches 0x8d7f20).

Lanes are plain C++ float arithmetic; MXCSR stays at its default (round to nearest, exceptions masked, no flush to
zero), which is the only state the game uses (it never executes ldmxcsr).
"""

from capstone import x86 as X


def xreg(ctx, op):
    return 'cpu.xmm.%s' % ctx.ins.reg_name(op.reg)


def is_xmm(ctx, op):
    return op.type == X.X86_OP_REG and ctx.ins.reg_name(op.reg).startswith('xmm')


def x128(ctx, op):
    """A 128-bit operand as an x86::regxmm rvalue."""
    if op.type == X.X86_OP_REG:
        return xreg(ctx, op)
    return 'rc::ldx(%s)' % ctx.address(op)


def block(lines):
    return ['{'] + ['    ' + l for l in lines] + ['}']


# ---- moves ---------------------------------------------------------------------------------------------------------

def _move128(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::stx(%s, %s);' % (ctx.address(dst), x128(ctx, src))]
    return ['%s = %s;' % (xreg(ctx, dst), x128(ctx, src))]


cg_movaps = _move128
cg_movups = _move128
cg_movapd = _move128
cg_movupd = _move128


def cg_movss(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::stf32(%s, %s.f[0]);' % (ctx.address(dst), xreg(ctx, src))]
    if src.type == X.X86_OP_MEM:
        # From memory the upper lanes are cleared.
        return block(['x86::regxmm r = {};', 'r.f[0] = rc::ldf32(%s);' % ctx.address(src),
                      '%s = r;' % xreg(ctx, dst)])
    return ['%s.u[0] = %s.u[0];' % (xreg(ctx, dst), xreg(ctx, src))]


def cg_movlps(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::st64(%s, %s.q[0]);' % (ctx.address(dst), xreg(ctx, src))]
    return ['%s.q[0] = rc::ld64(%s);' % (xreg(ctx, dst), ctx.address(src))]


def cg_movhps(ctx, dst, src):
    if dst.type == X.X86_OP_MEM:
        return ['rc::st64(%s, %s.q[1]);' % (ctx.address(dst), xreg(ctx, src))]
    return ['%s.q[1] = rc::ld64(%s);' % (xreg(ctx, dst), ctx.address(src))]


def cg_movlhps(ctx, dst, src):
    return ['%s.q[1] = %s.q[0];' % (xreg(ctx, dst), xreg(ctx, src))]


def cg_movhlps(ctx, dst, src):
    return ['%s.q[0] = %s.q[1];' % (xreg(ctx, dst), xreg(ctx, src))]


# ---- arithmetic ----------------------------------------------------------------------------------------------------

def _packed(op):
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return block(['const x86::regxmm b = %s;' % x128(ctx, src),
                      'for (int i = 0; i < 4; ++i)', '    %s.f[i] = %s.f[i] %s b.f[i];' % (d, d, op)])
    return cg


def _packed_function(expression):
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return block(['const x86::regxmm b = %s;' % x128(ctx, src),
                      'for (int i = 0; i < 4; ++i)',
                      '    %s.f[i] = %s;' % (d, expression.format(a='%s.f[i]' % d, b='b.f[i]'))])
    return cg


def _scalar_source(ctx, src):
    if src.type == X.X86_OP_MEM:
        return 'rc::ldf32(%s)' % ctx.address(src)
    return '%s.f[0]' % xreg(ctx, src)


def _scalar(op):
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return ['%s.f[0] = %s.f[0] %s %s;' % (d, d, op, _scalar_source(ctx, src))]
    return cg


def _scalar_function(expression):
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return ['%s.f[0] = %s;' % (d, expression.format(a='%s.f[0]' % d, b=_scalar_source(ctx, src)))]
    return cg


cg_addps = _packed('+')
cg_subps = _packed('-')
cg_mulps = _packed('*')
cg_divps = _packed('/')
cg_addss = _scalar('+')
cg_subss = _scalar('-')
cg_mulss = _scalar('*')
cg_divss = _scalar('/')
# minps/maxps return the second operand when either is NaN or both are zero, as SSE does.
cg_minps = _packed_function('({a} < {b} ? {a} : {b})')
cg_maxps = _packed_function('({a} > {b} ? {a} : {b})')
cg_minss = _scalar_function('({a} < {b} ? {a} : {b})')
cg_maxss = _scalar_function('({a} > {b} ? {a} : {b})')
cg_sqrtss = _scalar_function('__builtin_sqrtf({b})')


def cg_sqrtps(ctx, dst, src):
    d = xreg(ctx, dst)
    return block(['const x86::regxmm b = %s;' % x128(ctx, src),
                  'for (int i = 0; i < 4; ++i)', '    %s.f[i] = __builtin_sqrtf(b.f[i]);' % d])


def _bitwise(expression):
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return block(['const x86::regxmm b = %s;' % x128(ctx, src),
                      'for (int i = 0; i < 4; ++i)',
                      '    %s.u[i] = %s;' % (d, expression.format(a='%s.u[i]' % d, b='b.u[i]'))])
    return cg


cg_andps = _bitwise('{a} & {b}')
cg_andnps = _bitwise('~{a} & {b}')
cg_orps = _bitwise('{a} | {b}')
cg_xorps = _bitwise('{a} ^ {b}')
cg_andpd = cg_andps
cg_orpd = cg_orps
cg_xorpd = cg_xorps


def _compare(expression):
    """cmpXXps: all-ones lanes where the predicate holds (unordered compares false except the 'not' forms)."""
    def cg(ctx, dst, src):
        d = xreg(ctx, dst)
        return block(['const x86::regxmm b = %s;' % x128(ctx, src),
                      'for (int i = 0; i < 4; ++i)',
                      '    %s.u[i] = (%s) ? 0xffffffffu : 0u;'
                      % (d, expression.format(a='%s.f[i]' % d, b='b.f[i]'))])
    return cg


cg_cmpeqps = _compare('{a} == {b}')
cg_cmpltps = _compare('{a} < {b}')
cg_cmpleps = _compare('{a} <= {b}')
cg_cmpunordps = _compare('{a} != {a} || {b} != {b}')
cg_cmpneqps = _compare('!({a} == {b})')
cg_cmpnltps = _compare('!({a} < {b})')
cg_cmpnleps = _compare('!({a} <= {b})')
cg_cmpordps = _compare('{a} == {a} && {b} == {b}')


# ---- shuffles --------------------------------------------------------------------------------------------------------

def cg_shufps(ctx, dst, src, imm):
    d = xreg(ctx, dst)
    i = imm.imm & 0xff
    return block(['const x86::regxmm a = %s;' % d, 'const x86::regxmm b = %s;' % x128(ctx, src),
                  '%s.u[0] = a.u[%d];' % (d, i & 3), '%s.u[1] = a.u[%d];' % (d, (i >> 2) & 3),
                  '%s.u[2] = b.u[%d];' % (d, (i >> 4) & 3), '%s.u[3] = b.u[%d];' % (d, (i >> 6) & 3)])


def cg_pshufd(ctx, dst, src, imm):
    d = xreg(ctx, dst)
    i = imm.imm & 0xff
    return block(['const x86::regxmm b = %s;' % x128(ctx, src)]
                 + ['%s.u[%d] = b.u[%d];' % (d, k, (i >> (2 * k)) & 3) for k in range(4)])


def cg_unpcklps(ctx, dst, src):
    d = xreg(ctx, dst)
    return block(['const x86::regxmm a = %s;' % d, 'const x86::regxmm b = %s;' % x128(ctx, src),
                  '%s.u[0] = a.u[0];' % d, '%s.u[1] = b.u[0];' % d, '%s.u[2] = a.u[1];' % d, '%s.u[3] = b.u[1];' % d])


def cg_unpckhps(ctx, dst, src):
    d = xreg(ctx, dst)
    return block(['const x86::regxmm a = %s;' % d, 'const x86::regxmm b = %s;' % x128(ctx, src),
                  '%s.u[0] = a.u[2];' % d, '%s.u[1] = b.u[2];' % d, '%s.u[2] = a.u[3];' % d, '%s.u[3] = b.u[3];' % d])


# ---- conversions -----------------------------------------------------------------------------------------------------

def _mmx_name(ctx, op):
    return 'cpu.mmx.%s' % ctx.ins.reg_name(op.reg)


def cg_cvtps2pi(ctx, dst, src):
    """Two floats (xmm low half or m64) to two int32 in an MMX register."""
    if src.type == X.X86_OP_MEM:
        a = ctx.address(src)
        lo, hi = 'rc::ldf32(%s)' % a, 'rc::ldf32(%s + 4)' % a
    else:
        lo, hi = '%s.f[0]' % xreg(ctx, src), '%s.f[1]' % xreg(ctx, src)
    return ['%s = x86::from_reg64(x86::reg64(x86::reg32(rc::cvtRound(%s))) | x86::reg64(x86::reg32(rc::cvtRound(%s))) << 32);'
            % (_mmx_name(ctx, dst), lo, hi)]


def cg_cvttps2pi(ctx, dst, src):
    if src.type == X.X86_OP_MEM:
        a = ctx.address(src)
        lo, hi = 'rc::ldf32(%s)' % a, 'rc::ldf32(%s + 4)' % a
    else:
        lo, hi = '%s.f[0]' % xreg(ctx, src), '%s.f[1]' % xreg(ctx, src)
    return ['%s = x86::from_reg64(x86::reg64(x86::reg32(x86::sreg32(%s))) | x86::reg64(x86::reg32(x86::sreg32(%s))) << 32);'
            % (_mmx_name(ctx, dst), lo, hi)]


def cg_cvtpi2ps(ctx, dst, src):
    """Two int32 (MMX register or m64) to the two low floats; the upper lanes stay."""
    if src.type == X.X86_OP_MEM:
        v = 'rc::ld64(%s)' % ctx.address(src)
    else:
        v = 'x86::reg64(%s)' % _mmx_name(ctx, src)
    d = xreg(ctx, dst)
    return block(['const x86::reg64 v = %s;' % v, '%s.f[0] = float(x86::sreg32(x86::reg32(v)));' % d,
                  '%s.f[1] = float(x86::sreg32(x86::reg32(v >> 32)));' % d])


def cg_cvtsi2ss(ctx, dst, src):
    return ['%s.f[0] = float(x86::sreg32(%s));' % (xreg(ctx, dst), ctx.val(src))]


def cg_cvtss2si(ctx, dst, src):
    return ctx.put(dst, 'x86::reg32(rc::cvtRound(%s))' % _scalar_source(ctx, src))


def cg_cvttss2si(ctx, dst, src):
    v = _scalar_source(ctx, src)
    return ctx.put(dst, 'x86::reg32(!(%s >= -2147483648.0f && %s < 2147483648.0f) ? 0x80000000u : x86::reg32(x86::sreg32(%s)))'
                   % (v, v, v))
