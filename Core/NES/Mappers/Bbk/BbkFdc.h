#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"

//uPD765-style floppy disk controller used by the BBK/YuXing "软驱一号" drive add-on.
//Ported from the VirtuaNES-BBK fork (NES/FDC.cpp, author fanoble).
//The controller is exposed to the CPU at $FF80-$FFB8 (port = (addr >> 3) & 7).
//Disk geometry is fixed 1.44MB: 80 cylinders x 2 heads x 18 sectors x 512 bytes.
class BbkFdc final : public ISerializable
{
private:
	//Main status register ($3F4 equivalent)
	static constexpr uint8_t MsRqm = 0x80;
	static constexpr uint8_t MsDataIn = 0x40;
	static constexpr uint8_t MsExecution = 0x20; //Execution mode (doubles as the non-DMA mode flag, like the reference emulator)

	//ST0 bits
	static constexpr uint8_t St0SeekEnd = 0x20;
	static constexpr uint8_t St0Ic0 = 0x40;
	static constexpr uint8_t St0Ic1 = 0x80;

	//ST1 bits
	static constexpr uint8_t St1EndOfCylinder = 0x80;

	enum class FdcPhase : uint8_t
	{
		Idle = 0,
		Command = 1,
		Execution = 2,
		Result = 3
	};

	struct CmdDesc
	{
		uint8_t WriteLen;
		uint8_t ReadLen;
	};

	//Transfer pacing (CPU cycles @ ~1.77MHz). A real 1.44MB drive moves data at 500kbps
	//(~16us/byte) with several ms of rotational latency per sector. Instant transfers
	//let games slurp entire files in one frame and then blast the copied tiles into
	//CHR-RAM in a single go, overrunning vblank (visible blocks of garbage); pacing the
	//DRQ line like real hardware spreads the load across frames the way games expect.
	static constexpr int32_t ByteDelayCycles = 28; //~16us
	static constexpr int32_t SectorDelayCycles = 12000; //~6.8ms rotational latency/gap

	//Command table indexed by (command byte & 0x1F): {command length, result length}
	static constexpr CmdDesc _cmdTable[32] = {
		{ 1, 1 }, { 1, 1 }, { 9, 7 }, { 3, 0 }, { 2, 1 }, { 9, 7 }, { 9, 7 }, { 2, 0 },
		{ 1, 2 }, { 9, 7 }, { 2, 7 }, { 1, 1 }, { 9, 7 }, { 6, 7 }, { 1, 1 }, { 3, 0 },
		{ 1, 1 }, { 9, 7 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 },
		{ 1, 1 }, { 9, 7 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 9, 7 }, { 1, 1 }, { 1, 1 }
	};

	bool _irq = false;
	bool _hwReset = false;
	bool _softReset = false;

	bool _dmaInt = false;
	uint8_t _drvSel = 0;
	uint8_t _motor = 0;

	uint8_t _mainStatus = MsRqm;
	uint8_t _status[4] = {};

	uint8_t _cycle = 0;
	uint8_t _commands[10] = {};
	uint8_t _results[8] = {};
	uint8_t _cmdIndex = 0;
	uint8_t _lastCommand = 0; //Previous completed command (Sense Interrupt Status reports on it)

	FdcPhase _phase = FdcPhase::Idle;

	uint8_t _cylinder = 0;
	int32_t _dataPos = 0;
	int32_t _dataBytes = 0; //Bytes left in the current sector transfer
	int32_t _currentLba = 0; //Sector set up by the last read/write (Format Track fills it)
	int32_t _delayCycles = 0;
	int32_t _activityCycles = 0; //Non-zero while the drive was recently accessed (LED)

	bool _dirty = false;
	bool _abortCommand = false; //Set by a command that gives no response at all (Read ID on an empty drive)
	bool _diskChanged = false; //DSKCHG line: set when a disk is swapped, cleared by the next head step
	vector<uint8_t> _diskData;
	string _diskFilename;

	void ArmTransferDelay()
	{
		//Sector boundaries incur rotational latency; bytes within a sector stream at 500kbps
		_delayCycles = (_dataPos % 512) == 0 ? SectorDelayCycles : ByteDelayCycles;
	}

	uint8_t ReadDiskByte()
	{
		uint8_t value = 0xFF;
		if(_dataPos >= 0 && _dataPos < (int32_t)_diskData.size()) {
			value = _diskData[_dataPos];
		}
		_dataPos++;
		ArmTransferDelay();
		return value;
	}

	void WriteDiskByte(uint8_t value)
	{
		if(_dataPos >= 0 && _dataPos < (int32_t)_diskData.size()) {
			_diskData[_dataPos] = value;
			_dirty = true;
		}
		_dataPos++;
		ArmTransferDelay();
	}

