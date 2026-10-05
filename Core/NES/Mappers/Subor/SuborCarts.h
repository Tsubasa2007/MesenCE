#pragma once
#include "pch.h"

//The Subor (小霸王) learning cartridges. They sit on several boards - mappers 177, 178, 241 and 518,
//Subor168 and Subor560 - and their headers say nothing about the machine around them, so each is
//known by its PRG CRC32. All of them want what the Subor Windows 2002 wants (see
//SuborWindows2002): Dendy timing, the cut-down Subor keyboard on $4016/$4017 and the 24-bit serial
//mouse answering on $4016. The reference emulator gives every one of these the same keyboard and
//mouse.
//
//The 18-pin expansion cards (数奇王) run inside these machines too. One is not a dump of a single
//chip: built as a 1MB mapper 178 image - the Subor V15.0 V2 ROM as the host in the lower 512KB,
//where the card's label font and its "exit to host" target are, and the card above it, where it
//runs. The other cards boot on their own as 512KB mapper 178 images, but they too read their
//label font from the host, so each also comes as a host image the same way. All of them scan the
//same 10-row keyboard and bring their own mouse, a PS/2 one (see Ps2Mouse).
class SuborCarts
{
public:
	static constexpr uint32_t Ps2MouseCard = 0x90257DA1;

	//The expansion cards, whose mouse is the PS/2 one instead of the 24-bit serial mouse
	static bool HasPs2Mouse(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x5E660855: //multimedia card 1 in a V15.0 V2 host
			case 0xFDA673F6: //multimedia card 2 in a V15.0 V2 host
			case 0xBD8ACE09: //multimedia card 4 in a V15.0 V2 host
			case 0x0D4F90B2: //Windows 98 practice card in a V15.0 V2 host
			case Ps2MouseCard: //voice-mouse card in a V15.0 V2 host
			case 0x7E432D70: //multimedia card 1
			case 0x14774592: //multimedia card 2
			case 0x4DC15B83: //multimedia card 4
			case 0xE520960E: //Windows 98 practice card
				return true;

			default:
				return false;
		}
	}

	//The cartridges whose BIOS scans 13 rows: they read row 9's extended-keyboard bit and then rows
	//10-12. V1.0, V1.1, V3 and V8.0 stop at row 8, and V15.0 / V15.0 V2 (and the card's host) step to row 9
	//but take anything there for a key: they skip their splash screen and see a held key unless
	//row 9 is the reduced keyboard's empty gap. V12.0 scans 12 rows, and its key table gives row 9's
	//extended-keyboard bit no key, so it wants that bit there too.
	static bool HasExtendedKeyboard(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x41401C6D: //V4
			case 0x40A4C574: //V5
			case 0xFDD9321C: //V5 with add-ons
			case 0xCA501706:
			case 0x85068811: //V6
			case 0x6F84076D: //V6+
			case 0x04260DBC: //V7.0
			case 0x79C85E71: //V7.1
			case 0xE475D89A: //V9.0
			case 0x900D9E00: //V9.1
			case 0x12D61CE8: //V11
			case 0x9EA48F04: //V12.0
			case 0x5F693117: //V13.0
			case 0xF18BC238: //V14.0
				return true;

			default:
				return false;
		}
	}

	//The cartridges that read $4016 bit 0 only to wait for the printer: nothing in their menus,
	//WPS, lessons or add-on cards reads a pad or mouse there (F-BASIC's pad is on bit 1), so the
	//printer can stay attached whatever the printer setting says
	static bool HasPrinterAlways(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x40A4C574: //V5
			case 0xFDD9321C: //V5 with add-ons
			case 0xCA501706:
				return true;

			default:
				return false;
		}
	}

	//The cartridges carrying SB DOS (Copyright by SUBOR, 1996) for the Subor floppy drive: it reads
	//1.44MB PC-format disks and runs the Subor .EXE/.COM programs on them. That is the V5 and its
	//add-on images - the same ones as HasPrinterAlways.
	static bool HasFloppyDrive(uint32_t prgCrc)
	{
		return HasPrinterAlways(prgCrc);
	}

	static bool IsSuborCart(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0xC07ADC88: //V1.0 (Subor560)
			case 0xE4460DF2: //V1.1 (mapper 241)
			case 0x0930349E: //V3 (mapper 241)
			case 0x41401C6D: //V4 (mapper 167)
			case 0x40A4C574: //V5 (mapper 518)
			case 0xFDD9321C: //V5 with a 1MB add-on (mapper 518)
			case 0xCA501706: //V5 with another 1MB add-on (mapper 518)
			case 0x85068811: //V6 (mapper 241)
			case 0x6F84076D: //V6+ (mapper 178)
			case 0x04260DBC: //V7.0 (Subor168)
			case 0x79C85E71: //V7.1 (Subor168)
			case 0xF58761D0: //V8.0 (mapper 177)
			case 0xE475D89A: //V9.0 (mapper 241)
			case 0x900D9E00: //V9.1 (mapper 241)
			case 0x12D61CE8: //V11 (mapper 518)
			case 0x9EA48F04: //V12.0, Windows 2000 (mapper 241)
			case 0x5F693117: //V13.0 (mapper 518)
			case 0xF18BC238: //V14.0, 视窗系统2000 (mapper 178)
			case 0x6058DB1C: //V15.0, Windows 2002 (mapper 177)
			case 0x9B004BF8: //V15.0 V2, Windows 2005 (mapper 178)
			case 0x5E660855: //multimedia card 1 in a V15.0 V2 host
			case 0xFDA673F6: //multimedia card 2 in a V15.0 V2 host
			case 0xBD8ACE09: //multimedia card 4 in a V15.0 V2 host
			case 0x0D4F90B2: //Windows 98 practice card in a V15.0 V2 host
			case Ps2MouseCard: //expansion card in a V15.0 V2 host (mapper 178)
			case 0x7E432D70: //expansion cards on their own (mapper 178)
			case 0x14774592:
			case 0x4DC15B83:
			case 0xE520960E:
				return true;

			default:
				return false;
		}
	}
};
