#!/usr/bin/env python3
"""Read-only FAT32 consistency check, for a card chkdsk will not or cannot look at.

    python fat32_check.py \\\\.\\F:          (elevated, a mounted volume)
    python fat32_check.py card.img          (an image of the first partition)

Opens the target read-only and never writes. Checks the boot sector, that both FAT copies agree, the
clean-shutdown flags, every directory (short and long name entries, "." and ".."), every cluster chain
(ends properly, no loops, no free or bad clusters inside, size agrees with length), clusters shared by two
files, and clusters that are allocated but belong to nothing. Exit status is 1 when it finds an error.
"""
import struct
import sys
from array import array

EOC = 0x0FFFFFF8
BAD = 0x0FFFFFF7
MASK = 0x0FFFFFFF
BAD_NAME_CHARS = set('<>:"/\\|?*')


class Disk:
    def __init__(self, path):
        self.f = open(path, 'rb', buffering=0)
        self.size_hint = 0

    def read(self, off, n):
        a = off & ~511
        end = (off + n + 511) & ~511
        self.f.seek(a)
        buf = b''
        while len(buf) < end - a:
            chunk = self.f.read(min(end - a - len(buf), 1 << 20))
            if not chunk:
                break
            buf += chunk
        return buf[off - a: off - a + n]


def lfn_sum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


