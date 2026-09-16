#include "pch.h"
#include "SuperAcan/SacPpu.h"

//Development aid, like SAC_DUMP_FRAMES: SAC_PERF=1 skips the rotate/zoom layer and =3 skips all
//rendering, to time each stage; =5 reads the rotate/zoom per-line tables and sprite positions
//exactly as MAME does, to compare against. Read once, and never from inside a per-pixel loop.
static int SacPerfMode()
{
	static const int mode = [] {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
		const char* value = std::getenv("SAC_PERF");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
		return value ? atoi(value) : 0;
	}();
	return mode;
}

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

//The rotate/zoom layer draws a picture rather than a map of tiles when it is at its deepest setting
//and has been given no map to read - the same game uses both, a map for its title screen and a
//picture for the scene after it.
bool SacPpu::IsRozBitmap()
{
	return (RozMode() & 0x03) == 3 && Reg(0x194) == 0 && Reg(0x196) != 0;
}

//A pixel of a whole layer, at a position inside it. A layer flipped as a whole shows its
//tiles mirrored and each one flipped, which is sampling the mirrored position.
uint16_t SacPpu::SampleTilemap(int layer, int region, int x, int y, int xsize, int ysize, bool* aboveSprites)
{
	if(layer == 3 && IsRozBitmap()) {
		//No map to read, so the layer is a plain 8bpp picture instead: one byte a pixel, laid out
		//row by row from where the bank register points. Read as tiles - which is what MAME does -
		//it comes out as scrambled blocks. The per-line scroll tables are what mirror it, which is
		//how one game draws a scene reflected in water.
		uint32_t base = (uint32_t)((Reg(0x196) & 0xF000) >> 4) * 64;
		return _vram[(base + (uint32_t)y * (uint32_t)(xsize * 8) + (uint32_t)x) & 0x1FFFF];
	}

	if(layer != 3) {
		uint16_t flags = TilemapFlags(layer);
		if(flags & 0x02) {
			x = xsize * 8 - 1 - x;
		}
		if(flags & 0x01) {
			y = ysize * 8 - 1 - y;
		}
	}

	SacPpu::LayerTileInfo info = DecodeTile(layer, region, (uint32_t)((y >> 3) * xsize + (x >> 3)));
	if(aboveSprites) {
		*aboveSprites = info.AboveSprites;
	}

	int tileX = x & 7;
	int tileY = y & 7;
	if(info.XFlip) {
		tileX = 7 - tileX;
	}
	if(info.YFlip) {
		tileY = 7 - tileY;
	}
	return GetColorIndex(region, info.Palette, GetTilePixel(region, info.Tile, tileX, tileY));
}

//The entry at a position in a layer's map (row by row), and the tile it names
SacPpu::LayerTileInfo SacPpu::DecodeTile(int layer, int region, uint32_t count)
{
	LayerTileInfo info;

	if(layer == 3 && (RozMode() & 0x03) == 0) {
		//MAME's reading of the one mode only the boot logo uses: a single 64x64 tile it
		//rearranges as 8x8 ones
		uint32_t tile = 0x880 + ((count & 7) * 2);
		if(count & 0x20) {
			tile ^= 1;
		}
		info.Tile = tile | ((count & 0xC0) >> 2);
	} else {
		uint32_t base;
		uint32_t tileBank;
		uint16_t tileMode;
		if(layer == 3) {
			base = (uint32_t)Reg(0x194) << 1;
			//The bank is a place in video RAM, the same bytes whatever the depth, so it counts half as
			//many tiles for each doubling of their size: 8bpp tiles (64 bytes) shift by 4, 4bpp by 3
			//and 2bpp by 2. Shifting by 3 regardless lands 8bpp one whole set of tiles along, which is
			//no move at all once the tile number wraps, and 2bpp half way to its tiles - where one
			//title's lightning silhouette reads every tile as a single column of pixels.
			tileBank = (Reg(0x196) & 0xF000) >> (region == 0 ? 4 : (region == 2 ? 2 : 3));
			tileMode = Reg(0x182);
		} else {
			base = (uint32_t)Reg(0x108 + layer * 0x20) << 1;
			uint32_t gfxMode = (TilemapMode(layer) & 0x7000) >> 12;
			tileBank = gfxMode << (8 + region);
			tileMode = TilemapTileMode(layer);
		}

		uint32_t wordIndex = (base + count) & 0xFFFF;
		uint16_t entry = VramWord(wordIndex);
		uint32_t paletteBase = entry >> 12;
		//Bit 9 of the tile mode takes the palette's top bit for a priority instead: the tile uses the
		//upper eight palettes either way, and one whose bit is clear goes in front of sprites of its
		//layer's own priority rather than behind them. MAME keeps it as a tile category it never
		//reads. One title screen puts the lower edge of its cloud layer over a band of cloud sprites
		//and another its grass over the foot of its mountain sprites, as Bcan draws both; the
		//tiles around them, with the bit set, stay behind the sprites.
		if(tileMode & 0x0200) {
			info.AboveSprites = (paletteBase & 8) == 0;
			paletteBase |= 8;
		}

		info.MapAddress = wordIndex * 2;
		info.Entry = entry;

		//Bit 11 flips a tile horizontally and bit 10 vertically, as in a sprite entry. MAME's
		//tilemaps have the two the other way round, but a picture drawn as its left half and a
		//mirror of it has the right half's entries with bit 11 set.
		info.Tile = (entry & 0x03FF) + tileBank;
		info.XFlip = (entry & 0x0800) != 0;
		info.YFlip = (entry & 0x0400) != 0;
		//The 2bpp text layer steps its palettes by four
		info.Palette = (layer != 3 && region == 2) ? (paletteBase << 2) : paletteBase;
	}

	info.ColorBase = GetColorIndex(region, info.Palette, 0);
	return info;
}

