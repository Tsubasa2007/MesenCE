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

	//ST0 bits
	static constexpr uint8_t St0SeekEnd = 0x20;
	static constexpr uint8_t St0Ic0 = 0x40;
	static constexpr uint8_t St0Ic1 = 0x80;

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

	FdcPhase _phase = FdcPhase::Idle;

	uint8_t _cylinder = 0;
	int32_t _dataPos = 0;
	int32_t _delayCycles = 0;
	int32_t _activityCycles = 0; //Non-zero while the drive was recently accessed (LED)

	bool _dirty = false;
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
		_dataPos = (h * 18 + c * 36 + (r - 1)) * 512;
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

		_results[0] = _status[0];
		_results[1] = _status[1];
		_results[2] = _status[2];
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
				break;

			case 0x04: //Sense drive status
				break;

			case 0x05: //Write data
			case 0x06: //Read data
				SetupChsTransfer(_commands[2], _commands[3], _commands[4], _commands[5]);
				break;

			case 0x07: //Recalibrate
				_status[0] = (_commands[1] & 0x03) ? (St0SeekEnd | St0Ic0) : St0SeekEnd;
				//A head step with a disk present clears the disk-change line (uPD765/PC behavior)
				if(IsDiskInserted()) {
					_diskChanged = false;
				}
				break;

			case 0x08: //Sense interrupt status
				if(_drvSel == 0) {
					//Drive A only
					_status[0] = St0Ic0 | St0Ic1;
				} else {
					_status[0] = St0Ic0 | St0SeekEnd;
				}
				_results[0] = _status[0];
				_results[1] = _cylinder;
				break;

			case 0x09: //Write deleted data
			case 0x0C: //Read deleted data
				break;

			case 0x0A: //Read ID
				_status[0] = 0;
				_results[0] = _status[0];
				break;

			case 0x0D: //Format track
				SetupChsTransfer(_commands[1], _commands[2], _commands[3], _commands[4]);
				break;

			case 0x0F: //Seek
				_cylinder = _commands[2];
				_status[0] = St0SeekEnd;
				if(IsDiskInserted()) {
					_diskChanged = false;
				}
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

public:
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
			case 1: //FDCDMATcIO
				return ReadDiskByte();

			case 2: //FDCDRQPortI - D6: FDC DRQ (data ready once the transfer delay elapsed)
				return _delayCycles == 0 ? 0x40 : 0x00;

			case 3: //FDCIRQPortI - D6: IRQ
				return _irq ? 0x40 : 0x00;

			case 4: //FDCStatPortI - main status register
				return _mainStatus;

			case 5: { //FDCDataPortIO - result phase
				uint8_t value = _results[_cycle];
				_cycle++;
				if(_cycle >= _cmdTable[_cmdIndex].ReadLen) {
					//Prepare for next command
					_cycle = 0;
					_phase = FdcPhase::Idle;

					_mainStatus &= ~MsDataIn;
					_mainStatus |= MsRqm;
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

			case 5: //FDCDataPortIO - command phase
				switch(_phase) {
					case FdcPhase::Execution:
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

							//Prepare for result phase
							if(_cmdTable[_cmdIndex].ReadLen) {
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
		SV(_mainStatus); SV(_cycle); SV(_cmdIndex); SV(_phase); SV(_cylinder); SV(_dataPos); SV(_delayCycles); SV(_activityCycles); SV(_dirty); SV(_diskChanged);
		SVArray(_status, 4);
		SVArray(_commands, 10);
		SVArray(_results, 8);
		SVVector(_diskData);
	}
};
