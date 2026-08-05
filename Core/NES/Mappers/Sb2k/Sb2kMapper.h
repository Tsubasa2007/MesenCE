#pragma once
#include "pch.h"
#include "NES/BaseMapper.h"
#include "NES/BaseNesPpu.h"
#include "NES/NesConsole.h"
#include "NES/NesCpu.h"
#include "NES/NesMemoryManager.h"
#include "NES/Mappers/Bbk/BbkFdc.h"
#include "NES/Mappers/Bbk/BbkLpcAudio.h"
#include "NES/Input/Sb2kKeyboard.h"
#include "NES/Input/Sb2kMouse.h"
#include "Shared/BaseControlManager.h"
#include "Shared/MessageManager.h"
#include "Shared/NotificationManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/Serializer.h"

//Subor SB-2000 learning machine - iNES mapper 171 variant 1 (header byte 9 = $02,
//the VirtuaNES-BBK fork's private "mapper variant" convention).
//Ported from the VirtuaNES-BBK fork's sb2k branch (NES/Mapper/MapperSB2K.cpp, author fanoble).
//
//The machine has two personalities:
// - Famiclone mode (power-on default): an MMC3-clone cartridge slot whose PRG/CHR both
//   live in RAM - 512KB DRAM ("EPRAM") and 512KB VRAM ("EVRAM"), banked in 128KB slices
//   selected by $5006 ("EBank"). The BIOS itself is fetched through eight 4KB program
//   bank registers ($4040-$4047; bank < $80 = BIOS ROM page, >= $80 = EPRAM page).
//   Software is loaded from floppy into EPRAM and then "RAM boot" ($4301 = $55) flips
//   $8000-$FFFF over to the MMC3 mapping so the loaded program runs like a cartridge.
//   The MMC3 clone's IRQ is a per-scanline counter (like the BBK Holtek), not A12-based.
// - UM6576 native mode (BIOS writes $65,$76 to $4031): the video chip switches to its
//   own register set with 16-bit tile names and a direct 64-entry palette (see Sb2kPpu),
//   reading from 32KB "CRAM" and a banked 32KB window into EVRAM ($4300 = bank). The
//   CPU also gets 8KB of unmirrored internal RAM at $0000-$1FFF (both modes).
//
//Peripherals (extension registers $4018-$40FF):
// - $4020-$4026: PS/2 keyboard (command/response protocol) and HT6513B serial mouse.
//   The keyboard's command responder and the mouse's plug-and-play announcement are
//   implemented (the BIOS boot needs both); host key scanning and mouse movement
//   reporting are not wired up yet.
// - $4027: 8-bit DAC; the CPU paces its sample writes off the programmable timer.
// - $4031: UM6576 mode unlock sequence. $4032/$4033: IRQ mask/status for the four
//   mapper IRQ sources (timer $80, mouse $40, keyboard $20, FDC $08) sharing one line.
// - $4034-$4036: programmable timer (prescaler + auto-reload counter, CPU-clock or
//   per-scanline source).
// - $4040-$4047: program bank registers. $4048-$404F: DMA engine (BIOS/EPRAM source,
//   CPU-bus or VRAM destination).
//Super-IO range $4100-$43FF: uPD765 FDC at $4200-$4207 (same controller as the BBK
//drive unit, different address decode), LPC-10 speech synthesizer data/status port at
//$4302 (SB-2000 variant of BbkLpcAudio; reset via $4026 EIO6), EVRAM bank at $4300,
//RAM-boot control at $4301.
//
//CPU memory map (famiclone mode):
// - $5000-$57FF: 2KB work RAM (mirrored in $5800-$5FFF); $5006 doubles as the EBank register
// - $6000-$7FFF: 8KB external work RAM
// - $8000-$FFFF: eight 4KB program banks, or the MMC3 mapping after RAM boot
class Sb2kMapper : public BaseMapper
{
private:
	//Handles the FDS disk shortcut keys (eject / insert next / insert number) for disk swapping
	class DiskSwapListener final : public INotificationListener
	{
	private:
		Sb2kMapper* _mapper;

	public:
		DiskSwapListener(Sb2kMapper* mapper) : _mapper(mapper) {}

		void ProcessNotification(ConsoleNotificationType type, void* parameter) override
		{
			if(type == ConsoleNotificationType::ExecuteShortcut) {
				ExecuteShortcutParams* params = (ExecuteShortcutParams*)parameter;
				switch(params->Shortcut) {
					case EmulatorShortcut::FdsEjectDisk: _mapper->EjectDisk(); break;
					case EmulatorShortcut::FdsInsertNextDisk: _mapper->InsertNextDisk(); break;
					case EmulatorShortcut::FdsInsertDiskNumber: _mapper->InsertDisk(params->Param); break;
					default: break;
				}
			}
		}
	};

	shared_ptr<DiskSwapListener> _swapListener;
	bool _diskChecked = false;

	//A power cycle recreates the mapper, so the inserted disk is remembered here (like a floppy
	//physically staying in the drive) and re-mounted instead of the paired boot disk. Scoped to
	//the ROM path so a different game doesn't inherit a stale selection.
	inline static string _persistedDiskRom;
	inline static string _persistedDiskPath;

	//Work RAM layout: 512KB EPRAM + 2KB WRAM ($5000) + 8KB ERAM ($6000)
	static constexpr uint32_t EpramSize = 0x80000;
	static constexpr uint32_t WramOffset = 0x80000;
	static constexpr uint32_t EramOffset = 0x80800;

	//1ms in CPU cycles (the reference emulator uses the PAL CPU clock for these delays)
	static constexpr int32_t CyclesPerMs = 1663;

	BbkFdc _fdc;

	//LPC-10 speech synthesizer (SB-2000 variant - see BbkLpcAudio) and the $4027 DAC's
	//current output level
	unique_ptr<BbkLpcAudio> _lpcAudio;
	int16_t _dacLastOutput = 0;

	//Program banking
	uint8_t _pbank[8] = {};
	uint8_t _eBank = 0;
	bool _ramBoot = false;
	uint8_t _vbank = 0;

	//MMC3 clone
	uint8_t _c3Sel = 0;
	uint8_t _c3Reg[8] = {};
	uint8_t _c3IrqEnable = 0;
	uint8_t _c3IrqRequest = 0;
	uint8_t _c3IrqLatch = 0;
	uint8_t _c3IrqCounter = 0;
	uint8_t _c3IrqPreset = 0;
	uint8_t _c3IrqPresetVbl = 0;