void SacPpu::GetLayerSize(int layer, int& width, int& height)
{
	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(layer, xsize, ysize);
	width = xsize * 8;
	height = ysize * 8;
}

//A pixel as the layer would show it before scrolling, and whether it is transparent
uint16_t SacPpu::GetLayerPixel(int layer, int x, int y, bool& transparent)
{
	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(layer, xsize, ysize);
	int region = GetTilemapRegion(layer);
	int transMask = region == 0 ? 0xFF : (region == 1 ? 0x0F : (region == 2 ? 0x03 : 0x01));

	uint16_t pixel = SampleTilemap(layer, region, x, y, xsize, ysize);
	transparent = (pixel & transMask) == 0;
	return pixel;
}

uint32_t SacPpu::GetSpriteCount()
{
	return (uint32_t)Reg(0x22) + 1;
}

//The same reading of an entry as DrawSpritesLine's. A direct sprite is a single 8x8 tile.
SacPpu::SpriteViewInfo SacPpu::GetSprite(uint32_t index)
{
	static constexpr int ySizes[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 22, 24, 26 };

	SpriteViewInfo sprite;
	uint32_t entry = (((uint32_t)Reg(0x20) << 2) >> 1) + index * 4;
	uint16_t w0 = VramWord(entry);
	uint16_t w1 = VramWord(entry + 1);
	uint16_t w2 = VramWord(entry + 2);
	sprite.Pointer = VramWord(entry + 3);

	sprite.Enabled = sprite.Pointer != 0;
	sprite.RawY = w0 & 0x1FF;
	sprite.RawX = w2 & 0x1FF;
	sprite.Y = sprite.RawY >= 0x180 ? sprite.RawY - 0x200 : sprite.RawY;
	sprite.X = sprite.RawX >= 0x180 ? sprite.RawX - 0x200 : sprite.RawX;

	sprite.Bank = w1 >> 12;
	sprite.Mask = (w1 >> 8) & 0x03;
	sprite.XFlip = (w1 & 0x0800) != 0;
	sprite.YFlip = (w1 & 0x0400) != 0;
	sprite.Priority = (w2 >> 9) & 0x03;
	sprite.WidthTiles = 1 << (w1 & 0x07);
	sprite.HeightTiles = ySizes[(w0 >> 9) & 0x0F];
	sprite.Region = (Reg(0x26) & 0x01) ? 0 : 1;

	uint32_t bankSize = 0x100u << sprite.Region;
	sprite.Direct = (sprite.Pointer & 0x8000) || (sprite.WidthTiles == 1 && sprite.HeightTiles == 1);
	if(sprite.Direct) {
		sprite.WidthTiles = 1;
		sprite.HeightTiles = 1;
		sprite.FirstTile = sprite.Bank * bankSize + (sprite.Pointer & 0x03FF);
		sprite.Palette = sprite.Pointer >> 12;
	} else {
		uint16_t data = VramWord((uint32_t)sprite.Pointer << 1);
		sprite.FirstTile = sprite.Bank * bankSize + (data & 0x03FF);
		sprite.Palette = data >> 12;
	}
	return sprite;
}

