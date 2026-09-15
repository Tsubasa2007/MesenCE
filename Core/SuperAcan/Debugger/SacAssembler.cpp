#include "pch.h"
#include <unordered_set>
#include "SuperAcan/Debugger/SacAssembler.h"
#include "SuperAcan/SacCpu.h"
#include "Debugger/LabelManager.h"
#include "Shared/CpuType.h"
#include "Utilities/StringUtilities.h"

namespace
{
	enum class SacAsmKind : uint8_t
	{
		Dn, An, Ind, PostInc, PreDec, Disp, Index, AbsShort, AbsLong, PcDisp, PcIndex, Imm, RegList, Sr, Ccr, Usp,
		Value //a bare address: a branch target, or an absolute address written without .w/.l
	};

	struct SacAsmOperand
	{
		SacAsmKind Kind = SacAsmKind::Value;
		uint8_t Reg = 0; //the data/address register, or the base register of an address mode
		int64_t Value = 0; //displacement, address, immediate, target or register mask
		uint8_t IndexReg = 0; //0-7 = D0-D7, 8-15 = A0-A7
		bool IndexLong = false;
		uint8_t Scale = 0; //the 68000 ignores the scale bits, but the disassembler shows them
	};

	struct SacAsmLine
	{
		string Mnemonic;
		vector<SacAsmOperand> Operands;
	};

	//What labels resolve against, and where the line being assembled goes
	struct SacAsmContext
	{
		unordered_map<string, uint32_t>* Labels = nullptr;
		LabelManager* LabelMgr = nullptr;
		uint32_t Address = 0;
		bool FirstPass = false;
		bool NeedSecondPass = false;
	};

	struct SacAsmCandidate
	{
		uint16_t OpCode;
		uint8_t Length;
	};

	struct SacAsmIndex
	{
		unordered_map<string, vector<SacAsmCandidate>> ByShape;
		std::unordered_set<string> Mnemonics;
	};
}

using K = SacAsmKind;
using Code = AssemblerSpecialCodes;

static bool EndsWith(const string& text, const char* suffix)
{
	size_t len = strlen(suffix);
	return text.size() >= len && text.compare(text.size() - len, len, suffix) == 0;
}

static bool Fits(int64_t value, int64_t min, int64_t max)
{
	return value >= min && value <= max;
}

//An absolute short address is sign-extended, so it reaches $0-$7FFF and $FF8000-$FFFFFF
static bool FitsShortAddress(int64_t value, bool explicitSize)
{
	return Fits(value, -0x8000, explicitSize ? 0xFFFF : 0x7FFF) || Fits(value, 0xFF8000, 0xFFFFFF) || Fits(value, 0xFFFF8000LL, 0xFFFFFFFFLL);
}

//The distance from a branch's extension word to its target, on the 68000's 24-bit bus (so a
//target written as $FFFFC900 or $FFC900 is the same place)
static int64_t GetBranchDisplacement(int64_t target, uint32_t instructionAddress)
{
	int32_t disp = (int32_t)((target - (int64_t)(instructionAddress + 2)) & 0xFFFFFF);
	return (disp << 8) >> 8;
}

static bool IsLabelChar(char c, bool first)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '@' || (!first && c >= '0' && c <= '9');
}

static bool IsLabelName(const string& text)
{
	if(text.empty()) {
		return false;
	}
	for(size_t i = 0; i < text.size(); i++) {
		if(!IsLabelChar(text[i], i == 0)) {
			return false;
		}
	}
	return true;
}

