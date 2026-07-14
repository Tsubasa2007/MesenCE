#pragma once
#include "pch.h"
#include "NES/NesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/Mappers/Sb2k/Sb2kMapper.h"
#include "Utilities/Serializer.h"

//PPU for the Subor SB-2000's UM6576 chip (ported from the VirtuaNES-BBK fork's
//PPU_UM6576, author fanoble).
//
//The chip has two personalities, switched by the mapper's $4031 unlock sequence:
// - Famiclone mode (power-on default): 2C02-compatible. Handled entirely by the
//   base class - every hook below defers to the stock implementation.
// - Native mode: a different register set on the same $2000 page. 16-bit tile names
//   (32x32-tile pages, 2 bytes per entry), 2-plane 4-color or 4-plane 16-color tiles
//   (planes at +0/+8/+16/+24), a direct 64-entry palette at $2040-$207F holding
//   final color values, a flat 16-bit VRAM pointer ("GA") written high-byte-first
//   through $2006, and an X/Y screen origin through $2005. There is no sprite-0 hit
//   and no scrolling registers beyond the origin. NMI enable and the vblank flag
//   work like the 2C02 ($2000.7 / $2002.7, flag cleared on read).
//
//Native mode reuses the base class frame loop (scanline/cycle counters, vblank flag,
//NMI timing, frame output) and replaces the register interface and the rendering:
//each visible scanline is rendered in one pass at cycle 256, reading video memory
//through the mapper (GA < $8000 = CRAM, GA >= $8000 = the banked EVRAM window).
//
//Known gaps inherited from the reference implementation: the $2008.0 "page mode"
//and sprite mirroring attributes are not emulated (marked "to do" in the fork).
class Sb2kPpu final : public NesPpu<Sb2kPpu>
{
private:
	Sb2kMapper* _sb2k = nullptr;

	//UM6576 native-mode registers
	uint16_t _ga = 0; //16-bit VRAM pointer ($2006, high byte first; incremented by $2007 access)
	uint8_t _nativeControl = 0; //$2000: bit 7 = NMI enable, bit 5 = 8x16 sprites, bit 2 = +32 increment, bits 0-1 = BG page
	uint8_t _nativeMask = 0; //$2001: bit 3 = BG enable, bit 4 = sprite enable
	uint8_t _nativeOamAddr = 0; //$2003 (shared by $2004 reads and writes, both post-increment)
	uint8_t _reg2008 = 0; //bit 7 = 16-color mode, bits 2-3 = sprite pattern base (4KB units)
	uint8_t _originX = 0; //$2005 first write
	uint8_t _originY = 0; //$2005 second write
	bool _toggle2005 = false;
	bool _toggle2006 = false;
	uint8_t _vramReadBuffer = 0; //$2007 reads are buffered like the 2C02
	uint8_t _nativePalette[64] = {}; //$2040-$207F: final color values, not indexes into palette RAM

	__forceinline bool IsNativeMode() { return _sb2k->IsUm6576Mode(); }

	uint8_t ReadNativeRegister(uint16_t addr, bool forDebugger)
	{
		switch(addr) {
			case 0x2002: {
				uint8_t status = _statusFlags.VerticalBlank ? 0x80 : 0;
				if(!forDebugger) {
					_statusFlags.VerticalBlank = false;
					_console->GetCpu()->ClearNmiFlag();
					_toggle2005 = false;
					_toggle2006 = false;
				}
				return status;
			}

			case 0x2004: {
				uint8_t value = _spriteRam[_nativeOamAddr];
				if(!forDebugger) {
					_nativeOamAddr++;
				}
				return value;
			}

			case 0x2007: {
				uint8_t value = _vramReadBuffer;
				if(!forDebugger) {
					_vramReadBuffer = _sb2k->VideoRead(_ga);
					_ga += (_nativeControl & 0x04) ? 32 : 1;
				}
				return value;
			}

			case 0x2008:
				return _reg2008;

			default:
				if(addr >= 0x2040 && addr < 0x2080) {
					return _nativePalette[addr & 0x3F];
				}
				return 0;
		}
	}

	void WriteNativeRegister(uint16_t addr, uint8_t value)
	{
		switch(addr) {
			case 0x2000: {
				_nativeControl = value;
				//Keep the base class NMI-enable flag in sync so its vblank logic fires the NMI
				bool nmiEnabled = (value & 0x80) != 0;
				if(!nmiEnabled) {
					_console->GetCpu()->ClearNmiFlag();
				} else if(!_control.NmiOnVerticalBlank && _statusFlags.VerticalBlank) {
					//Enabling NMIs while the vblank flag is set triggers one immediately
					_console->GetCpu()->SetNmiFlag();
				}
				_control.NmiOnVerticalBlank = nmiEnabled;
				break;
			}

			case 0x2001:
				_nativeMask = value;
				break;

			case 0x2003:
				_nativeOamAddr = value;
				break;

			case 0x2004:
				_spriteRam[_nativeOamAddr] = value;
				_nativeOamAddr++;
				break;

			case 0x2005:
				if(_toggle2005) {
					_originY = value;
				} else {
					_originX = value;
				}
				_toggle2005 = !_toggle2005;
				break;

			case 0x2006:
				if(_toggle2006) {
					_ga = (_ga & 0xFF00) | value;
				} else {
					_ga = (_ga & 0x00FF) | (value << 8);
				}
				_toggle2006 = !_toggle2006;
				break;

			case 0x2007:
				_sb2k->VideoWrite(_ga, value);
				_ga += (_nativeControl & 0x04) ? 32 : 1;
				break;

			case 0x2008:
				_reg2008 = value;
				break;

			default:
				if(addr >= 0x2040 && addr < 0x2080) {
					_nativePalette[addr & 0x3F] = value;
				}
				break;
		}
	}

