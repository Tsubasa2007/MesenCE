"""nsf2bbk.py - prepares an NSF for NSFPLAY.CMD on the BBK learning machine.

usage: python nsf2bbk.py <file.nsf> [-o OUT.NSB] [--seconds N] [--no-scan]

NSFPLAY plays plain NSFs by itself. This converter is for the two kinds it cannot:

- Bank switching. An NSF switches 4K banks by writing $5FF8-$5FFF. On the BBK that is
  ordinary memory and the page registers work in 8K. The converter replaces each such
  store with a JSR to NSFPLAY's bank routines ($5000 up) and lists the 8K pairs of banks
  the tune uses that NSFPLAY has to put together itself.
- Expansion sound (VRC6, VRC7, MMC5, Namco 163, Sunsoft 5B). The BBK has none of these
  chips, so only the 2A03 part plays. Their register writes would land in the BBK's
  memory and its I/O page; the converter points those stores at a scratch area instead.

It finds the stores by running every song (INIT, then PLAY for --seconds, default 60) in
a 6502 emulator, and adds bank stores it finds in the code by pattern (--no-scan to skip
those). Then it runs the patched tune the way NSFPLAY does and checks that every song
writes exactly the same sound registers as the original did.

The output is an .NSB file: the NSF header, a 128-byte block for NSFPLAY, then the
patched data. Other NSF players cannot play it. Copy it to the BBK floppy next to
NSFPLAY.CMD.
"""
import argparse
import os
import re
import struct
import sys

# NSFPLAY's resident part (nsfplay.s, segment RESIDENT)
STUB_SA = 0x5000	# STA $5FF8+n
STUB_SX = 0x5018	# STX $5FF8+n
STUB_SY = 0x5030	# STY $5FF8+n
STUB_SAX = 0x5048	# STA $5FF8+k,X
STUB_SAY = 0x5060	# STA $5FF8+k,Y
STUB_END = 0x5078
SINK = 0x5700
MAXEXTRA = 32

EXPANSION = {0x01: 'VRC6', 0x02: 'VRC7', 0x04: 'FDS', 0x08: 'MMC5', 0x10: 'Namco 163', 0x20: 'Sunsoft 5B'}


class Halt(Exception):
	pass


# --------------------------------------------------------------------------------------
# 6502

MODES = {}
SIZES = {'imp': 1, 'acc': 1, 'imm': 2, 'zp': 2, 'zpx': 2, 'zpy': 2, 'rel': 2, 'izx': 2, 'izy': 2,
         'abs': 3, 'abx': 3, 'aby': 3, 'ind': 3}


class Cpu:
	def __init__(self, read, write):
		self.read = read
		self.write = write
		self.a = self.x = self.y = 0
		self.sp = 0xFD
		self.p = 0x24
		self.pc = 0

	def nz(self, v):
		v &= 0xFF
		self.p = (self.p & 0x7D) | (v & 0x80) | (0 if v else 2)
		return v

	def push(self, v):
		self.write(0x100 + self.sp, v & 0xFF)
		self.sp = (self.sp - 1) & 0xFF

	def pull(self):
		self.sp = (self.sp + 1) & 0xFF
		return self.read(0x100 + self.sp)

	def fetch(self):
		v = self.read(self.pc)
		self.pc = (self.pc + 1) & 0xFFFF
		return v

	def fetch16(self):
		lo = self.fetch()
		return lo | (self.fetch() << 8)

	def ea(self, mode):
		if mode == 'imm':
			a = self.pc
			self.pc = (self.pc + 1) & 0xFFFF
			return a
		if mode == 'zp':
			return self.fetch()
		if mode == 'zpx':
			return (self.fetch() + self.x) & 0xFF
		if mode == 'zpy':
			return (self.fetch() + self.y) & 0xFF
		if mode == 'abs':
			return self.fetch16()
		if mode == 'abx':
			return (self.fetch16() + self.x) & 0xFFFF
		if mode == 'aby':
			return (self.fetch16() + self.y) & 0xFFFF
		if mode == 'izx':
			z = (self.fetch() + self.x) & 0xFF
			return self.read(z) | (self.read((z + 1) & 0xFF) << 8)
		if mode == 'izy':
			z = self.fetch()
			return ((self.read(z) | (self.read((z + 1) & 0xFF) << 8)) + self.y) & 0xFFFF
		raise ValueError(mode)

	def adc(self, v):
		r = self.a + v + (self.p & 1)
		ov = (~(self.a ^ v) & (self.a ^ r) & 0x80) != 0
		self.p = (self.p & 0xBE) | (1 if r > 0xFF else 0) | (0x40 if ov else 0)
		self.a = self.nz(r)

	def cmp(self, reg, v):
		self.p = (self.p & 0xFE) | (1 if reg >= v else 0)
		self.nz(reg - v)

	def step(self):
		op = self.fetch()
		h = OPS.get(op)
		if h is None:
			raise Halt(f'unknown opcode ${op:02X} at ${(self.pc - 1) & 0xFFFF:04X}')
		h(self)