//$hex, %binary, decimal (each optionally negative) or a label
static Code ParseNumber(string text, int64_t& value, SacAsmContext* ctx)
{
	text = StringUtilities::Trim(text);
	bool negative = !text.empty() && text[0] == '-';
	if(negative) {
		text = StringUtilities::Trim(text.substr(1));
	}
	if(text.empty()) {
		return Code::MissingOperand;
	}

	uint64_t result = 0;
	if(text[0] == '$') {
		if(text.size() < 2 || text.size() > 9) {
			return Code::InvalidHex;
		}
		for(size_t i = 1; i < text.size(); i++) {
			char c = text[i];
			int digit;
			if(c >= '0' && c <= '9') {
				digit = c - '0';
			} else if(c >= 'a' && c <= 'f') {
				digit = c - 'a' + 10;
			} else if(c >= 'A' && c <= 'F') {
				digit = c - 'A' + 10;
			} else {
				return Code::InvalidHex;
			}
			result = (result << 4) | (uint64_t)digit;
		}
	} else if(text[0] == '%') {
		if(text.size() < 2 || text.size() > 33) {
			return Code::InvalidBinaryValue;
		}
		for(size_t i = 1; i < text.size(); i++) {
			if(text[i] != '0' && text[i] != '1') {
				return Code::InvalidBinaryValue;
			}
			result = (result << 1) | (uint64_t)(text[i] - '0');
		}
	} else if(text[0] >= '0' && text[0] <= '9') {
		if(text.size() > 10) {
			return Code::OperandOutOfRange;
		}
		for(char c : text) {
			if(c < '0' || c > '9') {
				return Code::InvalidOperands;
			}
			result = result * 10 + (uint64_t)(c - '0');
		}
		if(result > 0xFFFFFFFF) {
			return Code::OperandOutOfRange;
		}
	} else {
		if(!IsLabelName(text) || !ctx) {
			return Code::InvalidOperands;
		}

		int64_t addr = -1;
		auto found = ctx->Labels->find(text);
		if(found != ctx->Labels->end()) {
			addr = found->second;
		} else if(ctx->LabelMgr) {
			addr = ctx->LabelMgr->GetLabelRelativeAddress(text, CpuType::Sac);
		}

		if(addr < 0) {
			if(!ctx->FirstPass) {
				return Code::UnknownLabel;
			}
			//It may be defined further down: stand in the line's own address until the next pass
			ctx->NeedSecondPass = true;
			addr = ctx->Address;
		}
		result = (uint64_t)addr;
	}

	value = negative ? -(int64_t)result : (int64_t)result;
	return Code::OK;
}

//D0-D7 = 0-7, A0-A7 (and SP) = 8-15
static int ParseRegister(const string& text)
{
	string reg = StringUtilities::ToLower(StringUtilities::Trim(text));
	if(reg == "sp") {
		return 15;
	}
	if(reg.size() == 2 && (reg[0] == 'd' || reg[0] == 'a') && reg[1] >= '0' && reg[1] <= '7') {
		return (reg[0] == 'a' ? 8 : 0) + (reg[1] - '0');
	}
	return -1;
}

//"D0-D3/A0" as a mask, bit n for register n
static int ParseRegisterList(const string& text)
{
	int mask = 0;
	for(string item : StringUtilities::Split(text, '/')) {
		size_t dash = item.find('-');
		int first = ParseRegister(dash == string::npos ? item : item.substr(0, dash));
		int last = dash == string::npos ? first : ParseRegister(item.substr(dash + 1));
		if(first < 0 || last < first) {
			return -1;
		}
		for(int i = first; i <= last; i++) {
			mask |= 1 << i;
		}
	}
	return mask;
}

//"D4.w", "A2.l*4"
static bool ParseIndexRegister(string text, SacAsmOperand& op)
{
	text = StringUtilities::ToLower(StringUtilities::Trim(text));
	op.Scale = 0;
	size_t star = text.find('*');
	if(star != string::npos) {
		string scale = text.substr(star + 1);
		text = text.substr(0, star);
		if(scale == "1") {
			op.Scale = 0;
		} else if(scale == "2") {
			op.Scale = 1;
		} else if(scale == "4") {
			op.Scale = 2;
		} else if(scale == "8") {
			op.Scale = 3;
		} else {
			return false;
		}
	}

	op.IndexLong = EndsWith(text, ".l");
	if(op.IndexLong || EndsWith(text, ".w")) {
		text.resize(text.size() - 2);
	}

	int reg = ParseRegister(text);
	if(reg < 0) {
		return false;
	}
	op.IndexReg = (uint8_t)reg;
	return true;
}