	//Renders one visible scanline. Same one-pass structure as the reference emulator:
	//a 272-pixel line with an 8-pixel left margin keeps its tile/sprite X math intact
	//(a 33rd partial tile and sprite X near 255 both spill past 256).
	void RenderNativeScanline()
	{
		uint8_t line[272];

		if(_nativeMask & 0x08) {
			//Background: 32x32-tile pages of 16-bit tile names. Bits 0-1 of $2000 pick
			//the page (2KB units); crossing the bottom edge moves to the other page pair.
			int32_t tileY = (_scanline + _originY) / 8;
			int32_t tileBase = (_nativeControl & 0x03) * 2048;
			if(tileY >= 32) {
				tileY -= 32;
				if(_nativeControl & 0x02) {
					tileBase -= 4096; //pages 2,3 wrap to 0,1
				} else {
					tileBase += 4096; //pages 0,1 continue into 2,3
				}
			}

			int32_t y = (_scanline + _originY) & 0x07;
			int32_t dstX = 8;

			for(int32_t i = 0; i < 33; i++) {
				int32_t tileX = (_originX / 8) + i;

				int32_t tileAddr;
				if(tileX & 0x20) {
					//Crossed the right edge into the horizontally adjacent page
					tileAddr = tileBase + 2048 + tileY * 64 + (tileX & 0x1F) * 2;
				} else {
					tileAddr = tileBase + tileY * 64 + tileX * 2;
				}

				uint16_t tileName = _sb2k->VideoRead((uint16_t)(tileAddr + 1)) * 256 + _sb2k->VideoRead((uint16_t)tileAddr);

				uint8_t mask = i ? 0x80 : (0x80 >> (_originX & 0x07));
				int32_t tileWidth;
				if(i == 0) {
					tileWidth = 8 - (_originX & 0x07);
				} else if(i == 32) {
					tileWidth = _originX & 0x07;
				} else {
					tileWidth = 8;
				}

				if(_reg2008 & 0x80) {
					//16-color mode: 4 planes, tile name bit 0 ignored, palette bank from bits 14-15
					int32_t tileBank = (tileName >> 12) & 0x0C;
					uint16_t tileOffset = (tileName & 0xFFE) * 16;

					uint8_t plane[4];
					plane[0] = _sb2k->VideoRead(tileOffset + y);
					plane[1] = _sb2k->VideoRead(tileOffset + 8 + y);
					plane[2] = _sb2k->VideoRead(tileOffset + 16 + y);
					plane[3] = _sb2k->VideoRead(tileOffset + 24 + y);

					for(int32_t x = 0; x < tileWidth; x++) {
						int32_t index = 0;
						if(plane[0] & mask) index |= 0x01;
						if(plane[1] & mask) index |= 0x02;
						if(plane[2] & mask) index |= 0x04;
						if(plane[3] & mask) index |= 0x08;

						line[dstX++] = _nativePalette[tileBank * 4 + index];
						mask >>= 1;
					}
				} else {
					//4-color mode: 2 planes, palette bank from the top 4 bits of the tile name
					int32_t tileBank = tileName >> 12;
					uint16_t tileOffset = (tileName & 0xFFF) * 16;

					uint8_t plane[2];
					plane[0] = _sb2k->VideoRead(tileOffset + y);
					plane[1] = _sb2k->VideoRead(tileOffset + 8 + y);

					for(int32_t x = 0; x < tileWidth; x++) {
						int32_t index = 0;
						if(plane[0] & mask) index |= 0x01;
						if(plane[1] & mask) index |= 0x02;

						line[dstX++] = _nativePalette[tileBank * 4 + index];
						mask >>= 1;
					}
				}
			}
		} else {
			memset(line, 0, sizeof(line));
		}

		if(_nativeMask & 0x10) {
			//Sprites: 64 4-byte entries (Y/tile/attr/X like the 2C02), drawn back to front
			//so lower-index sprites end up on top. Always 4-color, palette entries 16-31,
			//8-bit tile names with the pattern base from $2008 bits 2-3.
			int32_t spriteHeight = (_nativeControl & 0x20) ? 16 : 8;
			uint16_t patternBase = ((_reg2008 >> 2) & 0x03) << 12;

			for(int32_t i = 63; i >= 0; i--) {
				uint8_t* sprite = &_spriteRam[i * 4];
				int32_t spriteY = sprite[0] + 1;

				if(_scanline < spriteY || _scanline >= spriteY + spriteHeight) {
					continue;
				}
				if(sprite[2] & 0x20) {
					//Behind-background sprites are not drawn (reference behavior)
					continue;
				}

				int32_t y = _scanline - spriteY;
				int32_t tileOffset = patternBase + sprite[1] * 16;
				if(_nativeControl & 0x20) {
					//8x16: the top half comes from the preceding tile
					if(y < 8) {
						tileOffset -= 16;
					} else {
						y -= 8;
					}
				}

				uint8_t plane[2];
				plane[0] = _sb2k->VideoRead((uint16_t)(tileOffset + y));
				plane[1] = _sb2k->VideoRead((uint16_t)(tileOffset + 8 + y));

				uint8_t mask = 0x80;
				for(int32_t x = 0; x < 8; x++) {
					int32_t index = 0;
					if(plane[0] & mask) index |= 0x01;
					if(plane[1] & mask) index |= 0x02;

					if(index) {
						index += (sprite[2] & 0x03) * 4;
						line[sprite[3] + 8 + x] = _nativePalette[16 + index];
					}
					mask >>= 1;
				}
			}
		}

		uint16_t* dst = _currentOutputBuffer + (_scanline << 8);
		for(int32_t i = 0; i < 256; i++) {
			dst[i] = line[i + 8];
		}
	}

public:
	Sb2kPpu(NesConsole* console) : NesPpu(console)
	{
		_sb2k = dynamic_cast<Sb2kMapper*>(console->GetMapper());
	}