class Check:
    def __init__(self, path):
        self.d = Disk(path)
        self.errors = []
        self.warnings = []
        self.files = 0
        self.dirs = 0

    def err(self, m):
        self.errors.append(m)

    def warn(self, m):
        self.warnings.append(m)

    def boot(self):
        b = self.d.read(0, 512)
        if len(b) < 512 or b[510:512] != b'\x55\xaa':
            self.err('boot sector: signature 55AA missing')
            return False
        self.bps, self.spc, self.rsv, self.nfats = struct.unpack_from('<HBHB', b, 11)
        self.total = struct.unpack_from('<I', b, 32)[0] or struct.unpack_from('<H', b, 19)[0]
        self.fatsz = struct.unpack_from('<I', b, 36)[0]
        self.root = struct.unpack_from('<I', b, 44)[0]
        self.fsinfo = struct.unpack_from('<H', b, 48)[0]
        if self.bps != 512 or self.spc not in (1, 2, 4, 8, 16, 32, 64, 128) or self.nfats < 1 or self.fatsz == 0:
            self.err(f'boot sector: implausible geometry (bps={self.bps} spc={self.spc} fats={self.nfats} fatsz={self.fatsz})')
            return False
        if struct.unpack_from('<H', b, 17)[0] != 0 or struct.unpack_from('<H', b, 22)[0] != 0:
            self.err('boot sector: not FAT32 (fixed root directory or 16-bit FAT size present)')
            return False
        self.data_start = self.rsv + self.nfats * self.fatsz
        self.clusters = (self.total - self.data_start) // self.spc
        self.cbytes = self.bps * self.spc
        print(f'volume: {self.total * self.bps / 1e9:.1f} GB, cluster {self.cbytes // 1024} KiB, {self.clusters} clusters, '
              f'{self.nfats} FATs of {self.fatsz} sectors, root cluster {self.root}')
        return True

    def load_fat(self):
        nbytes = (self.clusters + 2) * 4
        fats = []
        for i in range(self.nfats):
            raw = self.d.read((self.rsv + i * self.fatsz) * self.bps, self.fatsz * self.bps)
            if len(raw) < self.fatsz * self.bps:
                self.err(f'FAT {i + 1}: could only read {len(raw)} of {self.fatsz * self.bps} bytes')
            fats.append(raw)
        self.raw_fats = fats
        self.fat = array('I')
        self.fat.frombytes(fats[0][:nbytes] if len(fats[0]) >= nbytes else fats[0] + b'\0' * (nbytes - len(fats[0])))
        if sys.byteorder == 'big':
            self.fat.byteswap()

    def fat_copies(self):
        base = self.raw_fats[0]
        for i in range(1, self.nfats):
            other = self.raw_fats[i]
            if other == base:
                continue
            bad = [s for s in range(0, min(len(base), len(other)), 512) if base[s:s + 512] != other[s:s + 512]]
            self.err(f'FAT 1 and FAT {i + 1} differ in {len(bad)} sector(s), first at FAT sector {bad[0] // 512 if bad else "?"}')

    def flags(self):
        v = self.fat[1]
        if not v & 0x08000000:
            self.warn('FAT[1]: the volume was not cleanly unmounted (dirty flag set)')
        if not v & 0x04000000:
            self.warn('FAT[1]: a disk error was recorded (hardware error flag set)')
        if self.fat[0] & 0xFF != 0xF8:
            self.warn(f'FAT[0]: media byte is {self.fat[0] & 0xFF:#x}, expected 0xF8')

    def fsinfo_check(self):
        if not self.fsinfo or self.fsinfo >= self.rsv:
            self.warn('FSInfo: not present')
            return
        b = self.d.read(self.fsinfo * self.bps, 512)
        if b[0:4] != b'RRaA' or b[484:488] != b'rrAa' or b[510:512] != b'\x55\xaa':
            self.warn('FSInfo: signatures are wrong')
            return
        self.fsinfo_free = struct.unpack_from('<I', b, 488)[0]

    def chain(self, first, label, size=None, is_dir=False):
        """Follows a cluster chain, records ownership, returns the list of clusters (stops at the first error)."""
        out = []
        c = first
        while True:
            if c < 2 or c >= self.clusters + 2:
                self.err(f'{label}: chain runs to invalid cluster {c} after {len(out)} cluster(s)')
                return out
            if self.owner[c]:
                other = self.names[self.owner[c]]
                if other == label:
                    self.err(f'{label}: chain loops back to cluster {c}')
                else:
                    self.err(f'{label}: cluster {c} is also used by {other}')
                return out
            self.owner[c] = self.nid
            out.append(c)
            n = self.fat[c] & MASK
            if n >= EOC:
                break
            if n == 0:
                self.err(f'{label}: chain runs into a free cluster after {len(out)} cluster(s)')
                break
            if n == BAD:
                self.err(f'{label}: chain runs into a bad cluster after {len(out)} cluster(s)')
                break
            c = n
        if size is not None and not is_dir:
            need = (size + self.cbytes - 1) // self.cbytes
            if len(out) != need:
                self.err(f'{label}: size {size} needs {need} cluster(s) but the chain has {len(out)}')
        return out

    def read_dir(self, clusters):
        parts = []
        for c in clusters:
            parts.append(self.d.read((self.data_start + (c - 2) * self.spc) * self.bps, self.cbytes))
        return b''.join(parts)

    def walk(self):
        self.owner = array('I', [0]) * (self.clusters + 2)
        self.names = ['']
        self.nid = 1
        self.names.append('/')
        rootc = self.chain(self.root, '/', is_dir=True)
        todo = [('/', rootc, 0)]
        while todo:
            path, clusters, parent = todo.pop()
            self.dirs += 1
            data = self.read_dir(clusters)
            self.scan_dir(path, data, clusters[0] if clusters else 0, parent, todo)

    def scan_dir(self, path, data, self_cluster, parent, todo):
        lfn = []          # entries seen since the last short entry, in disk order
        for off in range(0, len(data), 32):
            e = data[off:off + 32]
            if e[0] == 0x00:
                if any(data[off:]):
                    self.warn(f'{path}: data after the end-of-directory marker')
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr & 0x3F == 0x0F:
                lfn.append(e)
                continue
            if attr & 0x08:
                lfn = []
                continue
            short = bytes(e[:11])
            name = self.long_name(path, short, lfn)
            lfn = []
            hi = struct.unpack_from('<H', e, 20)[0]
            lo = struct.unpack_from('<H', e, 26)[0]
            first = (hi << 16) | lo
            size = struct.unpack_from('<I', e, 28)[0]
            is_dir = bool(attr & 0x10)
            if short[:2] == b'. ':
                if first != self_cluster:
                    self.err(f'{path}: "." points at cluster {first}, the directory is at {self_cluster}')
                continue
            if short[:2] == b'..':
                if first != parent:
                    self.err(f'{path}: ".." points at cluster {first}, the parent is at {parent}')
                continue
            full = path + name + ('/' if is_dir else '')
            if short[0] == 0x20 or short == b' ' * 11:
                self.err(f'{full}: short name is blank')
            if is_dir and size != 0:
                self.warn(f'{full}: directory entry has a size of {size}')
            self.nid += 1
            self.names.append(full)
            if first == 0:
                if size and not is_dir:
                    self.err(f'{full}: size {size} but no clusters')
                if is_dir:
                    self.err(f'{full}: directory with no cluster')
                if not is_dir:
                    self.files += 1
                continue
            cl = self.chain(first, full, size, is_dir)
            if is_dir:
                if cl:
                    todo.append((full, cl, self_cluster if path != '/' else 0))
            else:
                self.files += 1

    def long_name(self, path, short, lfn):
        base = short[:8].decode('cp437', 'replace').rstrip()
        ext = short[8:].decode('cp437', 'replace').rstrip()
        sname = base + ('.' + ext if ext else '')
        if short[0] == 0x05:
            sname = '\xe5' + sname[1:]
        if not lfn:
            return sname
        want = lfn_sum(short)
        seqs = [e[0] & 0x1F for e in lfn]
        ok = True
        if not lfn[0][0] & 0x40:
            ok = False
        if seqs != list(range(len(lfn), 0, -1)):
            ok = False
        if any(e[13] != want for e in lfn):
            ok = False
        if not ok:
            self.err(f'{path}{sname}: long name entries are damaged or do not belong to this file (seq {seqs}, checksum)')
            return sname
        chars = []
        for e in reversed(lfn):
            u = e[1:11] + e[14:26] + e[28:32]
            for i in range(0, 26, 2):
                v = u[i] | (u[i + 1] << 8)
                if v in (0x0000, 0xFFFF):
                    break
                chars.append(v)
            else:
                continue
            break
        raw = b''.join(c.to_bytes(2, 'little') for c in chars)
        try:
            name = raw.decode('utf-16-le')
        except UnicodeDecodeError:
            self.err(f'{path}{sname}: long name is not valid UTF-16')
            name = raw.decode('utf-16-le', 'replace')
        if not name:
            self.err(f'{path}{sname}: long name is empty')
            return sname
        if any(ord(c) < 32 or c in BAD_NAME_CHARS for c in name):
            self.warn(f'{path}{name}: name has characters Windows does not allow')
        return name

    def lost(self):
        lost, bad, free = [], 0, 0
        for c in range(2, self.clusters + 2):
            v = self.fat[c] & MASK
            if v == 0:
                free += 1
            elif v == BAD:
                bad += 1
            elif not self.owner[c]:
                lost.append(c)
        self.free = free
        if lost:
            self.err(f'{len(lost)} cluster(s) are allocated but belong to no file or folder (lost chains), first at cluster {lost[0]}')
        if bad:
            self.warn(f'{bad} cluster(s) are marked bad')
        fi = getattr(self, 'fsinfo_free', None)
        if fi not in (None, 0xFFFFFFFF) and fi != free:
            self.warn(f'FSInfo says {fi} free clusters, the FAT has {free} (harmless: Windows recomputes it)')

    def run(self):
        if not self.boot():
            return self.report()
        self.load_fat()
        self.fat_copies()
        self.flags()
        self.fsinfo_check()
        self.walk()
        self.lost()
        return self.report()

    def report(self):
        print(f'checked {self.dirs} folder(s) and {self.files} file(s)')
        for m in self.errors:
            print('ERROR  ', m)
        for m in self.warnings:
            print('WARNING', m)
        print(f'{len(self.errors)} error(s), {len(self.warnings)} warning(s)')
        return 1 if self.errors else 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(Check(sys.argv[1]).run())
