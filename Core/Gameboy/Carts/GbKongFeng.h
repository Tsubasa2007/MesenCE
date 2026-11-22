#pragma once
#include "pch.h"
#include "Gameboy/Carts/GbCart.h"
#include "Gameboy/GbMemoryManager.h"
#include "Utilities/Serializer.h"
#include "Utilities/HexUtilities.h"

//Kong Feng's board for its mainland China release of a Vast Fame Game Boy Color RPG (header cart
//type $1B). An MBC5 board with a challenge/response chip in the $6000-$7FFF space:
//
//  $7081 (write x4)  A 4-byte key. It selects a ROM bank: the game's "bank" numbers $F0-$FF are
//                    keys from its table at $286C, and it then jumps into $4001 of the new bank.
//  $7080 (write)     Bytes for the chip - the game sends a stretch of its own code, then one
//                    challenge byte (always $80-$BF)
//  $A000-$BFFF (read, cart RAM disabled)  The reply to the last challenge byte
//  $7080 = $31 after a reply ends the exchange
//
//The reply is F[challenge] ^ G, F being a fixed 64-entry table of a 4-bit result spread over 8
//bits, and G following the number of bytes sent before the challenge - a second challenge in
//the same exchange gets the same G. Both tables are worked out from the game itself: Vast
//Fame's English release of the same build (on a GGB-81 board) has no chip, so its scripts hold
//the plain values this one's hold encoded, its code calls the routines and banks this one asks
//the chip for, and its scripts hold the plain pointers this one gets from the chip's data
//stubs (one script table in bank $0A uses stubs 5-14, with the byte counts the other calls
//never send). Every exchange the game makes is covered. The bank of a key that isn't one of
//the four below is unknown.
class GbKongFeng : public GbCart
{
private:
	static constexpr uint8_t _replyTable[64] = {
		0x00, 0x34, 0xE4, 0x7A, 0xAA, 0xDD, 0x00, 0xD0,
		0x4E, 0x4E, 0xDD, 0x4E, 0xDD, 0x93, 0xAA, 0xDD,
		0x93, 0x77, 0x9E, 0x34, 0x39, 0x7A, 0x93, 0x39,
		0xE9, 0xAA, 0x39, 0x00, 0xE4, 0x4E, 0xA7, 0x0D,
		0xE9, 0x39, 0x4E, 0xE4, 0x34, 0xAA, 0x43, 0xE9,
		0x39, 0x0D, 0x7A, 0xD0, 0x34, 0xA7, 0x77, 0xDD,
		0x34, 0x9E, 0xD0, 0xE9, 0xAA, 0x4E, 0xE4, 0xA7,
		0x0D, 0x43, 0x00, 0xE4, 0x39, 0x7A, 0x34, 0x4E,
	};

	struct KeyBank
	{
		uint32_t Key;
		uint8_t Bank;
	};

	static constexpr KeyBank _keyBanks[4] = {
		{ 0x30443E35, 0x04 },
		{ 0x607E7887, 0x08 },
		{ 0xA0BEA3FF, 0x05 },
		{ 0x213F25DF, 0x06 },
	};

	bool _ramEnabled = false;
	uint16_t _prgBank = 1;
	uint8_t _ramBank = 0;

	uint32_t _key = 0;
	uint8_t _keyCount = 0;
	uint8_t _sentCount = 0;
	uint8_t _challenge = 0;
	bool _replied = false;
	uint8_t _replyMask = 0;

	static int GetReplyMask(uint8_t sentCount)
	{
		//By the number of bytes sent before the challenge: 3 and 19 give the same value
		switch(sentCount & 0x0F) {
			case 0x00: return 0x20;
			case 0x03: return 0x82;
			case 0x06: return 0xA8;
			case 0x09: return 0x0A;
			case 0x0A: return 0x08;
			case 0x0B: return 0x02;
			case 0x0C: return 0x00;
			case 0x0D: return 0x88;
			case 0x0F: return 0x02;
		}
		return -1;
	}