static Code ParseOperand(string text, SacAsmOperand& op, SacAsmContext* ctx)
{
	text = StringUtilities::Trim(text);
	string lower = StringUtilities::ToLower(text);
	op = {};

	if(text.empty()) {
		//MOVEM with no registers
		op.Kind = K::RegList;
		return Code::OK;
	} else if(text[0] == '#') {
		op.Kind = K::Imm;
		return ParseNumber(text.substr(1), op.Value, ctx);
	} else if(lower == "sr") {
		op.Kind = K::Sr;
		return Code::OK;
	} else if(lower == "ccr") {
		op.Kind = K::Ccr;
		return Code::OK;
	} else if(lower == "usp") {
		op.Kind = K::Usp;
		return Code::OK;
	}

	int reg = ParseRegister(text);
	if(reg >= 0) {
		op.Kind = reg < 8 ? K::Dn : K::An;
		op.Reg = (uint8_t)(reg & 7);
		return Code::OK;
	}

	//-(An) and (An)+
	if(lower.size() > 3 && lower[0] == '-' && lower[1] == '(' && lower.back() == ')') {
		reg = ParseRegister(lower.substr(2, lower.size() - 3));
		if(reg < 8) {
			return Code::InvalidOperands;
		}
		op.Kind = K::PreDec;
		op.Reg = (uint8_t)(reg - 8);
		return Code::OK;
	} else if(lower.size() > 3 && lower[0] == '(' && EndsWith(lower, ")+")) {
		reg = ParseRegister(lower.substr(1, lower.size() - 3));
		if(reg < 8) {
			return Code::InvalidOperands;
		}
		op.Kind = K::PostInc;
		op.Reg = (uint8_t)(reg - 8);
		return Code::OK;
	}

	//Absolute addresses: $xxxx.w and $xxxxxx.l, also written (xxx).w and (xxx).l
	bool absShort = EndsWith(lower, ".w");
	if((absShort || EndsWith(lower, ".l")) && lower.find(',') == string::npos) {
		string addr = StringUtilities::Trim(text.substr(0, text.size() - 2));
		if(addr.size() >= 2 && addr[0] == '(' && addr.back() == ')') {
			addr = addr.substr(1, addr.size() - 2);
		}
		op.Kind = absShort ? K::AbsShort : K::AbsLong;
		return ParseNumber(addr, op.Value, ctx);
	}

	//(An), (d,An), (d,An,Xi), (d,PC), (d,PC,Xi) and the older d(An) forms
	size_t open = text.find('(');
	if(open != string::npos && text.back() == ')') {
		string disp = StringUtilities::Trim(text.substr(0, open));
		bool hasPrefix = !disp.empty();
		vector<string> parts = StringUtilities::Split(text.substr(open + 1, text.size() - open - 2), ',');
		for(string& part : parts) {
			part = StringUtilities::Trim(part);
		}
		if(!hasPrefix && parts.size() > 1 && ParseRegister(parts[0]) < 0 && StringUtilities::ToLower(parts[0]) != "pc") {
			disp = parts[0];
			parts.erase(parts.begin());
		}
		if(parts.empty() || parts.size() > 2) {
			return Code::InvalidOperands;
		}

		bool pc = StringUtilities::ToLower(parts[0]) == "pc";
		int baseReg = pc ? 0 : ParseRegister(parts[0]);
		if(!pc && baseReg < 8) {
			if(parts.size() == 1 && disp.empty()) {
				//(xxx): an address in parentheses
				op.Kind = K::Value;
				return ParseNumber(parts[0], op.Value, ctx);
			}
			return Code::InvalidOperands;
		}

		op.Reg = (uint8_t)(pc ? 0 : baseReg - 8);
		if(!pc && parts.size() == 1 && disp.empty()) {
			op.Kind = K::Ind;
			return Code::OK;
		}
		if(!disp.empty()) {
			Code result = ParseNumber(disp, op.Value, ctx);
			if(result != Code::OK) {
				return result;
			}
		}
		if(parts.size() == 1) {
			op.Kind = pc ? K::PcDisp : K::Disp;
		} else {
			op.Kind = pc ? K::PcIndex : K::Index;
			if(!ParseIndexRegister(parts[1], op)) {
				return Code::InvalidOperands;
			}
		}
		return Code::OK;
	}

	//A register list (MOVEM)
	if(lower.find('/') != string::npos || lower.find('-') != string::npos) {
		int mask = ParseRegisterList(text);
		if(mask >= 0) {
			op.Kind = K::RegList;
			op.Value = mask;
			return Code::OK;
		}
	}

	op.Kind = K::Value;
	return ParseNumber(text, op.Value, ctx);
}