	//UM6576 mode unlock state: 0 = locked, 1 = $65 seen, 2 = unlocked (native mode)
	uint8_t _mode6576 = 0;

	//Extension registers
	uint8_t _regKbdIn = 0xFF;
	uint8_t _regKbdOut = 0;
	uint8_t _reg4020 = 0; //timing setting control
	uint8_t _reg4022 = 0; //keyboard data control
	uint8_t _reg4023 = 0; //0: joystick, 1: mouse
	uint8_t _reg4024 = 0; //mouse data
	uint8_t _reg4026 = 0; //external I/O lines
	uint8_t _reg4032 = 0xFF; //IRQ mask (1 = masked)
	uint8_t _reg4033 = 0; //IRQ status
	uint8_t _reg4034 = 0; //timer control
	uint8_t _reg4035 = 0; //timer reload value
	uint8_t _reg4036 = 0; //timer current value
	uint8_t _dmaReg[8] = {};

	uint8_t _timerPrescaleCnt = 0;
	int32_t _kbdSendIrqDelay = 0;

	//PS/2 keyboard command responder. The BIOS init sequence sends reset/config commands
	//and waits for the ACK/self-test/ID replies, so these must be answered with realistic
	//delays even before key input is wired up.
	uint8_t _kbdTxData = 0xFF;
	bool _kbdDataReady = false;
	int32_t _kbdRspDelay = 0;
	uint8_t _kbdRetData[8] = {};
	uint8_t _kbdRetIndex = 0;
	uint8_t _kbdRetLength = 0;
	int32_t _kbdRetDelay = 0;
	int32_t _kbdRetDelayReload = 0;
	uint8_t _kbdCmd = 0;
	bool _kbdRxOption = false;
	uint8_t _kbdLedStatus = 0;
	uint8_t _kbdScanCodeSet = 2;
	bool _kbdInitialized = false;
	int32_t _kbdReportTimer = 0;

	//HT6513B serial mouse (MS-PnP mode, 1200bps). On an RTS pulse (EIO1) the mouse sends
	//its 30-byte plug-and-play announcement; the BIOS waits for it during boot to detect
	//the mouse, so it must be answered. Movement/button reporting is not wired up yet.
	static constexpr uint8_t MousePnpId[30] = {
		0x4D, 0x33,                                     //Old mouse ID "M3"
		0x08,                                           //Begin PnP
		0x01, 0x24,                                     //PnP rev 1.0
		0x28, 0x34, 0x2B,                               //EISA ID "HTK"
		0x10, 0x10, 0x10, 0x11,                         //Product ID "0001"
		0x3C,                                           //Extended
		0x3C, 0x2D, 0x2F, 0x35, 0x33, 0x25,             //Class name "\MOUSE"
		0x3C, 0x30, 0x2E, 0x30, 0x10, 0x26, 0x10, 0x23, //Driver ID "\PNP0F0C"
		0x19, 0x12,                                     //Checksum
		0x09                                            //End PnP
	};
	uint8_t _mouseRetData[32] = {};
	uint8_t _mouseRetIndex = 0;
	uint8_t _mouseRetLength = 0;
	int32_t _mouseRetDelay = 0;
	int32_t _mouseRetDelayReload = 0;
	bool _mouseInitialized = false;
	int32_t _mousePollTimer = 0;

	//UM6576-mode video RAM (10KB used by the chip; sized like the reference emulator's
	//shared buffer because the DMA engine can address the full 32KB window). Held in
	//BaseMapper's mapper-RAM buffer so it shows up in the debugger, is serialized and
	//is power-on initialized like every other mapper-owned RAM.
	static constexpr uint32_t CramSize = 0x8000;

	int32_t _lastPpuScanline = -2;

	void UpdatePrgMapping()
	{
		if(_ramBoot) {
			//$8000-$FFFF follows the MMC3 mapping into the current 128KB EPRAM slice.
			//The reference emulator marks these banks read-only (writes in famiclone
			//mode hit the MMC3 registers instead).
			uint32_t base = _eBank * 0x20000;
			if(_c3Sel & 0x40) {
				SetCpuMemoryMapping(0x8000, 0x9FFF, PrgMemoryType::WorkRam, base + 0x20000 - 0x4000, MemoryAccessType::Read);
				SetCpuMemoryMapping(0xA000, 0xBFFF, PrgMemoryType::WorkRam, base + (_c3Reg[7] & 0x0F) * 0x2000, MemoryAccessType::Read);
				SetCpuMemoryMapping(0xC000, 0xDFFF, PrgMemoryType::WorkRam, base + (_c3Reg[6] & 0x0F) * 0x2000, MemoryAccessType::Read);
			} else {
				SetCpuMemoryMapping(0x8000, 0x9FFF, PrgMemoryType::WorkRam, base + (_c3Reg[6] & 0x0F) * 0x2000, MemoryAccessType::Read);
				SetCpuMemoryMapping(0xA000, 0xBFFF, PrgMemoryType::WorkRam, base + (_c3Reg[7] & 0x0F) * 0x2000, MemoryAccessType::Read);
				SetCpuMemoryMapping(0xC000, 0xDFFF, PrgMemoryType::WorkRam, base + 0x20000 - 0x4000, MemoryAccessType::Read);
			}
			SetCpuMemoryMapping(0xE000, 0xFFFF, PrgMemoryType::WorkRam, base + 0x20000 - 0x2000, MemoryAccessType::Read);
		} else {
			//Eight 4KB banks via $4040-$4047: bank < $80 = BIOS page, >= $80 = EPRAM page
			for(int i = 0; i < 8; i++) {
				uint16_t start = 0x8000 + i * 0x1000;
				if(_pbank[i] < 0x80) {
					SetCpuMemoryMapping(start, start + 0x0FFF, PrgMemoryType::PrgRom, (_pbank[i] & 0x7F) * 0x1000, MemoryAccessType::Read);
				} else {
					SetCpuMemoryMapping(start, start + 0x0FFF, PrgMemoryType::WorkRam, (_pbank[i] & 0x7F) * 0x1000, MemoryAccessType::Read);
				}
			}
		}
	}

