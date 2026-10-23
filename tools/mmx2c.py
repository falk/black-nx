#!/usr/bin/env python3
"""mmx2c.py <default.xbe> <start> <end> <name>: a straight-line C++ body of the MMX leaf function at [start, end) of the
game's own default.xbe, for src/game/hot_xmv.cpp. The build writes the bodies into the generated code (switch/recomp.sh),
so no game code is kept in the repo. The bytes are disassembled by objdump (intel syntax); each instruction becomes the
same operation on 64-bit MMX values (SWAR helpers in hot_xmv.cpp), in the same order, with the same memory writes;
jumps become gotos. Only the subset the XMV decoder's leaves use is accepted; anything else stops the translation."""
import re, struct, subprocess, sys, tempfile

def code(xbe, start, end):
    d = open(xbe, 'rb').read()
    base, = struct.unpack_from('<I', d, 0x104)
    count, = struct.unpack_from('<I', d, 0x11C)
    headers, = struct.unpack_from('<I', d, 0x120)
    for i in range(count):
        _, va, vsize, raw, rsize = struct.unpack_from('<5I', d, headers - base + i * 56)
        if va <= start and end <= va + min(vsize, rsize):
            return d[raw + start - va:raw + end - va]
    raise SystemExit(f'{start:#x}..{end:#x} is not in a section of {xbe}')

start, end, name = int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
with tempfile.NamedTemporaryFile(suffix='.bin') as f:
    f.write(code(sys.argv[1], start, end)); f.flush()
    listing = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel', '--no-show-raw-insn',
                              f'--adjust-vma={start:#x}', f.name], check=True, capture_output=True, text=True).stdout
lines = []
for raw in listing.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)$', raw.rstrip())
    if m: lines.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
targets = set()
for a, mn, ops in lines:
    if mn.startswith('j'): targets.add(int(ops, 16))

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']

def ea(expr):
    expr = expr.replace('ds:', '')
    parts = re.findall(r'[+-]?[^+-]+', expr)
    out = []
    for p in parts:
        sign = '-' if p.startswith('-') else '+'
        p = p.lstrip('+-')
        if '*' in p:
            r, s = p.split('*'); out.append(f'{sign} ctx.{r} * {s}u')
        elif p in R32: out.append(f'{sign} ctx.{p}')
        else: out.append(f'{sign} {int(p, 16):#x}u')
    s = ' '.join(out)
    return '(uint32_t)(' + (s[2:] if s.startswith('+ ') else '0u ' + s) + ')'

def mem(op):
    m = re.match(r'(QWORD|DWORD|WORD) PTR (.*)$', op)
    if m: return m.group(1), ea(m.group(2).strip('[]'))
    m = re.match(r'ds:(0x[0-9a-f]+)$', op)
    if m: return 'QWORD', f'{int(m.group(1),16):#x}u'
    return None, None

def mmsrc(op):
    if op.startswith('mm'): return f'mm{op[2]}'
    size, a = mem(op)
    if size == 'QWORD': return f'Q(base, {a})'
    if size == 'DWORD': return f'uint64_t(X86_MEM32({a}))'
    raise SystemExit(f'operand {op}')

BIN = {'paddd': 'AddD', 'psubd': 'SubD', 'paddw': 'AddW', 'pmaddwd': 'MaddWD', 'punpckldq': 'UnpackLoDQ',
       'punpckhdq': 'UnpackHiDQ', 'punpcklwd': 'UnpackLoWD', 'packssdw': 'PackSSDW', 'packuswb': 'PackUSWB'}
