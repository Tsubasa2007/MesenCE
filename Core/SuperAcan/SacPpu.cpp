#include "pch.h"
#include "SuperAcan/SacPpu.h"

void SacPpu::Init(uint8_t* vram, uint8_t* paletteRam, uint16_t* regs, uint16_t* frameBuffer)
{
	_vram = vram;
	_paletteRam = paletteRam;
	_regs = regs;
	_frameBuffer = frameBuffer;
}

uint16_t SacPpu::VramWord(uint32_t wordIndex)
{
	wordIndex &= 0xFFFF;
	return (uint16_t)((_vram[wordIndex * 2] << 8) | _vram[wordIndex * 2 + 1]);
}

//Bit 8 of the video flags selects 320 pixels across instead of 256; bit 9 shows lines 8-231
//rather than 0-239
uint32_t SacPpu::GetScreenWidth()
{
	return (Reg(0x08) & 0x0100) ? 320 : 256;
}

uint32_t SacPpu::GetScreenHeight()
{
	return (Reg(0x08) & 0x0200) ? 224 : 240;
}

uint32_t SacPpu::GetFirstLine()
{
	return (Reg(0x08) & 0x0200) ? 8 : 0;
}

//Which tile format a layer uses: 0 = 8bpp, 1 = 4bpp, 2 = 2bpp, 4 = 1bpp. The first two
//tilemaps follow the global graphics mode at $F001F0, the third is always 2bpp, and the
//rotate/zoom layer has a depth of its own.
int SacPpu::GetTilemapRegion(int layer)
{
	uint8_t gfxMode = Reg(0x1F0) & 0x07;
	switch(layer) {
		case 0: {
			static constexpr int layer0Mode[8] = { 2, 1, 0, 1, 0, 0, 0, 0 };
			return layer0Mode[gfxMode];
		}

		case 1: {
			static constexpr int layer1Mode[8] = { 2, 1, 1, 1, 2, 2, 2, 2 };
			return layer1Mode[gfxMode];
		}

		case 2:
			return 2;

		default: {
			static constexpr int rozMode[4] = { 4, 2, 1, 0 };
			return rozMode[RozMode() & 0x03];
		}
	}
}

//Bits 8-11 of a layer's flags give its size in 8x8 tiles
void SacPpu::GetTilemapDimensions(int layer, int& xsize, int& ysize)
{
	uint16_t select = (layer == 3 ? RozMode() : TilemapFlags(layer)) & 0x0F00;
	switch(select) {
		case 0x200: xsize = 16; ysize = 16; break;
		case 0x600: xsize = 64; ysize = 32; break;
		case 0xA00: xsize = 128; ysize = 32; break;
		case 0xC00: xsize = 64; ysize = 64; break;
		default: xsize = 32; ysize = 32; break;
	}
}

//One pixel of an 8x8 tile in video RAM. Pixels are packed most significant first. The 1bpp
//format, used only by the rotate/zoom layer's first mode, reads video RAM through the address
//swap MAME applies to it (bits 0-2 and 3-6 of the byte address trade places).
uint8_t SacPpu::GetTilePixel(int region, uint32_t tile, int x, int y)
{
	switch(region) {
		case 0:
			tile %= 0x800;
			return _vram[(tile * 64 + y * 8 + x) & 0x1FFFF];

		case 1: {
			tile %= 0x1000;
			uint8_t value = _vram[(tile * 32 + y * 4 + (x >> 1)) & 0x1FFFF];
			return (x & 1) ? (value & 0x0F) : (value >> 4);
		}

		case 2: {
			tile %= 0x2000;
			uint8_t value = _vram[(tile * 16 + y * 2 + (x >> 2)) & 0x1FFFF];
			return (value >> (6 - (x & 3) * 2)) & 0x03;
		}

		case 4: {
			tile %= 0x4000;
			uint32_t swapped = tile * 8 + y;
			uint32_t addr = (swapped & ~0x7F) | ((swapped >> 4) & 0x07) | ((swapped & 0x0F) << 3);
			return (_vram[addr & 0x1FFFF] >> (7 - x)) & 0x01;
		}

		default:
			return 0;
	}
}

//A palette index: the tile's palette picks a block the size of its depth
uint16_t SacPpu::GetColorIndex(int region, uint32_t palette, uint8_t pixel)
{
	switch(region) {
		case 0: return pixel;
		case 1: return (uint16_t)((palette % 0x10) * 16 + pixel);
		case 2: return (uint16_t)((palette % 0x40) * 4 + pixel);
		default: return (uint16_t)((palette % 0x80) * 2 + pixel);
	}
}

