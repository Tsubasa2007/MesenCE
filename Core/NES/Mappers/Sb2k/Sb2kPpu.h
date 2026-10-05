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
//
//The UM6578 game cartridges (mapper 405) run in native mode from power-on and need what
//the reference emulator's other model of this chip (its 2C02 core in UM6576 mode) draws:
//flipped sprites, sprites behind the background, the sprite 0 hit flag, colour 0 of every
//background palette showing the backdrop, and $2008 bit 0's single name table page. Those
//are done for the cartridges only, so the SB-2000 draws as it always has.
//
//The cartridges also scroll the way that model does: a 2C02-style address register (v/t)
//that $2000, $2005 and $2006 load, copied to the start of each line and stepped down the
//page, with 32 rows per page (no attribute rows) and each 16-bit tile name read from
//(v & $FFF) * 2. A $2006 write part way down the frame therefore moves the picture - which is
//how the cartridges' games put their status panels below the play field.
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
	bool _nativeSprite0Hit = false; //UM6578 cartridges only - see the class comment
	uint16_t _cartV = 0; //UM6578 cartridges only: the rendering address (2C02 "v")
	uint16_t _cartT = 0; //and its reload value ("t")
	uint8_t _cartFineX = 0; //fine X scroll from the first $2005 write

	//The model's vertical step: 32 rows to a page, then on to the page below
	void IncrementCartRow()
	{
		if((_cartV & 0x7000) != 0x7000) {
			_cartV += 0x1000;
		} else {
			_cartV &= 0x8FFF;
			if((_cartV & 0x03E0) == 0x03E0) {
				_cartV = (_cartV ^ 0x0800) & 0xFC1F;
			} else {
				_cartV += 0x0020;
			}
		}
	}

	__forceinline bool IsNativeMode() { return _sb2k->IsUm6576Mode(); }

	uint8_t ReadNativeRegister(uint16_t addr, bool forDebugger)
	{
		switch(addr) {
			case 0x2002: {
				uint8_t status = (_statusFlags.VerticalBlank ? 0x80 : 0) | (_nativeSprite0Hit ? 0x40 : 0);
				if(!forDebugger) {
					_statusFlags.VerticalBlank = false;
					if(!_sb2k->IsUm6578Cart()) {
						_console->GetCpu()->ClearNmiFlag();
					}
					//The cartridges: a read on the dot the flag rises does not take back the NMI,
					//as it does on a 2C02 (the reference emulator never takes it back either). A
					//game polling $2002 with its NMI on otherwise lost a frame's NMI whenever a
					//poll landed on that dot, and its split screen jumped for that frame.
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
				_cartT = (_cartT & 0xF3FF) | ((value & 0x03) << 10);
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
					_cartT = (_cartT & 0x8C1F) | ((value & 0xF8) << 2) | ((value & 0x07) << 12);
				} else {
					_originX = value;
					_cartT = (_cartT & 0xFFE0) | (value >> 3);
					_cartFineX = value & 0x07;
				}
				_toggle2005 = !_toggle2005;
				break;

			case 0x2006:
				if(_toggle2006) {
					_ga = (_ga & 0xFF00) | value;
					_cartT = (_cartT & 0xFF00) | value;
					_cartV = _cartT;
				} else {
					_ga = (_ga & 0x00FF) | (value << 8);
					//All 8 bits, unlike the 2C02's 6
					_cartT = (_cartT & 0x00FF) | (value << 8);
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
		bool bgOpaque[272] = {};
		bool cart = _sb2k->IsUm6578Cart();

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

			int32_t y = cart ? ((_cartV >> 12) & 0x07) : ((_scanline + _originY) & 0x07);
			uint8_t fineX = cart ? _cartFineX : (_originX & 0x07);
			uint16_t v = _cartV;
			int32_t dstX = 8;

			for(int32_t i = 0; i < 33; i++) {
				int32_t tileX = (_originX / 8) + i;

				int32_t tileAddr;
				if(cart) {
					//From the address register (see the class comment). Page mode: one 2KB
					//name table at $2000, whatever the page bits say.
					tileAddr = (v & 0x0FFF) << 1;
					if(_reg2008 & 0x01) {
						tileAddr = (tileAddr & 0x7FF) | 0x2000;
					}
					v = ((v & 0x1F) == 0x1F) ? (v ^ 0x041F) : (v + 1);
				} else if(tileX & 0x20) {
					//Crossed the right edge into the horizontally adjacent page
					tileAddr = tileBase + 2048 + tileY * 64 + (tileX & 0x1F) * 2;
				} else {
					tileAddr = tileBase + tileY * 64 + tileX * 2;
				}

				uint16_t tileName = _sb2k->VideoRead((uint16_t)(tileAddr + 1)) * 256 + _sb2k->VideoRead((uint16_t)tileAddr);

				uint8_t mask = i ? 0x80 : (0x80 >> fineX);
				int32_t tileWidth;
				if(i == 0) {
					tileWidth = 8 - fineX;
				} else if(i == 32) {
					tileWidth = fineX;
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

						bgOpaque[dstX] = index != 0;
						line[dstX++] = _nativePalette[(cart && !index) ? 0 : tileBank * 4 + index];
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

						bgOpaque[dstX] = index != 0;
						line[dstX++] = _nativePalette[(cart && !index) ? 0 : tileBank * 4 + index];
						mask >>= 1;
					}
				}
			}
		} else {
			memset(line, cart ? _nativePalette[0] : 0, sizeof(line));
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
				bool behind = (sprite[2] & 0x20) != 0;
				if(behind && !cart) {
					//Behind-background sprites are not drawn (reference behavior)
					continue;
				}

				int32_t y = _scanline - spriteY;
				if(cart && (sprite[2] & 0x80)) {
					y = spriteHeight - 1 - y; //vertical flip
				}
				int32_t tileOffset = patternBase + sprite[1] * 16;
				if((_nativeControl & 0x20) && cart) {
					//8x16 on the cartridges: an even/odd tile pair, as the reference
					//emulator's model of the chip for them reads it
					tileOffset = patternBase + (sprite[1] & 0xFE) * 16;
					if(y >= 8) {
						tileOffset += 16;
						y -= 8;
					}
				} else if(_nativeControl & 0x20) {
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

				bool hFlip = cart && (sprite[2] & 0x40);
				for(int32_t x = 0; x < 8; x++) {
					uint8_t mask = hFlip ? (0x01 << x) : (0x80 >> x);
					int32_t index = 0;
					if(plane[0] & mask) index |= 0x01;
					if(plane[1] & mask) index |= 0x02;

					if(index) {
						int32_t px = sprite[3] + 8 + x;
						if(cart && i == 0 && bgOpaque[px] && px < 8 + 255) {
							_nativeSprite0Hit = true;
						}
						if(!behind || !bgOpaque[px]) {
							line[px] = _nativePalette[16 + index + (sprite[2] & 0x03) * 4];
						}
					}
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
		bool cartScroll = (_nativeMask & 0x18) && _sb2k->IsUm6578Cart();
		if(_scanline >= 0) {
			if(_cycle == 256) {
				RenderNativeScanline();
				if(cartScroll) {
					//Next row, then the start of the line from t - as the 2C02 at dots 256/257
					IncrementCartRow();
					_cartV = (_cartV & 0xFBE0) | (_cartT & 0x041F);
				}
			}
		} else if(_cycle == 1) {
			//Pre-render scanline: end of vblank (same as the base class pipeline)
			_statusFlags.VerticalBlank = false;
			_nativeSprite0Hit = false;
			_console->GetCpu()->ClearNmiFlag();
		} else if(_cycle == 304 && cartScroll) {
			_cartV = _cartT;
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
		SV(_nativeSprite0Hit);
		SV(_cartV); SV(_cartT); SV(_cartFineX);
	}
};
