#include "pch.h"
#include "SuperAcan/W65C02.h"

//Base cycles for each opcode on a WDC 65C02. Crossing a page on an indexed read, a branch that
//is taken and decimal-mode ADC/SBC each add one on top, as the datasheet says.
static constexpr uint8_t _cycles[256] = {
	7, 6, 2, 1, 5, 3, 5, 5, 3, 2, 2, 1, 6, 4, 6, 5, //0x
	2, 5, 5, 1, 5, 4, 6, 5, 2, 4, 2, 1, 6, 4, 6, 5, //1x
	6, 6, 2, 1, 3, 3, 5, 5, 4, 2, 2, 1, 4, 4, 6, 5, //2x
	2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 2, 1, 4, 4, 6, 5, //3x
	6, 6, 2, 1, 3, 3, 5, 5, 3, 2, 2, 1, 3, 4, 6, 5, //4x
	2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 3, 1, 8, 4, 6, 5, //5x
	6, 6, 2, 1, 3, 3, 5, 5, 4, 2, 2, 1, 6, 4, 6, 5, //6x
	2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 4, 1, 6, 4, 6, 5, //7x
	3, 6, 2, 1, 3, 3, 3, 5, 2, 2, 2, 1, 4, 4, 4, 5, //8x
	2, 6, 5, 1, 4, 4, 4, 5, 2, 5, 2, 1, 4, 5, 5, 5, //9x
	2, 6, 2, 1, 3, 3, 3, 5, 2, 2, 2, 1, 4, 4, 4, 5, //Ax
	2, 5, 5, 1, 4, 4, 4, 5, 2, 4, 2, 1, 4, 4, 4, 5, //Bx
	2, 6, 2, 1, 3, 3, 5, 5, 2, 2, 2, 3, 4, 4, 6, 5, //Cx
	2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 3, 3, 4, 4, 7, 5, //Dx
	2, 6, 2, 1, 3, 3, 5, 5, 2, 2, 2, 1, 4, 4, 6, 5, //Ex
	2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 4, 1, 4, 4, 7, 5  //Fx
};

uint16_t W65C02::FetchWord()
{
	uint8_t lo = Fetch();
	uint8_t hi = Fetch();
	return (uint16_t)(lo | (hi << 8));
}

void W65C02::Push(uint8_t value)
{
	Write((uint16_t)(0x100 | _state.SP), value);
	_state.SP--;
}

uint8_t W65C02::Pop()
{
	_state.SP++;
	return Read((uint16_t)(0x100 | _state.SP));
}

void W65C02::SetNZ(uint8_t value)
{
	SetFlag(Zero, value == 0);
	SetFlag(Negative, (value & 0x80) != 0);
}

//A pointer in zero page wraps within zero page
uint16_t W65C02::ReadZpWord(uint8_t zp)
{
	uint8_t lo = Read(zp);
	uint8_t hi = Read((uint8_t)(zp + 1));
	return (uint16_t)(lo | (hi << 8));
}

uint16_t W65C02::AddrAbsIndexed(uint8_t index, bool pagePenalty)
{
	uint16_t base = FetchWord();
	uint16_t addr = (uint16_t)(base + index);
	if(pagePenalty && ((base ^ addr) & 0xFF00)) {
		_state.CycleCount++;
	}
	return addr;
}

uint16_t W65C02::AddrIndirectX()
{
	return ReadZpWord((uint8_t)(Fetch() + _state.X));
}

uint16_t W65C02::AddrIndirectY(bool pagePenalty)
{
	uint16_t base = ReadZpWord(Fetch());
	uint16_t addr = (uint16_t)(base + _state.Y);
	if(pagePenalty && ((base ^ addr) & 0xFF00)) {
		_state.CycleCount++;
	}
	return addr;
}