	uint8_t GetReply()
	{
		if(!_replied) {
			_replied = true;
			int mask = GetReplyMask(_sentCount - 1);
			if(mask < 0) {
				MessageManager::Log("[Kong Feng] Unknown reply for " + std::to_string(_sentCount - 1) + " bytes sent");
				mask = 0;
			}
			_replyMask = (uint8_t)mask;
		}
		if(_challenge < 0x80 || _challenge >= 0xC0) {
			MessageManager::Log("[Kong Feng] Unknown reply for challenge $" + HexUtilities::ToHex(_challenge));
		}
		return _replyTable[_challenge & 0x3F] ^ _replyMask;
	}

	void EndExchange()
	{
		_sentCount = 0;
		_replied = false;
	}

	void WriteKey(uint8_t value)
	{
		EndExchange();
		_key = (_key << 8) | value;
		if(++_keyCount < 4) {
			return;
		}
		_keyCount = 0;
		for(const KeyBank& entry : _keyBanks) {
			if(entry.Key == _key) {
				_prgBank = entry.Bank;
				return;
			}
		}
		//The game also sends keys ahead of its checks, which then read the bank they pick - but
		//only the byte count matters to the reply, and the game puts its bank back afterwards
		LogDebug("[Kong Feng] Key without a known bank: $" + HexUtilities::ToHex32(_key));
	}

	void WriteChip(uint8_t value)
	{
		_keyCount = 0;
		if(_replied) {
			if(value == 0x31) {
				EndExchange();
			} else {
				_challenge = value;
			}
		} else {
			_challenge = value;
			if(_sentCount < 0xFF) {
				_sentCount++;
			}
		}
	}

public:
	static bool IsKongFengCart(vector<uint8_t>& prgRom)
	{
		//The bank-switch handler that sends "banks" $F0-$FF to the chip as keys
		static constexpr uint8_t handler[] = { 0xFE, 0xF0, 0x38, 0xD3, 0xE6, 0x0F, 0x87, 0x87, 0x01, 0x6C, 0x28 };
		return prgRom.size() >= 0x8000 && memcmp(&prgRom[0x60], handler, sizeof(handler)) == 0;
	}

	uint32_t GetCartRamSize(uint32_t headerSize) override
	{
		//The header says 2 KB, but the game keeps its saves at $A640, $AF40 and $B000
		return std::max<uint32_t>(headerSize, 0x2000);
	}

	void InitCart() override
	{
		_memoryManager->MapRegisters(0x0000, 0x7FFF, RegisterAccess::Write);
	}

	void RefreshMappings() override
	{
		constexpr int prgBankSize = 0x4000;
		constexpr int ramBankSize = 0x2000;

		Map(0x0000, 0x3FFF, GbMemoryType::PrgRom, 0, true);
		Map(0x4000, 0x7FFF, GbMemoryType::PrgRom, _prgBank * prgBankSize, true);

		if(_ramEnabled) {
			Map(0xA000, 0xBFFF, GbMemoryType::CartRam, _ramBank * ramBankSize, false);
			_memoryManager->MapRegisters(0xA000, 0xBFFF, RegisterAccess::None);
		} else {
			Unmap(0xA000, 0xBFFF);
			_memoryManager->MapRegisters(0xA000, 0xBFFF, RegisterAccess::Read);
		}
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		//With the cart RAM disabled, $A000-$BFFF answers the chip's reply - open bus otherwise
		return _sentCount ? GetReply() : 0xFF;
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		switch(addr & 0x7000) {
			case 0x0000:
			case 0x1000:
				_ramEnabled = (value == 0x0A);
				break;

			case 0x2000:
				_prgBank = (value & 0xFF) | (_prgBank & 0x100);
				break;

			case 0x3000:
				_prgBank = (_prgBank & 0xFF) | ((value & 0x01) << 8);
				break;

			case 0x4000:
			case 0x5000:
				_ramBank = value & 0x0F;
				break;

			case 0x7000:
				if(addr & 0x01) {
					WriteKey(value);
				} else {
					WriteChip(value);
				}
				break;
		}
		RefreshMappings();
	}

	void Serialize(Serializer& s) override
	{
		SV(_ramEnabled);
		SV(_prgBank);
		SV(_ramBank);
		SV(_key);
		SV(_keyCount);
		SV(_sentCount);
		SV(_challenge);
		SV(_replied);
		SV(_replyMask);
	}
};
