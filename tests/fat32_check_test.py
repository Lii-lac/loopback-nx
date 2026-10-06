#!/usr/bin/env python3
"""Builds a small FAT32 image with mkfs.fat and mtools, checks that fat32_check.py passes it, then breaks it in
five known ways and checks that each one is reported. Needs dosfstools and mtools (see run_fat32_check_test.sh)."""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from fat32_check import Check  # noqa: E402


def run(*a):
    subprocess.run(a, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build(path):
    run('truncate', '-s', '40M', path)
    run('mkfs.fat', '-F', '32', '-s', '1', path)
    tmp = tempfile.mkdtemp()
    small = os.path.join(tmp, 's.txt')
    big = os.path.join(tmp, 'big.bin')
    open(small, 'w').write('hello\n')
    open(big, 'wb').write(os.urandom(300000))
    run('mmd', '-i', path, '::/dir', '::/dir/sub', '::/Nintendo')
    run('mcopy', '-i', path, small, '::/short.txt')
    run('mcopy', '-i', path, small, '::/A very long file name with spaces.txt')
    run('mcopy', '-i', path, big, '::/dir/sub/big.bin')
    run('mcopy', '-i', path, big, '::/Nintendo/second.bin')
    shutil.rmtree(tmp)


class Img:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        b = self.read(0, 512)
        self.spc = b[13]
        self.rsv = struct.unpack_from('<H', b, 14)[0]
        self.nf = b[16]
        self.fatsz = struct.unpack_from('<I', b, 36)[0]
        self.root = struct.unpack_from('<I', b, 44)[0]
        self.data = self.rsv + self.nf * self.fatsz

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def write(self, off, data):
        self.f.seek(off)
        self.f.write(data)
        self.f.flush()

    def fat_get(self, c):
        return struct.unpack('<I', self.read(self.rsv * 512 + c * 4, 4))[0] & 0x0FFFFFFF

    def fat_set(self, c, v, copies=(0, 1)):
        for i in copies:
            self.write((self.rsv + i * self.fatsz) * 512 + c * 4, struct.pack('<I', v))

    def cluster_off(self, c):
        return (self.data + (c - 2) * self.spc) * 512

    def find_entry(self, dir_cluster, short):
        """Returns the byte offset of the short entry named `short` in a one-cluster directory."""
        off = self.cluster_off(dir_cluster)
        data = self.read(off, self.spc * 512)
        for i in range(0, len(data), 32):
            if data[i:i + 11] == short:
                return off + i
        raise KeyError(short)

    def first_cluster(self, entry_off):
        e = self.read(entry_off, 32)
        return (struct.unpack_from('<H', e, 20)[0] << 16) | struct.unpack_from('<H', e, 26)[0]

    def set_first_cluster(self, entry_off, c):
        self.write(entry_off + 20, struct.pack('<H', c >> 16))
        self.write(entry_off + 26, struct.pack('<H', c & 0xFFFF))

    def free_cluster(self):
        for c in range(2, 40000):
            if self.fat_get(c) == 0:
                return c


def check(path):
    c = Check(path)
    import io
    import contextlib
    with contextlib.redirect_stdout(io.StringIO()):
        c.run()
    return c


def expect(name, c, want_error=None, want_warning=None):
    text = ' | '.join(c.errors)
    ok = (want_error is None and not c.errors) or (want_error and want_error in text)
    if want_warning:
        ok = ok and want_warning in ' | '.join(c.warnings)
    print(('PASS ' if ok else 'FAIL ') + name + ('' if ok else f'\n  errors={c.errors}\n  warnings={c.warnings}'))
    return ok


def main():
    work = tempfile.mkdtemp()
    base = os.path.join(work, 'base.img')
    build(base)
    results = []
    results.append(expect('clean image has no errors', check(base)))

    def broken(tag):
        p = os.path.join(work, tag + '.img')
        shutil.copy(base, p)
        return p, Img(p)

    p, im = broken('fat2')
    im.write((im.rsv + im.fatsz) * 512 + 100 * 4, b'\x01\x02\x03\x04')
    results.append(expect('FAT copies that differ', check(p), 'FAT 1 and FAT 2 differ'))

    p, im = broken('lost')
    im.fat_set(im.free_cluster(), 0x0FFFFFFF)
    results.append(expect('allocated cluster nobody owns', check(p), 'belong to no file'))

    p, im = broken('cross')
    sub = im.first_cluster(im.find_entry(im.root, b'DIR        '))
    big = im.find_entry(sub, b'SUB        ')
    sub2 = im.first_cluster(big)
    e_big = im.find_entry(sub2, b'BIG     BIN')
    nint = im.first_cluster(im.find_entry(im.root, b'NINTENDO   '))
    e_second = im.find_entry(nint, b'SECOND  BIN')
    im.set_first_cluster(e_second, im.first_cluster(e_big))
    results.append(expect('two files sharing clusters', check(p), 'is also used by'))

    p, im = broken('cut')
    sub = im.first_cluster(im.find_entry(im.root, b'DIR        '))
    sub2 = im.first_cluster(im.find_entry(sub, b'SUB        '))
    first = im.first_cluster(im.find_entry(sub2, b'BIG     BIN'))
    im.fat_set(im.fat_get(first), 0)
    results.append(expect('chain with a free cluster in it', check(p), 'free cluster'))

    p, im = broken('lfn')
    # the long entry sits just before the short one; spoil its checksum byte
    off = im.find_entry(im.root, b'AVERYL~1TXT')
    im.write(off - 32 + 13, bytes([im.read(off - 32 + 13, 1)[0] ^ 0xFF]))
    results.append(expect('long name with a wrong checksum', check(p), 'long name entries'))

    shutil.rmtree(work)
    print('%d/%d passed' % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