uint16_t SacPpu::GetSpritePixel(const SpriteViewInfo& sprite, int x, int y, bool& transparent, uint32_t& tile)
{
	uint32_t bankSize = 0x100u << sprite.Region;
	uint32_t palette;
	bool tileXFlip;
	bool tileYFlip;

	if(sprite.Direct) {
		tile = sprite.FirstTile;
		palette = sprite.Palette;
		tileXFlip = sprite.XFlip ^ ((sprite.Pointer & 0x0800) != 0);
		tileYFlip = sprite.YFlip ^ ((sprite.Pointer & 0x0400) != 0);
	} else {
		//A flipped sprite also takes its tiles from the table in reverse
		int column = x >> 3;
		int row = y >> 3;
		int xtile = sprite.XFlip ? sprite.WidthTiles - 1 - column : column;
		int ytile = sprite.YFlip ? sprite.HeightTiles - 1 - row : row;
		uint16_t data = VramWord(((uint32_t)sprite.Pointer << 1) + ytile * sprite.WidthTiles + xtile);
		if(data == 0) {
			transparent = true;
			tile = 0;
			return 0;
		}
		tile = sprite.Bank * bankSize + (data & 0x03FF);
		palette = data >> 12;
		tileXFlip = sprite.XFlip ^ ((data & 0x0800) != 0);
		tileYFlip = sprite.YFlip ^ ((data & 0x0400) != 0);
	}

	int tileX = x & 7;
	int tileY = y & 7;
	if(tileXFlip) {
		tileX = 7 - tileX;
	}
	if(tileYFlip) {
		tileY = 7 - tileY;
	}
	uint8_t pixel = GetTilePixel(sprite.Region, tile, tileX, tileY);
	transparent = pixel == 0;
	return GetColorIndex(sprite.Region, palette, pixel);
}

//The entry under a tile position, taking a flipped layer into account as SampleTilemap does
SacPpu::LayerTileInfo SacPpu::GetLayerTile(int layer, int column, int row)
{
	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(layer, xsize, ysize);
	if(layer != 3) {
		uint16_t flags = TilemapFlags(layer);
		if(flags & 0x02) {
			column = xsize - 1 - column;
		}
		if(flags & 0x01) {
			row = ysize - 1 - row;
		}
	}
	return DecodeTile(layer, GetTilemapRegion(layer), (uint32_t)(row * xsize + column));
}