OPS = {}


def define(code, mode, fn):
	MODES[code] = mode
	if mode in ('imp',):
		OPS[code] = fn
	else:
		OPS[code] = lambda c, fn=fn, mode=mode: fn(c, mode)


def group(table, fn):
	for code, mode in table.items():
		define(code, mode, fn)


def _rmw(fn):
	def f(c, m):
		if m == 'acc':
			c.a = fn(c, c.a)
		else:
			a = c.ea(m)
			v = c.read(a)
			c.write(a, v)
			c.write(a, fn(c, v))
	return f


def _asl(c, v):
	c.p = (c.p & 0xFE) | (v >> 7)
	return c.nz(v << 1)


def _lsr(c, v):
	c.p = (c.p & 0xFE) | (v & 1)
	return c.nz(v >> 1)


def _rol(c, v):
	r = (v << 1) | (c.p & 1)
	c.p = (c.p & 0xFE) | (v >> 7)
	return c.nz(r)


def _ror(c, v):
	r = (v >> 1) | ((c.p & 1) << 7)
	c.p = (c.p & 0xFE) | (v & 1)
	return c.nz(r)


def _bit(c, m):
	v = c.read(c.ea(m))
	c.p = (c.p & 0x3D) | (v & 0xC0) | (0 if v & c.a else 2)


def _setreg(name):
	def f(c, m):
		setattr(c, name, c.nz(c.read(c.ea(m))))
	return f


def _store(name):
	def f(c, m):
		c.write(c.ea(m), getattr(c, name))
	return f


group({0xA9: 'imm', 0xA5: 'zp', 0xB5: 'zpx', 0xAD: 'abs', 0xBD: 'abx', 0xB9: 'aby', 0xA1: 'izx', 0xB1: 'izy'}, _setreg('a'))
group({0xA2: 'imm', 0xA6: 'zp', 0xB6: 'zpy', 0xAE: 'abs', 0xBE: 'aby'}, _setreg('x'))
group({0xA0: 'imm', 0xA4: 'zp', 0xB4: 'zpx', 0xAC: 'abs', 0xBC: 'abx'}, _setreg('y'))
group({0x85: 'zp', 0x95: 'zpx', 0x8D: 'abs', 0x9D: 'abx', 0x99: 'aby', 0x81: 'izx', 0x91: 'izy'}, _store('a'))
group({0x86: 'zp', 0x96: 'zpy', 0x8E: 'abs'}, _store('x'))
group({0x84: 'zp', 0x94: 'zpx', 0x8C: 'abs'}, _store('y'))
for base, fn in ((0x00, lambda c, v: setattr(c, 'a', c.nz(c.a | v))),
                 (0x20, lambda c, v: setattr(c, 'a', c.nz(c.a & v))),
                 (0x40, lambda c, v: setattr(c, 'a', c.nz(c.a ^ v))),
                 (0x60, lambda c, v: c.adc(v)),
                 (0xC0, lambda c, v: c.cmp(c.a, v)),
                 (0xE0, lambda c, v: c.adc(v ^ 0xFF))):
	group({base + 0x09: 'imm', base + 0x05: 'zp', base + 0x15: 'zpx', base + 0x0D: 'abs', base + 0x1D: 'abx',
	       base + 0x19: 'aby', base + 0x01: 'izx', base + 0x11: 'izy'},
	      lambda c, m, fn=fn: fn(c, c.read(c.ea(m))))
