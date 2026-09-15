#include "pch.h"
#include "SuperAcan/Debugger/SacPpuTools.h"
#include "SuperAcan/SacConsole.h"
#include "SuperAcan/SacPpu.h"
#include "SuperAcan/SacTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "Shared/ColorUtilities.h"

SacPpuTools::SacPpuTools(Debugger* debugger, Emulator* emu, SacConsole* console) : PpuTools(debugger, emu)
{
	_console = console;
}

//Tilemaps are sized by their layer's registers: 16x16 up to 128x32 or 64x64 tiles
FrameInfo SacPpuTools::GetTilemapSize(GetTilemapOptions options, BaseState& baseState)
{
	SacPpuState& state = (SacPpuState&)baseState;
	SacPpu ppu;
	ppu.Init(nullptr, nullptr, state.VideoRegs, nullptr);

	int width = 0;
	int height = 0;
	ppu.GetLayerSize(options.Layer, width, height);
	return { (uint32_t)width, (uint32_t)height };
}

DebugTilemapInfo SacPpuTools::GetTilemap(GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer)
{
	SacPpuState& state = (SacPpuState&)baseState;
	SacPpu ppu;
	ppu.Init(vram, nullptr, state.VideoRegs, nullptr);

	int layer = options.Layer;
	int width = 0;
	int height = 0;
	ppu.GetLayerSize(layer, width, height);
	int region = ppu.GetLayerRegion(layer);

	bool grayscale = options.DisplayMode == TilemapDisplayMode::Grayscale;
	uint32_t bgColor = GetTilemapBackgroundColor(options.Background, palette[0]);

	for(int y = 0; y < height; y++) {
		for(int x = 0; x < width; x++) {
			bool transparent = false;
			uint16_t colorIndex = ppu.GetLayerPixel(layer, x, y, transparent);
			uint32_t color;
			if(transparent) {
				color = bgColor;
			} else if(grayscale) {
				color = _grayscaleColorsBpp4[colorIndex & 0x0F];
			} else {
				color = palette[colorIndex & 0xFF];
			}
			outBuffer[y * width + x] = color;
		}
	}

	DebugTilemapInfo result = {};
	switch(region) {
		case 0: result.Bpp = 8; result.Format = TileFormat::GbaBpp8; break;
		case 1: result.Bpp = 4; result.Format = TileFormat::WsBpp4Packed; break;
		case 2: result.Bpp = 2; result.Format = TileFormat::WsBpp4Packed; break;
		default: result.Bpp = 1; result.Format = TileFormat::SmsSgBpp1; break;
	}

	uint16_t flags = state.VideoRegs[(layer == 3 ? 0x180 : 0x100 + layer * 0x20) >> 1];
	result.TileWidth = 8;
	result.TileHeight = 8;
	result.ColumnCount = width / 8;
	result.RowCount = height / 8;
	result.TilemapAddress = (uint32_t)state.VideoRegs[(layer == 3 ? 0x194 : 0x108 + layer * 0x20) >> 1] << 2;
	result.TilesetAddress = 0;
	result.Priority = (int8_t)((flags >> 13) & 0x07);

	//The visible part: scroll registers for the tilemaps; the rotate/zoom layer has no simple one
	if(layer < 3 && width > 0 && height > 0) {
		result.ScrollX = (state.VideoRegs[(0x104 + layer * 0x20) >> 1] & 0xFFF) % width;
		result.ScrollY = (state.VideoRegs[(0x106 + layer * 0x20) >> 1] & 0xFFF) % height;
	}
	result.ScrollWidth = (state.VideoRegs[0x08 >> 1] & 0x0100) ? 320 : 256;
	result.ScrollHeight = (state.VideoRegs[0x08 >> 1] & 0x0200) ? 224 : 240;
	return result;
}