//A pixel of a whole layer, at a position inside it. A layer flipped as a whole shows its
//tiles mirrored and each one flipped, which is sampling the mirrored position.
uint16_t SacPpu::SampleTilemap(int layer, int region, int x, int y, int xsize, int ysize)
{
	if(layer != 3) {
		uint16_t flags = TilemapFlags(layer);
		if(flags & 0x02) {
			x = xsize * 8 - 1 - x;
		}
		if(flags & 0x01) {
			y = ysize * 8 - 1 - y;
		}
	}

	uint32_t count = (uint32_t)((y >> 3) * xsize + (x >> 3));
	uint32_t tile = 0;
	uint32_t palette = 0;
	bool xflip = false;
	bool yflip = false;

	if(layer == 3 && (RozMode() & 0x03) == 0) {
		//MAME's reading of the one mode only the boot logo uses: a single 64x64 tile it
		//rearranges as 8x8 ones
		tile = 0x880 + ((count & 7) * 2);
		if(count & 0x20) {
			tile ^= 1;
		}
		tile |= (count & 0xC0) >> 2;
	} else {
		uint32_t base;
		uint32_t tileBank;
		uint16_t tileMode;
		if(layer == 3) {
			base = (uint32_t)Reg(0x194) << 1;
			tileBank = (Reg(0x196) & 0xF000) >> 3;
			tileMode = Reg(0x182);
		} else {
			base = (uint32_t)Reg(0x108 + layer * 0x20) << 1;
			uint32_t gfxMode = (TilemapMode(layer) & 0x7000) >> 12;
			tileBank = gfxMode << (8 + region);
			tileMode = TilemapTileMode(layer);
		}

		uint16_t entry = VramWord(base + count);
		uint32_t paletteBase = entry >> 12;
		if(tileMode & 0x0200) {
			paletteBase |= 8;
		}

		//Bit 11 flips a tile horizontally and bit 10 vertically, as in a sprite entry. MAME's
		//tilemaps have the two the other way round, but a picture drawn as its left half and a
		//mirror of it has the right half's entries with bit 11 set.
		tile = (entry & 0x03FF) + tileBank;
		xflip = (entry & 0x0800) != 0;
		yflip = (entry & 0x0400) != 0;
		//The 2bpp text layer steps its palettes by four
		palette = (layer != 3 && region == 2) ? (paletteBase << 2) : paletteBase;
	}

	int tileX = x & 7;
	int tileY = y & 7;
	if(xflip) {
		tileX = 7 - tileX;
	}
	if(yflip) {
		tileY = 7 - tileY;
	}
	return GetColorIndex(region, palette, GetTilePixel(region, tile, tileX, tileY));
}

