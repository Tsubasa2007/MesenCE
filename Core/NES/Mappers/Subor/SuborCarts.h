#pragma once
#include "pch.h"

//The Subor (小霸王) learning cartridges. They sit on several boards - mappers 177, 178, 241 and 518,
//Subor168 and Subor560 - and their headers say nothing about the machine around them, so each is
//known by its PRG CRC32. All of them want Dendy timing and the cut-down Subor keyboard on
//$4016/$4017; the ones with a mouse driver also want the 24-bit serial mouse answering on $4016 (see
//HasSerialMouse). The reference emulator gives every one of these the same keyboard and mouse.
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

	//The expansion cards, whose mouse is the PS/2 one instead of the 24-bit serial mouse - and other
	//makers' learning cartridges with the same 10-row keyboard scan and PS/2 driver ($EB, then a
	//check for the $FA acknowledge, the data line driven through $4016 bit 0)
	static bool HasPs2Mouse(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x5DD45CFA: //Kingwon learning cartridge (mapper 241)
			case 0x5E660855: //multimedia card 1 in a V15.0 V2 host
			case 0xFDA673F6: //multimedia card 2 in a V15.0 V2 host
			case 0xBD8ACE09: //multimedia card 4 in a V15.0 V2 host
			case 0x0D4F90B2: //Windows 98 practice card in a V15.0 V2 host
			case Ps2MouseCard: //voice-mouse card in a V15.0 V2 host
			case 0x7E432D70: //multimedia card 1
			case 0x14774592: //multimedia card 2
			case 0x4DC15B83: //multimedia card 4
			case 0xE520960E: //Windows 98 practice card
			case 0x8B23F1AA: //other makers' learning cartridges (mapper 241)
			case 0x77E83AE2: //(mapper 178)
			case 0x488482C0: //(mapper 241)
			case 0x24DC1303: //(mapper 241)
			case 0x6DAD612B: //(mapper 241)
			case 0xD58CDBDA: //(mapper 178)
			case 0x6AA52ED1: //(mapper 241)
			case 0x5CE9864D: //(mapper 241)
			case 0x85FD927E: //(mapper 178)
			case 0x0BEB8BCD: //the previous one as a mapper 241 image
			case 0xBACB4CDE: //(mapper 241)
			case 0x6922D4CE: //(mapper 133, with the bank register at $6001)
				return true;

			default:
				return false;
		}
	}

	//Two of those read the packet the other way round (see Ps2Mouse)
	static bool HasPs2MouseXFirst(uint32_t prgCrc)
	{
		return prgCrc == 0x6DAD612B || prgCrc == 0x5CE9864D;
	}

	//The cartridges with a driver for the 24-bit serial mouse (a 24-read loop on $4016). The others
	//never read one - their $4016 reads are the data recorder (bit 1), the printer and a joypad - so
	//they get a joypad in the first port instead.
	static bool HasSerialMouse(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x1A13AB55: //another maker's learning cartridge (mapper 178)
			case 0x34D9C9F2: //another maker's learning cartridge (mapper 241, 1MB)
			case 0x935CE367: //another maker's learning cartridge (mapper 241, 1MB)
			case 0xF58761D0: //V8.0
			case 0x12D61CE8: //V11
			case 0x9EA48F04: //V12.0
			case 0x5F693117: //V13.0
			case 0xF18BC238: //V14.0
			case 0x6058DB1C: //V15.0
			case 0x9B004BF8: //V15.0 V2
				return true;

			default:
				return false;
		}
	}

	//The cartridges with the LPC speech chip at $5000-$5FFF (see Mapper241): their speech routine
	//waits for bit 6 of $5FF0 and sends each byte as two $5x nibbles, low first
	static bool HasLpcSpeech(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x2B74253D: //another maker's learning cartridge (mapper 241)
			case 0x33A93F6E: //another maker's, with the Kingwon-type keyboard (mapper 241)
			case 0x8FA597F8: //typing and dictionary cartridge (mapper 241)
			case 0x957ABE27: //Wubi typing and English cartridge (mapper 241)
				return true;

			default:
				return false;
		}
	}

	//One of those whose mouse sends its two counts inverted (see SuborMouse24)
	static bool HasInvertedMouseCounts(uint32_t prgCrc)
	{
		return prgCrc == 0x34D9C9F2;
	}

	//One cartridge reads the same 24-bit mouse in the second port instead: its 24-read loop is on
	//$4017, bit 0 beside the keyboard's bits 1-4
	static bool HasSerialMouseOnPort2(uint32_t prgCrc)
	{
		return prgCrc == 0x328A2876; //another maker's learning cartridge (mapper 178)
	}

	//A study card from another maker whose menus and programs read the Family BASIC keyboard's
	//matrix instead of the Subor one (the letters pick its menu entries), on a Dendy-timed machine
	static bool HasFamilyBasicKeyboard(uint32_t prgCrc)
	{
		return prgCrc == 0x9D91BF0F; //mapper 241
	}

	//Three cartridges wait for vblank in a loop the Dendy frame divides exactly - the karaoke one's song
	//start polls $2002 with its NMI off in an 8-cycle loop, the Wubi one's text editor and the study
	//card's menu (after leaving its editor) with their NMI on -
	//so the read lands on the dot that suppresses the flag in every frame and they wait for ever. One
	//more line before the NMI (313 lines, as the YuXing machines have) breaks the lock. Not measured
	//on the hardware; PAL timing frees them too but lowers the music by about 6%.
	static bool HasLongDendyFrame(uint32_t prgCrc)
	{
		return prgCrc == 0x0A9808AE || prgCrc == 0x957ABE27 || //karaoke, Wubi typing
			prgCrc == 0x9D91BF0F; //study card with the Family BASIC keyboard
	}

	//The cartridges with the cut-down keyboard whose text editor reads its Alt modifier from row 1
	//bit 3, where the full keyboard has End (see SuborKeyboard)
	static bool HasAltAtEnd(uint32_t prgCrc)
	{
		switch(prgCrc) {
			case 0x5F2FA921: //its help page lists Alt+F1-F4
			case 0x5C422ED2:
			case 0x6B03AC00:
			case 0xB72B17ED:
			case 0x389785A0:
			case 0xD58CDBDA:
			case 0x5CE9864D:
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
			case 0xD3113B3F: //V7.1 with an add-on
			case 0xE475D89A: //V9.0
			case 0x900D9E00: //V9.1
			case 0x12D61CE8: //V11
			case 0x9EA48F04: //V12.0
			case 0x5F693117: //V13.0
			case 0xF18BC238: //V14.0
			case 0x0A9329B5: //puzzle cartridge, part A
			case 0x15058291: //puzzle cartridge, part B
			case 0x8B265862: //English vocabulary cartridge
			case 0x36A8FDF6: //astronomy and household-accounts cartridge
			case 0xD3906F32: //third-grade maths cartridge
			case 0x60E33D27: //character-code input cartridge
			case 0x669A32F2: //idioms and maze cartridge
			case 0x957ABE27: //Wubi typing and English cartridge
				return true;

			default:
				return false;
		}
	}

	//A cartridge from another maker that scans the Kingwon-type keyboard instead (see Mapper241)
	static bool HasKingwonKeyboard(uint32_t prgCrc)
	{
		return prgCrc == 0x33A93F6E; //(mapper 241)
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
			case 0x366C20D7: //LOGO V1.0 (SuborLogo)
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
			case 0xD3113B3F: //V7.1 with a 1MB add-on (Subor168)
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
			//Single-subject learning cartridges: the same keyboard scan, and no other way in
			case 0x0A9329B5: //puzzle cartridge, part A (mapper 241)
			case 0x15058291: //puzzle cartridge, part B (mapper 241)
			case 0x8B265862: //English vocabulary cartridge (mapper 167)
			case 0x36A8FDF6: //astronomy and household-accounts cartridge (mapper 241)
			case 0xD3906F32: //third-grade maths cartridge (mapper 241)
			case 0x60E33D27: //character-code input cartridge (mapper 167)
			case 0x669A32F2: //idioms and maze cartridge (mapper 241)
			case 0x8FA597F8: //typing and dictionary cartridge (mapper 241)
			case 0x957ABE27: //Wubi typing and English cartridge (mapper 241)
			case 0x51C41A5A: //16-in-1 study card from another maker (mapper 241)
			case 0x5DD45CFA: //Kingwon learning cartridge, with a PS/2 mouse (mapper 241)
			//Other makers' learning cartridges with the same 10-row keyboard scan. Most bring a
			//mouse too (see HasPs2Mouse, HasSerialMouse and HasSerialMouseOnPort2); these ten don't.
			case 0xE406218A: //(mapper 241)
			case 0xCC330906: //(mapper 241)
			case 0x2726FC11: //(mapper 178)
			case 0xE90A6AEB: //(mapper 241)
			case 0x2B74253D: //(mapper 241)
			case 0xB72B17ED: //(mapper 178)
			case 0x389785A0: //the previous one as a mapper 241 image
			case 0x5F2FA921: //(mapper 241)
			case 0x5C422ED2: //(mapper 241)
			case 0x6B03AC00: //(mapper 241)
			case 0x8B23F1AA: //PS/2 mouse
			case 0x77E83AE2:
			case 0x488482C0:
			case 0x24DC1303:
			case 0x6DAD612B:
			case 0xD58CDBDA:
			case 0x6AA52ED1:
			case 0x5CE9864D:
			case 0x85FD927E:
			case 0x0BEB8BCD:
			case 0xBACB4CDE:
			case 0x1A13AB55: //24-bit mouse
			case 0x34D9C9F2:
			case 0x328A2876: //24-bit mouse in the second port
			case 0x935CE367: //24-bit mouse
			case 0x6922D4CE: //PS/2 mouse (mapper 133)
				return true;

			default:
				return false;
		}
	}
};