//Operands are separated by the commas outside parentheses
static vector<string> SplitOperands(const string& text)
{
	vector<string> result;
	string current;
	int depth = 0;
	for(char c : text) {
		if(c == '(') {
			depth++;
		} else if(c == ')') {
			depth--;
		}

		if(c == ',' && depth == 0) {
			result.push_back(current);
			current.clear();
		} else {
			current += c;
		}
	}
	result.push_back(current);
	return result;
}

static Code ParseLine(string text, SacAsmLine& line, SacAsmContext* ctx)
{
	size_t comment = text.find(';');
	if(comment != string::npos) {
		text = text.substr(0, comment);
	}
	text = StringUtilities::Trim(text);

	size_t space = text.find_first_of(" \t");
	line.Mnemonic = StringUtilities::ToLower(space == string::npos ? text : text.substr(0, space));
	line.Operands.clear();
	if(space != string::npos) {
		string operands = StringUtilities::Trim(text.substr(space));
		if(!operands.empty()) {
			for(string& part : SplitOperands(operands)) {
				SacAsmOperand op;
				Code result = ParseOperand(part, op, ctx);
				if(result != Code::OK) {
					return result;
				}
				line.Operands.push_back(op);
			}
		}
	}

	//MOVEM's register list can be a single register
	if(line.Mnemonic.compare(0, 5, "movem") == 0) {
		for(SacAsmOperand& op : line.Operands) {
			if(op.Kind == K::Dn || op.Kind == K::An) {
				op.Value = (int64_t)1 << (op.Reg + (op.Kind == K::An ? 8 : 0));
				op.Kind = K::RegList;
			}
		}
	}
	return Code::OK;
}

//The mnemonic and what kind of operands it has, without their values
static string GetShapeKey(const string& mnemonic, const vector<SacAsmOperand>& operands)
{
	string key = mnemonic;
	for(size_t i = 0; i < operands.size(); i++) {
		const SacAsmOperand& op = operands[i];
		string reg = std::to_string(op.Reg);
		key += i == 0 ? " " : ",";
		switch(op.Kind) {
			case K::Dn: key += "D" + reg; break;
			case K::An: key += "A" + reg; break;
			case K::Ind: key += "(A" + reg + ")"; break;
			case K::PostInc: key += "(A" + reg + ")+"; break;
			case K::PreDec: key += "-(A" + reg + ")"; break;
			case K::Disp: key += "(d,A" + reg + ")"; break;
			case K::Index: key += "(d,A" + reg + ",X)"; break;
			case K::AbsShort: key += "abs.w"; break;
			case K::AbsLong: key += "abs.l"; break;
			case K::PcDisp: key += "(d,PC)"; break;
			case K::PcIndex: key += "(d,PC,X)"; break;
			case K::Imm: key += "#"; break;
			case K::RegList: key += "list"; break;
			case K::Sr: key += "SR"; break;
			case K::Ccr: key += "CCR"; break;
			case K::Usp: key += "USP"; break;
			case K::Value: key += "val"; break;
		}
	}
	return key;
}