	//CRTP hooks - famiclone mode behaves exactly like the stock PPU
	__forceinline void StoreSpriteInformation(bool horizontalMirror, bool verticalMirror, uint16_t tileAddr, uint8_t lineOffset, NesSpriteInfo& sprite) {}
	__forceinline void StoreTileInformation() {}
	__forceinline void PushTileInformation() {}
	__forceinline bool RemoveSpriteLimit() { return _console->GetNesConfig().RemoveSpriteLimit; }
	__forceinline bool UseAdaptiveSpriteLimit() { return _console->GetNesConfig().AdaptiveSpriteLimit; }

	void* OnBeforeSendFrame() { return nullptr; }

	__forceinline void ProcessScanline()
	{
		if(!IsNativeMode()) {
			ProcessScanlineImpl();
			return;
		}

		//Native mode: the base class per-cycle pipeline is idle (its rendering flags stay
		//off), so only the frame bookkeeping runs here. The vblank flag and NMI are still
		//set by the base class at the NMI scanline.
		if(_scanline >= 0) {
			if(_cycle == 256) {
				RenderNativeScanline();
			}
		} else if(_cycle == 1) {
			//Pre-render scanline: end of vblank (same as the base class pipeline)
			_statusFlags.VerticalBlank = false;
			_console->GetCpu()->ClearNmiFlag();
		}
	}

	__forceinline void DrawPixel()
	{
		//Famiclone mode only (native mode never runs the base pixel pipeline)
		if(IsRenderingEnabled() || !_paletteBgHackEnabled || ((_videoRamAddr & 0x3F00) != 0x3F00)) {
			uint32_t color = GetPixelColor();
			_currentOutputBuffer[(_scanline << 8) + _cycle - 1] = _paletteRam[color & 0x03 ? color : 0];
		} else {
			_currentOutputBuffer[(_scanline << 8) + _cycle - 1] = _paletteRam[_videoRamAddr & 0x1F];
		}
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if(!IsNativeMode()) {
			return NesPpu<Sb2kPpu>::ReadRam(addr);
		}
		//Native mode: no address mirroring and no open bus; unmapped addresses read 0
		return ReadNativeRegister(addr, false);
	}

	uint8_t PeekRam(uint16_t addr) override
	{
		if(!IsNativeMode()) {
			return NesPpu<Sb2kPpu>::PeekRam(addr);
		}
		return ReadNativeRegister(addr, true);
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		if(!IsNativeMode() || addr == 0x4014) {
			//$4014 sprite DMA works the same in both modes (the DMA unit writes $2004,
			//which lands in the native handler below while in native mode)
			NesPpu<Sb2kPpu>::WriteRam(addr, value);
			return;
		}
		WriteNativeRegister(addr, value);
	}

	void Serialize(Serializer& s) override
	{
		NesPpu<Sb2kPpu>::Serialize(s);

		SV(_ga);
		SV(_nativeControl); SV(_nativeMask); SV(_nativeOamAddr); SV(_reg2008);
		SV(_originX); SV(_originY);
		SV(_toggle2005); SV(_toggle2006);
		SV(_vramReadBuffer);
		SVArray(_nativePalette, sizeof(_nativePalette));
	}
};