	//Sets the transfer position for CHS-addressed commands and fills the standard 7-byte result
	void SetupChsTransfer(uint8_t c, uint8_t h, uint8_t r, uint8_t n)
	{
		int32_t lba = c * 36 + h * 18 + (r - 1);
		if(lba > 2879) {
			lba = 2879;
		}
		_currentLba = lba;
		_dataPos = lba * 512;
		_dataBytes = 512;
		_delayCycles = SectorDelayCycles;

		r++;
		if(r == 19) {
			r = 1;
			h++;
			if(h == 2) {
				h = 0;
				c++;
				if(c == 80) {
					c = 0;
				}
			}
		}

		_status[0] = 0;

		//In non-DMA mode the SB-DOS expects ST0 = IC0 and ST1 = end-of-cylinder; the BBK
		//path keeps the all-clear results it was verified with
		bool nonDma = (_mainStatus & MsExecution) != 0;
		_results[0] = nonDma ? St0Ic0 : _status[0];
		_results[1] = nonDma ? St1EndOfCylinder : _status[1];
		_results[2] = 0;
		_results[3] = c;
		_results[4] = h;
		_results[5] = r;
		_results[6] = n;
	}

	void ExecCommand()
	{
		switch(_commands[0] & 0x1F) {
			case 0x02: //Read track
				break;

			case 0x03: //Specify
				//Bit 0 of the second parameter byte is ND (non-DMA mode). The SB-2000 DOS
				//runs the controller in non-DMA mode: sector data moves through the data
				//port during an execution phase instead of the DMA ports.
				if(_commands[2] & 0x01) {
					_mainStatus |= MsExecution;
				} else {
					_mainStatus &= ~MsExecution;
				}
				break;

			case 0x04: //Sense drive status
				break;

			case 0x05: //Write data
			case 0x06: //Read data
				SetupChsTransfer(_commands[2], _commands[3], _commands[4], _commands[5]);
				_irq = true;
				break;

			case 0x07: //Recalibrate
				_status[0] = (_commands[1] & 0x03) ? (St0SeekEnd | St0Ic0) : St0SeekEnd;
				//A head step with a disk present clears the disk-change line (uPD765/PC behavior)
				if(IsDiskInserted()) {
					_diskChanged = false;
				}
				_irq = true; //seek-end interrupt (the SB2K routes it into its IRQ status register)
				break;

			case 0x08: //Sense interrupt status
				switch(_lastCommand) {
					case 0x07: //After a recalibrate: report its seek-end status, PCN = 0
						_results[0] = _status[0];
						_results[1] = 0;
						break;

					case 0x0F: //After a seek: seek end, PCN = new cylinder
						_results[0] = St0SeekEnd;
						_results[1] = _cylinder;
						break;

					default:
						if(_drvSel == 0) {
							//Drive A only
							_status[0] = St0Ic0 | St0Ic1;
						} else {
							_status[0] = St0Ic0 | St0SeekEnd;
						}
						_results[0] = _status[0];
						_results[1] = _cylinder;
						break;
				}
				_irq = false; //acknowledges the pending interrupt
				break;

			case 0x09: //Write deleted data
			case 0x0C: //Read deleted data
				break;

			case 0x0A: //Read ID
				if(!IsDiskInserted()) {
					//No index pulses without a disk - the command never completes and there is
					//no interrupt; the BIOS detects an empty drive by timing out on this
					_abortCommand = true;
					break;
				}
				_status[0] = 0;
				_results[0] = 0;
				_results[1] = 0;
				_results[2] = 0;
				_results[3] = 0;
				_results[4] = 0;
				_results[5] = 0;
				_results[6] = 0;
				_irq = true;
				break;

			case 0x0D: { //Format track
				//Parameters: HD/US, N (bytes/sector), SC (sectors/track), GPL, D (filler).
				//Fills the sector set up by the previous read/write with the filler byte.
				int32_t start = _currentLba * 512;
				for(int32_t i = 0; i < 512; i++) {
					if(start + i >= 0 && start + i < (int32_t)_diskData.size()) {
						_diskData[start + i] = _commands[5];
						_dirty = true;
					}
				}

				_status[0] = 0;
				_results[0] = _commands[1] & 0x07;
				_results[1] = 0;
				_results[2] = 0;
				_results[3] = 0;
				_results[4] = 0;
				_results[5] = 0;
				_results[6] = _commands[2];
				_irq = true;
				break;
			}

			case 0x0F: //Seek
				_cylinder = _commands[2];
				_status[0] = St0SeekEnd;
				if(IsDiskInserted()) {
					_diskChanged = false;
				}
				_irq = true; //seek-end interrupt
				break;

			case 0x11: //Scan equal
			case 0x19: //Scan low or equal
			case 0x1D: //Scan high or equal
				break;

			default: //Invalid command
				_status[0] = St0Ic1;
				_results[0] = _status[0];
				break;
		}
	}