void W65C02::Reset()
{
	_state.SP = 0xFD;
	_state.PS = Reserved | IrqDisable;
	_state.Waiting = false;
	_state.Stopped = false;
	_state.NmiPending = false;
	_state.PC = (uint16_t)(Read(0xFFFC) | (Read(0xFFFD) << 8));
	_state.CycleCount += 7;
}

//Unlike the NMOS 6502, the 65C02 clears decimal mode on every interrupt, BRK included
void W65C02::Interrupt(uint16_t vector, bool fromBrk)
{
	Push((uint8_t)(_state.PC >> 8));
	Push((uint8_t)_state.PC);
	Push((uint8_t)((_state.PS | Reserved | (fromBrk ? Break : 0)) & (fromBrk ? 0xFF : ~Break)));
	SetFlag(IrqDisable, true);
	SetFlag(Decimal, false);
	_state.PC = (uint16_t)(Read(vector) | (Read((uint16_t)(vector + 1)) << 8));
}

void W65C02::TakeInterrupt(uint16_t vector, bool forNmi)
{
	uint16_t originalPc = _state.PC;
	_state.CycleCount += 7;
	Interrupt(vector, false);
	_bus->OnInterrupt(originalPc, _state.PC, forNmi);
}

void W65C02::Exec()
{
	if(_state.Stopped) {
		_state.CycleCount++;
		_bus->OnHalted();
		return;
	}

	if(_state.NmiPending) {
		_state.NmiPending = false;
		_state.Waiting = false;
		TakeInterrupt(0xFFFA, true);
		return;
	}

	if(_state.IrqLine) {
		//WAI ends on an interrupt request whether or not it is then taken
		_state.Waiting = false;
		if(!GetFlag(IrqDisable)) {
			TakeInterrupt(0xFFFE, false);
			return;
		}
	}

	if(_state.Waiting) {
		_state.CycleCount++;
		_bus->OnHalted();
		return;
	}

	_bus->OnInstruction();
	uint8_t opCode = _bus->ReadOpCode(_state.PC++);
	_state.CycleCount += _cycles[opCode];
	ExecOpCode(opCode);
}

void W65C02::Branch(bool condition)
{
	int8_t offset = (int8_t)Fetch();
	if(condition) {
		uint16_t target = (uint16_t)(_state.PC + offset);
		_state.CycleCount += ((_state.PC ^ target) & 0xFF00) ? 2 : 1;
		_state.PC = target;
	}
}

void W65C02::Ora(uint8_t value)
{
	_state.A |= value;
	SetNZ(_state.A);
}

void W65C02::And(uint8_t value)
{
	_state.A &= value;
	SetNZ(_state.A);
}

void W65C02::Eor(uint8_t value)
{
	_state.A ^= value;
	SetNZ(_state.A);
}

//Decimal mode on the 65C02 leaves N and Z valid for the decimal result (the NMOS part does
//not) and costs a cycle. V is computed as it comes out of the binary high-nibble sum.
void W65C02::Adc(uint8_t value)
{
	uint8_t carry = GetFlag(Carry) ? 1 : 0;
	if(GetFlag(Decimal)) {
		_state.CycleCount++;
		int lo = (_state.A & 0x0F) + (value & 0x0F) + carry;
		int hi = (_state.A & 0xF0) + (value & 0xF0);
		if(lo > 0x09) {
			lo += 0x06;
		}
		if(lo > 0x0F) {
			hi += 0x10;
		}
		SetFlag(Overflow, (~(_state.A ^ value) & (_state.A ^ hi) & 0x80) != 0);
		if(hi > 0x90) {
			hi += 0x60;
		}
		SetFlag(Carry, hi > 0xFF);
		_state.A = (uint8_t)((hi & 0xF0) | (lo & 0x0F));
		SetNZ(_state.A);
	} else {
		uint16_t sum = (uint16_t)(_state.A + value + carry);
		SetFlag(Overflow, (~(_state.A ^ value) & (_state.A ^ sum) & 0x80) != 0);
		SetFlag(Carry, sum > 0xFF);
		_state.A = (uint8_t)sum;
		SetNZ(_state.A);
	}
}