out = []
flags = None
for a, mn, ops in lines:
    if a in targets: out.append(f'loc_{a:08X}:')
    o = [x.strip() for x in re.split(r',(?![^\[]*\])', ops)] if ops else []
    c = None
    if mn in BIN: c = f'{o[0]} = {BIN[mn]}({o[0]}, {mmsrc(o[1])});'
    elif mn == 'pand': c = f'{o[0]} &= {mmsrc(o[1])};'
    elif mn == 'por': c = f'{o[0]} |= {mmsrc(o[1])};'
    elif mn in ('pslld', 'psrld', 'psrad', 'psllq', 'psrlq', 'psllw', 'psrlw', 'psraw'):
        f = {'pslld': 'SllD', 'psrld': 'SrlD', 'psrad': 'SraD', 'psllq': 'SllQ', 'psrlq': 'SrlQ', 'psllw': 'SllW',
             'psrlw': 'SrlW', 'psraw': 'SraW'}[mn]
        c = f'{o[0]} = {f}({o[0]}, {int(o[1],16)});'
    elif mn == 'pcmpeqb' and o[0] == o[1]: c = f'{o[0]} = ~0ull;'
    elif mn == 'movq':
        if o[0].startswith('mm'): c = f'{o[0]} = {mmsrc(o[1])};'
        else: c = f'PutQ(base, {mem(o[0])[1]}, {o[1]});'
    elif mn == 'movd':
        if o[0].startswith('mm') and o[1] in R32: c = f'{o[0]} = ctx.{o[1]};'
        elif o[0].startswith('mm'): c = f'{o[0]} = {mmsrc(o[1])};'
        elif o[0] in R32: c = f'ctx.{o[0]} = uint32_t({o[1]});'
        else: c = f'X86_MEM32({mem(o[0])[1]}) = uint32_t({o[1]});'
    elif mn == 'emms': c = '// emms'
    elif mn == 'push': c = f'ctx.esp -= 4; X86_MEM32(ctx.esp) = ctx.{o[0]};'
    elif mn == 'pop': c = f'ctx.{o[0]} = X86_MEM32(ctx.esp); ctx.esp += 4;'
    elif mn == 'leave': c = 'ctx.esp = ctx.ebp; ctx.ebp = X86_MEM32(ctx.esp); ctx.esp += 4;'
    elif mn == 'ret': c = f'ctx.esp += 4 + {int(o[0],16) if o else 0}; return;'
    elif mn == 'lea': c = f'ctx.{o[0]} = {ea(o[1].strip("[]"))};'
    elif mn == 'mov':
        if o[0] in R32 and o[1] in R32: c = f'ctx.{o[0]} = ctx.{o[1]};'
        elif o[0] in R32 and o[1].startswith('0x'): c = f'ctx.{o[0]} = {int(o[1],16):#x}u;'
        elif o[0] in R32: c = f'ctx.{o[0]} = X86_MEM32({mem(o[1])[1]});'
        elif o[0].startswith('WORD') and o[1] == 'ax': c = f'X86_MEM16({mem(o[0])[1]}) = uint16_t(ctx.eax);'
        elif o[0].startswith('DWORD') and o[1] in R32: c = f'X86_MEM32({mem(o[0])[1]}) = ctx.{o[1]};'
    elif mn in ('sub', 'add', 'and', 'xor', 'cmp', 'test') and (o[0] in R32 or o[0].startswith('DWORD')):
        src = f'ctx.{o[1]}' if o[1] in R32 else (f'{int(o[1],16):#x}u' if o[1].startswith('0x') else f'X86_MEM32({mem(o[1])[1]})')
        dst = f'ctx.{o[0]}' if o[0] in R32 else f'X86_MEM32({mem(o[0])[1]})'
        opc = {'sub': '-', 'add': '+', 'and': '&', 'xor': '^', 'cmp': '-', 'test': '&'}[mn]
        c = f'fa = {dst}; fb = {src};'
        if mn not in ('cmp', 'test'): c += f' {dst} = fa {opc} fb;'
        flags = {'sub': 'sub', 'cmp': 'sub', 'add': 'add', 'and': 'logic', 'xor': 'logic', 'test': 'logic'}[mn]
        if mn in ('and', 'xor', 'test'): c += f' fa = fa {opc} fb;'
    elif mn in ('inc', 'dec'):
        c = f'fa = ctx.{o[0]}; fb = 1; ctx.{o[0]} = fa {"+" if mn == "inc" else "-"} 1u;'
        flags = 'add' if mn == 'inc' else 'sub'
    elif mn == 'shr' and o[0].startswith('DWORD'):
        c = f'X86_MEM32({mem(o[0])[1]}) = X86_MEM32({mem(o[0])[1]}) >> {int(o[1],16)};'
        flags = None
    elif mn in ('jl', 'jge', 'jne', 'je', 'jmp'):
        if mn != 'jmp' and not flags: raise SystemExit(f'{a:x}: {mn} after an instruction whose flags are not kept')
        if mn == 'jmp': cond = 'true'
        elif mn in ('je', 'jne'):
            z = {'sub': 'fa == fb', 'add': 'fa + fb == 0u', 'logic': 'fa == 0u'}[flags]
            cond = z if mn == 'je' else f'!({z})'
        else:
            lt = {'sub': '(int32_t)fa < (int32_t)fb', 'add': 'int64_t(int32_t(fa)) + int32_t(fb) < 0',
                  'logic': '(int32_t)fa < 0'}[flags]
            cond = lt if mn == 'jl' else f'!({lt})'
        c = f'if ({cond}) goto loc_{int(o[0],16):08X};'
    if c is None: raise SystemExit(f'{a:x}: {mn} {ops} not in the subset')
    out.append(f'    {c}  // {a:06X} {mn} {ops}'.rstrip())
print(f'// generated by tools/mmx2c.py from {start:08X}..{end:08X} of default.xbe; do not edit')
print(f'void {name}(X86Context& ctx, uint8_t* base) {{')
print('    uint64_t mm0 = ctx.mm[0], mm1 = ctx.mm[1], mm2 = ctx.mm[2], mm3 = ctx.mm[3], mm4 = ctx.mm[4], mm5 = ctx.mm[5],')
print('             mm6 = ctx.mm[6], mm7 = ctx.mm[7];')
print('    uint32_t fa = 0, fb = 0;  // the operands of the last flag setting instruction, for the next jump')
print('    struct Sync { X86Context& c; uint64_t* m[8]; ~Sync() { for (int i = 0; i < 8; i++) c.mm[i] = *m[i]; } };')
print('    Sync sync{ctx, {&mm0, &mm1, &mm2, &mm3, &mm4, &mm5, &mm6, &mm7}};')
print('\n'.join(out))
print('}')