	void ResetController()
	{
		_drvSel = 0;
		_motor = 0;
		_mainStatus = MsRqm;

		_status[0] = _status[1] = _status[2] = _status[3] = 0;

		_cycle = 0;
		_phase = FdcPhase::Idle;
	}

	//Queued from the UI when a floppy image is opened directly (see SetPendingBootDisk)
	inline static string _pendingBootDiskPath;

public:
	//A floppy image can't boot on its own, so opening one from the UI loads the BIOS ROM of
	//the machine it belongs to and mounts the image on that fresh boot. The path is queued
	//here because no mapper exists yet at that point. Both machines that use this controller
	//share the one slot and *take* the path rather than copy it, so an image queued for one
	//of them can't be left behind and picked up by the other's next boot.
	static void SetPendingBootDisk(string path) { _pendingBootDiskPath = path; }

	static string TakePendingBootDisk()
	{
		string path = _pendingBootDiskPath;
		_pendingBootDiskPath.clear();
		return path;
	}

	//Called once per CPU cycle to advance the transfer delay
	void Clock()
	{
		if(_delayCycles > 0) {
			_delayCycles--;
		}
		if(_activityCycles > 0) {
			_activityCycles--;
		}
	}

	//Keeps the activity LED lit for ~50ms after any controller access
	void MarkActivity()
	{
		_activityCycles = 90000;
	}

	bool IsActive() { return _activityCycles > 0; }

	//IRQ line state (the SB2K routes it into its IRQ status register; the BBK reads it via port 3)
	bool IsIrqAsserted() { return _irq; }

	bool IsDiskInserted() { return !_diskData.empty(); }
	bool IsDirty() { return _dirty; }
	string GetDiskFilename() { return _diskFilename; }

	bool LoadDiskImage(string filename)
	{
		ifstream file(filename, ios::in | ios::binary);
		if(!file) {
			return false;
		}

		file.seekg(0, ios::end);
		size_t size = (size_t)file.tellg();
		file.seekg(0, ios::beg);

		if(size == 0 || size > 4 * 1024 * 1024) {
			return false;
		}

		_diskData.resize(size);
		file.read((char*)_diskData.data(), size);

		_dirty = false;
		_diskFilename = filename;
		return true;
	}

	void EjectDisk()
	{
		if(_dirty) {
			SaveDiskImage();
		}
		_diskData.clear();
		_diskFilename.clear();
		_dataPos = 0;
		_dirty = false;
		_diskChanged = true; //Signal the removal so a running program re-reads after a swap
	}

	bool SaveDiskImage()
	{
		if(_diskData.empty() || _diskFilename.empty()) {
			return false;
		}

		ofstream file(_diskFilename, ios::out | ios::binary);
		if(!file) {
			return false;
		}

		file.write((char*)_diskData.data(), _diskData.size());
		_dirty = false;
		return true;
	}

	uint8_t Read(uint8_t port)
	{
		switch(port) {
			case 0: //FDCDMADackIO
			case 1: { //FDCDMATcIO
				uint8_t value = ReadDiskByte();
				if(_dataBytes > 0) {
					_dataBytes--;
					if(_dataBytes == 0) {
						_phase = FdcPhase::Result;
					}
				}
				return value;
			}

			case 2: //FDCDRQPortI - D6: FDC DRQ (data ready once the transfer delay elapsed)
				return _delayCycles == 0 ? 0x40 : 0x00;

			case 3: //FDCIRQPortI - D6: IRQ
				return _irq ? 0x40 : 0x00;

			case 4: //FDCStatPortI - main status register
				return _mainStatus;

			case 5: { //FDCDataPortIO - execution phase data (non-DMA reads), then result phase
				if(_phase == FdcPhase::Execution) {
					if(_lastCommand == 0x02 || _lastCommand == 0x06) {
						uint8_t value = ReadDiskByte();
						if(_dataBytes > 0) {
							_dataBytes--;
							if(_dataBytes == 0) {
								_phase = FdcPhase::Result;
							}
						}
						return value;
					}
					return 0;
				}

				uint8_t value = _results[_cycle];
				_cycle++;
				if(_cycle >= _cmdTable[_cmdIndex].ReadLen) {
					//Prepare for next command
					_cycle = 0;
					_phase = FdcPhase::Idle;

					_mainStatus &= ~MsDataIn;
					_mainStatus |= MsRqm;

					_irq = false; //reading the full result acknowledges the interrupt
				}
				return value;
			}

			case 7: //FDCChangePortI - D7: disk changed
				//Asserted after a swap (until the next head step) and, like a real drive, whenever
				//the drive is empty - so a machine booted with no disk detects the disk once inserted.
				return (_diskChanged || !IsDiskInserted()) ? 0x80 : 0x00;

			default:
				return 0;
		}
	}