//Every opcode word, disassembled with its extension words all zero, by shape. Built on first use.
static const SacAsmIndex& GetIndex()
{
	static SacAsmIndex index = [] {
		SacAsmIndex result;
		for(uint32_t opCode = 0; opCode <= 0xFFFF; opCode++) {
			uint8_t bytes[10] = { (uint8_t)(opCode >> 8), (uint8_t)opCode };
			string text;
			int length = SacCpu::DisassembleBytes(0, bytes, sizeof(bytes), text);
			SacAsmLine line;
			if(ParseLine(text, line, nullptr) != Code::OK || line.Mnemonic.empty() || line.Mnemonic.compare(0, 3, "dc.") == 0) {
				continue;
			}
			result.ByShape[GetShapeKey(line.Mnemonic, line.Operands)].push_back({ (uint16_t)opCode, (uint8_t)length });
			result.Mnemonics.insert(line.Mnemonic);
		}
		return result;
	}();
	return index;
}

static uint8_t GetSize(const string& mnemonic)
{
	if(EndsWith(mnemonic, ".b")) {
		return 1;
	} else if(EndsWith(mnemonic, ".w")) {
		return 2;
	} else if(EndsWith(mnemonic, ".l")) {
		return 4;
	}
	return 0;
}

static uint16_t Reverse16(uint16_t value)
{
	uint16_t result = 0;
	for(int i = 0; i < 16; i++) {
		if(value & (1 << i)) {
			result |= (uint16_t)(1 << (15 - i));
		}
	}
	return result;
}

//Whether an operand as typed and the same operand as disassembled say the same thing
static bool Matches(const SacAsmOperand& a, const SacAsmOperand& b, uint8_t size, const string& mnemonic)
{
	if(a.Kind != b.Kind) {
		return false;
	}

	auto same = [&](int64_t mask) { return (a.Value & mask) == (b.Value & mask); };
	bool sameIndex = a.IndexReg == b.IndexReg && a.IndexLong == b.IndexLong && a.Scale == b.Scale;
	switch(a.Kind) {
		case K::Dn: case K::An: case K::Ind: case K::PostInc: case K::PreDec: return a.Reg == b.Reg;
		case K::Disp: return a.Reg == b.Reg && same(0xFFFF);
		case K::Index: return a.Reg == b.Reg && sameIndex && same(0xFF);
		case K::PcDisp: return same(0xFFFF);
		case K::PcIndex: return sameIndex && same(0xFF);
		case K::AbsShort: return same(0xFFFF);
		case K::AbsLong: return same(0xFFFFFFFF);
		case K::Imm: return same(size == 4 ? 0xFFFFFFFF : (size == 1 || mnemonic == "moveq") ? 0xFF : 0xFFFF);
		case K::RegList: return same(0xFFFF);
		case K::Value: return same(0xFFFFFF);
		default: return true;
	}
}

