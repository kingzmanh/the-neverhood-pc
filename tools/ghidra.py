"""Ghidra headless analysis of the game binaries -> re/<tag>/ghidra (functions.tsv + decomp.c).

usage: python tools/ghidra.py [tag...]      tags: nhc smk
"""
import os, shutil, subprocess, sys

GHIDRA = os.environ.get('GHIDRA', r'C:\Users\Msi\Tools\ghidra_12.1.4_PUBLIC')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINS = {'nhc': 'nhc.exe', 'smk': 'smackw32.dll'}


def run(tag):
    head = os.path.join(GHIDRA, 'support', 'analyzeHeadless.bat')
    d = os.path.join(ROOT, 're', tag)
    pd = os.path.join(d, 'proj')
    shutil.rmtree(pd, ignore_errors=True)
    os.makedirs(pd)
    os.makedirs(os.path.join(d, 'ghidra'), exist_ok=True)
    with open(os.path.join(d, 'ghidra_headless.log'), 'w') as log:
        subprocess.call([head, pd, tag, '-import', os.path.join(ROOT, 'gamedata', BINS[tag]),
                         '-scriptPath', os.path.join(ROOT, 'tools', 'ghidra'), '-max-cpu', '6',
                         '-postScript', 'ExportAll.java', os.path.join(d, 'ghidra')],
                        stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    src = os.path.join(d, 'ghidra', 'functions.tsv')
    if os.path.exists(src):
        shutil.copy(src, os.path.join(d, 'functions.tsv'))
        print('%s: %d functions' % (tag, sum(1 for _ in open(src))))
    else:
        print('%s: no function list - see %s' % (tag, os.path.join(d, 'ghidra_headless.log')))


if __name__ == '__main__':
    for t in sys.argv[1:] or BINS:
        run(t)
