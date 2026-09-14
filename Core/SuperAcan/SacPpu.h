#pragma once
#include "pch.h"
#include "SuperAcan/SacTypes.h"

//The video chip, after MAME's supracan screen_update: three tilemaps, a rotate/zoom layer,
//sprites with their mask modes, the window, and priorities between them. MAME draws a region
//of lines each time a video register is written; here each visible line is drawn as the
//machine reaches it, which puts a mid-frame change in the same place.
//
//Video RAM and the palette are held as the 68000 sees them, big-endian.
class SacPpu
{
private:
	uint8_t* _vram = nullptr;
	uint8_t* _paletteRam = nullptr;
	uint16_t* _regs = nullptr;
	uint16_t* _frameBuffer = nullptr;

	//The line being built, indexed by x: MAME's bitmap (a palette index), its priority map
	//(tile priority in the high nibble, sprite priority in the low one), and the sprite layer
	//with its mask
	uint16_t _lineColor[SacConstants::MaxScreenWidth] = {};
	uint8_t _linePriority[SacConstants::MaxScreenWidth] = {};
	uint16_t _lineSprite[SacConstants::MaxScreenWidth] = {};
	uint8_t _lineSpriteMask[SacConstants::MaxScreenWidth] = {};

	uint16_t Reg(uint32_t addr) { return _regs[(addr >> 1) & 0xFF]; }
	uint16_t VramWord(uint32_t wordIndex);

	uint16_t TilemapFlags(int layer) { return Reg(0x100 + layer * 0x20); }
	uint16_t TilemapTileMode(int layer) { return Reg(0x102 + layer * 0x20); }
	uint16_t TilemapMode(int layer) { return Reg(0x10A + layer * 0x20); }
	uint16_t RozMode() { return Reg(0x180); }

	int GetTilemapRegion(int layer);
	void GetTilemapDimensions(int layer, int& xsize, int& ysize);
	uint8_t GetTilePixel(int region, uint32_t tile, int x, int y);
	uint16_t GetColorIndex(int region, uint32_t palette, uint8_t pixel);
	uint16_t SampleTilemap(int layer, int region, int x, int y, int xsize, int ysize);

	void DrawSpritesLine(int y, int width);
	void DrawSpriteTileLine(int y, int width, int region, uint32_t tile, uint32_t palette, bool xflip, bool yflip, int x, int tileY, int mask, int priority);
	void DrawTilemapLine(int layer, int y, int width, int layerPriority, int region, int transMask);
	void DrawRozLine(int y, int width, int region, int transMask);
	void DrawRozPixels(int y, int width, int region, int transMask, uint32_t startX, uint32_t startY, int incXX, int incXY, int incYX, int incYY, bool wrap);
	void DrawWindowLine(int priority, int y, int width);

public:
	void Init(uint8_t* vram, uint8_t* paletteRam, uint16_t* regs, uint16_t* frameBuffer);

	uint32_t GetScreenWidth();
	uint32_t GetScreenHeight();
	uint32_t GetFirstLine();

	void RenderLine(uint32_t line);
};