// Sprite table entry, four words:
// [0] zzz- ---- ---- ----  vertical zoom, 2 = life size (MAME's "enable" is its middle bit)
//     ---h hhh- ---- ----  height (through a table)
//     ---- ---y yyyy yyyy  Y position
// [1] bbbb ---- ---- ----  tile bank
//     ---- h--- ---- ----  horizontal flip
//     ---- -v-- ---- ----  vertical flip
//     ---- --mm ---- ----  mask mode
//     ---- ---- ---- -www  width, 1 << www tiles
// [2] zzzz z--- ---- ----  horizontal zoom, 5 = life size
//     ---- -pp- ---- ----  priority
//     ---- ---x xxxx xxxx  X position
// [3] d--- ---- ---- ----  direct: a single tile described here rather than a table in video RAM
//     -ooo oooo oooo oooo  address of the tile table
void SacPpu::DrawSpritesLine(int y, int width)
{
	//Heights are the value plus one, except at the top of the range
	static constexpr int ySizes[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 22, 24, 26 };

	uint32_t spriteCount = _spriteCount;
	uint32_t startWord = _spriteTableWord;
	//8bpp or 4bpp, and 8bpp sprites address tiles in banks of half the size
	int region = (_spriteFlags & 0x01) ? 0 : 1;
	uint32_t bankSize = 0x100u << region;

	for(uint32_t i = 0; i < spriteCount; i++) {
		uint32_t entry = startWord + i * 4;
		uint16_t w0 = VramWord(entry);
		uint16_t w1 = VramWord(entry + 1);
		uint16_t w2 = VramWord(entry + 2);
		uint16_t spritePtr = VramWord(entry + 3);

		//MAME reads bit 14 of the first word as "enabled", but it is part of the vertical zoom below,
		//which is 2 (life size) in nearly every sprite. A sprite drawn three times its size has the
		//bit clear.
		if(!spritePtr) {
			continue;
		}

		int x = w2 & 0x1FF;
		int spriteY = w0 & 0x1FF;
		if(spriteY >= 0x180) {
			spriteY -= 0x200;
		}
		//A position past the screen is off to the right until it is far enough round to be off to the
		//left instead, which is a sprite's width of room past the edge: 0x180 on a 256 pixel screen,
		//0x1C0 on a 320 pixel one. MAME uses 0x180 always, so on the wider screen a sprite still off
		//to the right is drawn at the left edge, and a logo sliding in appears at the left, vanishes,
		//then jumps to the right.
		int negativeFrom = SacPerfMode() == 5 ? 0x180 : (int)width + 128;
		if(x >= negativeFrom) {
			x -= 0x200;
		}

		uint32_t bank = w1 >> 12;
		int mask = (w1 >> 8) & 0x03;
		bool xflip = (w1 & 0x0800) != 0;
		bool yflip = (w1 & 0x0400) != 0;
		int priority = (w2 >> 9) & 0x03;
		int xsize = 1 << (w1 & 0x07);
		int ysize = ySizes[(w0 >> 9) & 0x0F];

		//Zoom, as a step through the sprite's own pixels per screen pixel in twelfths: bits 13-15 of
		//the first word vertically, (z + 1) / 3, and bits 11-15 of the position word horizontally,
		//(v + 1) / 6. Nearly every sprite has 2 and 5, life size; smaller values enlarge it (at 0,
		//six times as wide and three times as tall) and larger ones shrink it. One cartridge's intro
		//flies characters away from the camera through 0-6 and back in to 2, and the sizes Bcan
		//draws them at fit these steps to a pixel or two at every stage.
		int zoomY = 4 * ((w0 >> 13) + 1);
		int zoomX = 2 * (w2 >> 11) + 2;

		bool direct = (spritePtr & 0x8000) || (xsize == 1 && ysize == 1);
		if(direct) {
			xsize = 1;
			ysize = 1;
		}
		int srcWidth = xsize * 8;
		int srcHeight = ysize * 8;
		int screenWidth = (srcWidth * 12 + zoomX - 1) / zoomX;
		int screenHeight = (srcHeight * 12 + zoomY - 1) / zoomY;

		//Nothing of this sprite on this line
		int dy = y - spriteY;
		if(dy < 0 || dy >= screenHeight) {
			continue;
		}

		//Mosaic, as bits 3-5 of the second word: screen blocks of n + 1 pixels, each showing the sprite
		//at its top left corner, as the rotate/zoom layer's mosaic does. One map screen steps its town
		//name's sprites down 5 to 0 in step with the layer, and Bcan draws the name in the same blocks
		//as the map while the hero's sprites, left at 0, stay sharp.
		int mosaicSize = ((w1 >> 3) & 0x07) + 1;
		if(mosaicSize > 1) {
			dy = (y - y % mosaicSize) - spriteY;
			if(dy < 0) {
				continue;
			}
		}

		int srcY = dy * zoomY / 12;
		if(yflip) {
			srcY = srcHeight - 1 - srcY;
		}
		int ytile = srcY / 8;

		int lastXTile = -1;
		uint16_t data = 0;
		uint32_t tile = 0;
		uint32_t palette = 0;
		bool tileXFlip = false;
		int row = 0;
		for(int sx = 0; sx < screenWidth; sx++) {
			int screenX = x + sx;
			if(SacPerfMode() == 5) {
				screenX &= 0x1FF;
			} else if(screenX >= 0x200) {
				//Carried past the 512 pixel edge by a sprite that is still off to the right: off
				//screen, not back at the left
				break;
			}
			if(screenX < 0 || screenX >= width) {
				continue;
			}

			int sampleX = mosaicSize > 1 ? (screenX - screenX % mosaicSize) - x : sx;
			if(sampleX < 0) {
				continue;
			}
			int srcX = sampleX * zoomX / 12;
			if(xflip) {
				srcX = srcWidth - 1 - srcX;
			}
			int xtile = srcX / 8;
			if(xtile != lastXTile) {
				lastXTile = xtile;
				data = direct ? spritePtr : VramWord(((uint32_t)spritePtr << 1) + ytile * xsize + xtile);
				tile = bank * bankSize + (data & 0x03FF);
				palette = data >> 12;
				tileXFlip = xflip ^ ((data & 0x0800) != 0);
				bool tileYFlip = yflip ^ ((data & 0x0400) != 0);
				row = srcY % 8;
				if(tileYFlip != yflip) {
					row = 7 - row;
				}
			}
			//An empty entry draws nothing
			if(!direct && data == 0) {
				continue;
			}

			int column = srcX % 8;
			if(tileXFlip != xflip) {
				column = 7 - column;
			}
			uint8_t pixel = GetTilePixel(region, tile, column, row);
			if(pixel == 0) {
				continue;
			}

			//Mask mode 2-3 only marks where the sprite has pixels, mode 1 draws only where an earlier
			//sprite marked, mode 0 draws normally. With no marking sprite in the frame, mode 1 mixes
			//instead: each pixel comes out half its own colour and half what is beneath. One title
			//screen drops a shadow under its falling logo this way and another puts shadows behind
			//its portraits; every colour in Bcan's shadow is such an average. Inside the boot logo's
			//mask the sprite stays solid, as Bcan draws it. Among sprites the priority counts as it does against
			//the layers: a later sprite covers an earlier one only if its priority is not above it. One
			//map screen builds its hero from a feather (priority 0) over a body (3) that comes later in
			//the table, under a town name (1), and Bcan keeps the feather over the hair and the name
			//over the feet.
			bool spriteCovered = _lineSprite[screenX] != 0 && priority > (_linePriority[screenX] & 0x0F);
			if(mask > 1) {
				_lineSpriteMask[screenX] = 1;
			} else if(!spriteCovered && (mask == 0 || !_spriteMasksUsed || _lineSpriteMask[screenX])) {
				bool mix = mask == 1 && !_spriteMasksUsed;
				_lineSpriteUnder[screenX] = mix ? _lineSprite[screenX] : 0;
				_lineSpriteMix[screenX] = mix;
				_lineSprite[screenX] = GetColorIndex(region, palette, pixel);
				_linePriority[screenX] = (uint8_t)((_linePriority[screenX] & 0xF0) | priority);
			}
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

	//Mosaic: blocks of n + 1 pixels, as on the rotate/zoom layer and sprites. MAME makes them 2^n; one
	//game's fade-in steps all three layers from 7 to 1, and Bcan's blocks measure 8, 7, 6, 5, 4, 3
	//and 2 pixels, eight frames each.
	int mosaicSize = ((flags & 0x001C) >> 2) + 1;

	//Without bit 5 the layer shows only once: positions count round in 2048 pixels, and the layer
	//takes up the start of them. MAME sees a 12-bit position that is negative or past the layer as
	//off it, but one title screen runs its logo on from the rotate/zoom layer into a 1024 pixel
	//tilemap scrolled to $76A, which in 2048 pixels puts that tilemap's start 150 pixels in, where
	//Bcan draws it. A picture slid in from the right ($F76) still starts 138 pixels in either way.
	bool wrap = (flags & 0x20) != 0;
	if(!wrap && ((scrollY + y) & 0x7FF) >= layerHeight) {
		return;
	}

	int actualY = y - y % mosaicSize;
	int realY = actualY + scrollY;

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
		if(!wrap && ((lineScrollX + x) & 0x7FF) >= layerWidth) {
			continue;
		}

		int realX = ((x - x % mosaicSize) + lineScrollX) & (layerWidth - 1);
		bool aboveSprites = false;
		uint16_t pixel = SampleTilemap(layer, region, realX, realY, xsize, ysize, &aboveSprites);
		if((pixel & transMask) != 0 && layerPriority < (_linePriority[x] >> 4)) {
			_lineColor[x] = pixel;
			_lineFromRoz[x] = false;
			_lineAboveSprites[x] = aboveSprites;
			_linePriority[x] = (uint8_t)((_linePriority[x] & 0x0F) | (layerPriority << 4));
		}
	}
}

//The rotate/zoom layer, in 16.16 fixed point. Like MAME it does not check priorities, but unlike
//MAME it records its own where it draws, as a tilemap does: otherwise a sprite of lower priority
//shows through it wherever no tilemap happened to be drawn first. One title screen puts flame
//sprites (priority 3) behind a mask on this layer (priority 2), and a pool table's letter bands
//cover the balls, both as Bcan shows them.
//The boot logo's 1bpp mode is left out: its ball (priority 3, masked) must stay in front of the
//patterned floor (priority 2). Whether that is the mode or the sprite's mask is not yet known -
//no screen seen so far tells the two apart.
void SacPpu::DrawRozPixels(int y, int width, int region, int transMask, uint32_t startX, uint32_t startY, int incXX, int incXY, int incYX, int incYY, bool wrap)
{
	uint8_t layerPriority = (uint8_t)(((RozMode() >> 13) & 0x07) << 4);
	bool setPriority = (RozMode() & 0x03) != 0;

	int xsize = 0;
	int ysize = 0;
	GetTilemapDimensions(3, xsize, ysize);
	int layerWidth = xsize * 8;
	int layerHeight = ysize * 8;
	uint32_t widthShifted = (uint32_t)layerWidth << 16;
	uint32_t heightShifted = (uint32_t)layerHeight << 16;

	//Mosaic, as bits 2-4 of the mode: blocks of n + 1 pixels each showing the layer at their top left
	//corner (the line is already moved to the block's top by DrawRozLine). One map screen opens
	//with the layer at 7 and steps it down to 0; Bcan's blocks measure 6, 4, 3 and 2 pixels on the
	//way - linear steps, not the powers of two MAME uses for the tilemaps.
	int mosaicSize = ((RozMode() >> 2) & 0x07) + 1;

	uint32_t lineX = startX + (uint32_t)(y * incYX);
	uint32_t lineY = startY + (uint32_t)(y * incYY);
	uint32_t cx = lineX;
	uint32_t cy = lineY;
	for(int x = 0; x < width; x++) {
		if(mosaicSize > 1) {
			int blockX = x - x % mosaicSize;
			cx = lineX + (uint32_t)(blockX * incXX);
			cy = lineY + (uint32_t)(blockX * incXY);
		}
		if(wrap || (cx < widthShifted && cy < heightShifted)) {
			bool aboveSprites = false;
			uint16_t pixel = SampleTilemap(3, region, (int)(cx >> 16) & (layerWidth - 1), (int)(cy >> 16) & (layerHeight - 1), xsize, ysize, &aboveSprites);
			if((pixel & transMask) != 0) {
				_lineColor[x] = pixel;
				_lineFromRoz[x] = true;
				if(setPriority) {
					_linePriority[x] = (uint8_t)((_linePriority[x] & 0x0F) | layerPriority);
					_lineAboveSprites[x] = aboveSprites;
				}
			}
		}
		cx += (uint32_t)incXX;
		cy += (uint32_t)incXY;
	}
}

//Whether the rotate/zoom layer's per-line zoom table holds anything for the visible lines. Checked
//once a frame: the table is written before the layer is turned on, and a line's own entry is read
//as it is drawn.
bool SacPpu::HasLineTable(uint32_t reg, uint32_t wordsPerLine)
{
	if(Reg(reg) == 0) {
		//No base, no table: a game that needs only some of the three leaves the others at zero,
		//where video RAM word 0 holds tile data that reads as nonsense steps and positions
		return false;
	}

	uint32_t base = ((uint32_t)Reg(reg) << 2) >> 1;
	uint32_t words = GetScreenHeight() * wordsPerLine;
	for(uint32_t i = 0; i < words; i++) {
		if(VramWord(base + i)) {
			return true;
		}
	}
	return false;
}

void SacPpu::DrawRozLine(int y, int width, int region, int transMask)
{
	if(SacPerfMode() == 1) {
		return;
	}

	uint16_t rozMode = RozMode();
	bool wrap = (rozMode & 0x20) != 0;

	//A mosaic block shows the layer as it is on the block's top line (see DrawRozPixels)
	int mosaicSize = ((rozMode >> 2) & 0x07) + 1;
	y -= y % mosaicSize;

	auto signExtend = [](int value) { return (value & 0x8000) ? value - 0x10000 : value; };
	int incXX = signExtend(Reg(0x18C));
	int incYX = signExtend(Reg(0x18E));
	int incXY = signExtend(Reg(0x190));
	int incYY = signExtend(Reg(0x192));
	uint32_t scrollX = ((uint32_t)Reg(0x184) << 16) | Reg(0x186);
	uint32_t scrollY = ((uint32_t)Reg(0x188) << 16) | Reg(0x18A);

	//Each of the three per-line tables is turned on by its own base register - a zoom step in
	//$198, an X scroll in $19A, a Y scroll in $19E - and a game uses whichever it needs. MAME
	//reads all three whenever the mode register looks right, so a game that sets only the scroll
	//tables has its zoom steps read out of tile data at video RAM word 0, which draws noise.
	//A base that is set but whose table is still all zeroes belongs to a game zooming with the
	//plain registers before filling the table in; MAME's "a zero step means don't draw this line"
	//rule then skips every line and the screen stays black.
	//MAME turns the tables on with the top nibble of the mode register, but that is where the layer's
	//priority lives; it is the top bit of the tile mode register that does. One intro scrolls its
	//text up a perspective plane with the layer at priority 0 and that bit set ($D540), another game
	//spins a picture at priority 0 with it clear over tables still holding the boot ROM's leftovers,
	//and a company logo zooms in at priority 2 with it clear, over the tables the text crawl left
	//behind, drawn by Bcan from the plain zoom registers.
	//SAC_PERF=5 restores MAME's exact reading throughout, to A/B against.
	bool legacy = SacPerfMode() == 5;
	bool zoomTable = legacy || HasLineTable(0x198, 1);
	bool scrollXTable = legacy || HasLineTable(0x19A, 2);
	bool scrollYTable = legacy || HasLineTable(0x19E, 2);
	bool tablesOn = legacy ? (rozMode & 0xF000) != 0 : (Reg(0x182) & 0x8000) != 0;
	if((zoomTable || scrollXTable || scrollYTable) && !(rozMode & 0x0200) && tablesOn) {
		//MAME's reading of the per-line tables some intros use (it calls it untrusted): a zoom
		//step and a scroll position for each line, with a zero step meaning the line is not drawn
		uint32_t base0 = ((uint32_t)Reg(0x198) << 2) >> 1;
		uint32_t base1 = ((uint32_t)Reg(0x19A) << 2) >> 1;
		uint32_t base2 = ((uint32_t)Reg(0x19E) << 2) >> 1;

		//The table holds this line's horizontal step outright, not an offset to add to $18C. Adding
		//them (as MAME does) doubles the zoom - 0x0226 instead of 0x0126 on the title screen - so the
		//512-pixel map spans only 238 of the 320 columns and the right of the screen stays black.
		//Read this way a zero entry means "no step", which is why such a line is not drawn.
		int lineIncXX = incXX;
		if(zoomTable) {
			uint16_t lineStep = VramWord(base0 + y);
			if(!lineStep) {
				return;
			}
			lineIncXX = legacy ? signExtend((Reg(0x18C) + lineStep) & 0xFFFF) : signExtend(lineStep);
		}

		uint32_t lineScrollX = scrollX;
		if(scrollXTable) {
			lineScrollX += ((uint32_t)VramWord(base1 + y * 2) << 16) + VramWord(base1 + y * 2 + 1);
		}
		uint32_t lineScrollY = scrollY;
		if(scrollYTable) {
			lineScrollY += ((uint32_t)VramWord(base2 + y * 2) << 16) + VramWord(base2 + y * 2 + 1);
		}
		//The tables give this line's position outright, so the per-line advance must not be added
		//on top of them - MAME passes incYX/incYY here as well, which counts the vertical step two
		//or three times over and is why its notes call the rotate/zoom layer misaligned on intros.
		//Where a table gives this line's position outright the per-line advance must not be added on
		//top of it - MAME passes incYX/incYY regardless, counting the vertical step twice over, which
		//is why its notes call the rotate/zoom layer misaligned on intros. Where no table supplies a
		//position the advance is still what moves from one line to the next.
		int lineIncYX = (legacy || !scrollXTable) ? incYX : 0;
		int lineIncYY = (legacy || !scrollYTable) ? incYY : 0;
		DrawRozPixels(y, width, region, transMask, lineScrollX << 8, lineScrollY << 8, lineIncXX << 8, incXY << 8, lineIncYX << 8, lineIncYY << 8, wrap);
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
			_lineFromRoz[x] = false;
			_lineAboveSprites[x] = false;
			_linePriority[x] = (uint8_t)((_linePriority[x] & 0x0F) | (windowPriority << 4));
		}
	}
}