DebugTilemapTileInfo SacPpuTools::GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	DebugTilemapTileInfo result = {};

	FrameInfo size = GetTilemapSize(options, baseState);
	if(x >= size.Width || y >= size.Height) {
		return result;
	}

	SacPpuState& state = (SacPpuState&)baseState;
	SacPpu ppu;
	ppu.Init(vram, nullptr, state.VideoRegs, nullptr);

	int region = ppu.GetLayerRegion(options.Layer);
	uint32_t bytesPerTile = region == 0 ? 64 : (region == 1 ? 32 : (region == 2 ? 16 : 8));
	SacPpu::LayerTileInfo tile = ppu.GetLayerTile(options.Layer, x / 8, y / 8);

	result.Column = x / 8;
	result.Row = y / 8;
	result.Width = 8;
	result.Height = 8;
	result.TileMapAddress = tile.MapAddress;
	result.TileIndex = tile.Tile;
	result.AddAddress((tile.Tile * bytesPerTile) & 0x1FFFF);
	result.PaletteIndex = tile.Palette;
	result.PaletteAddress = tile.ColorBase * 2;
	result.HorizontalMirroring = (NullableBoolean)tile.XFlip;
	result.VerticalMirroring = (NullableBoolean)tile.YFlip;
	return result;
}

DebugSpritePreviewInfo SacPpuTools::GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	SacPpuState& state = (SacPpuState&)baseState;
	SacPpu ppu;
	ppu.Init(nullptr, nullptr, state.VideoRegs, nullptr);

	DebugSpritePreviewInfo info = {};
	info.Width = SpriteCanvasSize;
	info.Height = SpriteCanvasSize;
	info.SpriteCount = std::min(ppu.GetSpriteCount(), MaxSprites);
	info.CoordOffsetX = 0;
	info.CoordOffsetY = 0;
	info.WrapBottomToTop = true;
	info.WrapRightToLeft = true;

	info.VisibleX = 0;
	info.VisibleY = ppu.GetFirstLine();
	info.VisibleWidth = ppu.GetScreenWidth();
	info.VisibleHeight = ppu.GetScreenHeight();
	return info;
}