// Sprite table entry, four words:
// [0] -e-- ---- ---- ----  enable
//     ---h hhh- ---- ----  height (through a table)
//     ---- ---y yyyy yyyy  Y position
// [1] bbbb ---- ---- ----  tile bank
//     ---- h--- ---- ----  horizontal flip
//     ---- -v-- ---- ----  vertical flip
//     ---- --mm ---- ----  mask mode
//     ---- ---- ---- -www  width, 1 << www tiles
// [2] zzz- ---- ---- ----  X scale (not emulated, as in MAME)
//     ---- -pp- ---- ----  priority
//     ---- ---x xxxx xxxx  X position
// [3] d--- ---- ---- ----  direct: a single tile described here rather than a table in video RAM
//     -ooo oooo oooo oooo  address of the tile table
void SacPpu::DrawSpritesLine(int y, int width)
{
	//Heights are the value plus one, except at the top of the range
	static constexpr int ySizes[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 22, 24, 26 };

	uint32_t spriteCount = (uint32_t)Reg(0x22) + 1;
	uint32_t startWord = ((uint32_t)Reg(0x20) << 2) >> 1;
	//8bpp or 4bpp, and 8bpp sprites address tiles in banks of half the size
	int region = (Reg(0x26) & 0x01) ? 0 : 1;
	uint32_t bankSize = 0x100u << region;

	for(uint32_t i = 0; i < spriteCount; i++) {
		uint32_t entry = startWord + i * 4;
		uint16_t w0 = VramWord(entry);
		uint16_t w1 = VramWord(entry + 1);
		uint16_t w2 = VramWord(entry + 2);
		uint16_t spritePtr = VramWord(entry + 3);

		if(!(w0 & 0x4000) || !spritePtr) {
			continue;
		}

		int x = w2 & 0x1FF;
		int spriteY = w0 & 0x1FF;
		if(spriteY >= 0x180) {
			spriteY -= 0x200;
		}
		if(x >= 0x180) {
			x -= 0x200;
		}

		uint32_t bank = w1 >> 12;
		int mask = (w1 >> 8) & 0x03;
		bool xflip = (w1 & 0x0800) != 0;
		bool yflip = (w1 & 0x0400) != 0;
		int priority = (w2 >> 9) & 0x03;
		int xsize = 1 << (w1 & 0x07);
		int ysize = ySizes[(w0 >> 9) & 0x0F];

		//Nothing of this sprite on this line
		if(y < spriteY || y >= spriteY + ysize * 8) {
			continue;
		}

		if((spritePtr & 0x8000) || (xsize == 1 && ysize == 1)) {
			//A single tile described by the entry itself
			uint32_t tile = bank * bankSize + (spritePtr & 0x03FF);
			uint32_t palette = spritePtr >> 12;
			bool tileXFlip = xflip ^ ((spritePtr & 0x0800) != 0);
			bool tileYFlip = yflip ^ ((spritePtr & 0x0400) != 0);
			if(y < spriteY + 8) {
				DrawSpriteTileLine(y, width, region, tile, palette, tileXFlip, tileYFlip, x, spriteY, mask, priority);
			}
		} else {
			for(int ytile = 0; ytile < ysize; ytile++) {
				int ypos = yflip ? (spriteY - (ytile + 1) * 8 + ysize * 8) : (spriteY + ytile * 8);
				if(y < ypos || y >= ypos + 8) {
					continue;
				}

				for(int xtile = 0; xtile < xsize; xtile++) {
					uint16_t data = VramWord(((uint32_t)spritePtr << 1) + ytile * xsize + xtile);
					//An empty entry draws nothing
					if(data == 0) {
						continue;
					}

					uint32_t tile = bank * bankSize + (data & 0x03FF);
					uint32_t palette = data >> 12;
					int xpos = xflip ? (x - (xtile + 1) * 8 + xsize * 8) : (x + xtile * 8);
					//MAME wraps a tile at the 512 pixel edge
					xpos &= 0x1FF;

					bool tileXFlip = xflip ^ ((data & 0x0800) != 0);
					bool tileYFlip = yflip ^ ((data & 0x0400) != 0);
					DrawSpriteTileLine(y, width, region, tile, palette, tileXFlip, tileYFlip, xpos, ypos, mask, priority);
				}
			}
		}
	}
}

//One line of one 8x8 sprite tile. Mask mode 2-3 only marks where the tile has pixels, mode 1
//draws only where an earlier sprite marked, mode 0 draws normally.
void SacPpu::DrawSpriteTileLine(int y, int width, int region, uint32_t tile, uint32_t palette, bool xflip, bool yflip, int x, int tileY, int mask, int priority)
{
	int row = y - tileY;
	if(yflip) {
		row = 7 - row;
	}

	for(int px = 0; px < 8; px++) {
		int screenX = x + px;
		if(screenX < 0 || screenX >= width) {
			continue;
		}

		uint8_t pixel = GetTilePixel(region, tile, xflip ? 7 - px : px, row);
		if(pixel == 0) {
			continue;
		}

		if(mask > 1) {
			_lineSpriteMask[screenX] = 1;
		} else if(mask == 0 || _lineSpriteMask[screenX]) {
			_lineSprite[screenX] = GetColorIndex(region, palette, pixel);
			_linePriority[screenX] = (uint8_t)((_linePriority[screenX] & 0xF0) | priority);
		}
	}
}