	void UpdateChrMapping()
	{
		//MMC3 CHR banking in 1KB pages of EVRAM, within the current 128KB EBank slice
		uint16_t base = _eBank * 0x80;
		if(_c3Sel & 0x80) {
			SelectChrPage(0, base + (_c3Reg[2] & 0x7F));
			SelectChrPage(1, base + (_c3Reg[3] & 0x7F));
			SelectChrPage(2, base + (_c3Reg[4] & 0x7F));
			SelectChrPage(3, base + (_c3Reg[5] & 0x7F));
			SelectChrPage(4, base + (_c3Reg[0] & 0x7E));
			SelectChrPage(5, base + (_c3Reg[0] & 0x7E) + 1);
			SelectChrPage(6, base + (_c3Reg[1] & 0x7E));
			SelectChrPage(7, base + (_c3Reg[1] & 0x7E) + 1);
		} else {
			SelectChrPage(0, base + (_c3Reg[0] & 0x7E));
			SelectChrPage(1, base + (_c3Reg[0] & 0x7E) + 1);
			SelectChrPage(2, base + (_c3Reg[1] & 0x7E));
			SelectChrPage(3, base + (_c3Reg[1] & 0x7E) + 1);
			SelectChrPage(4, base + (_c3Reg[2] & 0x7F));
			SelectChrPage(5, base + (_c3Reg[3] & 0x7F));
			SelectChrPage(6, base + (_c3Reg[4] & 0x7F));
			SelectChrPage(7, base + (_c3Reg[5] & 0x7F));
		}
	}

	//Raises one of the four maskable mapper IRQ sources ($4032 bit = 1 masks it)
	void RaiseIrq(uint8_t bit)
	{
		if(!(_reg4032 & bit)) {
			_reg4033 |= bit;
			_console->GetCpu()->SetIrqSource(IRQSource::External);
		}
	}

	//Programmable timer: 4-bit prescaler ($4034 low nibble) feeding an 8-bit down-counter
	//($4036, reloaded from $4035 when auto-reload is on). $4034 bit 7 = run, bit 5 =
	//per-scanline source (else CPU clock), bit 6 = auto-reload.
	void TimerTick(int32_t nTick)
	{
		if(nTick >= _timerPrescaleCnt) {
			nTick -= _timerPrescaleCnt;

			int32_t prePeriod = (_reg4034 & 15) + 1;
			int32_t cycles = (nTick / prePeriod) + 1;
			_timerPrescaleCnt = (uint8_t)(prePeriod - (nTick % prePeriod));

			if(cycles > _reg4036) {
				RaiseIrq(0x80);
				_reg4036 = (_reg4034 & 0x40) ? _reg4035 : 0;
			} else {
				_reg4036 -= (uint8_t)cycles;
			}
		} else {
			_timerPrescaleCnt -= (uint8_t)nTick;
		}
	}

	//8-bit DAC at $4027: unsigned samples centered at $80. The CPU paces its writes off
	//the programmable timer, so holding the last sample between writes reproduces the
	//stream at the right rate with no explicit sample clock. Mixed on the VRC7 slot at
	//x1 like the speech synth (see BbkLpcAudio for the scaling rationale); +/-4096 is
	//comparable to full APU music volume.
	void SetDacLevel(int16_t level)
	{
		if(level != _dacLastOutput) {
			_console->GetApu()->AddExpansionAudioDelta(AudioChannel::VRC7, level - _dacLastOutput);
			_dacLastOutput = level;
		}
	}

	void WriteDac(uint8_t value)
	{
		SetDacLevel((int16_t)((int8_t)(value ^ 0x80)) * 32);
	}

	//PS/2 keyboard: queue a single response byte after the standard command-ACK delay
	void KbdQueueAck()
	{
		_kbdTxData = 0xFA;
		_kbdRspDelay = CyclesPerMs * 2;
	}

	//PS/2 keyboard: host-to-device command byte (clocked in through $4021/$4022)
	void KbdCommand(uint8_t data)
	{
		if(_kbdRxOption) {
			//Option byte for a two-byte command
			switch(_kbdCmd) {
				case 0xED: _kbdLedStatus = data; break;
				case 0xF0:
					if(data != 0) {
						_kbdScanCodeSet = data;
						_kbdInitialized = true; //start reporting keys
					}
					break;
				default: break; //F3/FB/FC/FD - accepted, no effect
			}
			_kbdRxOption = false;
			KbdQueueAck();
		} else {
			switch(data) {
				case 0xED: case 0xF0: case 0xF3: case 0xFB: case 0xFC: case 0xFD:
					//Two-byte commands: ACK and wait for the option byte
					_kbdCmd = data;
					_kbdRxOption = true;
					KbdQueueAck();
					break;

				case 0xF2: //Read ID
					KbdQueueAck();
					_kbdRetData[0] = 0x12;
					_kbdRetData[1] = 0x34;
					_kbdRetIndex = 0;
					_kbdRetLength = 2;
					_kbdRetDelayReload = CyclesPerMs;
					_kbdRetDelay = _kbdRetDelayReload;
					break;

				case 0xFF: //Reset: ACK, then self-test pass after ~100ms
					KbdQueueAck();
					_kbdRetData[0] = 0xAA;
					_kbdRetIndex = 0;
					_kbdRetLength = 1;
					_kbdRetDelayReload = CyclesPerMs * 100;
					_kbdRetDelay = _kbdRetDelayReload;
					break;

				default:
					KbdQueueAck();
					break;
			}
		}
	}

