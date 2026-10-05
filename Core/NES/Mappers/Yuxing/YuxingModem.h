#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/Serializer.h"

//The modem on the floppy model's serial port: a 16550 UART with a Hayes modem behind it.
//The system disc's online program drives it. It programs the UART for 19200 baud 8N1, raises
//DTR and RTS, waits for clear-to-send, sends AT&F and waits for the reply to arrive through
//its receive interrupt; only then does a key on its title open its main screen, where the
//dial key sends ATDT 163.
//
//There is no telephone line here, so this modem answers and never connects: it reports
//clear-to-send and data-set-ready, echoes and acknowledges commands as a modem fresh from
//AT&F does, and a dial gives up with NO CARRIER after the few seconds a real one spends
//trying - which the program shows as no carrier, redial. Carrier detect never rises. Without
//any of it the program waited for clear-to-send for ever, on its title screen.
//
//The UART is decoded at $424C-$43FC: address bits 7, 4 and 5 select the register (in that
//order, least significant first), and bit 8 is not decoded.
class YuxingModem final : public ISerializable
{
private:
	static constexpr uint32_t RxSize = 256;
	//How long a dial runs before giving up, and how long a command takes to answer, in CPU
	//cycles at the machine's ~1.77MHz
	static constexpr uint32_t DialCycles = 1770000 * 4;
	static constexpr uint32_t ReplyCycles = 1770000 / 50;
	static constexpr uint32_t MaxCommand = 64;

	uint8_t _ier = 0;
	uint8_t _lcr = 0;
	uint8_t _mcr = 0;
	uint8_t _scr = 0;
	uint8_t _dll = 0;
	uint8_t _dlm = 0;

	uint8_t _rx[RxSize] = {};
	uint32_t _rxHead = 0;
	uint32_t _rxCount = 0;

	uint8_t _command[MaxCommand + 1] = {};
	uint32_t _commandLength = 0;
	bool _echo = true;
	bool _verbose = true;
	bool _quiet = false;

	//A reply waiting for its delay to run out: 0 none, 1 OK, 2 NO CARRIER, 3 ERROR
	uint8_t _pendingReply = 0;
	uint32_t _replyDelay = 0;

	void Push(uint8_t value)
	{
		if(_rxCount < RxSize) {
			_rx[(_rxHead + _rxCount) % RxSize] = value;
			_rxCount++;
		}
	}

	void PushText(const char* text)
	{
		while(*text) {
			Push((uint8_t)*text++);
		}
	}

	void Reply(uint8_t code)
	{
		if(_quiet) {
			return;
		}
		//The numeric codes are the Hayes ones for OK, NO CARRIER and ERROR
		static const char* words[] = { "", "OK", "NO CARRIER", "ERROR" };
		static const char* digits[] = { "", "0", "3", "4" };
		if(_verbose) {
			PushText("\r\n");
			PushText(words[code]);
			PushText("\r\n");
		} else {
			PushText(digits[code]);
			PushText("\r");
		}
	}

	void Schedule(uint8_t code, uint32_t delay)
	{
		_pendingReply = code;
		_replyDelay = delay;
	}

	//One command line, "AT" already found at its start
	void RunCommand(const char* line)
	{
		bool dial = false;
		for(const char* p = line; *p; p++) {
			char c = (char)toupper((uint8_t)*p);
			char next = p[1];
			switch(c) {
				case 'D': dial = true; break;
				case 'E': _echo = next != '0'; break;
				case 'V': _verbose = next != '0'; break;
				case 'Q': _quiet = next == '1'; break;
				case 'Z':
					_echo = true; _verbose = true; _quiet = false;
					break;
				case '&':
					//&F - the factory settings the program starts from
					if(toupper((uint8_t)next) == 'F') {
						_echo = true; _verbose = true; _quiet = false;
						p++;
					}
					break;
			}
			if(dial) {
				//Everything after D is the number
				break;
			}
		}

		if(dial) {
			MessageManager::Log(std::string("[YuXing] Modem dialling (no line): ") + line);
			Schedule(2, DialCycles);
		} else {
			Schedule(1, ReplyCycles);
		}
	}

