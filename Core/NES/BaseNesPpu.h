#pragma once
#include "pch.h"
#include "NES/INesMemoryHandler.h"
#include "Utilities/ISerializable.h"
#include "NES/NesTypes.h"

enum class ConsoleRegion;

class Emulator;
class BaseMapper;
class SnesControlManager;
class NesConsole;
class EmuSettings;

class BaseNesPpu : public INesMemoryHandler, public ISerializable
{
protected:
	uint64_t _masterClock = 0;
	uint32_t _cycle = 0;
	int16_t _scanline = 0;
	bool _emulatorBgEnabled = false;
	bool _emulatorSpritesEnabled = false;
	//16
	uint16_t _videoRamAddr = 0;
	uint16_t _tmpVideoRamAddr = 0;
	uint16_t _highBitShift = 0;
	uint16_t _lowBitShift = 0;
	uint8_t _masterClockDivider = 0;
	uint8_t _spriteRamAddr = 0;
	uint8_t _openBus = 0;
	uint8_t _xScroll = 0;
	bool _enableOamDecay = false;
	bool _needStateUpdate = false;
	bool _renderingEnabled = false;
	bool _prevRenderingEnabled = false;
	//32
	bool _sprite0Visible = false;
	uint8_t _spriteCount = 0;
	uint8_t _secondaryOamAddr = 0;
	uint8_t _oamCopybuffer = 0;
	bool _spriteInRange = false;
	bool _sprite0Added = false;
	uint8_t _overflowBugCounter = 0;
	bool _oamCopyDone = false;
	uint16_t _ppuBusAddress = 0;
	uint16_t _minimumDrawBgCycle = 0;
	uint16_t _minimumDrawSpriteCycle = 0;
	uint16_t _minimumDrawSpriteStandardCycle = 0;
	//48
	BaseMapper* _mapper = nullptr;
	uint16_t* _currentOutputBuffer = nullptr;
	////////////////////////
	//64 : end of cache line
	////////////////////////
	uint8_t _paletteRam[0x20] = {};
	uint8_t _secondarySpriteRam[0x20] = {};
	////////////////////////
	//128 : end of cache line
	////////////////////////
	TileInfo _tile = {};
	uint16_t _vblankEnd = 0;
	uint16_t _nmiScanline = 0;
	uint8_t _currentTilePalette = 0;
	uint8_t _previousTilePalette = 0;
	uint8_t _pendingTilePalette = 0; //extra pipeline stage, only used when _attributeLagEnabled
	uint16_t _intensifyColorBits = 0;
	uint8_t _paletteRamMask = 0;
	uint8_t _updateVramAddrDelay = 0;
	//144
	uint32_t _spriteIndex = 0;
	int32_t _lastUpdatedPixel = 0;
	uint32_t _frameCount = 0;
	uint16_t _updateVramAddr = 0;
	bool _preventVblFlag = false;
	bool _writeToggle = false; //not used in rendering
	bool _paletteBgHackEnabled = true; //Show palette color when V is in $3F00-$3FFF during forced blanking (2C02 quirk; some famiclone PPUs lack it)
	bool _nmiSuppressRaceEnabled = true; //2C02 suppresses the NMI when $2002 is read one dot before vblank; some famiclone PPUs (BBK) lack this race
	bool _paletteMirroringEnabled = true; //2C02 mirrors $3F10/$14/$18/$1C onto $3F00/$04/$08/$0C; on some famiclone PPUs all 32 entries are independent
	bool _attributeLagEnabled = false; //Test switch - color a tile with the previous tile column's attribute
	bool _vramWriteGlitchEnabled = true; //A $2007 write during rendering smears the bus address' LSB into VRAM (2C02; unconfirmed, and some famiclone PPUs simply drop the write)
	bool _vramAddrRealignEnabled = false; //A $2007 access re-aligns the $2006 write latch on some famiclone PPUs (see EnablePpuVramAddrRealign)
	uint8_t _splitBgFetchMode = 0; //YuXing video chip split screen: 0 = 2C02 fetch, 1 = 2-screen (1bpp), 2 = 4-band (see YuxingMapper)
	//160
	NesSpriteInfo* _lastSprite = nullptr; //used by HD ppu
	NesConsole* _console = nullptr;
	//176
	PpuControlFlags _control = {}; // 8 bytes
	PpuMaskFlags _mask = {}; // 8 bytes
	////////////////////////
	//192 : end of cache line
	////////////////////////
	uint8_t _spriteRam[0x100] = {};
	////////////////////////
	//448 : end of cache line
	////////////////////////
	NesSpriteInfo _spriteTiles[64] = {};