	//PS/2 keyboard: advance the response delays by one CPU cycle, and report host key
	//transitions while idle
	void KbdClock()
	{
		if(_kbdInitialized) {
			_kbdReportTimer++;
		}

		if(_kbdDataReady) {
			return; //single-entry FIFO: wait until the CPU consumes the pending byte
		}

		if(_kbdRspDelay) {
			if(--_kbdRspDelay == 0) {
				_kbdDataReady = true;
			}
			return; //the command response goes out before any queued return bytes
		}

		if(_kbdRetDelay) {
			if(--_kbdRetDelay == 0) {
				_kbdTxData = _kbdRetData[_kbdRetIndex];
				_kbdDataReady = true;

				_kbdRetIndex++;
				if(_kbdRetIndex < _kbdRetLength) {
					_kbdRetDelay = _kbdRetDelayReload;
				}
			}
			return;
		}

		//Idle: send one make/break sequence per ~20ms report interval
		if(_kbdInitialized && _kbdReportTimer >= CyclesPerMs * 20) {
			_kbdReportTimer = 0;
			shared_ptr<Sb2kKeyboard> kbd = _console->GetControlManager()->GetControlDevice<Sb2kKeyboard>();
			if(kbd) {
				int32_t keyEvent = kbd->GetNextKeyEvent();
				if(keyEvent >= 0) {
					bool released = (keyEvent & 0x100) != 0;
					uint8_t code = keyEvent & 0xFF;
					if(code < 0x80) {
						_kbdRetData[0] = code | (released ? 0x80 : 0);
						_kbdRetLength = 1;
					} else {
						//Extended key: $E0 prefix + code
						_kbdRetData[0] = 0xE0;
						_kbdRetData[1] = (code & 0x7F) | (released ? 0x80 : 0);
						_kbdRetLength = 2;
					}
					_kbdRetIndex = 0;
					_kbdRetDelayReload = CyclesPerMs;
					_kbdRetDelay = 1; //first byte goes out immediately
				}
			}
		}
	}

	//Serial mouse: advance the transmit delay by one CPU cycle. Bytes land in $4024
	//bit-inverted (the CPU sees the idle-high /RXD line) and raise the mouse IRQ, but
	//only while $4023 selects the mouse; otherwise the byte is dropped.
	void MouseClock()
	{
		if(_mouseRetDelay) {
			if(--_mouseRetDelay == 0) {
				uint8_t value = _mouseRetData[_mouseRetIndex];
				if(_reg4023 & 0x01) {
					_reg4024 = ~value;
					RaiseIrq(0x40);
				}

				_mouseRetIndex++;
				if(_mouseRetIndex < _mouseRetLength) {
					_mouseRetDelay = _mouseRetDelayReload;
				}
			}
			return;
		}

		//Idle: the mouse transmits a report only when something changed (no idle chatter)
		if(_mouseInitialized && ++_mousePollTimer >= CyclesPerMs * 10) {
			_mousePollTimer = 0;
			shared_ptr<Sb2kMouse> mouse = _console->GetControlManager()->GetControlDevice<Sb2kMouse>();
			if(mouse) {
				uint8_t packet[3];
				if(mouse->GetPacket(packet)) {
					memcpy(_mouseRetData, packet, 3);
					_mouseRetIndex = 0;
					_mouseRetLength = 3;
					_mouseRetDelay = 1;
					_mouseRetDelayReload = CyclesPerMs * 5; //~8ms per byte at 1200bps
				}
			}
		}
	}

	//DMA engine ($4048-$404F): copies from BIOS ROM or EPRAM to the CPU bus or to video
	//memory. The reference emulator performs the whole copy instantly; so does this.
	void RunDma()
	{
		uint32_t srcAddr = ((_dmaReg[1] & 0x0F) << 15) | ((_dmaReg[3] & 0x7F) << 8) | _dmaReg[2];
		uint8_t* src = ((_dmaReg[1] & 0x10) ? _workRam : _prgRom) + srcAddr;
		uint32_t srcLimit = (_dmaReg[1] & 0x10) ? EpramSize : _prgSize;
		uint16_t dstAddr = (_dmaReg[5] << 8) | _dmaReg[4];
		int32_t len = (_dmaReg[7] << 8) | _dmaReg[6];
		if(len & 1) {
			len++;
		}
		len = std::min<int32_t>(len, srcLimit - srcAddr);

		if(_dmaReg[0] & 0x20) {
			//CPU-bus destination (16-bit address, wraps)
			NesMemoryManager* mm = _console->GetMemoryManager();
			while(len-- > 0) {
				mm->Write(dstAddr++, *src++, MemoryOperationType::Write);
			}
		} else if(dstAddr & 0x8000) {
			//EVRAM destination, through the $4300 bank
			uint32_t offset = _vbank * 0x8000 + (dstAddr & 0x7FFF);
			len = std::min<int32_t>(len, _chrRamSize - offset);
			if(len > 0) {
				memcpy(_chrRam + offset, src, len);
			}
		} else {
			//CRAM destination
			len = std::min<int32_t>(len, (int32_t)CramSize - dstAddr);
			if(len > 0) {
				memcpy(_mapperRam + dstAddr, src, len);
			}
		}
	}

	//Extension register read ($4018-$40FF)
	uint8_t ExRead(uint16_t addr)
	{
		switch(addr) {
			case 0x4020: return _regKbdIn;
			case 0x4022: return _reg4022;
			case 0x4023: return _reg4023 & 0x01;
			case 0x4024: return _reg4024;

			case 0x4026:
				//Bits 3/4/5 read back the printer's ack/paper-out/select lines;
				//no printer is emulated, so they read low
				return _reg4026 & ~0x38;

			case 0x4032: return _reg4032;
			case 0x4033: return _reg4033;
			case 0x4034: return _reg4034;
			case 0x4036: return _reg4036;

			default:
				if(addr >= 0x4040 && addr < 0x4048) {
					return _pbank[addr & 0x07];
				} else if(addr >= 0x4048 && addr < 0x4050) {
					return _dmaReg[addr & 0x07];
				}
				return 0;
		}
	}

