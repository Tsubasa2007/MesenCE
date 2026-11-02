#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/Serializer.h"
#include "NES/Mappers/Yuxing/YuxingHost.h"

//The modem on the floppy model's serial port: a 16550 UART with a Hayes modem behind it.
//The system disc's online program drives it. It programs the UART for 19200 baud 8N1, raises
//DTR and RTS, waits for clear-to-send, sends AT&F and waits for the reply to arrive through
//its receive interrupt; only then does a key on its title open its main screen, where the
//dial key sends ATDT 163.
//
//There is no telephone line here. The modem reports clear-to-send and data-set-ready, echoes
//and acknowledges commands as a modem fresh from AT&F does - without that the program waited for
//clear-to-send for ever, on its title screen - and what a dial reaches depends on the online
//option: off, it gives up with NO CARRIER after the few seconds a real one spends trying, which
//the program shows as no carrier, redial; on, it connects to YuxingHost, which plays the service
//the program was written for. Online, what the host sends comes at the line's 19200 baud, and
//"+++" returns to commands, where ATH hangs up.
//
//The UART is decoded at $424C-$43FC: address bits 7, 4 and 5 select the register (in that
//order, least significant first), and bit 8 is not decoded.
//
//The earlier online programs, off floppy, have no UART: they bit-bang the same 19200 baud 8N1
//through the controller port. Out on $4016 bit 1, inverted, with a write for every bit and a
//delay between them ($02 is the start bit, $00 the stop bit). In on $4017 bit 1: the start bit
//raises the IRQ and the handler reads the eight bits, least significant first. $4016 bit 0 is
//their RTS. These programs talk to the modem behind this port exactly as the later one does.
class YuxingModem final : public ISerializable
{
private:
	static constexpr uint32_t RxSize = 256;
	//How long a dial runs before giving up, and how long a command takes to answer, in CPU
	//cycles at the machine's ~1.77MHz
	static constexpr uint32_t DialCycles = 1770000 * 4;
	static constexpr uint32_t ReplyCycles = 1770000 / 50;
	static constexpr uint32_t ConnectCycles = 1770000 * 3;
	static constexpr uint32_t GuardCycles = 1770000;
	//One character at 19200 baud, 10 bits with start and stop
	static constexpr uint32_t CharCycles = 1770000 / 1920;
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

	//A reply waiting for its delay to run out: 0 none, 1 OK, 2 NO CARRIER, 3 ERROR, 4 CONNECT
	uint8_t _pendingReply = 0;
	uint32_t _replyDelay = 0;

	//The online option, as the machine's settings have it - read when a dial starts
	bool _serverEnabled = false;
	YuxingHost _host;
	//Connected and passing data, rather than taking commands (after +++ the call stays up)
	bool _online = false;
	//A command has come in: the online program is running
	bool _inUse = false;
	//'+' characters held back while they might be the escape
	uint8_t _plusCount = 0;
	uint32_t _charDelay = 0;