	void ReceiveFromMachine(uint8_t value)
	{
		//A character sent while a dial is under way abandons it, as on a real modem
		if(_pendingReply == 2) {
			Schedule(2, ReplyCycles);
		}

		if(_echo) {
			Push(value);
		}

		if(value == '\r') {
			_command[_commandLength] = 0;
			//A modem passes over anything before the AT: the program hangs up with "+++ATH",
			//the escape and the command on one line
			for(const char* line = (const char*)_command; line[0] && line[1]; line++) {
				if(toupper((uint8_t)line[0]) == 'A' && toupper((uint8_t)line[1]) == 'T') {
					RunCommand(line + 2);
					break;
				}
			}
			_commandLength = 0;
		} else if(value == 8 || value == 0x7F) {
			if(_commandLength > 0) {
				_commandLength--;
			}
		} else if(value >= 0x20 && _commandLength < MaxCommand) {
			_command[_commandLength++] = value;
		}
	}

public:
	//The modem sends to the machine only while the machine holds RTS - hardware flow control.
	//The program raises it exactly when it is ready to take a reply and drops it as it turns
	//its interrupt off, so what the modem says meanwhile waits in the modem rather than in the
	//UART: a program that left with a reply unread would otherwise have handed the next one
	//an interrupt nothing services.
	bool CanDeliver() { return _rxCount > 0 && (_mcr & 0x02); }

	//The receive interrupt. The program never writes the interrupt-enable register: its
	//handler takes a byte whenever the line says one has arrived, so on this board the UART's
	//interrupt is the receiver's data-ready and nothing else gates it.
	bool IsInterruptPending() { return CanDeliver(); }

	//Whether an address is one of the UART's registers, and which
	static bool IsModemAddress(uint16_t addr) { return (addr & 0xFE4F) == 0x424C; }
	static uint8_t RegisterOf(uint16_t addr) { return (uint8_t)(((addr >> 7) & 1) | ((addr >> 3) & 2) | ((addr >> 3) & 4)); }

	void Reset()
	{
		_ier = _lcr = _mcr = _scr = _dll = _dlm = 0;
		_rxHead = _rxCount = 0;
		_commandLength = 0;
		_echo = _verbose = true;
		_quiet = false;
		_pendingReply = 0;
		_replyDelay = 0;
	}

	void Clock()
	{
		if(_pendingReply && --_replyDelay == 0) {
			Reply(_pendingReply);
			_pendingReply = 0;
		}
	}

	uint8_t Read(uint16_t addr)
	{
		switch(RegisterOf(addr)) {
			case 0:
				if(_lcr & 0x80) {
					return _dll;
				}
				if(CanDeliver()) {
					uint8_t value = _rx[_rxHead];
					_rxHead = (_rxHead + 1) % RxSize;
					_rxCount--;
					return value;
				}
				return 0;

			case 1: return (_lcr & 0x80) ? _dlm : _ier;
			//No interrupt pending, FIFOs on
			case 2: return 0xC1;
			case 3: return _lcr;
			case 4: return _mcr;
			//Transmitter always empty; data ready while anything is waiting
			case 5: return (uint8_t)(0x60 | (CanDeliver() ? 0x01 : 0x00));
			//Clear to send and data set ready while the machine holds RTS and DTR; carrier
			//detect never
			case 6: return (uint8_t)(((_mcr & 0x02) ? 0x10 : 0) | ((_mcr & 0x01) ? 0x20 : 0));
			case 7: return _scr;
		}
		return 0;
	}

	void Write(uint16_t addr, uint8_t value)
	{
		switch(RegisterOf(addr)) {
			case 0:
				if(_lcr & 0x80) {
					_dll = value;
				} else {
					ReceiveFromMachine(value);
				}
				break;

			case 1:
				if(_lcr & 0x80) {
					_dlm = value;
				} else {
					_ier = value;
				}
				break;

			case 2:
				//FIFO control: bit 1 clears the receive FIFO
				if(value & 0x02) {
					_rxHead = _rxCount = 0;
				}
				break;

			case 3: _lcr = value; break;
			case 4: _mcr = value; break;
			case 7: _scr = value; break;
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_ier); SV(_lcr); SV(_mcr); SV(_scr); SV(_dll); SV(_dlm);
		SVArray(_rx, RxSize); SV(_rxHead); SV(_rxCount);
		SVArray(_command, MaxCommand + 1); SV(_commandLength);
		SV(_echo); SV(_verbose); SV(_quiet);
		SV(_pendingReply); SV(_replyDelay);
	}
};