//Completes one candidate opcode with extension words and checks it disassembles back to the line
static bool TryCandidate(const SacAsmCandidate& candidate, const string& mnemonic, const vector<SacAsmOperand>& operands, SacAsmContext& ctx, vector<uint8_t>& out, Code& error)
{
	uint8_t size = GetSize(mnemonic);
	bool isMovem = mnemonic.compare(0, 5, "movem") == 0;

	//Extension words the operand modes need; what is left over belongs to the immediate (none
	//when it sits in the opcode, as ADDQ's and MOVEQ's do)
	int fixedWords = isMovem ? 1 : 0;
	int immCount = 0;
	for(const SacAsmOperand& op : operands) {
		switch(op.Kind) {
			case K::Disp: case K::Index: case K::PcDisp: case K::PcIndex: case K::AbsShort: case K::Value: fixedWords++; break;
			case K::AbsLong: fixedWords += 2; break;
			case K::Imm: immCount++; break;
			default: break;
		}
	}
	int immWords = (candidate.Length - 2) / 2 - fixedWords;
	if(immWords < 0 || immWords > 2 || immCount > 1 || (immCount == 0 && immWords != 0)) {
		return false;
	}

	vector<uint16_t> words;
	if(isMovem) {
		//The register mask comes first, bit-reversed for -(An)
		uint16_t mask = 0;
		bool preDec = false;
		for(const SacAsmOperand& op : operands) {
			if(op.Kind == K::RegList) {
				mask = (uint16_t)op.Value;
			} else if(op.Kind == K::PreDec) {
				preDec = true;
			}
		}
		words.push_back(preDec ? Reverse16(mask) : mask);
	}

	auto outOfRange = [&error](Code code) { error = code; return false; };
	for(const SacAsmOperand& op : operands) {
		switch(op.Kind) {
			case K::Imm:
				if(immWords == 1) {
					if(size == 1 ? !Fits(op.Value, -0x80, 0xFF) : !Fits(op.Value, -0x8000, 0xFFFF)) {
						return outOfRange(Code::OperandOutOfRange);
					}
					words.push_back((uint16_t)(op.Value & (size == 1 ? 0xFF : 0xFFFF)));
				} else if(immWords == 2) {
					if(!Fits(op.Value, -0x80000000LL, 0xFFFFFFFFLL)) {
						return outOfRange(Code::OperandOutOfRange);
					}
					words.push_back((uint16_t)(op.Value >> 16));
					words.push_back((uint16_t)op.Value);
				}
				break;

			case K::Disp:
			case K::PcDisp:
				if(!Fits(op.Value, -0x8000, 0x7FFF)) {
					return outOfRange(Code::OperandOutOfRange);
				}
				words.push_back((uint16_t)op.Value);
				break;

			case K::Index:
			case K::PcIndex:
				if(!Fits(op.Value, -0x80, 0x7F)) {
					return outOfRange(Code::OperandOutOfRange);
				}
				words.push_back((uint16_t)((op.IndexReg << 12) | (op.IndexLong ? 0x800 : 0) | (op.Scale << 9) | (op.Value & 0xFF)));
				break;

			case K::AbsShort:
				if(!FitsShortAddress(op.Value, true)) {
					return outOfRange(Code::OperandOutOfRange);
				}
				words.push_back((uint16_t)op.Value);
				break;

			case K::AbsLong:
				if(!Fits(op.Value, -0x80000000LL, 0xFFFFFFFFLL)) {
					return outOfRange(Code::OperandOutOfRange);
				}
				words.push_back((uint16_t)(op.Value >> 16));
				words.push_back((uint16_t)op.Value);
				break;

			case K::Value: {
				//DBcc's target, as a distance from the extension word
				int64_t disp = GetBranchDisplacement(op.Value, ctx.Address);
				if(!Fits(disp, -0x8000, 0x7FFF)) {
					if(!ctx.FirstPass) {
						return outOfRange(Code::OutOfRangeJump);
					}
					disp = 0;
				}
				words.push_back((uint16_t)disp);
				break;
			}

			default:
				break;
		}
	}

	out.clear();
	out.push_back((uint8_t)(candidate.OpCode >> 8));
	out.push_back((uint8_t)candidate.OpCode);
	for(uint16_t word : words) {
		out.push_back((uint8_t)(word >> 8));
		out.push_back((uint8_t)word);
	}
	if(out.size() != candidate.Length) {
		return false;
	}

	string text;
	int length = SacCpu::DisassembleBytes(ctx.Address, out.data(), (uint32_t)out.size(), text);
	SacAsmLine decoded;
	if(length != (int)out.size() || ParseLine(text, decoded, nullptr) != Code::OK || decoded.Mnemonic != mnemonic || decoded.Operands.size() != operands.size()) {
		return false;
	}
	for(size_t i = 0; i < operands.size(); i++) {
		if(!Matches(operands[i], decoded.Operands[i], size, mnemonic)) {
			return false;
		}
	}
	return true;
}