	//The controller-port link: the bit being sent (0 = none, 1-8 data, 9 stop), the byte so far
	//and when the last bit came; whether a byte has come this way, RTS, and the byte being read
	static constexpr uint32_t PortBitCycles = 300;
	static constexpr uint32_t PortGapCycles = 300;
	uint8_t _portPhase = 0;
	uint8_t _portByte = 0;
	uint64_t _portCycle = 0;
	bool _portActive = false;
	bool _portRts = false;
	uint8_t _portShift = 0;
	uint8_t _portBits = 0;
	uint32_t _portWait = 0;
	bool _portIrq = false;
	bool _dialPadded = false;

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
		static const char* words[] = { "", "OK", "NO CARRIER", "ERROR", "CONNECT 19200" };
		static const char* digits[] = { "", "0", "3", "4", "1" };
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
		_inUse = true;
		//The controller port is a bare line: what the modem sent while nobody listened is gone by the
		//time the next command comes. Otherwise a reply the program left unread - the OK to the ATH
		//it hangs up with on leaving - is the first thing the next dial reads, and it gives up
		if(_portActive) {
			_rxHead = _rxCount = 0;
			_portBits = 0;
			_portIrq = false;
		}
		bool dial = false;
		for(const char* p = line; *p; p++) {
			char c = (char)toupper((uint8_t)*p);
			char next = p[1];
			switch(c) {
				case 'D': dial = true; break;
				case 'H':
					if(_host.IsConnected()) {
						MessageManager::Log("[YuXing] Modem hung up");
					}
					HangUp();
					break;
				case 'O':
					//Back to the call +++ left
					if(_host.IsConnected()) {
						_online = true;
						Schedule(4, ReplyCycles);
						return;
					}
					break;
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

		if(dial && _serverEnabled) {
			MessageManager::Log(std::string("[YuXing] Modem dialling the built-in host: ") + line);
			Schedule(4, ConnectCycles);
		} else if(dial) {
			MessageManager::Log(std::string("[YuXing] Modem dialling (no line): ") + line);
			Schedule(2, DialCycles);
		} else {
			Schedule(1, ReplyCycles);
		}
	}

	void HangUp()
	{
		_host.Reset();
		_online = false;
		_plusCount = 0;
	}

	void SendOnline(uint8_t value)
	{
		//Three '+' in a row leave the call for commands. Held back until it is clear they are
		//not the escape, so the far end never sees one
		if(value == '+') {
			if(++_plusCount == 3) {
				_plusCount = 0;
				_online = false;
				Schedule(1, GuardCycles);
			}
			return;
		}
		for(; _plusCount > 0; _plusCount--) {
			_host.Receive('+');
		}
		_host.Receive(value);
	}

	void ReceiveFromMachine(uint8_t value)
	{
		if(_online) {
			SendOnline(value);
			return;
		}

		//A character sent while a dial is under way abandons it, as on a real modem - but not the
		//padding of the earliest program's dial string, a fixed field sent to its last CR, which is
		//also how that program is told from the others
		if(value == 0 && _pendingReply == 4) {
			_dialPadded = true;
		}
		//What follows the padding is the rest of that field: whatever an edit of the number left there
		if((_pendingReply == 2 || _pendingReply == 4) && _dialPadded) {
			return;
		}
		if((_pendingReply == 2 || _pendingReply == 4) && value >= 0x20) {
			Schedule(2, ReplyCycles);
		}

		//The controller port cannot take a byte while it sends one, so the echo never gets there. The
		//programs read the reply at a fixed place in what follows, and count on that
		if(_echo && !_portActive) {
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
	bool IsInterruptPending() { return CanDeliver() || _portIrq; }
	bool IsInUse() { return _inUse; }

	//Whether an address is one of the UART's registers, and which
	static bool IsModemAddress(uint16_t addr) { return (addr & 0xFE4F) == 0x424C; }
	static uint8_t RegisterOf(uint16_t addr) { return (uint8_t)(((addr >> 7) & 1) | ((addr >> 3) & 2) | ((addr >> 3) & 4)); }

	bool IsPortActive() { return _portActive; }

	//Once a cycle while the port is in use. A byte starts only while the program listens - RTS up
	//and interrupts on, which is how it waits and not how it runs (a real line would lose it
	//otherwise). $4017 is read by much else, the mouse and the program's own clear-to-send
	//check, and those reads must not take bits; the handler is what comes after the IRQ.
	void PortClock(bool listening)
	{
		if(_portWait) {
			_portWait--;
		} else if(listening && _portRts && _portPhase == 0 && _portBits == 0 && _rxCount > 0) {
			_portShift = _rx[_rxHead];
			_rxHead = (_rxHead + 1) % RxSize;
			_rxCount--;
			_portBits = 8;
			_portIrq = true;
		}
	}

	//A write to $4016. Each bit is one write, so the count of writes is the clock; anything that
	//is not one of the link's values, or that comes too late, is the port's other users
	void PortWrite(uint8_t value, uint64_t cycle)
	{
		if(value > 2 || (_portPhase && cycle - _portCycle > PortBitCycles)) {
			_portPhase = 0;
			if(value > 2) {
				return;
			}
		}
		_portCycle = cycle;
		if(_portPhase == 0) {
			if(value == 2) {
				_portPhase = 1;
				_portByte = 0;
				//Bit 0 is low from here on: the program has stopped listening
				_portRts = false;
			} else {
				//Nothing comes back the instant it listens: a byte takes a character's time, and the
				//earliest program clears its flag just after raising RTS
				if(value == 1 && !_portRts && _portWait < PortGapCycles) {
					_portWait = PortGapCycles;
				}
				_portRts = value == 1;
			}
		} else if(_portPhase <= 8) {
			if(value == 1) {
				_portPhase = 0;
			} else {
				if(value == 0) {
					_portByte |= (uint8_t)(1 << (_portPhase - 1));
				}
				_portPhase++;
			}
		} else {
			_portPhase = 0;
			if(value == 0) {
				_portActive = true;
				ReceiveFromMachine(_portByte);
			}
		}
	}

	//The handler's reads of $4017; the first lets go of the IRQ line
	uint8_t PortRead()
	{
		_portIrq = false;
		if(_portBits == 0) {
			return 0;
		}
		uint8_t bit = (uint8_t)((_portShift & 1) << 1);
		_portShift >>= 1;
		if(--_portBits == 0) {
			_portWait = PortGapCycles;
		}
		return bit;
	}

	void SetServerEnabled(bool enabled) { _serverEnabled = enabled; }
	void SetEmulator(Emulator* emu) { _host.SetEmulator(emu); }

	void Reset()
	{
		_ier = _lcr = _mcr = _scr = _dll = _dlm = 0;
		_rxHead = _rxCount = 0;
		_commandLength = 0;
		_echo = _verbose = true;
		_quiet = false;
		_pendingReply = 0;
		_replyDelay = 0;
		HangUp();
		_charDelay = 0;
		_portPhase = _portBits = 0;
		_portActive = _portRts = _portIrq = _dialPadded = false;
		_portWait = 0;
	}

	void Clock()
	{
		if(_pendingReply && --_replyDelay == 0) {
			uint8_t code = _pendingReply;
			_pendingReply = 0;
			if(code == 4 && !_host.IsConnected()) {
				MessageManager::Log("[YuXing] Modem connected to the built-in host");
				_host.Connect(_dialPadded, _portActive);
			}
			//The earlier programs' modem is set to W1: it reports the carrier first, and the earliest
			//one takes that line, not CONNECT, for the call being up
			if(code == 4 && _portActive) {
				PushText("\r\nCARRIER 14400\r\n");
			}
			Reply(code);
			if(code == 4) {
				_online = true;
			}
		}

		_host.Clock();
		if(_charDelay) {
			_charDelay--;
		} else if(_online && _rxCount == 0 && _host.HasOutput()) {
			//One character at a time, at the line's speed: the program takes each in its
			//receive interrupt
			Push(_host.TakeOutput());
			_charDelay = CharCycles;
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
		SV(_host); SV(_online); SV(_inUse); SV(_plusCount); SV(_charDelay);
		SV(_portPhase); SV(_portByte); SV(_portCycle); SV(_portActive); SV(_portRts);
		SV(_portShift); SV(_portBits); SV(_portWait); SV(_portIrq); SV(_dialPadded);
	}
};