	static constexpr int SpriteShifterDone = 0x8000;
	uint16_t _spriteShifterList[9] = { SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone, SpriteShifterDone }; //Ordered by X coordinate.
	uint8_t _nextSpriteShifter = 0;
	uint16_t _nextSpriteShifterCycle = 0;
	uint8_t _activeSpriteShifters = 0;
	uint8_t _countingSpriteShifters = 0;
	uint8_t _expiredSpriteShifters = 0;
	uint8_t _dotSkipped = 0;
	bool _processSprites = false;

	Emulator* _emu = nullptr;
	EmuSettings* _settings = nullptr;
	uint16_t* _outputBuffers[2] = {};

	ConsoleRegion _region = {};
	uint16_t _standardVblankEnd = 0;
	uint16_t _standardNmiScanline = 0;
	uint16_t _palSpriteEvalScanline = 0;

	bool _needVideoRamIncrement = false;
	bool _allowFullPpuAccess = false;

	uint8_t _ppuMemoryDataReadStateMachine = 0;
	uint8_t _ppuMemoryDataWriteStateMachine = 0;
	uint8_t _ppuMemoryDataWriteLatch = 0;
	uint8_t _memoryReadBuffer = 0;
	PPUStatusFlags _statusFlags = {};

	uint8_t _firstVisibleSpriteAddr = 0; //For extra sprites
	uint8_t _lastVisibleSpriteAddr = 0; //For extra sprites

	uint32_t _ignoreVramRead = 0;
	int32_t _openBusDecayStamp[8] = {};

	uint64_t _oamDecayCycles[0x20] = {};

	bool IsRenderingEnabled();
	void UpdateGrayscaleAndIntensifyBits();
	void UpdateColorBitMasks();
	void UpdateMinimumDrawCycles();

public:
	virtual void Reset(bool softReset) = 0;
	virtual void Run(uint64_t runTo) = 0;

	//The YuXing video chip's split-screen modes. In 2-screen mode the background tile
	//address comes from the mapper instead of $2000 bit 4, both bit planes read the same
	//byte, and sprites are colored two entries further into the palette. In 4-band mode
	//the fetch is ordinary but the mapper rebanks video RAM per band.
	void SetSplitBgFetch(uint8_t mode) { _splitBgFetchMode = mode; }

	//Whether $2001 has the display turned on. Mappers whose per-scanline logic is gated on
	//it (see YuxingMapper's MMC3 clone) need this without the cost of a full GetState().
	bool IsDisplayOn() { return _mask.BackgroundEnabled || _mask.SpritesEnabled; }

	//Current scroll address. Video chips that rebank per scanline take the row from it the
	//way the reference emulator reads loopy_v at the start of a line (see YuxingMapper).
	uint16_t GetVideoRamAddr() { return _videoRamAddr; }

	//$2000 bit 3, the sprite pattern table. The Bung Doctor PC Jr. picks between its two
	//per-tile CHR bank sources by comparing this against the table a background fetch came
	//from, so its mapper needs the bit on its own (see DrPcJrMapper).
	uint16_t GetSpritePatternAddr() { return _control.SpritePatternAddr; }
	uint16_t GetBgPatternAddr() { return _control.BackgroundPatternAddr; }

	uint32_t GetFrameCount() { return _frameCount; }
	uint32_t GetCurrentCycle() { return _cycle; }
	int32_t GetCurrentScanline() { return _scanline; }
	int32_t GetScanlineCount() { return _vblankEnd + 2; }
	uint32_t GetFrameCycle() { return ((_scanline + 1) * 341) + _cycle; }

	virtual uint16_t* GetScreenBuffer(bool previousBuffer, bool processGrayscaleEmphasisBits = false) = 0;
	virtual void UpdateTimings(ConsoleRegion region, bool overclockAllowed = true) = 0;

	void GetState(NesPpuState& state);
	void SetState(NesPpuState& state);

	uint16_t GetCurrentBgColor();

	uint8_t ReadPaletteRam(uint16_t addr);
	void WritePaletteRam(uint16_t addr, uint8_t value);

	void DebugSendFrame();

	virtual PpuModel GetPpuModel() = 0;
	virtual uint32_t GetPixelBrightness(uint8_t x, uint8_t y) = 0;

	virtual void GetMemoryRanges(MemoryRanges& ranges) override {}
	virtual uint8_t ReadRam(uint16_t addr) override { return 0; }
	virtual void WriteRam(uint16_t addr, uint8_t value) override {}

	virtual void Serialize(Serializer& s) override {}
};