static int GetBranchOpCode(const string& mnemonic)
{
	static const char* names[16] = { "bra", "bsr", "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc", "bvs", "bpl", "bmi", "bge", "blt", "bgt", "ble" };
	for(int i = 0; i < 16; i++) {
		if(mnemonic == names[i]) {
			return 0x6000 | (i << 8);
		}
	}
	if(mnemonic == "bhs") {
		return 0x6400;
	} else if(mnemonic == "blo") {
		return 0x6500;
	}
	return -1;
}

//Bcc, BRA and BSR take the short form when the target is close enough, unless .w is given
static Code AssembleBranch(int opCode, bool forceShort, bool forceWord, const SacAsmLine& line, SacAsmContext& ctx, vector<uint8_t>& out)
{
	if(line.Operands.size() != 1 || line.Operands[0].Kind != K::Value) {
		return Code::InvalidOperands;
	}

	//A displacement byte of 0 means a word follows, and $FF is the 68020's long form
	int64_t disp = GetBranchDisplacement(line.Operands[0].Value, ctx.Address);
	bool shortFits = Fits(disp, -0x80, 0x7F) && disp != 0 && disp != -1;
	if(forceShort || (!forceWord && shortFits)) {
		if(!shortFits) {
			if(!ctx.FirstPass) {
				return Code::OutOfRangeJump;
			}
			disp = 2;
		}
		out = { (uint8_t)(opCode >> 8), (uint8_t)disp };
	} else {
		if(!Fits(disp, -0x8000, 0x7FFF)) {
			if(!ctx.FirstPass) {
				return Code::OutOfRangeJump;
			}
			disp = 0;
		}
		out = { (uint8_t)(opCode >> 8), 0, (uint8_t)(disp >> 8), (uint8_t)disp };
	}
	return Code::OK;
}

static Code AssembleInstruction(const SacAsmLine& line, SacAsmContext& ctx, vector<uint8_t>& out)
{
	string mnemonic = line.Mnemonic == "dbf" ? "dbra" : line.Mnemonic;

	size_t dot = mnemonic.find('.');
	string suffix = dot == string::npos ? "" : mnemonic.substr(dot + 1);
	int branch = GetBranchOpCode(mnemonic.substr(0, dot));
	if(branch >= 0 && (suffix.empty() || suffix == "s" || suffix == "b" || suffix == "w")) {
		return AssembleBranch(branch, suffix == "s" || suffix == "b", suffix == "w", line, ctx, out);
	}

	//Without a size, try .w (the usual default); with one, also try without (LEA.L, MOVEQ.L)
	vector<string> mnemonics = { mnemonic };
	mnemonics.push_back(dot == string::npos ? mnemonic + ".w" : mnemonic.substr(0, dot));

	//An address written without .w or .l can be either, the short form first when it reaches
	vector<vector<SacAsmOperand>> variants = { line.Operands };
	for(size_t i = 0; i < line.Operands.size(); i++) {
		if(line.Operands[i].Kind != K::Value) {
			continue;
		}
		vector<vector<SacAsmOperand>> next;
		for(const vector<SacAsmOperand>& variant : variants) {
			next.push_back(variant);
			if(FitsShortAddress(variant[i].Value, false)) {
				next.push_back(variant);
				next.back()[i].Kind = K::AbsShort;
			}
			next.push_back(variant);
			next.back()[i].Kind = K::AbsLong;
		}
		variants = next;
	}

	const SacAsmIndex& index = GetIndex();
	bool knownMnemonic = false;
	Code error = Code::InvalidOperands;
	for(const string& name : mnemonics) {
		if(index.Mnemonics.find(name) == index.Mnemonics.end()) {
			continue;
		}
		knownMnemonic = true;
		for(const vector<SacAsmOperand>& operands : variants) {
			auto found = index.ByShape.find(GetShapeKey(name, operands));
			if(found == index.ByShape.end()) {
				continue;
			}
			for(const SacAsmCandidate& candidate : found->second) {
				if(TryCandidate(candidate, name, operands, ctx, out, error)) {
					return Code::OK;
				}
			}
		}
	}
	return knownMnemonic ? error : Code::InvalidInstruction;
}