	void Write(uint8_t port, uint8_t value)
	{
		switch(port) {
			case 0: //FDCDMADackIO
			case 1: //FDCDMATcIO
				WriteDiskByte(value);
				if(_dataBytes > 0) {
					_dataBytes--;
					if(_dataBytes == 0) {
						_cycle = 0;
						_phase = FdcPhase::Result;
						_mainStatus |= MsDataIn;
					}
				}
				break;

			case 2: //FDCCtrlPortO
				//D5/D4: drive motors, D3: enable INT+DMA, D2: /soft reset, D[1:0]: drive select
				_dmaInt = (value & 0x08) != 0;
				_drvSel = value & 0x03;
				_motor = value >> 4;

				if(value & 0x04) {
					if(_softReset) {
						ResetController();
						_softReset = false;

						//IRQ after soft reset (drive A only). A real uPD765 raises this regardless of
						//whether a disk is present; the BIOS then detects "no disk" when the boot read
						//returns nothing. Gating it on disk presence hung a diskless boot forever.
						_irq = (_drvSel == 0);
					}
				} else {
					if(!_softReset) {
						_softReset = true;
						_irq = false;
					}
				}
				break;

			case 3: //FDCDMADackIO
				break;

			case 4: //FDCResetPortO - D6: FDC pin reset
				if(value & 0x40) {
					if(!_hwReset) {
						_hwReset = true;
						_irq = false;
					}
				} else {
					if(_hwReset) {
						_dmaInt = false;
						ResetController();
						_hwReset = false;
					}
				}
				break;

			case 5: //FDCDataPortIO - command phase (and non-DMA write data during execution)
				switch(_phase) {
					case FdcPhase::Execution:
						if(_lastCommand == 0x05) {
							WriteDiskByte(value);
							if(_dataBytes > 0) {
								_dataBytes--;
								if(_dataBytes == 0) {
									_cycle = 0;
									_phase = FdcPhase::Result;
									_mainStatus |= MsDataIn;
									_irq = false;
								}
							}
						}
						break;

					case FdcPhase::Result:
						//Error - command byte during wrong phase, ignore
						break;

					case FdcPhase::Idle:
					default:
						_cycle = 0;
						_phase = FdcPhase::Command;
						_cmdIndex = value & 0x1F;
						[[fallthrough]];

					case FdcPhase::Command:
						_commands[_cycle] = value;
						_cycle++;
						if(_cycle >= _cmdTable[_cmdIndex].WriteLen) {
							_phase = FdcPhase::Execution;

							ExecCommand();
							_lastCommand = _cmdIndex;

							//Prepare for result phase
							if(_abortCommand) {
								//Command gave no response at all (no result, no interrupt)
								_abortCommand = false;
								_phase = FdcPhase::Idle;
							} else if((_mainStatus & MsExecution) && (_cmdIndex == 0x05 || _cmdIndex == 0x06)) {
								//Non-DMA read/write data: the 512 sector bytes stream through the
								//data port before the result phase. DATA_IN points fdc->host for
								//a read and host->fdc for a write.
								_phase = FdcPhase::Execution;
								if(_cmdIndex == 0x06) {
									_mainStatus |= MsDataIn;
								} else {
									_mainStatus &= ~MsDataIn;
								}
							} else if(_cmdTable[_cmdIndex].ReadLen) {
								_mainStatus |= MsDataIn;
								_phase = FdcPhase::Result;
							} else {
								_phase = FdcPhase::Idle;
							}

							_cycle = 0;
						}
						break;
				}
				break;

			case 7: //FDCSpeedPortO - D[1:0]: data rate
				break;

			default:
				break;
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_irq); SV(_hwReset); SV(_softReset); SV(_dmaInt); SV(_drvSel); SV(_motor);
		SV(_mainStatus); SV(_cycle); SV(_cmdIndex); SV(_lastCommand); SV(_phase); SV(_cylinder); SV(_dataPos); SV(_dataBytes); SV(_currentLba); SV(_delayCycles); SV(_activityCycles); SV(_dirty); SV(_diskChanged);
		SVArray(_status, 4);
		SVArray(_commands, 10);
		SVArray(_results, 8);
		SVVector(_diskData);
	}
};