	//Extension register write ($4018-$40FF)
	void ExWrite(uint16_t addr, uint8_t value)
	{
		switch(addr) {
			case 0x4020: _reg4020 = value; break;
			case 0x4021: _regKbdOut = value; break;

			case 0x4022:
				//Keyboard data control: bit 7 = clock, bit 0 = direction (1 = host-to-device),
				//bit 2 = raise the send-complete IRQ ~1ms after the transfer
				if((_reg4022 ^ value) & 0x80) {
					if(!(_reg4022 & 0x80) && (value & 0x01)) {
						//Rising clock edge with write direction: send the byte to the keyboard
						KbdCommand(_regKbdOut);
						if(value & 0x04) {
							_kbdSendIrqDelay = CyclesPerMs;
						}
					}
				}
				_reg4022 = value;
				break;

			case 0x4023: _reg4023 = value & 0x01; break;

			case 0x4025: break; //printer data port (printer not emulated)

			case 0x4026:
				//External I/O lines: EIO1 = mouse reset/RTS, EIO2 = printer strobe,
				//EIO6 = speech reset, EIO7 = keyboard reset
				if((_reg4026 ^ value) & 0x02) {
					if(value & 0x02) {
						//RTS rising edge: the mouse answers with its PnP announcement
						//(~15ms to the first byte, then one byte per ~8ms at 1200bps)
						memcpy(_mouseRetData, MousePnpId, sizeof(MousePnpId));
						_mouseRetIndex = 0;
						_mouseRetLength = sizeof(MousePnpId);
						_mouseRetDelay = CyclesPerMs * 15;
						_mouseRetDelayReload = CyclesPerMs * 5;
						_mouseInitialized = true;
					}
				}
				if((_reg4026 ^ value) & 0x40) {
					if(value & 0x40) {
						//EIO6 rising edge: speech synthesizer reset (flushes the FIFO and
						//re-arms the header scan for the next phrase)
						_lpcAudio->Reset();
					}
				}
				_reg4026 = value & 0x7F;
				break;

			case 0x4027: WriteDac(value); break;

			case 0x4031:
				//UM6576 mode unlock: $65 then $76
				if(_mode6576 == 0) {
					if(value == 0x65) {
						_mode6576 = 1;
					}
				} else if(_mode6576 == 1) {
					if(value == 0x76) {
						_mode6576 = 2;
					} else {
						_mode6576 = 0;
					}
				}
				break;

			case 0x4032:
				//IRQ mask; writing 1s also clears the matching status bits
				_reg4032 = value;
				_reg4033 &= ~value;
				if(_reg4033 == 0) {
					_console->GetCpu()->ClearIrqSource(IRQSource::External);
				}
				break;

			case 0x4034:
				if(!(value & 0x80)) {
					//Timer stopped: the DAC stream is over, return the output to center
					//so no DC offset lingers
					SetDacLevel(0);
				}
				_reg4034 = value;
				_timerPrescaleCnt = (value & 15) + 1;
				break;

			case 0x4035:
				_reg4035 = value;
				_reg4036 = value; //also sets the current value
				break;

			default:
				if(addr >= 0x4040 && addr < 0x4048) {
					_pbank[addr & 0x07] = value;
					if(!_ramBoot) {
						UpdatePrgMapping();
					}
				} else if(addr >= 0x4048 && addr < 0x4050) {
					_dmaReg[addr & 0x07] = value;
					if(addr == 0x4048 && (value & 0x80)) {
						RunDma();
					}
				}
				break;
		}
	}

	//User-configured folder to scan for floppy images, or "" to use the game's own folder
	//(shares the BBK machine's setting - both are floppy-based learning machines)
	string GetConfiguredDiskFolder()
	{
		const char* folder = _console->GetNesConfig().BbkDiskFolder;
		return folder[0] ? string(folder) : string();
	}

	//Mounts "<rom name>.img" found next to the ROM (or in the configured disk folder), if
	//present. Deferred until first FDC access so the emulator's rom info is fully set up.
	void CheckForDiskImage()
	{
		if(_diskChecked) {
			return;
		}
		_diskChecked = true;

		string romPath = _emu->GetRomInfo().RomFile.GetFilePath();
		string baseName = FolderUtilities::GetFilename(romPath, false);

		//A .img opened directly from the UI boots the BIOS and mounts that specific image,
		//taking precedence over the paired/persisted disk (taken, so it only applies to this boot).
		string pending = BbkFdc::TakePendingBootDisk();
		if(!pending.empty()) {
			ifstream test(pending, ios::in | ios::binary);
			if(test) {
				test.close();
				_fdc.LoadDiskImage(pending);
				_persistedDiskRom = romPath;
				_persistedDiskPath = pending; //keep it across a power cycle, like a manually inserted disk
				MessageManager::Log("[SB2K] Mounted disk image: " + pending);
				return;
			}
		}

		//A power cycle recreates the mapper: re-mount the disk the user last selected for this ROM
		//(the "floppy stays in the drive"), so power cycle reboots from it like a soft reset does.
		if(_persistedDiskRom == romPath && !_persistedDiskPath.empty()) {
			ifstream test(_persistedDiskPath, ios::in | ios::binary);
			if(test) {
				test.close();
				_fdc.LoadDiskImage(_persistedDiskPath);
				MessageManager::Log("[SB2K] Re-mounted disk image: " + _persistedDiskPath);
				return;
			}
		}

		//Look next to the ROM first (paired-disk convention), then in the configured folder
		vector<string> folders = { FolderUtilities::GetFolderName(romPath) };
		string configured = GetConfiguredDiskFolder();
		if(!configured.empty() && configured != folders[0]) {
			folders.push_back(configured);
		}

		for(string& folder : folders) {
			for(string ext : { ".img", ".IMG", ".ima", ".IMA" }) {
				string diskPath = FolderUtilities::CombinePath(folder, baseName + ext);
				ifstream test(diskPath, ios::in | ios::binary);
				if(test) {
					test.close();
					_fdc.LoadDiskImage(diskPath);
					MessageManager::Log("[SB2K] Mounted disk image: " + diskPath);
					return;
				}
			}
		}
	}

	//Super-IO read ($4100-$43FF)
	uint8_t SuperIoRead(uint16_t addr)
	{
		if(addr >= 0x4200 && addr < 0x4208) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			return _fdc.Read(addr & 0x07);
		}

