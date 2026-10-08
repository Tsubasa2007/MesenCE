#pragma once
#include "pch.h"
#include "NES/Input/Zapper.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

//The light gun that comes with the City Patrolman cartridge (NES 2.0 expansion device $2F). It
//does not sense the picture: it reports where it points as a pair of coordinates, with its trigger
//and reload buttons, in a 19-bit word shifted out two bits at a time on $4016 bits 0-1.
//
//Each read is taken while $4016 bit 1 is set (the game writes $02, reads, then writes $00): bit 0
//carries the next X bit and bit 1 the next Y bit, low bits first, for 8 reads; then the trigger
//and reload, then whether it points at the screen. A write latches a new word once at least 4096
//CPU cycles have passed since the last one, so the game's burst of ten reads sees one sample.
//The cartridge also wants $4026 bit 4 low before it reads (see Sb2kMapper::ReadCart4026).
//
//It takes the Zapper's two keys: Fire is the trigger, and holding "aim offscreen" points the gun
//away from the screen and presses reload - the game reloads only from that button, one bullet per
//press, not from a shot off the screen.
//
//Ported from the reference emulator's EXPAD_CityPatrolmanGun (from NintendulatorNRS), including its
//8-pixel offset between the pointer and the reported position.
class CityPatrolmanGun : public Zapper
{
private:
	NesConsole* _console;
	uint32_t _shiftReg = 0;
	uint8_t _strobe = 0;
	uint64_t _lastLatchCycle = 0;

protected:
	enum GunButtons { Reload = 1 };

	string GetKeyNames() override
	{
		return "FR";
	}

	void InternalSetStateFromInput() override
	{
		Zapper::InternalSetStateFromInput();
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(GunButtons::Reload, KeyManager::IsKeyPressed(keyMapping.CustomKeys[1]));
		}
	}

	void Latch()
	{
		//Away from the screen it reports $FF for both, which is what the game tests for
		MousePosition pos = GetCoordinates();
		bool onScreen = pos.X >= 0 && pos.Y >= 0;
		uint8_t x = onScreen ? (uint8_t)(pos.X - 8) : 0xFF;
		uint8_t y = onScreen ? (uint8_t)(pos.Y - 8) : 0xFF;
		uint32_t value = 0;
		for(int i = 0; i < 8; i++) {
			value |= ((x >> i) & 0x01) << (i * 2);
			value |= ((y >> i) & 0x01) << (i * 2 + 1);
		}
		if(IsPressed(Zapper::Buttons::Fire)) {
			value |= 0x10000;
		}
		if(IsPressed(GunButtons::Reload)) {
			value |= 0x20000;
		}
		if(onScreen) {
			value |= 0x40000;
		}
		_shiftReg = value;
	}

	void Serialize(Serializer& s) override
	{
		Zapper::Serialize(s);
		SV(_shiftReg);
		SV(_strobe);
		SV(_lastLatchCycle);
	}

public:
	CityPatrolmanGun(NesConsole* console, KeyMappingSet keyMappings) : Zapper(console, ControllerType::CityPatrolmanGun, BaseControlDevice::ExpDevicePort, keyMappings)
	{
		_console = console;
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		uint8_t output = 0;
		if(addr == 0x4016 && (_strobe & 0x02)) {
			output = _shiftReg & 0x03;
			_shiftReg >>= 2;
		}
		return output;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		if(addr != 0x4016) {
			return;
		}
		uint64_t cycle = _console->GetCpu()->GetCycleCount();
		if(cycle - _lastLatchCycle >= 4096) {
			_lastLatchCycle = cycle;
			Latch();
		}
		_strobe = value & 0x02;
	}
};
