#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//BBK/YuXing FD-1 mouse - an EM84502 serial mouse wired to $4016/$4017, shared with the
//SuborKeyboard (keyboard uses $4017 bits 1-4, the mouse uses bit 0). Ported from the
//VirtuaNES-BBK fork (NES/PadEX/EXPAD_BBK_FD1.cpp). The BIOS mouse routine ($F50E):
//  - activates the mouse by reading $4016 nine times in a row,
//  - bit-bangs a command over $4016 writes (SET_REMOTE_MODE $F0 or READ_DATA $EB),
//  - clocks the reply out of $4017 bit 0 as a 32-bit waveform per byte (leading $FA).
class BbkMouse : public BaseControlDevice
{
private:
	enum EmState : uint8_t { Idle = 0, RxCommand = 1, TxData = 2 };

	static constexpr uint8_t CmdSetRemoteMode = 0xF0;
	static constexpr uint8_t CmdReadData = 0xEB;
	static constexpr uint8_t CmdResponse = 0xFA;

	bool _active = false;
	uint8_t _read4016Count = 0;
	uint8_t _read4017Count = 0;
	uint8_t _state = EmState::Idle;
	uint32_t _command = 0;
	uint8_t _bitCount = 0;
	uint32_t _txData = 0;
	uint8_t _txCount = 0;
	uint8_t _data[3] = {};

	//Mouse movement accumulates across frames until the BIOS issues READ_DATA (ClearState()
	//wipes the base coordinate buffer every frame, so it can't be used for this)
	int32_t _accumX = 0;
	int32_t _accumY = 0;

	//Encode a byte into the raw serial waveform: 010p10d10d...10d1011 (LSB first, odd parity)
	static uint32_t GenTxData(uint8_t data)
	{
		uint32_t txd = 0x0B; //start bits 4'b1011
		uint32_t mask = 0x10;
		int parity = 0;
		for(int i = 0; i < 8; i++) {
			if((data & 1) == 0) {
				txd |= mask;
				parity ^= 1;
			}
			txd |= mask << 2;
			mask <<= 3;
			data >>= 1;
		}
		if(parity) {
			txd |= mask;
		}
		mask <<= 2;
		txd |= mask;
		return txd;
	}

protected:
	bool HasCoordinates() override { return true; }
	enum Buttons { Left = 0, Right, Middle };

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_active); SV(_read4016Count); SV(_read4017Count); SV(_state);
		SV(_command); SV(_bitCount); SV(_txData); SV(_txCount);
		SVArray(_data, 3);
		SV(_accumX); SV(_accumY);
	}

	void InternalSetStateFromInput() override
	{
		//Movement accumulates across frames until READ_DATA (buttons are sampled directly at
		//READ_DATA time in PackReadData, so no per-frame button latching is needed here)
		MouseMovement mov = KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity);
		_accumX += mov.dx;
		_accumY += mov.dy;
	}

	void PackReadData()
	{
		int dx = -_accumX; //the drive reports X in the opposite direction
		int dy = _accumY;
		_accumX = 0;
		_accumY = 0;

		uint8_t flag = 0;
		if(dx < 0) { flag |= 0x20; }
		if(dy < 0) { flag |= 0x10; }
		if(dx > 255 || dx < -256) { flag |= 0x80; dx = 0; }
		if(dy > 255 || dy < -256) { flag |= 0x40; dy = 0; }

		//Read the physical mouse buttons straight from the key manager, so the auto-connected
		//device needs no button mapping. Bit layout matches the hardware/BIOS (as fed by the
		//VirtuaNES-BBK fork): right=0x01, middle=0x02, left=0x04 (NOT the unused BBK_MS_* defines).
		uint8_t keys =
			(KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::RightButton) ? 0x01 : 0) |
			(KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::MiddleButton) ? 0x02 : 0) |
			(KeyManager::IsKeyPressed(IKeyManager::BaseMouseButtonIndex + (int)MouseButton::LeftButton) ? 0x04 : 0);

		_data[0] = keys | flag;
		_data[1] = (uint8_t)dy;
		_data[2] = (uint8_t)dx;
	}

public:
	BbkMouse(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::BbkMouse, port, keyMappings)
	{
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		uint8_t data = 0;
		if(addr == 0x4016) {
			if(!_active) {
				_read4016Count++;
				if(_read4016Count == 9) {
					_active = true;
					_state = EmState::Idle;
				}
			}
		} else if(addr == 0x4017) {
			_read4016Count = 0;
			if(_active) {
				switch(_state) {
					case EmState::Idle:
						data |= 0x01;
						break;

					case EmState::RxCommand:
						if((_read4017Count & 1) == 0) {
							data |= 0x01;
						}
						_read4017Count++;
						break;

					case EmState::TxData:
						if(_read4017Count < 32) {
							if(_txData & (1u << _read4017Count)) {
								data |= 0x01;
							}
							_read4017Count++;
						} else {
							_read4017Count = 0;
							_txCount--;
							if(_txCount > 0) {
								_txData = GenTxData(_data[3 - _txCount]);
							} else {
								_active = false;
								_state = EmState::Idle;
							}
						}
						break;
				}
			}
		}
		return data;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		if(addr != 0x4016) {
			return;
		}

		_read4016Count = 0;
		if(!_active) {
			return;
		}

		switch(_state) {
			case EmState::Idle:
				if(value == 0x05) {
					_read4017Count = 0;
					_command = 0;
					_bitCount = 0;
					_state = EmState::RxCommand;
				}
				break;

			case EmState::RxCommand:
				if(value & 0x04) {
					_command |= (value & 0x01) ? 0 : (1u << _bitCount);
					_bitCount++;
					if(_bitCount == 22) {
						uint8_t cmd = _command & 0xFF;
						_txData = GenTxData(CmdResponse);
						if(cmd == CmdSetRemoteMode) {
							_txCount = 1;
						} else if(cmd == CmdReadData) {
							PackReadData();
							_txCount = 4;
						} else {
							_txCount = 1;
						}
						_read4017Count = 0;
						_bitCount = 0;
						_state = EmState::TxData;
					}
				}
				break;

			case EmState::TxData:
				break;
		}
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
			{ "middle", Buttons::Middle },
		};
	}
};