		switch(addr) {
			case 0x4302:
				//Speech synthesizer status: bit 7 = ready (input FIFO can take data),
				//low nibble = $F once the end-of-stream marker was decoded
				return (uint8_t)((_lpcAudio->IsFull() ? 0x00 : 0x80) | (_lpcAudio->IsSpeechEnd() ? 0x0F : 0x00));
			default:
				return 0;
		}
	}

	//Super-IO write ($4100-$43FF)
	void SuperIoWrite(uint16_t addr, uint8_t value)
	{
		if(addr >= 0x4200 && addr < 0x4208) {
			CheckForDiskImage();
			_fdc.MarkActivity();
			_fdc.Write(addr & 0x07, value);
			if(_fdc.IsIrqAsserted()) {
				RaiseIrq(0x08);
			}
			return;
		}

		switch(addr) {
			case 0x4300:
				_vbank = value & 0x0F;
				break;

			case 0x4301:
				if(value == 0x55) {
					//RAM boot: leave UM6576 mode, reset the peripheral registers and run
					//the program loaded into EPRAM through the MMC3 mapping
					_mode6576 = 0;
					_ramBoot = true;

					_c3IrqEnable = 0;
					_c3IrqRequest = 0;
					_c3IrqLatch = 0;
					_c3IrqCounter = 0;
					_c3IrqPreset = 0;
					_c3IrqPresetVbl = 0;

					_regKbdIn = 0xFF;
					_regKbdOut = 0;
					_reg4022 = 0;
					_reg4023 = 0;
					_reg4024 = 0;
					_reg4026 = 0;
					_reg4032 = 0xFF;
					_reg4033 = 0;
					_reg4034 = 0;
					_reg4035 = 0;
					_reg4036 = 0;
					_timerPrescaleCnt = 0;
					_kbdSendIrqDelay = 0;

					SetDacLevel(0);
					_lpcAudio->Reset();

					UpdatePrgMapping();
					UpdateChrMapping();

					//Leaving native mode is a hardware reset: the reference emulator swaps the
					//UM6576 chip back out for the stock PPU and resets both it and the CPU. The
					//loader's last write is the reset itself - execution resumes at the reset
					//vector, which now reads out of the game in EPRAM through the MMC3 mapping.
					_console->GetPpu()->Reset(false);
					_console->GetCpu()->Reset(false, _console->GetRegion());
				}
				break;

			case 0x4302:
				//Speech synthesizer data byte
				_lpcAudio->WriteData(value);
				break;

			default:
				break;
		}
	}

	//MMC3-clone register write ($8000-$FFFF, famiclone mode)
	void Mmc3Write(uint16_t addr, uint8_t value)
	{
		switch((addr >> 13) & 0x03) {
			case 0: //$8000-$9FFF: bank select / bank data
				if(addr & 0x01) {
					_c3Reg[_c3Sel & 0x07] = value;
					UpdateChrMapping();
					if(_ramBoot) {
						UpdatePrgMapping();
					}
				} else {
					_c3Sel = value;
					UpdateChrMapping();
					if(_ramBoot) {
						UpdatePrgMapping();
					}
				}
				break;

			case 1: //$A000-$BFFF: mirroring / PRG-RAM protect
				if(!(addr & 0x01)) {
					SetMirroringType((value & 0x01) ? MirroringType::Horizontal : MirroringType::Vertical);
				}
				break;

			case 2: //$C000-$DFFF: IRQ latch / reload
				if(addr & 0x01) {
					int32_t scanline = _console->GetPpu()->GetCurrentScanline();
					if(scanline >= 0 && scanline < 240) {
						_c3IrqCounter |= 0x80;
						_c3IrqPreset = 0xFF;
					} else {
						_c3IrqCounter |= 0x80;
						_c3IrqPresetVbl = 0xFF;
						_c3IrqPreset = 0;
					}
				} else {
					_c3IrqLatch = value;
				}
				break;

			case 3: //$E000-$FFFF: IRQ disable / enable
				if(addr & 0x01) {
					_c3IrqRequest = 0;
					_c3IrqEnable = 1;
				} else {
					_c3IrqEnable = 0;
					_c3IrqRequest = 0;
					_console->GetCpu()->ClearIrqSource(IRQSource::External);
				}
				break;
		}
	}

	//Per-scanline work, run once per line during hblank (same placement as the BBK port)
	void HSync(int32_t scanline)
	{
		if((_reg4034 & 0xA0) == 0xA0) {
			//Timer driven by the scanline clock
			TimerTick(1);
		}

		if(_mode6576 == 0 && scanline >= 0 && scanline < 240) {
			//MMC3-clone scanline counter (only counts while rendering is enabled)
			NesPpuState state;
			_console->GetPpu()->GetState(state);
			if(state.Mask.BackgroundEnabled || state.Mask.SpritesEnabled) {
				if(_c3IrqPresetVbl) {
					_c3IrqCounter = _c3IrqLatch;
					_c3IrqPresetVbl = 0;
				}

				if(_c3IrqPreset) {
					_c3IrqCounter = _c3IrqLatch;
					_c3IrqPreset = 0;
				} else if(_c3IrqCounter > 0) {
					_c3IrqCounter--;
				}

				if(_c3IrqCounter == 0) {
					if(_c3IrqEnable) {
						_c3IrqRequest = 0xFF;
						_console->GetCpu()->SetIrqSource(IRQSource::External);
					}
					_c3IrqPreset = 0xFF;
				}
			}
		}
	}