void SacPpu::RenderLine(uint32_t line)
{
	if(SacPerfMode() == 3) {
		return;
	}

	//The sprite setup is taken once, as the frame begins. One intro turns sprites on part way down a
	//frame while the count still says 65536 and sets the real count only at vblank; read line by
	//line, the rest of that frame shows all of video RAM as sprites, which Bcan does not.
	if(line == 0) {
		_spritesEnabled = (Reg(0x08) & 0x08) != 0;
		_spriteCount = (uint32_t)Reg(0x22) + 1;
		_spriteTableWord = ((uint32_t)Reg(0x20) << 2) >> 1;
		_spriteFlags = Reg(0x26);

		//Whether any sprite marks a mask this frame, which decides what mode 1 does (see DrawSpritesLine)
		_spriteMasksUsed = false;
		for(uint32_t i = 0; _spritesEnabled && i < _spriteCount && !_spriteMasksUsed; i++) {
			uint32_t entry = _spriteTableWord + i * 4;
			_spriteMasksUsed = VramWord(entry + 3) != 0 && ((VramWord(entry + 1) >> 8) & 0x03) > 1;
		}
	}

	uint32_t firstLine = GetFirstLine();
	uint32_t height = GetScreenHeight();
	if(line < firstLine || line >= firstLine + height) {
		return;
	}

	int y = (int)line;
	int width = (int)GetScreenWidth();
	uint16_t videoFlags = (uint16_t)((Reg(0x08) & ~0x08) | (_spritesEnabled ? 0x08 : 0));

	//Bit 6 of the rotate/zoom mode mixes the layer with what is beneath it: its pixels give the upper
	//nibble of the palette index and whatever would show without the layer gives the lower one. A
	//game then fills the palette as a table of blends between the two - one title screen fades a
	//mask in over black and over flame sprites that way, moving only the blend in its palette.
	bool rozMix = (videoFlags & 0x04) && (RozMode() & 0x40) && SacPerfMode() != 6;
	if(rozMix) {
		ComposeLine(y, width, videoFlags & ~0x04);
		std::copy(_lineColor, _lineColor + width, _lineUnder);
	}
	ComposeLine(y, width, videoFlags);
	if(rozMix) {
		for(int x = 0; x < width; x++) {
			if(_lineFromRoz[x]) {
				_lineColor[x] = (uint16_t)((_lineColor[x] & 0xF0) | (_lineUnder[x] & 0x0F));
			}
		}
	}

	//Through the palette as it stands now, so a palette changed mid-frame shows where it changed
	uint16_t* out = _frameBuffer + (line - firstLine) * width;
	for(int x = 0; x < width; x++) {
		uint16_t index = _lineColor[x] & 0xFF;
		uint16_t color = (uint16_t)(((_paletteRam[index * 2] << 8) | _paletteRam[index * 2 + 1]) & 0x7FFF);
		if(_lineMix[x]) {
			uint16_t under = _lineMixUnder[x] & 0xFF;
			uint16_t underColor = (uint16_t)(((_paletteRam[under * 2] << 8) | _paletteRam[under * 2 + 1]) & 0x7FFF);
			color = (uint16_t)((((color & 0x7C00) + (underColor & 0x7C00)) >> 1) & 0x7C00) |
				(uint16_t)((((color & 0x03E0) + (underColor & 0x03E0)) >> 1) & 0x03E0) |
				(uint16_t)(((color & 0x001F) + (underColor & 0x001F)) >> 1);
		}
		out[x] = color;
	}
}

