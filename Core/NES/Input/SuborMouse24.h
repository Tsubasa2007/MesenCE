#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/KeyManager.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

//The 24-bit Subor serial mouse, as carried by the Subor Windows 2002 machine.
//Packet layout after NintendulatorNRS (NewRisingSun), by way of the VirtuaNES-BBK fork.
//
//This is NOT the protocol SuborMouse implements. That device sends one or three tagged bytes
//($01/$02/$03 in the low bits) and clamps its movement to 31; this one sends a single 24-bit
//packet and clamps to 127. The two are different enough that they cannot share a state
//machine, and both are in use on real hardware, so they are separate devices.
//
//The packet is assembled when the guest writes a value with the low three bits equal to 1,
//and is then shifted out MSB first over the next 24 reads. Reads past the end of the packet
//return nothing at all - the control manager ORs open bus into the port value on its own, so a
//device that forced bit 6 high the way the reference does would be setting it twice:
//
//  bits 0-6    Y magnitude, 0-127
//  bits 8-14   X magnitude, 0-127
//  bit 16      X moved right        bit 17   X moved left
//  bit 18      Y moved down         bit 19   Y moved up
//  bit 21      set when either axis moved by more than one - the guest uses it to pick a
//              coarser pointer step, so a slow drag and a flick are told apart
//  bits 22-23  buttons
//
//Movement is accumulated rather than sampled: whatever is reported in a packet is subtracted
//from the running total, so a movement larger than the 127 the packet can carry is delivered
//over as many packets as it takes instead of being thrown away.
class SuborMouse24 : public BaseControlDevice
{
private:
	uint32_t _bits = 0;
	uint8_t _bitPos = 24;
	int32_t _xMovement = 0;
	int32_t _yMovement = 0;

protected:
	bool HasCoordinates() override { return true; }

	enum Buttons
	{
		Left = 0,
		Right,
		Middle
	};

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_bits);
		SV(_bitPos);
		SV(_xMovement);
		SV(_yMovement);
	}

	void InternalSetStateFromInput() override
	{
		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Left, KeyManager::IsKeyPressed(keyMapping.CustomKeys[0]));
			SetPressedState(Buttons::Right, KeyManager::IsKeyPressed(keyMapping.CustomKeys[1]));
		}
		SetMovement(KeyManager::GetMouseMovement(_emu, _emu->GetSettings()->GetInputConfig().MouseSensitivity));
	}

	//GetMovement() hands over what has arrived since it was last called and zeroes it, so this
	//is safe to call as often as it likes - it drains, it does not sample.
	void AccumulateMovement()
	{
		//Halving the delta matches the step the guest's own pointer code expects; the low bit is
		//kept so that a one-pixel move is still a move rather than being rounded away.
		MouseMovement mov = GetMovement();
		_xMovement += (mov.dx >> 1) | (mov.dx & 1);
		_yMovement += (mov.dy >> 1) | (mov.dy & 1);
	}

	void RefreshStateBuffer() override
	{
		AccumulateMovement();
	}

public:
	SuborMouse24(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::SuborMouse24, port, keyMappings)
	{
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
		StrobeProcessWrite(value);

		//Drain before the test, so movement that arrived earlier in this frame is in the
		//accumulator by the time the guest asks for a packet
		AccumulateMovement();

		if((value & 0x07) != 0x01) {
			return;
		}

		int32_t amountX = std::min(std::abs(_xMovement), 127);
		int32_t amountY = std::min(std::abs(_yMovement), 127);

		_bits = 0;
		if(_yMovement < 0) {
			_bits |= 0x080000 | amountY;
			_yMovement += amountY;
		} else if(_yMovement > 0) {
			_bits |= 0x040000 | amountY;
			_yMovement -= amountY;
		}

		if(_xMovement < 0) {
			_bits |= 0x020000 | (amountX << 8);
			_xMovement += amountX;
		} else if(_xMovement > 0) {
			_bits |= 0x010000 | (amountX << 8);
			_xMovement -= amountX;
		}

		if(amountX > 1 || amountY > 1) {
			_bits |= 0x200000;
		}

		uint32_t buttons = (IsPressed(Buttons::Left) ? 0x02 : 0) | (IsPressed(Buttons::Right) ? 0x01 : 0);
		_bits |= buttons << 22;
		_bitPos = 0;
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		if((addr == 0x4016 && (_port & 0x01) == 0) || (addr == 0x4017 && (_port & 0x01) == 1)) {
			if(_bitPos < 24) {
				return (uint8_t)((_bits >> (23 - _bitPos++)) & 0x01);
			}
		}
		return 0;
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "xOffset", BaseControlDevice::DeviceXCoordButtonId, true },
			{ "yOffset", BaseControlDevice::DeviceYCoordButtonId, true },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right }
		};
	}
};