protected:
	uint16_t GetPrgPageSize() override { return 0x1000; }
	uint16_t GetChrPageSize() override { return 0x400; }
	uint32_t GetChrRamSize() override { return 0x80000; }
	//CRAM - the low half of the UM6576 video bus. Exposed as mapper RAM so it is
	//visible in the debugger; the DMA engine fills it far more often than EVRAM.
	uint32_t GetMapperRamSize() override { return CramSize; }
	uint16_t GetChrRamPageSize() override { return 0x400; }
	uint32_t GetWorkRamSize() override { return 0x82800; }
	uint32_t GetWorkRamPageSize() override { return 0x1000; }
	bool ForceWorkRamSize() override { return true; }
	uint32_t GetSaveRamSize() override { return 0; }
	uint32_t GetInternalRamSize() override { return 0x2000; } //8KB, unmirrored (like the FamicomBox)

	uint16_t RegisterStartAddress() override { return 0x4018; }
	uint16_t RegisterEndAddress() override { return 0x43FF; }
	bool AllowRegisterRead() override { return true; }
	bool EnableCpuClockHook() override { return true; }

	//The clone PPU keeps all 32 palette entries independent - it does not mirror
	//$3F10/$3F14/$3F18/$3F1C onto $3F00/$3F04/$3F08/$3F0C. Software relies on this: a
	//RAM-booted program writes the sprite palette's first entry without meaning to touch
	//the backdrop, and with 2C02 mirroring that turns the whole screen its color.
	bool EnablePpuPaletteMirroring() override { return false; }

	void InitMapper(RomData& romData) override
	{
		romData.Info.System = GameSystem::Dendy;
	}

	void InitMapper() override
	{
		_romInfo.System = GameSystem::Dendy;

		if(!_swapListener) {
			_swapListener.reset(new DiskSwapListener(this));
			_emu->GetNotificationManager()->RegisterNotificationListener(_swapListener);
		}
		_diskChecked = false;

		//MMC3 register writes; EPRAM stores in UM6576 mode
		AddRegisterRange(0x8000, 0xFFFF, MemoryOperation::Write);
		//WRAM writes (the $5006 EBank register doubles as a WRAM cell)
		AddRegisterRange(0x5000, 0x5FFF, MemoryOperation::Write);

		//$5000-$57FF: 2KB WRAM reads (mirrored), $6000-$7FFF: 8KB ERAM
		SetCpuMemoryMapping(0x5000, 0x57FF, PrgMemoryType::WorkRam, WramOffset, MemoryAccessType::Read);
		SetCpuMemoryMapping(0x5800, 0x5FFF, PrgMemoryType::WorkRam, WramOffset, MemoryAccessType::Read);
		SetCpuMemoryMapping(0x6000, 0x7FFF, PrgMemoryType::WorkRam, EramOffset, MemoryAccessType::ReadWrite);

		for(int i = 0; i < 8; i++) {
			_pbank[i] = i;
		}
		_eBank = 0;
		_ramBoot = false;
		_vbank = 0;
		_mode6576 = 0;

		_c3Sel = 0;
		memset(_c3Reg, 0, sizeof(_c3Reg));
		_c3IrqEnable = 0;
		_c3IrqRequest = 0;
		_c3IrqLatch = 0;
		_c3IrqCounter = 0;
		_c3IrqPreset = 0;
		_c3IrqPresetVbl = 0;

		_regKbdIn = 0xFF;
		_regKbdOut = 0;
		_reg4022 = 0;
		_reg4023 = 0;
		_reg4024 = 0;
		_reg4026 = 0;
		_reg4032 = 0xFF;
		_reg4033 = 0;
		_reg4034 = 0;
		_reg4035 = 0;
		_reg4036 = 0;
		memset(_dmaReg, 0, sizeof(_dmaReg));
		_timerPrescaleCnt = 0;
		_kbdSendIrqDelay = 0;

		_kbdTxData = 0xFF;
		_kbdDataReady = false;
		_kbdRspDelay = 0;
		_kbdRetIndex = 0;
		_kbdRetLength = 0;
		_kbdRetDelay = 0;
		_kbdRetDelayReload = 0;
		_kbdCmd = 0;
		_kbdRxOption = false;
		_kbdLedStatus = 0;
		_kbdScanCodeSet = 2;
		_kbdInitialized = false;
		_kbdReportTimer = 0;

		memset(_mouseRetData, 0, sizeof(_mouseRetData));
		_mouseRetIndex = 0;
		_mouseRetLength = 0;
		_mouseRetDelay = 0;
		_mouseRetDelayReload = 0;
		_mouseInitialized = false;
		_mousePollTimer = 0;

		_lpcAudio.reset(new BbkLpcAudio(_console, true));
		_lpcAudio->Reset();
		_dacLastOutput = 0;

		//The reference emulator zero-fills EPRAM/EVRAM at power-on and the BIOS relies on
		//it (e.g. it only writes the used bit-planes of its font tiles); random power-on
		//RAM leaves garbage in the unwritten planes
		memset(_workRam, 0, _workRamSize);
		memset(_chrRam, 0, _chrRamSize);

		memset(_mapperRam, 0, _mapperRamSize);
		_lastPpuScanline = -2;

		UpdatePrgMapping();
		for(int i = 0; i < 8; i++) {
			SelectChrPage(i, i);
		}
		SetMirroringType(MirroringType::Vertical);
	}

	uint8_t ReadRegister(uint16_t addr) override
	{
		if(addr < 0x4100) {
			return ExRead(addr);
		}
		return SuperIoRead(addr);
	}

	void WriteRegister(uint16_t addr, uint8_t value) override
	{
		if(addr < 0x4100) {
			ExWrite(addr, value);
		} else if(addr < 0x4400) {
			SuperIoWrite(addr, value);
		} else if(addr < 0x6000) {
			//$5000-$5FFF: WRAM store; $5006 is also the EBank register
			if(addr == 0x5006) {
				_eBank = ((value >> 2) & 0x02) | ((value >> 1) & 0x01);
				_c3IrqEnable = 0;
				_console->GetCpu()->ClearIrqSource(IRQSource::External);
				UpdateChrMapping();
				if(_ramBoot) {
					UpdatePrgMapping();
				}
			}
			_workRam[WramOffset + (addr & 0x7FF)] = value;
		} else {
			//$8000-$FFFF
			if(_mode6576 == 0) {
				Mmc3Write(addr, value);
			} else {
				//UM6576 mode: writes land in EPRAM through the program bank registers
				uint8_t bank = _pbank[(addr >> 12) & 0x07];
				if(bank >= 0x80) {
					_workRam[(bank & 0x7F) * 0x1000 + (addr & 0x0FFF)] = value;
				}
			}
		}
	}