group({0xE0: 'imm', 0xE4: 'zp', 0xEC: 'abs'}, lambda c, m: c.cmp(c.x, c.read(c.ea(m))))
group({0xC0: 'imm', 0xC4: 'zp', 0xCC: 'abs'}, lambda c, m: c.cmp(c.y, c.read(c.ea(m))))
group({0x24: 'zp', 0x2C: 'abs'}, _bit)
for base, fn in ((0x00, _asl), (0x20, _rol), (0x40, _lsr), (0x60, _ror)):
	group({base + 0x0A: 'acc', base + 0x06: 'zp', base + 0x16: 'zpx', base + 0x0E: 'abs', base + 0x1E: 'abx'}, _rmw(fn))
group({0xE6: 'zp', 0xF6: 'zpx', 0xEE: 'abs', 0xFE: 'abx'}, _rmw(lambda c, v: c.nz(v + 1)))
group({0xC6: 'zp', 0xD6: 'zpx', 0xCE: 'abs', 0xDE: 'abx'}, _rmw(lambda c, v: c.nz(v - 1)))


def _flag(mask, val):
	return lambda c: setattr(c, 'p', (c.p | mask) if val else (c.p & ~mask & 0xFF))


for code, mask, val in ((0x18, 1, 0), (0x38, 1, 1), (0x58, 4, 0), (0x78, 4, 1), (0xB8, 0x40, 0), (0xD8, 8, 0), (0xF8, 8, 1)):
	define(code, 'imp', _flag(mask, val))


def _transfer(src, dst, flags=True):
	def f(c):
		v = getattr(c, src)
		setattr(c, dst, c.nz(v) if flags else v)
	return f


define(0xAA, 'imp', _transfer('a', 'x'))
define(0xA8, 'imp', _transfer('a', 'y'))
define(0x8A, 'imp', _transfer('x', 'a'))
define(0x98, 'imp', _transfer('y', 'a'))
define(0xBA, 'imp', _transfer('sp', 'x'))
define(0x9A, 'imp', _transfer('x', 'sp', False))
define(0xE8, 'imp', lambda c: setattr(c, 'x', c.nz(c.x + 1)))
define(0xC8, 'imp', lambda c: setattr(c, 'y', c.nz(c.y + 1)))
define(0xCA, 'imp', lambda c: setattr(c, 'x', c.nz(c.x - 1)))
define(0x88, 'imp', lambda c: setattr(c, 'y', c.nz(c.y - 1)))
define(0x48, 'imp', lambda c: c.push(c.a))
define(0x08, 'imp', lambda c: c.push(c.p | 0x30))
define(0x68, 'imp', lambda c: setattr(c, 'a', c.nz(c.pull())))
define(0x28, 'imp', lambda c: setattr(c, 'p', (c.pull() & 0xEF) | 0x20))
for code in (0xEA, 0x1A, 0x3A, 0x5A, 0x7A, 0xDA, 0xFA):
	define(code, 'imp', lambda c: None)


def _jmp(c):
	c.pc = c.fetch16()


def _jmpi(c):
	a = c.fetch16()
	c.pc = c.read(a) | (c.read((a & 0xFF00) | ((a + 1) & 0xFF)) << 8)


def _jsr(c):
	t = c.fetch16()
	r = (c.pc - 1) & 0xFFFF
	c.push(r >> 8)
	c.push(r)
	c.pc = t


def _rts(c):
	lo = c.pull()
	c.pc = ((lo | (c.pull() << 8)) + 1) & 0xFFFF


def _rti(c):
	c.p = (c.pull() & 0xEF) | 0x20
	lo = c.pull()
	c.pc = lo | (c.pull() << 8)


def _brk(c):
	raise Halt(f'BRK at ${(c.pc - 1) & 0xFFFF:04X}')


