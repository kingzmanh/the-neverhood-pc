"""dis.py <hexaddr> [count] [module]: disassemble the game code (nhc default, smk)"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog
a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
p = Prog('gamedata/smackw32.dll', 0x10000000, 0x7f100000) if a >= 0x10000000 else Prog('gamedata/nhc.exe')
for _ in range(n):
    i = p.insn(a)
    if not i:
        print(hex(a), '??'); break
    print('%08x  %-8s %s' % (a, i.mnemonic, i.op_str))
    a += i.size