//Each sprite drawn as the screen would, clipped to the 128x128 preview Mesen keeps per sprite.
//Sprites later in the table are drawn over earlier ones; mask sprites only mark the mask, so
//they appear in the list but not on the screen preview.
void SacPpuTools::GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview)
{
	SacPpuState& state = (SacPpuState&)baseState;
	SacPpu ppu;
	ppu.Init(vram, nullptr, state.VideoRegs, nullptr);

	uint32_t count = std::min(ppu.GetSpriteCount(), MaxSprites);
	int screenWidth = (int)ppu.GetScreenWidth();
	int screenHeight = (int)ppu.GetScreenHeight();
	int firstLine = (int)ppu.GetFirstLine();

	uint32_t bgColor = GetSpriteBackgroundColor(options.Background, palette, false);
	uint32_t darkBg = GetSpriteBackgroundColor(options.Background, palette, true);
	std::fill(screenPreview, screenPreview + SpriteCanvasSize * SpriteCanvasSize, darkBg);
	for(int y = firstLine; y < firstLine + screenHeight; y++) {
		std::fill(screenPreview + y * SpriteCanvasSize, screenPreview + y * SpriteCanvasSize + screenWidth, bgColor);
	}

	for(uint32_t i = 0; i < count; i++) {
		DebugSpriteInfo& sprite = outBuffer[i];
		uint32_t* preview = spritePreviews + i * _spritePreviewSize;
		sprite.Init();

		SacPpu::SpriteViewInfo info = ppu.GetSprite(i);
		int width = std::min(info.WidthTiles * 8, 128);
		int height = std::min(info.HeightTiles * 8, 128);
		uint32_t bytesPerTile = info.Region == 0 ? 64 : 32;

		sprite.SpriteIndex = (int16_t)i;
		sprite.Bpp = info.Region == 0 ? 8 : 4;
		sprite.Format = info.Region == 0 ? TileFormat::GbaBpp8 : TileFormat::WsBpp4Packed;
		sprite.RawX = (int16_t)info.RawX;
		sprite.RawY = (int16_t)info.RawY;
		sprite.X = (int16_t)info.RawX;
		sprite.Y = (int16_t)info.RawY;
		sprite.Width = (uint16_t)width;
		sprite.Height = (uint16_t)height;
		sprite.TileIndex = (int32_t)info.FirstTile;
		sprite.TileAddress = (int32_t)((info.FirstTile * bytesPerTile) & 0x1FFFF);
		sprite.Palette = (int16_t)info.Palette;
		sprite.PaletteAddress = info.Region == 0 ? 0 : (int32_t)((info.Palette & 0x0F) * 16);
		sprite.Priority = (DebugSpritePriority)info.Priority;
		sprite.Mode = info.Mask > 1 ? DebugSpriteMode::Window : DebugSpriteMode::Normal;
		sprite.HorizontalMirror = info.XFlip ? NullableBoolean::True : NullableBoolean::False;
		sprite.VerticalMirror = info.YFlip ? NullableBoolean::True : NullableBoolean::False;

		if(!info.Enabled) {
			sprite.Visibility = SpriteVisibility::Disabled;
		} else if(info.X < screenWidth && info.X + width > 0 && info.Y < firstLine + screenHeight && info.Y + height > firstLine) {
			sprite.Visibility = SpriteVisibility::Visible;
		} else {
			sprite.Visibility = SpriteVisibility::Offscreen;
		}

		for(int y = 0; y < height; y++) {
			for(int x = 0; x < width; x++) {
				bool transparent = false;
				uint32_t tile = 0;
				uint16_t colorIndex = ppu.GetSpritePixel(info, x, y, transparent, tile);
				uint32_t color = transparent ? 0 : palette[colorIndex & 0xFF];

				if(((x | y) & 7) == 0 && sprite.TileCount < 64) {
					sprite.TileAddresses[sprite.TileCount++] = (tile * bytesPerTile) & 0x1FFFF;
				}

				if(color != 0) {
					if(sprite.Visibility != SpriteVisibility::Disabled && info.Mask <= 1) {
						uint32_t canvasX = (info.RawX + x) & (SpriteCanvasSize - 1);
						uint32_t canvasY = (info.RawY + y) & (SpriteCanvasSize - 1);
						screenPreview[canvasY * SpriteCanvasSize + canvasX] = color;
					}
					preview[y * width + x] = color;
				} else {
					preview[y * width + x] = bgColor;
				}
			}
		}
	}
}

//256 colours of 15 bits, red in the low bits, one big-endian word each. Background layers and
//sprites share them, 16 to a palette at 4bpp.
DebugPaletteInfo SacPpuTools::GetPaletteInfo(GetPaletteInfoOptions options)
{
	DebugPaletteInfo info = {};
	info.ColorCount = 256;
	info.BgColorCount = 256;
	info.SpriteColorCount = 0;
	info.ColorsPerPalette = 16;
	info.SpritePaletteOffset = 0;

	info.HasMemType = true;
	info.PaletteMemType = MemoryType::SacPaletteRam;
	info.PaletteMemOffset = 0;

	uint8_t* palette = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::SacPaletteRam);

	info.RawFormat = RawPaletteFormat::Rgb555;
	for(int i = 0; i < 256; i++) {
		info.RawPalette[i] = ((palette[i * 2] << 8) | palette[i * 2 + 1]) & 0x7FFF;
		info.RgbPalette[i] = ColorUtilities::Rgb555ToArgb(info.RawPalette[i]);
	}
	return info;
}

void SacPpuTools::SetPaletteColor(int32_t colorIndex, uint32_t color)
{
	uint8_t r = (color >> 19) & 0x1F;
	uint8_t g = (color >> 11) & 0x1F;
	uint8_t b = (color >> 3) & 0x1F;
	uint16_t rgb555 = (uint16_t)(r | (g << 5) | (b << 10));

	MemoryDumper* dumper = _debugger->GetMemoryDumper();
	dumper->SetMemoryValue(MemoryType::SacPaletteRam, colorIndex * 2, (uint8_t)(rgb555 >> 8));
	dumper->SetMemoryValue(MemoryType::SacPaletteRam, colorIndex * 2 + 1, (uint8_t)rgb555);
}
