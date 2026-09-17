#pragma once
#include "pch.h"
#include "Debugger/PpuTools.h"

class Debugger;
class Emulator;
class SacConsole;

//The video viewers' view of the Super A'Can: the palette, tiles in video RAM, the tilemaps and the
//sprites, all decoded by SacPpu itself so the viewers agree with the screen
class SacPpuTools final : public PpuTools
{
private:
	//The sprite table is in video RAM and its count register goes far beyond any game's needs
	static constexpr uint32_t MaxSprites = 256;
	//Sprite coordinates are 9 bits, so the screen preview covers 512x512
	static constexpr uint32_t SpriteCanvasSize = 512;

	SacConsole* _console = nullptr;

public:
	SacPpuTools(Debugger* debugger, Emulator* emu, SacConsole* console);

	DebugTilemapInfo GetTilemap(GetTilemapOptions options, BaseState& state, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer) override;
	FrameInfo GetTilemapSize(GetTilemapOptions options, BaseState& state) override;
	DebugTilemapTileInfo GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState) override;

	DebugSpritePreviewInfo GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& state, BaseState& ppuToolsState) override;
	void GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview) override;

	DebugPaletteInfo GetPaletteInfo(GetPaletteInfoOptions options) override;
	void SetPaletteColor(int32_t colorIndex, uint32_t color) override;
};
