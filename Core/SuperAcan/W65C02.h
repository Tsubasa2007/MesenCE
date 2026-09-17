#pragma once
#include <cstdint>

//A WDC 65C02, the Super A'Can's sound processor (MAME uses its W65C02 for it): the CMOS
//instruction set with the Rockwell bit instructions and WAI/STP, and every undefined opcode a
//NOP of the size and length WDC documents. It knows nothing about the machine around it -
//memory goes through a Bus - so the same code runs Klaus Dormann's functional tests outside
//the emulator and the sound processor inside it.
class W65C02
{
public:
	class Bus
	{
	public:
		virtual ~Bus() {}
		virtual uint8_t Read(uint16_t addr) = 0;
		virtual void Write(uint16_t addr, uint8_t value) = 0;

		//Optional, for a debugger: an opcode or operand fetch (both read like any other byte
		//unless overridden), the start of each instruction, an interrupt once its vector has
		//been taken, and a cycle spent waiting (WAI) or stopped (STP)
		virtual uint8_t ReadOpCode(uint16_t addr) { return Read(addr); }
		virtual uint8_t ReadOperand(uint16_t addr) { return Read(addr); }
		virtual void OnInstruction() {}
		virtual void OnInterrupt(uint16_t originalPc, uint16_t newPc, bool forNmi) {}
		virtual void OnHalted() {}
	};

	enum Flag : uint8_t
	{
		Carry = 0x01,
		Zero = 0x02,
		IrqDisable = 0x04,
		Decimal = 0x08,
		Break = 0x10,
		Reserved = 0x20,
		Overflow = 0x40,
		Negative = 0x80
	};

	struct State
	{
		uint16_t PC;
		uint8_t A;
		uint8_t X;
		uint8_t Y;
		uint8_t SP;
		uint8_t PS;
		uint64_t CycleCount;
		bool Waiting; //WAI: until an interrupt arrives
		bool Stopped; //STP: until a reset
		bool IrqLine;
		bool NmiPending;
	};

private:
	Bus* _bus = nullptr;
	State _state = {};

	uint8_t Read(uint16_t addr) { return _bus->Read(addr); }
	void Write(uint16_t addr, uint8_t value) { _bus->Write(addr, value); }
	uint8_t Fetch() { return _bus->ReadOperand(_state.PC++); }
	uint16_t FetchWord();
	void Push(uint8_t value);
	uint8_t Pop();

	void SetFlag(uint8_t flag, bool set) { _state.PS = set ? (_state.PS | flag) : (_state.PS & ~flag); }
	bool GetFlag(uint8_t flag) { return (_state.PS & flag) != 0; }
	void SetNZ(uint8_t value);

	uint16_t ReadZpWord(uint8_t zp);
	uint16_t AddrAbsIndexed(uint8_t index, bool pagePenalty);
	uint16_t AddrIndirectX();
	uint16_t AddrIndirectY(bool pagePenalty);

	void Interrupt(uint16_t vector, bool fromBrk);
	void TakeInterrupt(uint16_t vector, bool forNmi);
	void Branch(bool condition);

	void Ora(uint8_t value);
	void And(uint8_t value);
	void Eor(uint8_t value);
	void Adc(uint8_t value);
	void Sbc(uint8_t value);
	void Compare(uint8_t reg, uint8_t value);
	void Bit(uint8_t value);

	uint8_t Asl(uint8_t value);
	uint8_t Lsr(uint8_t value);
	uint8_t Rol(uint8_t value);
	uint8_t Ror(uint8_t value);
	uint8_t Inc(uint8_t value);
	uint8_t Dec(uint8_t value);
	void Modify(uint16_t addr, uint8_t (W65C02::*op)(uint8_t));
	void TestAndSet(uint16_t addr, bool set);

	void ExecOpCode(uint8_t opCode);

public:
	W65C02(Bus* bus) : _bus(bus) {}

	void Reset();
	void Exec();

	void SetIrq(bool asserted) { _state.IrqLine = asserted; }
	void SetProgramCounter(uint16_t pc) { _state.PC = pc; }
	void TriggerNmi() { _state.NmiPending = true; }

	State& GetState() { return _state; }
};