void W65C02::Sbc(uint8_t value)
{
	if(GetFlag(Decimal)) {
		_state.CycleCount++;
		int borrow = GetFlag(Carry) ? 0 : 1;
		int binary = _state.A - value - borrow;
		int lo = (_state.A & 0x0F) - (value & 0x0F) - borrow;
		int result = binary;
		if(result < 0) {
			result -= 0x60;
		}
		if(lo < 0) {
			result -= 0x06;
		}
		SetFlag(Overflow, ((_state.A ^ value) & (_state.A ^ binary) & 0x80) != 0);
		SetFlag(Carry, binary >= 0);
		_state.A = (uint8_t)result;
		SetNZ(_state.A);
	} else {
		//Binary subtraction is addition of the complement
		uint8_t carry = GetFlag(Carry) ? 1 : 0;
		uint8_t operand = (uint8_t)~value;
		uint16_t sum = (uint16_t)(_state.A + operand + carry);
		SetFlag(Overflow, (~(_state.A ^ operand) & (_state.A ^ sum) & 0x80) != 0);
		SetFlag(Carry, sum > 0xFF);
		_state.A = (uint8_t)sum;
		SetNZ(_state.A);
	}
}

void W65C02::Compare(uint8_t reg, uint8_t value)
{
	SetFlag(Carry, reg >= value);
	SetNZ((uint8_t)(reg - value));
}

void W65C02::Bit(uint8_t value)
{
	SetFlag(Zero, (_state.A & value) == 0);
	SetFlag(Negative, (value & 0x80) != 0);
	SetFlag(Overflow, (value & 0x40) != 0);
}

uint8_t W65C02::Asl(uint8_t value)
{
	SetFlag(Carry, (value & 0x80) != 0);
	value <<= 1;
	SetNZ(value);
	return value;
}

uint8_t W65C02::Lsr(uint8_t value)
{
	SetFlag(Carry, (value & 0x01) != 0);
	value >>= 1;
	SetNZ(value);
	return value;
}

uint8_t W65C02::Rol(uint8_t value)
{
	uint8_t carry = GetFlag(Carry) ? 1 : 0;
	SetFlag(Carry, (value & 0x80) != 0);
	value = (uint8_t)((value << 1) | carry);
	SetNZ(value);
	return value;
}

uint8_t W65C02::Ror(uint8_t value)
{
	uint8_t carry = GetFlag(Carry) ? 0x80 : 0;
	SetFlag(Carry, (value & 0x01) != 0);
	value = (uint8_t)((value >> 1) | carry);
	SetNZ(value);
	return value;
}

uint8_t W65C02::Inc(uint8_t value)
{
	value++;
	SetNZ(value);
	return value;
}

uint8_t W65C02::Dec(uint8_t value)
{
	value--;
	SetNZ(value);
	return value;
}

void W65C02::Modify(uint16_t addr, uint8_t (W65C02::*op)(uint8_t))
{
	uint8_t value = Read(addr);
	Write(addr, (this->*op)(value));
}

//TSB/TRB: Z reports what A and the operand had in common, before the bits change
void W65C02::TestAndSet(uint16_t addr, bool set)
{
	uint8_t value = Read(addr);
	SetFlag(Zero, (_state.A & value) == 0);
	Write(addr, set ? (uint8_t)(value | _state.A) : (uint8_t)(value & ~_state.A));
}