void SacPpu::DrawTilemapLine(int layer, int y, int width, int layerPriority, int region, int transMask)
{
	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(layer, xsize, ysize);
	int layerWidth = xsize * 8;
	int layerHeight = ysize * 8;

	uint16_t flags = TilemapFlags(layer);
	uint16_t tileMode = TilemapTileMode(layer);
	bool wrap = (flags & 0x20) != 0;

	//12-bit signed scroll values
	int scrollX = Reg(0x104 + layer * 0x20) & 0xFFF;
	int scrollY = Reg(0x106 + layer * 0x20) & 0xFFF;
	if(scrollX & 0x800) {
		scrollX -= 0x1000;
	}
	if(scrollY & 0x800) {
		scrollY -= 0x1000;
	}

	//Flipping the layer also turns its scroll around
	if(flags & 0x02) {
		scrollX ^= layerWidth - 1;
	}
	if(flags & 0x01) {
		scrollY ^= layerHeight - 1;
	}

	int mosaic = (flags & 0x001C) >> 2;
	int mosaicMask = (int)(0xFFFFFFFF << mosaic);

	int actualY = y & mosaicMask;
	int realY = actualY + scrollY;
	if(!wrap && (scrollY + y < 0 || scrollY + y > layerHeight - 1)) {
		return;
	}

	//Line select: each screen line names the layer line it shows
	if(tileMode & 0x0800) {
		int16_t lineSelect = (int16_t)VramWord(((uint32_t)Reg(0x10E + layer * 0x20) << 1) + y);
		realY = (lineSelect + scrollY) & (layerHeight - 1);
	}
	realY &= layerHeight - 1;

	//Line scroll: a horizontal offset per screen line
	int lineScrollX = scrollX;
	if(tileMode & 0x4000) {
		lineScrollX += (int16_t)VramWord(((uint32_t)Reg(0x10C + layer * 0x20) << 1) + y);
	}

	for(int x = 0; x < width; x++) {
		if(!wrap && (lineScrollX + x < 0 || lineScrollX + x > layerWidth - 1)) {
			continue;
		}

		int realX = ((x & mosaicMask) + lineScrollX) & (layerWidth - 1);
		uint16_t pixel = SampleTilemap(layer, region, realX, realY, xsize, ysize);
		if((pixel & transMask) != 0 && layerPriority < (_linePriority[x] >> 4)) {
			_lineColor[x] = pixel;
			_linePriority[x] = (uint8_t)((_linePriority[x] & 0x0F) | (layerPriority << 4));
		}
	}
}

//The rotate/zoom layer, in 16.16 fixed point. Like MAME it neither checks nor sets priorities.
void SacPpu::DrawRozPixels(int y, int width, int region, int transMask, uint32_t startX, uint32_t startY, int incXX, int incXY, int incYX, int incYY, bool wrap)
{
	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(3, xsize, ysize);
	int layerWidth = xsize * 8;
	int layerHeight = ysize * 8;
	uint32_t widthShifted = (uint32_t)layerWidth << 16;
	uint32_t heightShifted = (uint32_t)layerHeight << 16;

	uint32_t cx = startX + (uint32_t)(y * incYX);
	uint32_t cy = startY + (uint32_t)(y * incYY);
	for(int x = 0; x < width; x++) {
		if(wrap || (cx < widthShifted && cy < heightShifted)) {
			uint16_t pixel = SampleTilemap(3, region, (int)(cx >> 16) & (layerWidth - 1), (int)(cy >> 16) & (layerHeight - 1), xsize, ysize);
			if((pixel & transMask) != 0) {
				_lineColor[x] = pixel;
			}
		}
		cx += (uint32_t)incXX;
		cy += (uint32_t)incXY;
	}
}

void SacPpu::DrawRozLine(int y, int width, int region, int transMask)
{
	uint16_t rozMode = RozMode();
	bool wrap = (rozMode & 0x20) != 0;

	auto signExtend = [](int value) { return (value & 0x8000) ? value - 0x10000 : value; };
	int incXX = signExtend(Reg(0x18C));
	int incYX = signExtend(Reg(0x18E));
	int incXY = signExtend(Reg(0x190));
	int incYY = signExtend(Reg(0x192));
	uint32_t scrollX = ((uint32_t)Reg(0x184) << 16) | Reg(0x186);
	uint32_t scrollY = ((uint32_t)Reg(0x188) << 16) | Reg(0x18A);

	if(!(rozMode & 0x0200) && (rozMode & 0xF000)) {
		//MAME's reading of the per-line tables some intros use (it calls it untrusted): a zoom
		//step and a scroll position for each line, with a zero step meaning the line is not drawn
		uint32_t base0 = ((uint32_t)Reg(0x198) << 2) >> 1;
		uint32_t base1 = ((uint32_t)Reg(0x19A) << 2) >> 1;
		uint32_t base2 = ((uint32_t)Reg(0x19E) << 2) >> 1;

		uint16_t lineStep = VramWord(base0 + y);
		if(!lineStep) {
			return;
		}
		int lineIncXX = signExtend((Reg(0x18C) + lineStep) & 0xFFFF);
		uint32_t lineScrollX = scrollX + ((uint32_t)VramWord(base1 + y * 2) << 16) + VramWord(base1 + y * 2 + 1);
		uint32_t lineScrollY = scrollY + ((uint32_t)VramWord(base2 + y * 2) << 16) + VramWord(base2 + y * 2 + 1);
		DrawRozPixels(y, width, region, transMask, lineScrollX << 8, lineScrollY << 8, lineIncXX << 8, incXY << 8, incYX << 8, incYY << 8, wrap);
	} else {
		DrawRozPixels(y, width, region, transMask, scrollX << 8, scrollY << 8, incXX << 8, incXY << 8, incYX << 8, incYY << 8, wrap);
	}
}

