"""Copy files into the root directory of a FAT12 floppy image (BBGDOS disks).

usage: fat12add.py <image> <file> [<file> ...] [--as NAME.EXT]

Writes into the image in place - run it on a copy. An existing entry with the same
name is replaced (its clusters are freed first).
"""
import os
import struct
import sys


def main():
	args = sys.argv[1:]
	rename = None
	if '--as' in args:
		i = args.index('--as')
		rename = args[i + 1].upper()
		del args[i:i + 2]
	image, files = args[0], args[1:]
	if rename and len(files) != 1:
		sys.exit('--as needs exactly one file')

	d = bytearray(open(image, 'rb').read())
	bps, spc, res, nfats, rootEntries, total, _, spf = struct.unpack_from('<HBHBHHBH', d, 11)
	fatOff = res * bps
	rootOff = (res + nfats * spf) * bps
	dataOff = rootOff + rootEntries * 32
	clusterSize = bps * spc
	clusters = (total * bps - dataOff) // clusterSize + 2

	def getFat(c):
		v = struct.unpack_from('<H', d, fatOff + c * 3 // 2)[0]
		return v >> 4 if c & 1 else v & 0xFFF

	def setFat(c, value):
		for f in range(nfats):
			o = fatOff + f * spf * bps + c * 3 // 2
			v = struct.unpack_from('<H', d, o)[0]
			v = (v & 0x000F) | (value << 4) if c & 1 else (v & 0xF000) | value
			struct.pack_into('<H', d, o, v)

	for path in files:
		name = rename or os.path.basename(path).upper()
		base, _, ext = name.partition('.')
		entryName = base.ljust(8)[:8].encode('ascii') + ext.ljust(3)[:3].encode('ascii')
		data = open(path, 'rb').read()

		slot = None
		for i in range(rootEntries):
			o = rootOff + i * 32
			if d[o] not in (0, 0xE5) and d[o + 11] & 0x18 == 0 and d[o:o + 11] == entryName:
				c = struct.unpack_from('<H', d, o + 26)[0]
				while 2 <= c < 0xFF8:
					n = getFat(c)
					setFat(c, 0)
					c = n
				slot = o
				break
		if slot is None:
			for i in range(rootEntries):
				o = rootOff + i * 32
				if d[o] in (0, 0xE5):
					slot = o
					break
		if slot is None:
			sys.exit('root directory is full')

		need = max(1, -(-len(data) // clusterSize))
		free = [c for c in range(2, clusters) if getFat(c) == 0][:need]
		if len(free) < need:
			sys.exit('disk is full')
		for i, c in enumerate(free):
			setFat(c, free[i + 1] if i + 1 < need else 0xFFF)
			chunk = data[i * clusterSize:(i + 1) * clusterSize]
			o = dataOff + (c - 2) * clusterSize
			d[o:o + clusterSize] = chunk.ljust(clusterSize, b'\0')

		entry = bytearray(32)
		entry[0:11] = entryName
		entry[11] = 0x20
		struct.pack_into('<HHHI', entry, 22, 0, (2026 - 1980) << 9 | 10 << 5 | 13, free[0], len(data))
		d[slot:slot + 32] = entry
		print(f'{name}: {len(data)} bytes, clusters {free[0]}..{free[-1]}')

	open(image, 'wb').write(d)


main()