define(0x4C, 'abs', lambda c, m: _jmp(c))
define(0x6C, 'ind', lambda c, m: _jmpi(c))
define(0x20, 'abs', lambda c, m: _jsr(c))
define(0x60, 'imp', _rts)
define(0x40, 'imp', _rti)
define(0x00, 'imp', _brk)
for code, mask, val in ((0x10, 0x80, 0), (0x30, 0x80, 1), (0x50, 0x40, 0), (0x70, 0x40, 1),
                        (0x90, 1, 0), (0xB0, 1, 1), (0xD0, 2, 0), (0xF0, 2, 1)):
	def _branch(c, m, mask=mask, val=val):
		off = c.fetch()
		if bool(c.p & mask) == bool(val):
			c.pc = (c.pc + off - (0x100 if off & 0x80 else 0)) & 0xFFFF
	define(code, 'rel', _branch)

STORE_MODES = {0x8D: ('a', 'abs'), 0x8E: ('x', 'abs'), 0x8C: ('y', 'abs'), 0x9D: ('a', 'abx'), 0x99: ('a', 'aby')}
RMW_ABS = {0x0E, 0x2E, 0x4E, 0x6E, 0xEE, 0xCE, 0x1E, 0x3E, 0x5E, 0x7E, 0xFE, 0xDE}


# --------------------------------------------------------------------------------------
# The tune