//One line's palette indexes from the sprites, layers and window the flags enable
void SacPpu::ComposeLine(int y, int width, uint16_t videoFlags)
{
	for(int x = 0; x < width; x++) {
		_lineColor[x] = 0;
		_linePriority[x] = 0xFF;
		_lineSprite[x] = 0;
		_lineSpriteMask[x] = 0;
		_lineSpriteMix[x] = false;
		_lineMix[x] = false;
		_lineFromRoz[x] = false;
		_lineAboveSprites[x] = false;
	}

	//Only when they are enabled: the sprite table can name tens of thousands of entries (one game
	//leaves $22 at $FFFF with sprites off), and this runs per line where MAME's runs once a frame.
	if(videoFlags & 0x08) {
		DrawSpritesLine(y, width);
	}
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

	//Sprites go over any layer whose priority is not above theirs, except where an equal one drew a
	//tile marked to go above sprites (see DecodeTile)
	if(videoFlags & 0x08) {
		for(int x = 0; x < width; x++) {
			int spritePriority = _linePriority[x] & 0x0F;
			int layerPriority = _linePriority[x] >> 4;
			bool inFront = spritePriority < layerPriority || (spritePriority == layerPriority && !_lineAboveSprites[x]);
			if(_lineSprite[x] != 0 && inFront) {
				if(_lineSpriteMix[x]) {
					_lineMix[x] = true;
					_lineMixUnder[x] = _lineSpriteUnder[x] ? _lineSpriteUnder[x] : _lineColor[x];
				}
				_lineColor[x] = _lineSprite[x];
				_lineFromRoz[x] = false;
			}
		}
	}
}