void W65C02::ExecOpCode(uint8_t op)
{
	State& s = _state;

	//The Rockwell bit instructions fill two whole columns: RMB/SMB n in column 7, BBR/BBS n
	//in column F, the bit number in bits 4-6 and set/branch-if-set in bit 7
	uint8_t column = op & 0x0F;
	if(column == 0x07) {
		uint8_t zp = Fetch();
		uint8_t mask = (uint8_t)(1 << ((op >> 4) & 0x07));
		uint8_t value = Read(zp);
		Write(zp, (op & 0x80) ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask));
		return;
	} else if(column == 0x0F) {
		uint8_t zp = Fetch();
		uint8_t mask = (uint8_t)(1 << ((op >> 4) & 0x07));
		bool isSet = (Read(zp) & mask) != 0;
		Branch((op & 0x80) ? isSet : !isSet);
		return;
	} else if(column == 0x03 || (column == 0x0B && op != 0xCB && op != 0xDB)) {
		//One-byte, one-cycle NOPs
		return;
	}

	switch(op) {
		//Two-byte NOPs, immediate or zero page
		case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2:
		case 0x44: case 0x54: case 0xD4: case 0xF4:
			Fetch();
			break;

		//Three-byte NOPs
		case 0x5C: case 0xDC: case 0xFC:
			FetchWord();
			break;

		case 0x00:
			Fetch(); //the signature byte BRK skips
			Interrupt(0xFFFE, true);
			break;

		case 0x01: Ora(Read(AddrIndirectX())); break;
		case 0x04: TestAndSet(Fetch(), true); break;
		case 0x05: Ora(Read(Fetch())); break;
		case 0x06: Modify(Fetch(), &W65C02::Asl); break;
		case 0x08: Push((uint8_t)(s.PS | Break | Reserved)); break;
		case 0x09: Ora(Fetch()); break;
		case 0x0A: s.A = Asl(s.A); break;
		case 0x0C: TestAndSet(FetchWord(), true); break;
		case 0x0D: Ora(Read(FetchWord())); break;
		case 0x0E: Modify(FetchWord(), &W65C02::Asl); break;

		case 0x10: Branch(!GetFlag(Negative)); break;
		case 0x11: Ora(Read(AddrIndirectY(true))); break;
		case 0x12: Ora(Read(ReadZpWord(Fetch()))); break;
		case 0x14: TestAndSet(Fetch(), false); break;
		case 0x15: Ora(Read((uint8_t)(Fetch() + s.X))); break;
		case 0x16: Modify((uint8_t)(Fetch() + s.X), &W65C02::Asl); break;
		case 0x18: SetFlag(Carry, false); break;
		case 0x19: Ora(Read(AddrAbsIndexed(s.Y, true))); break;
		case 0x1A: s.A = Inc(s.A); break;
		case 0x1C: TestAndSet(FetchWord(), false); break;
		case 0x1D: Ora(Read(AddrAbsIndexed(s.X, true))); break;
		case 0x1E: Modify(AddrAbsIndexed(s.X, true), &W65C02::Asl); break;

		case 0x20: {
			uint16_t addr = FetchWord();
			uint16_t ret = (uint16_t)(s.PC - 1);
			Push((uint8_t)(ret >> 8));
			Push((uint8_t)ret);
			s.PC = addr;
			break;
		}
		case 0x21: And(Read(AddrIndirectX())); break;
		case 0x24: Bit(Read(Fetch())); break;
		case 0x25: And(Read(Fetch())); break;
		case 0x26: Modify(Fetch(), &W65C02::Rol); break;
		case 0x28: s.PS = (uint8_t)((Pop() & ~Break) | Reserved); break;
		case 0x29: And(Fetch()); break;
		case 0x2A: s.A = Rol(s.A); break;
		case 0x2C: Bit(Read(FetchWord())); break;
		case 0x2D: And(Read(FetchWord())); break;
		case 0x2E: Modify(FetchWord(), &W65C02::Rol); break;

		case 0x30: Branch(GetFlag(Negative)); break;
		case 0x31: And(Read(AddrIndirectY(true))); break;
		case 0x32: And(Read(ReadZpWord(Fetch()))); break;
		case 0x34: Bit(Read((uint8_t)(Fetch() + s.X))); break;
		case 0x35: And(Read((uint8_t)(Fetch() + s.X))); break;
		case 0x36: Modify((uint8_t)(Fetch() + s.X), &W65C02::Rol); break;
		case 0x38: SetFlag(Carry, true); break;
		case 0x39: And(Read(AddrAbsIndexed(s.Y, true))); break;
		case 0x3A: s.A = Dec(s.A); break;
		case 0x3C: Bit(Read(AddrAbsIndexed(s.X, true))); break;
		case 0x3D: And(Read(AddrAbsIndexed(s.X, true))); break;
		case 0x3E: Modify(AddrAbsIndexed(s.X, true), &W65C02::Rol); break;

		case 0x40: {
			s.PS = (uint8_t)((Pop() & ~Break) | Reserved);
			uint8_t lo = Pop();
			uint8_t hi = Pop();
			s.PC = (uint16_t)(lo | (hi << 8));
			break;
		}
		case 0x41: Eor(Read(AddrIndirectX())); break;
		case 0x45: Eor(Read(Fetch())); break;
		case 0x46: Modify(Fetch(), &W65C02::Lsr); break;
		case 0x48: Push(s.A); break;
		case 0x49: Eor(Fetch()); break;
		case 0x4A: s.A = Lsr(s.A); break;
		case 0x4C: s.PC = FetchWord(); break;
		case 0x4D: Eor(Read(FetchWord())); break;
		case 0x4E: Modify(FetchWord(), &W65C02::Lsr); break;

		case 0x50: Branch(!GetFlag(Overflow)); break;
		case 0x51: Eor(Read(AddrIndirectY(true))); break;
		case 0x52: Eor(Read(ReadZpWord(Fetch()))); break;
		case 0x55: Eor(Read((uint8_t)(Fetch() + s.X))); break;
		case 0x56: Modify((uint8_t)(Fetch() + s.X), &W65C02::Lsr); break;
		case 0x58: SetFlag(IrqDisable, false); break;
		case 0x59: Eor(Read(AddrAbsIndexed(s.Y, true))); break;
		case 0x5A: Push(s.Y); break;
		case 0x5D: Eor(Read(AddrAbsIndexed(s.X, true))); break;
		case 0x5E: Modify(AddrAbsIndexed(s.X, true), &W65C02::Lsr); break;

		case 0x60: {
			uint8_t lo = Pop();
			uint8_t hi = Pop();
			s.PC = (uint16_t)((lo | (hi << 8)) + 1);
			break;
		}
		case 0x61: Adc(Read(AddrIndirectX())); break;
		case 0x64: Write(Fetch(), 0); break;
		case 0x65: Adc(Read(Fetch())); break;
		case 0x66: Modify(Fetch(), &W65C02::Ror); break;
		case 0x68: s.A = Pop(); SetNZ(s.A); break;
		case 0x69: Adc(Fetch()); break;
		case 0x6A: s.A = Ror(s.A); break;
		case 0x6C: {
			//The 65C02 fixed the NMOS page-wrap bug in JMP ($xxFF)
			uint16_t ptr = FetchWord();
			s.PC = (uint16_t)(Read(ptr) | (Read((uint16_t)(ptr + 1)) << 8));
			break;
		}
		case 0x6D: Adc(Read(FetchWord())); break;
		case 0x6E: Modify(FetchWord(), &W65C02::Ror); break;

		case 0x70: Branch(GetFlag(Overflow)); break;
		case 0x71: Adc(Read(AddrIndirectY(true))); break;
		case 0x72: Adc(Read(ReadZpWord(Fetch()))); break;
		case 0x74: Write((uint8_t)(Fetch() + s.X), 0); break;
		case 0x75: Adc(Read((uint8_t)(Fetch() + s.X))); break;
		case 0x76: Modify((uint8_t)(Fetch() + s.X), &W65C02::Ror); break;
		case 0x78: SetFlag(IrqDisable, true); break;
		case 0x79: Adc(Read(AddrAbsIndexed(s.Y, true))); break;
		case 0x7A: s.Y = Pop(); SetNZ(s.Y); break;
		case 0x7C: {
			uint16_t ptr = (uint16_t)(FetchWord() + s.X);
			s.PC = (uint16_t)(Read(ptr) | (Read((uint16_t)(ptr + 1)) << 8));
			break;
		}
		case 0x7D: Adc(Read(AddrAbsIndexed(s.X, true))); break;
		case 0x7E: Modify(AddrAbsIndexed(s.X, true), &W65C02::Ror); break;

		case 0x80: Branch(true); break;
		case 0x81: Write(AddrIndirectX(), s.A); break;
		case 0x84: Write(Fetch(), s.Y); break;
		case 0x85: Write(Fetch(), s.A); break;
		case 0x86: Write(Fetch(), s.X); break;
		case 0x88: s.Y--; SetNZ(s.Y); break;
		case 0x89: SetFlag(Zero, (s.A & Fetch()) == 0); break; //BIT # changes Z alone
		case 0x8A: s.A = s.X; SetNZ(s.A); break;
		case 0x8C: Write(FetchWord(), s.Y); break;
		case 0x8D: Write(FetchWord(), s.A); break;
		case 0x8E: Write(FetchWord(), s.X); break;

		case 0x90: Branch(!GetFlag(Carry)); break;
		case 0x91: Write(AddrIndirectY(false), s.A); break;
		case 0x92: Write(ReadZpWord(Fetch()), s.A); break;
		case 0x94: Write((uint8_t)(Fetch() + s.X), s.Y); break;
		case 0x95: Write((uint8_t)(Fetch() + s.X), s.A); break;
		case 0x96: Write((uint8_t)(Fetch() + s.Y), s.X); break;
		case 0x98: s.A = s.Y; SetNZ(s.A); break;
		case 0x99: Write(AddrAbsIndexed(s.Y, false), s.A); break;
		case 0x9A: s.SP = s.X; break;
		case 0x9C: Write(FetchWord(), 0); break;
		case 0x9D: Write(AddrAbsIndexed(s.X, false), s.A); break;
		case 0x9E: Write(AddrAbsIndexed(s.X, false), 0); break;

		case 0xA0: s.Y = Fetch(); SetNZ(s.Y); break;
		case 0xA1: s.A = Read(AddrIndirectX()); SetNZ(s.A); break;
		case 0xA2: s.X = Fetch(); SetNZ(s.X); break;
		case 0xA4: s.Y = Read(Fetch()); SetNZ(s.Y); break;
		case 0xA5: s.A = Read(Fetch()); SetNZ(s.A); break;
		case 0xA6: s.X = Read(Fetch()); SetNZ(s.X); break;
		case 0xA8: s.Y = s.A; SetNZ(s.Y); break;
		case 0xA9: s.A = Fetch(); SetNZ(s.A); break;
		case 0xAA: s.X = s.A; SetNZ(s.X); break;
		case 0xAC: s.Y = Read(FetchWord()); SetNZ(s.Y); break;
		case 0xAD: s.A = Read(FetchWord()); SetNZ(s.A); break;
		case 0xAE: s.X = Read(FetchWord()); SetNZ(s.X); break;

		case 0xB0: Branch(GetFlag(Carry)); break;
		case 0xB1: s.A = Read(AddrIndirectY(true)); SetNZ(s.A); break;
		case 0xB2: s.A = Read(ReadZpWord(Fetch())); SetNZ(s.A); break;
		case 0xB4: s.Y = Read((uint8_t)(Fetch() + s.X)); SetNZ(s.Y); break;
		case 0xB5: s.A = Read((uint8_t)(Fetch() + s.X)); SetNZ(s.A); break;
		case 0xB6: s.X = Read((uint8_t)(Fetch() + s.Y)); SetNZ(s.X); break;
		case 0xB8: SetFlag(Overflow, false); break;
		case 0xB9: s.A = Read(AddrAbsIndexed(s.Y, true)); SetNZ(s.A); break;
		case 0xBA: s.X = s.SP; SetNZ(s.X); break;
		case 0xBC: s.Y = Read(AddrAbsIndexed(s.X, true)); SetNZ(s.Y); break;
		case 0xBD: s.A = Read(AddrAbsIndexed(s.X, true)); SetNZ(s.A); break;
		case 0xBE: s.X = Read(AddrAbsIndexed(s.Y, true)); SetNZ(s.X); break;

		case 0xC0: Compare(s.Y, Fetch()); break;
		case 0xC1: Compare(s.A, Read(AddrIndirectX())); break;
		case 0xC4: Compare(s.Y, Read(Fetch())); break;
		case 0xC5: Compare(s.A, Read(Fetch())); break;
		case 0xC6: Modify(Fetch(), &W65C02::Dec); break;
		case 0xC8: s.Y++; SetNZ(s.Y); break;
		case 0xC9: Compare(s.A, Fetch()); break;
		case 0xCA: s.X--; SetNZ(s.X); break;
		case 0xCB: s.Waiting = true; break;
		case 0xCC: Compare(s.Y, Read(FetchWord())); break;
		case 0xCD: Compare(s.A, Read(FetchWord())); break;
		case 0xCE: Modify(FetchWord(), &W65C02::Dec); break;

		case 0xD0: Branch(!GetFlag(Zero)); break;
		case 0xD1: Compare(s.A, Read(AddrIndirectY(true))); break;
		case 0xD2: Compare(s.A, Read(ReadZpWord(Fetch()))); break;
		case 0xD5: Compare(s.A, Read((uint8_t)(Fetch() + s.X))); break;
		case 0xD6: Modify((uint8_t)(Fetch() + s.X), &W65C02::Dec); break;
		case 0xD8: SetFlag(Decimal, false); break;
		case 0xD9: Compare(s.A, Read(AddrAbsIndexed(s.Y, true))); break;
		case 0xDA: Push(s.X); break;
		case 0xDB: s.Stopped = true; break;
		case 0xDD: Compare(s.A, Read(AddrAbsIndexed(s.X, true))); break;
		case 0xDE: Modify(AddrAbsIndexed(s.X, false), &W65C02::Dec); break;

		case 0xE0: Compare(s.X, Fetch()); break;
		case 0xE1: Sbc(Read(AddrIndirectX())); break;
		case 0xE4: Compare(s.X, Read(Fetch())); break;
		case 0xE5: Sbc(Read(Fetch())); break;
		case 0xE6: Modify(Fetch(), &W65C02::Inc); break;
		case 0xE8: s.X++; SetNZ(s.X); break;
		case 0xE9: Sbc(Fetch()); break;
		case 0xEA: break;
		case 0xEC: Compare(s.X, Read(FetchWord())); break;
		case 0xED: Sbc(Read(FetchWord())); break;
		case 0xEE: Modify(FetchWord(), &W65C02::Inc); break;

		case 0xF0: Branch(GetFlag(Zero)); break;
		case 0xF1: Sbc(Read(AddrIndirectY(true))); break;
		case 0xF2: Sbc(Read(ReadZpWord(Fetch()))); break;
		case 0xF5: Sbc(Read((uint8_t)(Fetch() + s.X))); break;
		case 0xF6: Modify((uint8_t)(Fetch() + s.X), &W65C02::Inc); break;
		case 0xF8: SetFlag(Decimal, true); break;
		case 0xF9: Sbc(Read(AddrAbsIndexed(s.Y, true))); break;
		case 0xFA: s.X = Pop(); SetNZ(s.X); break;
		case 0xFD: Sbc(Read(AddrAbsIndexed(s.X, true))); break;
		case 0xFE: Modify(AddrAbsIndexed(s.X, false), &W65C02::Inc); break;
	}
}