class Nsf:
	def __init__(self, path):
		d = open(path, 'rb').read()
		if d[:5] != b'NESM\x1a':
			sys.exit(f'{path}: not an NSF file')
		self.header = bytearray(d[:0x80])
		self.songs = d[6]
		self.load, self.init, self.play = struct.unpack_from('<HHH', d, 8)
		self.banks0 = list(d[0x70:0x78])
		self.banked = any(self.banks0)
		self.expansion = d[0x7B]
		self.data = d[0x80:]
		if d[5] >= 2 and struct.unpack_from('<I', d[0x7D:0x80] + b'\0')[0]:
			# NSF2: program data length, metadata after it
			self.data = self.data[:struct.unpack_from('<I', d[0x7D:0x80] + b'\0')[0]]
		if self.load < 0x8000:
			sys.exit(f'{path}: load address ${self.load:04X} is below $8000 (not supported)')
		if self.expansion & 0x04:
			sys.exit(f'{path}: FDS sound needs RAM at $6000-$DFFF (not supported)')
		self.pad = (self.load & 0xFFF) if self.banked else (self.load - 0x8000)
		img = bytes(self.pad) + self.data
		if not self.banked and len(img) > 0x8000:
			sys.exit(f'{path}: more than 32K of data without bank switching')
		self.nbanks = max(8 if not self.banked else 1, (len(img) + 0xFFF) // 0x1000)
		if self.nbanks > 255:
			sys.exit(f'{path}: too many banks')
		img = img.ljust(self.nbanks * 0x1000, b'\0')
		self.banks = [bytearray(img[i * 0x1000:(i + 1) * 0x1000]) for i in range(self.nbanks)]

	def initial(self):
		return self.banks0[:] if self.banked else list(range(8))

	def bank_byte(self, b, off):
		return self.banks[b][off] if b < self.nbanks else 0


class Run:
	"""Runs one song. `mode` 'trace' follows the NSF rules and records what the code
	does; 'bbk' follows NSFPLAY's rules for a patched tune."""

	def __init__(self, nsf, mode, extra=()):
		self.nsf = nsf
		self.mode = mode
		self.extra = set(extra)
		self.ram = bytearray(0x800)
		self.wram = bytearray(0x2000)
		self.cpu = Cpu(self.read, self.write)
		self.apu = []
		self.inst_pc = 0
		self.phase = 'init'
		# trace results (shared across songs by the caller)
		self.bank_sites = None
		self.stray_sites = None
		self.pairs = None
		self.stray_reads = None
		self.problems = None

	def reset(self, song):
		self.ram[:] = bytes(0x800)
		self.wram[:] = bytes(0x2000)
		self.slot = self.nsf.initial()
		self.apu = []
		if self.mode == 'bbk':
			self.window = [None] * 4
			for w in range(4):
				self.map_window(w)
		self.song = song

	# BBK runtime: MapWindow
	def map_window(self, w):
		lo, hi = self.slot[w * 2], self.slot[w * 2 + 1]
		pairs = (self.nsf.nbanks + 1) // 2
		if (lo & 1) == 0 and hi == lo + 1 and (lo >> 1) < pairs:
			self.window[w] = (lo, hi)
		elif (lo, hi) in self.extra:
			self.window[w] = (lo, hi)
		# else: left as it was

	def site(self):
		pc = self.inst_pc
		if pc >= 0x8000:
			slot = (pc - 0x8000) >> 12
			b = self.slot[slot] if self.mode == 'trace' else self.window_bank(pc)
			return ('bank', b, pc & 0xFFF, pc)
		return ('ram', None, pc, pc)

	def window_bank(self, addr):
		w = (addr - 0x8000) >> 13
		pair = self.window[w]
		if pair is None:
			raise Halt(f'window ${0x8000 + w * 0x2000:04X} not mapped at ${addr:04X}')
		return pair[(addr >> 12) & 1]

	def read(self, addr):
		if addr < 0x2000:
			return self.ram[addr & 0x7FF]
		if addr >= 0x8000:
			if self.mode == 'trace':
				slot = (addr - 0x8000) >> 12
				w = slot >> 1
				self.pairs[w].add((self.slot[w * 2], self.slot[w * 2 + 1]))
				return self.nsf.bank_byte(self.slot[slot], addr & 0xFFF)
			return self.nsf.bank_byte(self.window_bank(addr), addr & 0xFFF)
		if 0x6000 <= addr < 0x8000:
			return self.wram[addr - 0x6000]
		if addr == 0x4015:
			return 0
		if 0x4018 <= addr < 0x6000 and self.mode == 'trace':
			self.stray_reads[addr] = self.stray_reads.get(addr, 0) + 1
		return 0

	def write(self, addr, v):
		if addr < 0x2000:
			self.ram[addr & 0x7FF] = v
			return
		if 0x6000 <= addr < 0x8000:
			self.wram[addr - 0x6000] = v
			return
		if 0x4000 <= addr <= 0x4015:
			self.apu.append((addr, v))
			return
		if self.mode == 'bbk':
			if SINK <= addr <= SINK + 0xFF or 0x4018 <= addr < 0x4100:
				return
			if addr == 0x4017 and self.phase == 'init':
				return
			raise Halt(f'write ${addr:04X} from ${self.inst_pc:04X} reached the BBK')
		if addr == 0x4017 and self.phase == 'init':
			return
		site = self.site()
		if 0x5FF8 <= addr <= 0x5FFF and self.nsf.banked:
			self.slot[addr - 0x5FF8] = v
			self.bank_sites.setdefault(site, set()).add(addr)
			return
		if 0x4018 <= addr < 0x4100:
			return				# nothing there on the BBK
		self.stray_sites.setdefault(site, set()).add(addr)

	def stub(self):
		"""NSFPLAY's bank routines, entered by JSR"""
		c = self.cpu
		e = c.pc
		if e < STUB_SX:
			slot, val = (e - STUB_SA) // 3, c.a
		elif e < STUB_SY:
			slot, val = (e - STUB_SX) // 3, c.x
		elif e < STUB_SAX:
			slot, val = (e - STUB_SY) // 3, c.y
		elif e < STUB_SAY:
			slot, val = ((e - STUB_SAX) // 3 + c.x) & 0xFF, c.a
		else:
			slot, val = ((e - STUB_SAY) // 3 + c.y) & 0xFF, c.a
		if slot < 8:
			self.slot[slot] = val
			self.map_window(slot >> 1)
		_rts(c)

	def call(self, addr, a=0, x=0, limit=3_000_000):
		c = self.cpu
		self.ram[0x1FE] = self.ram[0x1FF] = 0xFF
		c.sp = 0xFD
		c.pc, c.a, c.x, c.y, c.p = addr, a, x, 0, 0x24
		n = 0
		while c.pc != 0:
			if self.mode == 'bbk' and STUB_SA <= c.pc < STUB_END:
				self.stub()
				continue
			self.inst_pc = c.pc
			c.step()
			n += 1
			if n > limit:
				raise Halt(f'no return from ${addr:04X} (at ${c.pc:04X})')

	def song_run(self, song, plays):
		self.reset(song)
		self.phase = 'init'
		for r in range(0x14):
			self.apu.append((0x4013 - r, 0))
		self.apu.append((0x4015, 0x0F))
		self.call(self.nsf.init, song, 0)
		self.phase = 'play'
		for _ in range(plays):
			self.call(self.nsf.play)
		return self.apu


def trace(nsf, plays):
	r = Run(nsf, 'trace')
	r.bank_sites, r.stray_sites, r.stray_reads = {}, {}, {}
	r.pairs = [set() for _ in range(4)]
	streams = []
	for s in range(nsf.songs):
		print(f'\r  tracing song {s + 1}/{nsf.songs}', end='', flush=True)
		try:
			streams.append(r.song_run(s, plays))
		except Halt as e:
			print(f'\n  song {s + 1}: {e}')
			streams.append(None)
	print()
	return r, streams


def stub_for(op, operand):
	reg, mode = STORE_MODES[op]
	k = operand - 0x5FF8
	if not 0 <= k <= 7:
		return None
	if mode == 'abs':
		return {'a': STUB_SA, 'x': STUB_SX, 'y': STUB_SY}[reg] + 3 * k
	return (STUB_SAX if mode == 'abx' else STUB_SAY) + 3 * k


def main():
	ap = argparse.ArgumentParser(description='Prepares an NSF for NSFPLAY.CMD on the BBK.')
	ap.add_argument('nsf')
	ap.add_argument('-o', '--output')
	ap.add_argument('--seconds', type=int, default=60, help='PLAY time traced per song (default 60)')
	ap.add_argument('--no-scan', action='store_true', help='patch only the bank stores the trace ran')
	args = ap.parse_args()

	nsf = Nsf(args.nsf)
	plays = args.seconds * 60
	chips = ', '.join(n for b, n in EXPANSION.items() if nsf.expansion & b) or 'none'
	print(f'{os.path.basename(args.nsf)}: {nsf.songs} songs, {nsf.nbanks} 4K banks, '
	      f'{"bank switching" if nsf.banked else "no bank switching"}, expansion sound: {chips}')

	r, original = trace(nsf, plays)
	errors = []
	patches = {}		# (bank, offset) -> new 3 bytes

	def patch(bank, off, new, why):
		if off > 0xFFD:
			errors.append(f'{why} at bank {bank} ${off:03X} crosses a bank boundary')
			return
		for o in range(off - 2, off + 3):
			if (bank, o) in patches and o != off:
				errors.append(f'{why} at bank {bank} ${off:03X} overlaps another patch')
				return
		patches[(bank, off)] = bytes(new)

	for (kind, bank, off, pc), addrs in sorted(r.bank_sites.items(), key=lambda t: (t[0][0], t[0][1] or 0, t[0][2])):
		if kind != 'bank':
			errors.append(f'bank switch from code in RAM at ${pc:04X}')
			continue
		op = nsf.bank_byte(bank, off)
		operand = nsf.bank_byte(bank, off + 1) | (nsf.bank_byte(bank, off + 2) << 8)
		target = stub_for(op, operand) if op in STORE_MODES else None
		if target is None:
			errors.append(f'bank switch by ${op:02X} ${operand:04X} at ${pc:04X} (bank {bank}) is not a form NSFPLAY handles')
			continue
		if any(not 0x5FF8 <= a <= 0x5FFF for a in addrs):
			errors.append(f'store at ${pc:04X} (bank {bank}) writes bank registers and other addresses')
			continue
		patch(bank, off, (0x20, target & 0xFF, target >> 8), 'bank switch')

	traced = len(patches)
	scanned = 0
	if nsf.banked and not args.no_scan:
		for b in range(nsf.nbanks):
			data = nsf.banks[b]
			for off in range(0xFFE):
				op = data[off]
				if op in STORE_MODES and data[off + 2] == 0x5F and data[off + 1] >= 0xF8 and (b, off) not in patches:
					target = stub_for(op, data[off + 1] | 0x5F00)
					if any((b, o) in patches for o in range(off - 2, off + 3)):
						continue
					patch(b, off, (0x20, target & 0xFF, target >> 8), 'bank switch')
					scanned += 1

	sinks = 0
	for (kind, bank, off, pc), addrs in sorted(r.stray_sites.items(), key=lambda t: (t[0][0], t[0][1] or 0, t[0][2])):
		harmful = [a for a in addrs if not (0x6000 <= a < 0x8000)]
		if not harmful:
			continue
		if kind != 'bank':
			errors.append(f'code in RAM at ${pc:04X} writes ${min(harmful):04X}')
			continue
		op = nsf.bank_byte(bank, off)
		mode = MODES.get(op)
		if mode not in ('abs', 'abx', 'aby') or op in (0x20, 0x4C):
			errors.append(f'write to ${min(harmful):04X} by ${op:02X} at ${pc:04X} (bank {bank}) cannot be redirected')
			continue
		if any(0x4000 <= a <= 0x4015 for a in addrs):
			errors.append(f'store at ${pc:04X} (bank {bank}) writes both sound registers and ${min(harmful):04X}')
			continue
		patch(bank, off, (op, SINK & 0xFF, SINK >> 8), 'expansion write')
		sinks += 1

	reads = sorted(a for a in r.stray_reads if 0x4100 <= a < 0x6000)
	if reads:
		print(f'  warning: the tune reads ${reads[0]:04X}' + (f' and {len(reads) - 1} more addresses' if len(reads) > 1 else '')
		      + ' (expansion registers); on the BBK it reads memory there instead')

	extra = set()
	for w in range(4):
		for lo, hi in r.pairs[w]:
			if not ((lo & 1) == 0 and hi == lo + 1 and (lo >> 1) < (nsf.nbanks + 1) // 2):
				extra.add((lo, hi))
	extra = sorted(extra)
	if len(extra) > MAXEXTRA:
		errors.append(f'{len(extra)} unaligned bank pairs (NSFPLAY holds {MAXEXTRA})')

	print(f'  bank switches patched: {traced} traced' + (f', {scanned} more found in the code' if scanned else ''))
	if sinks:
		print(f'  expansion writes redirected: {sinks}')
	print(f'  bank pairs NSFPLAY puts together: {len(extra)}' + (' (' + ' '.join(f'{a}/{b}' for a, b in extra) + ')' if extra else ''))
	if errors:
		for e in errors:
			print('  error: ' + e)
		sys.exit('not converted')

	for (b, off), new in patches.items():
		nsf.banks[b][off:off + 3] = new

	# Check: the patched tune under NSFPLAY's rules writes the same sound registers
	bad = 0
	v = Run(nsf, 'bbk', extra)
	for s in range(nsf.songs):
		print(f'\r  checking song {s + 1}/{nsf.songs}', end='', flush=True)
		if original[s] is None:
			continue
		try:
			got = v.song_run(s, plays)
		except Halt as e:
			print(f'\n  song {s + 1}: {e}')
			bad += 1
			continue
		if got != original[s]:
			n = next((i for i, (a, b) in enumerate(zip(got, original[s])) if a != b), min(len(got), len(original[s])))
			print(f'\n  song {s + 1}: sound differs from write {n}')
			bad += 1
	print()
	if bad:
		sys.exit(f'not converted: {bad} songs differ')

	img = b''.join(nsf.banks)
	data = img[nsf.pad:nsf.pad + len(nsf.data)]
	block = bytearray(128)
	block[0:5] = b'BBKN\x01'
	block[5] = len(extra)
	for i, (lo, hi) in enumerate(extra):
		block[8 + i] = lo
		block[8 + MAXEXTRA + i] = hi
	header = bytearray(nsf.header)
	header[0x7C:0x80] = bytes(4)		# no NSF2 metadata after the data
	out = args.output
	if not out:
		stem = re.sub(r'[^A-Za-z0-9_-]', '', os.path.splitext(os.path.basename(args.nsf))[0]).upper()[:8] or 'TUNE'
		out = os.path.join(os.path.dirname(os.path.abspath(args.nsf)), stem + '.NSB')
	with open(out, 'wb') as f:
		f.write(header + block + data)
	print(f'  all {nsf.songs} songs check out; wrote {out}')


if __name__ == '__main__':
	main()