//The window fills a span of each line - from a table of left and right edges in video RAM -
//with one colour, at its own priority. Bit 11 reverses it to fill outside the span.
void SacPpu::DrawWindowLine(int priority, int y, int width)
{
	uint16_t control = Reg(0x1D0);
	int windowPriority = (control >> 13) & 0x03;
	if(priority != windowPriority) {
		return;
	}

	bool reverse = (control & 0x0800) != 0;
	int scrollX = Reg(0x1D4) & 0x3FF;
	if(scrollX & 0x200) {
		scrollX -= 0x400;
	}
	uint8_t pen = control & 0xFF;

	int yBase = (control & 0x0100) ? y * 2 : 0;
	uint32_t clipBase = (((uint32_t)Reg(0x1D2) << 1) + yBase) & 0xFFFF;
	int16_t clipMin = (int16_t)(VramWord(clipBase) + scrollX);
	int16_t clipMax = (int16_t)(VramWord(clipBase + 1) + scrollX);

	for(int x = 0; x < width; x++) {
		if(windowPriority >= (_linePriority[x] >> 4)) {
			continue;
		}
		if((x >= clipMin && x < clipMax) != reverse) {
			_lineColor[x] = pen;
			_linePriority[x] = (uint8_t)((_linePriority[x] & 0x0F) | (windowPriority << 4));
		}
	}
}

void SacPpu::RenderLine(uint32_t line)
{
	uint32_t firstLine = GetFirstLine();
	uint32_t height = GetScreenHeight();
	if(line < firstLine || line >= firstLine + height) {
		return;
	}

	int y = (int)line;
	int width = (int)GetScreenWidth();

	for(int x = 0; x < width; x++) {
		_lineColor[x] = 0;
		_linePriority[x] = 0xFF;
		_lineSprite[x] = 0;
		_lineSpriteMask[x] = 0;
	}

	DrawSpritesLine(y, width);

	uint16_t videoFlags = Reg(0x08);
	for(int priority = 7; priority >= 0; priority--) {
		for(int layer = 0; layer < 4; layer++) {
			bool enabled = layer == 3 ? (videoFlags & 0x04) != 0 : (videoFlags & (0x80 >> layer)) != 0;
			if(!enabled) {
				continue;
			}

			int layerPriority = ((layer == 3 ? RozMode() : TilemapFlags(layer)) >> 13) & 0x07;
			if(layerPriority != priority) {
				continue;
			}

			int region = GetTilemapRegion(layer);
			int transMask = region == 0 ? 0xFF : (region == 1 ? 0x0F : (region == 2 ? 0x03 : 0x01));
			if(layer == 3) {
				DrawRozLine(y, width, region, transMask);
			} else {
				DrawTilemapLine(layer, y, width, layerPriority, region, transMask);
			}
		}

		if(videoFlags & 0x02) {
			DrawWindowLine(priority, y, width);
		}
	}

	//Sprites go over any layer whose priority is not above theirs
	if(videoFlags & 0x08) {
		for(int x = 0; x < width; x++) {
			if(_lineSprite[x] != 0 && (_linePriority[x] & 0x0F) <= (_linePriority[x] >> 4)) {
				_lineColor[x] = _lineSprite[x];
			}
		}
	}

	//Through the palette as it stands now, so a palette changed mid-frame shows where it changed
	uint16_t* out = _frameBuffer + (line - firstLine) * width;
	for(int x = 0; x < width; x++) {
		uint16_t index = _lineColor[x] & 0xFF;
		out[x] = (uint16_t)(((_paletteRam[index * 2] << 8) | _paletteRam[index * 2 + 1]) & 0x7FFF);
	}
}