static void ProcessLine(string code, SacAsmContext& ctx, vector<int16_t>& output, unordered_map<string, uint32_t>& currentPassLabels)
{
	size_t comment = code.find(';');
	if(comment != string::npos) {
		code = code.substr(0, comment);
	}
	code = StringUtilities::Trim(code);

	//A label definition, optionally followed by an instruction
	size_t colon = code.find(':');
	if(colon != string::npos && IsLabelName(code.substr(0, colon))) {
		string label = code.substr(0, colon);
		if(currentPassLabels.find(label) != currentPassLabels.end()) {
			output.push_back(Code::LabelRedefinition);
			return;
		}
		(*ctx.Labels)[label] = ctx.Address;
		currentPassLabels[label] = ctx.Address;
		ProcessLine(code.substr(colon + 1), ctx, output, currentPassLabels);
		return;
	}

	if(code.empty()) {
		output.push_back(Code::EndOfLine);
		return;
	}

	//.db $xx $xx ...
	string lower = StringUtilities::ToLower(code);
	if(lower.compare(0, 3, ".db") == 0 && (code.size() == 3 || code[3] == ' ' || code[3] == '\t')) {
		vector<uint8_t> bytes;
		for(string item : StringUtilities::Split(StringUtilities::Trim(code.substr(3)), ' ')) {
			if(StringUtilities::Trim(item).empty()) {
				continue;
			}
			int64_t value;
			Code result = ParseNumber(item, value, &ctx);
			if(result == Code::OK && !Fits(value, -0x80, 0xFF)) {
				result = Code::OperandOutOfRange;
			}
			if(result != Code::OK) {
				output.push_back(result);
				return;
			}
			bytes.push_back((uint8_t)value);
		}
		for(uint8_t value : bytes) {
			output.push_back(value);
		}
		ctx.Address += (uint32_t)bytes.size();
		output.push_back(Code::EndOfLine);
		return;
	}

	SacAsmLine line;
	vector<uint8_t> bytes;
	Code result = ParseLine(code, line, &ctx);
	if(result == Code::OK) {
		result = AssembleInstruction(line, ctx, bytes);
	}
	if(result != Code::OK) {
		output.push_back(result);
		return;
	}

	for(uint8_t value : bytes) {
		output.push_back(value);
	}
	ctx.Address += (uint32_t)bytes.size();
	output.push_back(Code::EndOfLine);
}

uint32_t SacAssembler::AssembleCode(string code, uint32_t startAddress, int16_t* assembledCode)
{
	unordered_map<string, uint32_t> labels;
	unordered_map<string, uint32_t> currentPassLabels;
	vector<string> lines = StringUtilities::Split(code, '\n');
	vector<int16_t> output;

	SacAsmContext ctx;
	ctx.Labels = &labels;
	ctx.LabelMgr = _labelManager;

	auto runPass = [&](bool firstPass) {
		output.clear();
		currentPassLabels.clear();
		ctx.Address = startAddress;
		ctx.FirstPass = firstPass;
		for(string& line : lines) {
			ProcessLine(line, ctx, output, currentPassLabels);
		}
	};

	//The first pass finds the labels; the next ones repeat until the sizes stop changing
	runPass(true);
	if(ctx.NeedSecondPass) {
		vector<int16_t> previous;
		for(int pass = 0; pass < 5 && previous != output; pass++) {
			previous = output;
			runPass(false);
		}
	}

	uint32_t count = (uint32_t)std::min<size_t>(output.size(), 100000);
	memcpy(assembledCode, output.data(), count * sizeof(int16_t));
	return count;
}