public:
	//Disk swapping - driven by the FDS disk shortcut keys via DiskSwapListener
	vector<string> GetDiskFileList()
	{
		string folder = GetConfiguredDiskFolder();
		if(folder.empty()) {
			folder = FolderUtilities::GetFolderName(_emu->GetRomInfo().RomFile.GetFilePath());
		}
		vector<string> files = FolderUtilities::GetFilesInFolder(folder, { ".img", ".ima" }, false);
		std::sort(files.begin(), files.end());
		return files;
	}

	uint32_t GetDiskCount()
	{
		return (uint32_t)GetDiskFileList().size();
	}

	//Full path of the disk image currently inserted in the drive, or "" if the drive is empty
	string GetCurrentDiskFilename()
	{
		return _fdc.IsDiskInserted() ? _fdc.GetDiskFilename() : "";
	}

	void EjectDisk()
	{
		auto lock = _emu->AcquireLock();
		if(_fdc.IsDiskInserted()) {
			MessageManager::DisplayMessage("SB2K", "Disk ejected: " + FolderUtilities::GetFilename(_fdc.GetDiskFilename(), true));
			_fdc.EjectDisk(); //Saves pending changes first
		}
		//Forget any remembered selection so a later power cycle boots from the paired disk
		_persistedDiskRom.clear();
		_persistedDiskPath.clear();
	}

	void InsertDisk(uint32_t index)
	{
		auto lock = _emu->AcquireLock();
		vector<string> disks = GetDiskFileList();
		if(index < disks.size()) {
			_fdc.EjectDisk();
			if(_fdc.LoadDiskImage(disks[index])) {
				_diskChecked = true;
				//Remember the selection so a power cycle reboots from this disk, not the paired one
				_persistedDiskRom = _emu->GetRomInfo().RomFile.GetFilePath();
				_persistedDiskPath = disks[index];
				MessageManager::DisplayMessage("SB2K", "Disk inserted: " + FolderUtilities::GetFilename(disks[index], true));
			}
		}
	}

	void InsertNextDisk()
	{
		vector<string> disks = GetDiskFileList();
		if(disks.empty()) {
			return;
		}

		int current = -1;
		for(size_t i = 0; i < disks.size(); i++) {
			if(disks[i] == _fdc.GetDiskFilename()) {
				current = (int)i;
				break;
			}
		}
		InsertDisk((current + 1) % disks.size());
	}

	//UM6576 native-mode video bus, used by Sb2kPpu for both $2007 access and rendering:
	//addresses below $8000 hit CRAM, $8000+ hits the 32KB EVRAM window selected by $4300
	bool IsUm6576Mode() { return _mode6576 == 2; }

	uint8_t VideoRead(uint16_t addr)
	{
		if(addr < 0x8000) {
			return _mapperRam[addr];
		}
		return _chrRam[_vbank * 0x8000 + (addr & 0x7FFF)];
	}

	void VideoWrite(uint16_t addr, uint8_t value)
	{
		if(addr < 0x8000) {
			_mapperRam[addr] = value;
		} else {
			_chrRam[_vbank * 0x8000 + (addr & 0x7FFF)] = value;
		}
	}

	//The debugger has to follow the same bus the chip uses. In UM6576 mode that bus is
	//16 bits wide - CRAM below $8000, the $4300-banked EVRAM window above it - so the
	//stock path (which masks to $3FFF and walks the CHR/nametable mapping) would show
	//unrelated memory. Famiclone mode keeps the 2C02 behaviour.
	uint8_t DebugReadVram(uint16_t addr, bool disableSideEffects = true) override
	{
		return IsUm6576Mode() ? VideoRead(addr) : BaseMapper::DebugReadVram(addr, disableSideEffects);
	}

	void DebugWriteVram(uint16_t addr, uint8_t value, bool disableSideEffects = true) override
	{
		if(IsUm6576Mode()) {
			VideoWrite(addr, value);
		} else {
			BaseMapper::DebugWriteVram(addr, value, disableSideEffects);
		}
	}

	uint32_t GetPpuAddressSpaceSize() override { return IsUm6576Mode() ? 0x10000 : 0x4000; }

	void ProcessCpuClock() override
	{
		BaseProcessCpuClock();

		//The clone APU's frame-counter IRQ line isn't wired up: the BIOS never writes
		//$4017 and its IRQ dispatcher only acks the $4033 sources, so the 2A03 power-on
		//frame IRQ (enabled by default) would re-enter the handler forever.
		_console->GetCpu()->ClearIrqSource(IRQSource::FrameCounter);

		_fdc.Clock();
		_lpcAudio->Clock();

		if((_reg4034 & 0xA0) == 0x80) {
			//Timer driven by the CPU clock
			TimerTick(1);
		}

		if(_kbdSendIrqDelay) {
			if(--_kbdSendIrqDelay == 0) {
				//Keyboard send-complete IRQ (~1ms after the host-to-device transfer)
				RaiseIrq(0x20);
			}
		}

		KbdClock();
		if(_kbdDataReady) {
			//Latch the keyboard byte into $4020 and raise the keyboard IRQ
			_regKbdIn = _kbdTxData;
			_kbdDataReady = false;
			RaiseIrq(0x20);
		}

		MouseClock();

		//Once-per-scanline hook during hblank (see the BBK port for the placement rationale)
		int32_t scanline = _console->GetPpu()->GetCurrentScanline();
		uint32_t cycle = _console->GetPpu()->GetCurrentCycle();
		if(scanline != _lastPpuScanline && cycle >= 321) {
			_lastPpuScanline = scanline;
			if(scanline >= 0) {
				HSync(scanline);
			}
		}
	}

	void Serialize(Serializer& s) override
	{
		BaseMapper::Serialize(s);

		SV(_fdc);
		SV(_lpcAudio);
		SV(_dacLastOutput);

		SVArray(_pbank, 8);
		SV(_eBank); SV(_ramBoot); SV(_vbank); SV(_mode6576);
		SV(_c3Sel);
		SVArray(_c3Reg, 8);
		SV(_c3IrqEnable); SV(_c3IrqRequest); SV(_c3IrqLatch); SV(_c3IrqCounter); SV(_c3IrqPreset); SV(_c3IrqPresetVbl);
		SV(_regKbdIn); SV(_regKbdOut); SV(_reg4020); SV(_reg4022); SV(_reg4023); SV(_reg4024); SV(_reg4026);
		SV(_reg4032); SV(_reg4033); SV(_reg4034); SV(_reg4035); SV(_reg4036);
		SVArray(_dmaReg, 8);
		SV(_timerPrescaleCnt); SV(_kbdSendIrqDelay);
		SV(_kbdTxData); SV(_kbdDataReady); SV(_kbdRspDelay);
		SVArray(_kbdRetData, 8);
		SV(_kbdRetIndex); SV(_kbdRetLength); SV(_kbdRetDelay); SV(_kbdRetDelayReload);
		SV(_kbdCmd); SV(_kbdRxOption); SV(_kbdLedStatus); SV(_kbdScanCodeSet); SV(_kbdInitialized);
		SV(_kbdReportTimer);
		SVArray(_mouseRetData, sizeof(_mouseRetData));
		SV(_mouseRetIndex); SV(_mouseRetLength); SV(_mouseRetDelay); SV(_mouseRetDelayReload);
		SV(_mouseInitialized); SV(_mousePollTimer);
		SV(_lastPpuScanline);

		if(!s.IsSaving()) {
			UpdatePrgMapping();
			UpdateChrMapping();
		}
	}
};
