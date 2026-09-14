#pragma once
#include "pch.h"
#include "Shared/BaseControlManager.h"
#include "Shared/CpuType.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "SNES/Input/SnesController.h"

//The machine's two pads have the same twelve buttons as a SNES pad. Until the console has input
//settings of its own, each port is a SNES pad taken from the SNES controller settings.
class SacControlManager final : public BaseControlManager
{
private:
	SnesConfig _prevConfig = {};

public:
	SacControlManager(Emulator* emu) : BaseControlManager(emu, CpuType::Sac) {}

	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override
	{
		if(type == ControllerType::None) {
			return nullptr;
		}
		SnesConfig& cfg = _emu->GetSettings()->GetSnesConfig();
		return shared_ptr<BaseControlDevice>(new SnesController(_emu, port, port == 0 ? cfg.Port1.Keys : cfg.Port2.Keys));
	}

	void UpdateControlDevices() override
	{
		SnesConfig cfg = _emu->GetSettings()->GetSnesConfig();
		if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && _controlDevices.size() > 0) {
			return;
		}

		auto lock = _deviceLock.AcquireSafe();
		ClearDevices();
		for(uint8_t port = 0; port < 2; port++) {
			shared_ptr<BaseControlDevice> device = CreateControllerDevice(port == 0 ? cfg.Port1.Type : cfg.Port2.Type, port);
			if(device) {
				RegisterControlDevice(device);
			}
		}
		_prevConfig = cfg;
	}

	//A pad as MAME lays it out, a pressed button reading 0: A, B, Start, Select, up, down, left,
	//right, X, Y, L, R from bit 15 down, and bits 0-3 unused
	uint16_t ReadPad(uint8_t port)
	{
		SetInputReadFlag();

		uint16_t value = 0xFFFF;
		for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
			if(device->GetPort() != port) {
				continue;
			}

			static constexpr uint8_t buttons[12] = {
				SnesController::R, SnesController::L, SnesController::Y, SnesController::X,
				SnesController::Right, SnesController::Left, SnesController::Down, SnesController::Up,
				SnesController::Select, SnesController::Start, SnesController::B, SnesController::A
			};
			for(int i = 0; i < 12; i++) {
				if(device->IsPressed(buttons[i])) {
					value &= ~(1 << (i + 4));
				}
			}
		}
		return value;
	}
};
