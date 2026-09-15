#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/InputHud.h"

//The machine's pad: the same twelve buttons as a SNES pad - a d-pad, A, B, X, Y, L, R, Select
//and Start. The console reads it as a whole (see SacControlManager::ReadPad), so there is no
//port-level protocol here.
class SacController : public BaseControlDevice
{
private:
	uint32_t _turboSpeed = 0;

protected:
	string GetKeyNames() override
	{
		return "ABXYLRSTUDLR";
	}

	void InternalSetStateFromInput() override
	{
		bool turboOn = IsTurboOn(_turboSpeed);

		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::A, keyMapping.A);
			SetPressedState(Buttons::B, keyMapping.B);
			SetPressedState(Buttons::X, keyMapping.X);
			SetPressedState(Buttons::Y, keyMapping.Y);
			SetPressedState(Buttons::L, keyMapping.L);
			SetPressedState(Buttons::R, keyMapping.R);
			SetPressedState(Buttons::Select, keyMapping.Select);
			SetPressedState(Buttons::Start, keyMapping.Start);
			SetPressedState(Buttons::Up, keyMapping.Up);
			SetPressedState(Buttons::Down, keyMapping.Down);
			SetPressedState(Buttons::Left, keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);

			if(turboOn) {
				SetPressedState(Buttons::A, keyMapping.TurboA);
				SetPressedState(Buttons::B, keyMapping.TurboB);
				SetPressedState(Buttons::X, keyMapping.TurboX);
				SetPressedState(Buttons::Y, keyMapping.TurboY);
				SetPressedState(Buttons::L, keyMapping.TurboL);
				SetPressedState(Buttons::R, keyMapping.TurboR);
			}
		}

		//A d-pad cannot press both ways at once
		if(IsPressed(Buttons::Up) && IsPressed(Buttons::Down)) {
			ClearBit(Buttons::Up);
			ClearBit(Buttons::Down);
		}
		if(IsPressed(Buttons::Left) && IsPressed(Buttons::Right)) {
			ClearBit(Buttons::Left);
			ClearBit(Buttons::Right);
		}
	}

	void RefreshStateBuffer() override
	{
	}

public:
	enum Buttons
	{
		A = 0,
		B,
		X,
		Y,
		L,
		R,
		Select,
		Start,
		Up,
		Down,
		Left,
		Right
	};

	SacController(Emulator* emu, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, ControllerType::SacController, port, keyMappings)
	{
		_turboSpeed = keyMappings.TurboSpeed;
	}

	uint8_t ReadRam(uint16_t addr) override
	{
		return 0;
	}

	void WriteRam(uint16_t addr, uint8_t value) override
	{
	}

	void InternalDrawController(InputHud& hud) override
	{
		hud.DrawOutline(35, 14);

		hud.DrawButton(5, 3, 3, 3, IsPressed(Buttons::Up));
		hud.DrawButton(5, 9, 3, 3, IsPressed(Buttons::Down));
		hud.DrawButton(2, 6, 3, 3, IsPressed(Buttons::Left));
		hud.DrawButton(8, 6, 3, 3, IsPressed(Buttons::Right));
		hud.DrawButton(5, 6, 3, 3, false);

		hud.DrawButton(27, 3, 3, 3, IsPressed(Buttons::X));
		hud.DrawButton(27, 9, 3, 3, IsPressed(Buttons::B));
		hud.DrawButton(30, 6, 3, 3, IsPressed(Buttons::A));
		hud.DrawButton(24, 6, 3, 3, IsPressed(Buttons::Y));

		hud.DrawButton(4, 0, 5, 2, IsPressed(Buttons::L));
		hud.DrawButton(26, 0, 5, 2, IsPressed(Buttons::R));

		hud.DrawButton(13, 9, 4, 2, IsPressed(Buttons::Select));
		hud.DrawButton(18, 9, 4, 2, IsPressed(Buttons::Start));

		hud.DrawNumber(hud.GetControllerIndex() + 1, 16, 2);
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "x", Buttons::X },
			{ "y", Buttons::Y },
			{ "l", Buttons::L },
			{ "r", Buttons::R },
			{ "select", Buttons::Select },
			{ "start", Buttons::Start },
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
		};
	}
};
