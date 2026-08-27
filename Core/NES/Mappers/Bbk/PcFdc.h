#pragma once
#include "pch.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Serializer.h"

//PC-compatible uPD765 floppy controller, modelled at the register level: the standard
//SRA/DOR/MSR/data register file, a one-byte latch handshaked with the CPU through MSR's
//ready bit, and a phase handler that is stepped once per CPU cycle.
//Ported from the VirtuaNES-BBK fork (NES/FDC.cpp, the readIO/writeIO/run implementation,
//author NewRisingSun).
//
//This is NOT the same controller as BbkFdc. The fork carries two implementations and
//picks between them with an `FDC_select` flag: the BBK drive unit uses the simplified
//command/result model that BbkFdc was ported from, while the YuXing (mapper 169) and
//Subor SB-2000 (mapper 171) BIOSes drive this one. The difference that matters is the
//handshake - every command, result and data byte passes through the single latch, and the
//CPU polls MSR between bytes - which is why the YuXing BIOS hung against the other model
//no matter how its individual command results were corrected.
//
//Drive geometry is taken from the image's size, so the 360KB/720KB/1.2MB images in
//circulation work alongside the usual 1.44MB ones.
class PcFdc final : public ISerializable
{
private:
	//Main status register
	static constexpr uint8_t MsrReady = 0x80; //RQM - the latch is ready to be read/written
	static constexpr uint8_t MsrDirection = 0x40; //DIO - set when the latch flows fdc->cpu
	static constexpr uint8_t MsrNoDma = 0x20; //EXM - non-DMA execution phase in progress
	static constexpr uint8_t MsrBusy = 0x10; //A command is in progress

	//Status register A
	static constexpr uint8_t SraIrq = 0x80;

	//Digital output register
	static constexpr uint8_t DorSelectDrive = 0x03;
	static constexpr uint8_t DorNotReset = 0x04;
	static constexpr uint8_t DorMotorA = 0x10;

	//ST0
	static constexpr uint8_t St0NormalTermination = 0x00;
	static constexpr uint8_t St0AbnormalTermination = 0x40;
	static constexpr uint8_t St0InvalidCommand = 0x80;
	static constexpr uint8_t St0Polling = 0xC0;
	static constexpr uint8_t St0SeekComplete = 0x20;

	//ST1
	static constexpr uint8_t St1EndOfTrack = 0x80;
	static constexpr uint8_t St1NoData = 0x04;
	static constexpr uint8_t St1NotWriteable = 0x02;
	static constexpr uint8_t St1MissingAddressMark = 0x01;

	//ST2
	static constexpr uint8_t St2WrongCylinder = 0x10;

	//Configure
	static constexpr uint8_t CfgNotPoll = 0x10;

	//Command byte offsets
	static constexpr uint8_t CmdMultiTrack = 0x80;
	static constexpr uint8_t CmdNumber = 0;
	static constexpr uint8_t CmdUsHd = 1;
	static constexpr uint8_t CmdCylinder = 2;
	static constexpr uint8_t CmdHead = 3;
	static constexpr uint8_t CmdSectorNumber = 4;
	static constexpr uint8_t CmdSectorSize = 5;
	static constexpr uint8_t CmdLastSector = 6;
	static constexpr uint8_t CmdFormatSectorSize = 2;
	static constexpr uint8_t CmdFormatLastSector = 3;
	static constexpr uint8_t CmdFormatFill = 5;
	static constexpr uint8_t CmdNcn = 2;

	enum class Phase : uint8_t { Command = 0, Result = 1, ReadData = 2, WriteData = 3, FormatTrack = 4 };

	//Command lengths, indexed by (command byte & 0x1F). Zero marks an unsupported command,
	//which the controller answers as invalid.
	static constexpr uint8_t _cmdLength[32] = {
		1, 1, 9, 3, 2, 9, 9, 2, 1, 9, 2, 1, 9, 6, 1, 3,
		1, 9, 2, 4, 1, 1, 9, 1, 1, 9, 1, 1, 1, 9, 1, 1
	};

	struct Drive
	{
		bool Exists = true;
		bool Ready = true;
		bool Inserted = false;
		bool Running = false;
		bool WriteProtected = false;
		bool Changed = false;
		int32_t Cylinder = 0;
		int32_t Cylinders = 0;
		int32_t Sides = 0;
		int32_t Sectors = 0;
		int32_t CorrectDataRate = 0;
	};

	//Only one drive is ever fitted on these machines, but the register file addresses four
	Drive _drives[4] = {};
	uint8_t _driveSel = 0;

	uint8_t _dor = 0;
	uint8_t _msr = MsrReady;
	uint8_t _st0 = 0, _st1 = 0, _st2 = 0;
	uint8_t _sra = 0;
	uint8_t _config = 0;
	uint8_t _latch = 0;

	bool _clearGeometryOnReset = false;
	uint8_t _cylinder = 0, _head = 0, _sectorNumber = 0;
	int32_t _sectorSize = 0;
	int32_t _bytesLeft = 0;
	int32_t _dataPos = -1; //Byte offset into the image, or -1 when no sector is set up
	int32_t _dataRate = 500;

	bool _resetPin = false;
	bool _useDma = false;

	Phase _phase = Phase::Command;
	uint8_t _command[10] = {};
	uint8_t _commandLen = 0;
	uint8_t _results[8] = {};
	uint8_t _resultCount = 0;
	uint8_t _resultPos = 0;

	//Drive polling: the controller raises an interrupt when a drive's ready line changes
	int32_t _pollTimer = 0;
	uint8_t _pollDrive = 0;
	bool _previouslyReady[4] = {};

	int32_t _activityCycles = 0;
	bool _dirty = false;
	vector<uint8_t> _diskData;
	string _diskFilename;

	Drive& CurrentDrive() { return _drives[_driveSel]; }

	void PushResult(std::initializer_list<uint8_t> values)
	{
		_resultCount = 0;
		_resultPos = 0;
		for(uint8_t v : values) {
			if(_resultCount < sizeof(_results)) {
				_results[_resultCount++] = v;
			}
		}
	}

	//Ends the current command: queues its result bytes (if any) and moves to the result
	//phase, where the CPU collects them one at a time.
	void EndCommand(bool raiseIrq, std::initializer_list<uint8_t> values)
	{
		PushResult(values);
		_phase = Phase::Result;
		if(raiseIrq) {
			_sra |= SraIrq;
		}
		_msr &= ~MsrNoDma;
	}

	void EndCommandFull(bool raiseIrq)
	{
		EndCommand(raiseIrq, { _st0, _st1, _st2, _cylinder, _head, _sectorNumber, _command[CmdSectorSize] });
	}

	//Steps to the next sector of a read/write, ending the command when the track runs out
	void NextSector()
	{
		Drive& d = CurrentDrive();

		if(_dataPos < 0) {
			_cylinder = _command[CmdCylinder];
			_head = _command[CmdHead];
			_sectorNumber = _command[CmdSectorNumber];
			_sectorSize = 128 << _command[CmdSectorSize];
			if(_sectorSize > 8192) {
				_sectorSize = 8192;
			}
		} else if(_sectorNumber == _command[CmdLastSector]) {
			if((_command[CmdNumber] & CmdMultiTrack) && _head == 0) {
				_head ^= 1;
				_command[CmdHead] ^= 1;
				_sectorNumber = 1;
			} else {
				_st0 |= St0AbnormalTermination;
				_st1 |= St1EndOfTrack;
				EndCommandFull(true);
				return;
			}
		} else {
			_sectorNumber++;
		}

		if(!d.Inserted || _cylinder >= d.Cylinders || _head >= d.Sides || !d.Running || _dataRate != d.CorrectDataRate) {
			_st1 |= St1MissingAddressMark;
			_st0 |= St0AbnormalTermination;
		} else if(_cylinder != d.Cylinder) {
			_st1 |= St1NoData;
			_st2 |= St2WrongCylinder;
			_st0 |= St0AbnormalTermination;
		} else if(_sectorNumber < 1 || _sectorNumber > d.Sectors || _sectorSize != 512) {
			_st1 |= St1NoData;
			_st0 |= St0AbnormalTermination;
		}

		if(_st0 & St0AbnormalTermination) {
			EndCommandFull(true);
			return;
		}

		int32_t lba = (_cylinder * d.Sides + _head) * d.Sectors + _sectorNumber - 1;
		_bytesLeft = _sectorSize;
		_dataPos = lba * _sectorSize;
	}

	uint8_t ReadImageByte()
	{
		uint8_t value = 0xFF;
		if(_dataPos >= 0 && _dataPos < (int32_t)_diskData.size()) {
			value = _diskData[_dataPos];
		}
		_dataPos++;
		return value;
	}

	void WriteImageByte(uint8_t value)
	{
		if(_dataPos >= 0 && _dataPos < (int32_t)_diskData.size()) {
			_diskData[_dataPos] = value;
			_dirty = true;
		}
		_dataPos++;
	}

	//===== Command implementations =====

	void CmdReadId()
	{
		Drive& d = CurrentDrive();
		if(!d.Inserted || _cylinder >= d.Cylinders || _head >= d.Sides || !d.Running || _dataRate != d.CorrectDataRate) {
			_st1 |= St1MissingAddressMark;
			_st0 |= St0AbnormalTermination;
		} else {
			_cylinder = (uint8_t)d.Cylinder;
			_head = _command[CmdUsHd] >> 2;
			_sectorSize = 512;
			if(_sectorNumber == 0 || _sectorNumber >= d.Sectors) {
				_sectorNumber = 1;
			}
			_st0 |= St0NormalTermination;
		}
		EndCommandFull(true);
	}

	void CmdSeek()
	{
		Drive& d = CurrentDrive();
		if(d.Cylinder != _command[CmdNcn]) {
			d.Changed = false;
		}
		d.Cylinder = _command[CmdNcn];
		_st0 |= St0SeekComplete;
		if(!d.Exists) {
			_st0 |= St0AbnormalTermination;
		}
		EndCommand(true, {});
	}

	void CmdRecalibrate()
	{
		Drive& d = CurrentDrive();
		if(d.Cylinder != 0) {
			d.Changed = false;
		}
		d.Cylinder = 0;
		_st0 |= St0SeekComplete;
		if(!d.Exists) {
			_st0 |= St0AbnormalTermination;
		}
		EndCommand(true, {});
	}

	void CmdSenseInterruptStatus()
	{
		_sra &= ~SraIrq;
		EndCommand(false, { _st0, (uint8_t)CurrentDrive().Cylinder });
	}

	void CmdSpecify()
	{
		//Bit 0 of the third byte is ND - set means the CPU moves the data, not a DMA channel
		_useDma = !(_command[2] & 0x01);
		EndCommand(false, {});
	}

	void CmdSenseDriveStatus()
	{
		Drive& d = CurrentDrive();
		uint8_t value = (uint8_t)((_dor & DorSelectDrive) | (_head << 2) |
			(d.Cylinder == 0 ? 0x10 : 0x00) |
			(d.WriteProtected ? 0x40 : 0x00) |
			(d.Inserted ? 0x20 : 0x00) | 0x28);
		EndCommand(false, { value });
	}

	void CmdConfigure()
	{
		_config = _command[2];
		EndCommand(false, {});
	}

	void CmdInvalid()
	{
		_st0 |= St0InvalidCommand;
		EndCommand(false, { _st0 });
	}

	//===== Phase handlers, stepped once per CPU cycle =====

	void PollDrives()
	{
		if(_config & CfgNotPoll) {
			return;
		}
		if(!_pollTimer) {
			_pollTimer = 394; //~220us polling interval
			if(_previouslyReady[_pollDrive] != _drives[_pollDrive].Ready) {
				_previouslyReady[_pollDrive] = _drives[_pollDrive].Ready;
				_st0 = (uint8_t)((_st0 & ~St0SeekComplete) | St0Polling | _pollDrive);
				_sra |= SraIrq;
			}
			_pollDrive = (_pollDrive + 1) & 3;
		} else {
			_pollTimer--;
		}
	}

	//Assembles a command byte by byte, then dispatches it
	void PhaseCommand()
	{
		PollDrives();
		if(_msr & MsrReady) {
			return; //Waiting for the CPU to write the next byte
		}

		_msr |= MsrBusy;
		if(_commandLen < sizeof(_command)) {
			_command[_commandLen++] = _latch;
		}

		uint8_t index = _command[CmdNumber] & 0x1F;
		if(_commandLen < _cmdLength[index]) {
			_msr |= MsrReady;
			return;
		}

		//A fresh command starts with clear status, except Sense Interrupt Status which
		//reports on the command before it
		if(index != 0x08) {
			_st0 = _command[CmdUsHd] & 7;
			_st1 = _st2 = 0;
		}
		_dataPos = -1;
		_bytesLeft = 0;

		switch(index) {
			case 0x03: CmdSpecify(); break;
			case 0x04: CmdSenseDriveStatus(); break;
			case 0x05: _phase = Phase::WriteData; break;
			case 0x06: _phase = Phase::ReadData; break;
			case 0x07: CmdRecalibrate(); break;
			case 0x08: CmdSenseInterruptStatus(); break;
			case 0x0A: CmdReadId(); break;
			case 0x0D: _phase = Phase::FormatTrack; break;
			case 0x0F: CmdSeek(); break;
			case 0x13: CmdConfigure(); break;
			default: CmdInvalid(); break;
		}
	}

	//Hands the result bytes over one at a time, then returns to the command phase
	void PhaseResult()
	{
		if(_msr & MsrReady) {
			return; //Waiting for the CPU to take the byte in the latch
		}

		if(_resultPos >= _resultCount) {
			_msr &= ~MsrDirection;
			_msr &= ~MsrBusy;
			_commandLen = 0;
			memset(_command, 0, sizeof(_command));
			_phase = Phase::Command;
		} else {
			_msr |= MsrDirection;
			_latch = _results[_resultPos++];
		}
		_msr |= MsrReady;
	}

	void PhaseReadData()
	{
		if(_msr & MsrReady) {
			return; //The CPU has not collected the previous byte yet
		}

		if(_bytesLeft == 0) {
			NextSector();
			if(_phase != Phase::ReadData) {
				return; //NextSector ended the command
			}
		}

		if(_bytesLeft) {
			_latch = ReadImageByte();
			_bytesLeft--;
			_msr |= MsrReady | MsrNoDma | MsrDirection;
			_sra |= SraIrq;
		}
	}

	void PhaseWriteData()
	{
		Drive& d = CurrentDrive();
		if(d.WriteProtected) {
			_st0 |= St0AbnormalTermination;
			_st1 |= St1NotWriteable;
			EndCommandFull(true);
			return;
		}

		if(_msr & MsrReady) {
			return;
		}

		if(_bytesLeft == 0) {
			NextSector();
			if(_phase != Phase::WriteData) {
				return;
			}
		} else {
			WriteImageByte(_latch);
			_bytesLeft--;
		}

		if(_bytesLeft) {
			_msr |= MsrReady | MsrNoDma;
			_msr &= ~MsrDirection;
			_sra |= SraIrq;
		}
	}

	void PhaseFormatTrack()
	{
		Drive& d = CurrentDrive();
		if(d.WriteProtected) {
			_st0 |= St0AbnormalTermination;
			_st1 |= St1NotWriteable;
			EndCommand(true, { _st0, _st1, _st2, _cylinder, _head, _sectorNumber, _command[CmdFormatSectorSize] });
			return;
		}

		if(_msr & MsrReady) {
			return;
		}

		if(_bytesLeft == 0) {
			if(_dataPos < 0) {
				_cylinder = (uint8_t)d.Cylinder;
				_head = _command[CmdUsHd] >> 2;
				_sectorNumber = 1;
			} else if(++_sectorNumber > _command[CmdFormatLastSector]) {
				_st0 |= St0NormalTermination;
				EndCommand(true, { _st0, _st1, _st2, _cylinder, _head, _sectorNumber, _command[CmdFormatSectorSize] });
				return;
			}
			_sectorSize = 512;
			int32_t lba = (_cylinder * d.Sides + _head) * d.Sectors + _sectorNumber - 1;
			_bytesLeft = _sectorSize;
			_dataPos = lba * _sectorSize;
		} else {
			WriteImageByte(_command[CmdFormatFill]);
			_bytesLeft--;
		}

		//The sector id is requested while the first four bytes of the sector go by
		if(_bytesLeft > 512 - 4) {
			_msr |= MsrReady | MsrNoDma;
			_msr &= ~MsrDirection;
			_sra |= SraIrq;
		}
	}

public:
	//Called once per CPU cycle - the whole controller is driven from here
	void Clock()
	{
		if(_activityCycles > 0) {
			_activityCycles--;
		}
		if(!(_dor & DorNotReset) || _resetPin) {
			return;
		}

		switch(_phase) {
			case Phase::Command: PhaseCommand(); break;
			case Phase::Result: PhaseResult(); break;
			case Phase::ReadData: PhaseReadData(); break;
			case Phase::WriteData: PhaseWriteData(); break;
			case Phase::FormatTrack: PhaseFormatTrack(); break;
		}
	}

	//The BBK and Bung machines' BIOSes reset the controller expecting the seek geometry to
	//come back zeroed; the YuXing one does not care either way. Off by default so the
	//existing caller is untouched.
	void SetClearGeometryOnReset(bool clear) { _clearGeometryOnReset = clear; }

	void Reset()
	{
		_phase = Phase::Command;
		_msr = MsrReady;
		if(_clearGeometryOnReset) {
			_cylinder = _head = _sectorNumber = 0;
			_sectorSize = 0;
		}
		_sra = _st0 = _st1 = _st2 = 0;
		_dataPos = -1;
		_bytesLeft = 0;
		_commandLen = 0;
		memset(_command, 0, sizeof(_command));
		_resultCount = _resultPos = 0;
		_pollTimer = 0;
		_pollDrive = 0;
		for(int i = 0; i < 4; i++) {
			_previouslyReady[i] = false;
		}
	}

	PcFdc()
	{
		_dataRate = 500;
		Reset();
	}

	void MarkActivity() { _activityCycles = 90000; }
	bool IsActive() { return _activityCycles > 0; }
	bool IsIrqAsserted() { return (_sra & SraIrq) != 0; }
	bool IsDiskInserted() { return !_diskData.empty(); }

	//For mappers that behave differently depending on which operating system the floppy
	//carries - the Dr. PC Jr. BIOSes sniff the boot area for a signature.
	const vector<uint8_t>& GetDiskData() { return _diskData; }
	bool IsDirty() { return _dirty; }
	string GetDiskFilename() { return _diskFilename; }

	uint8_t Read(uint8_t port)
	{
		switch(port & 0x07) {
			case 0: return _sra;
			case 2: return _dor;
			case 3: return (_sra & SraIrq) ? 0x40 : 0x00;
			case 4: return _msr;

			case 5:
				//Taking the byte lowers the ready flag; the phase handler refills the latch
				if((_msr & MsrReady) && (_msr & MsrDirection)) {
					_msr &= ~MsrReady;
					_sra &= ~SraIrq;
					return _latch;
				}
				return 0xFF;

			case 7: {
				//DSKCHG, cleared by reading it
				bool changed = CurrentDrive().Changed;
				CurrentDrive().Changed = false;
				return changed ? 0x80 : 0x00;
			}
		}
		return 0xFF;
	}

	void Write(uint8_t port, uint8_t value)
	{
		switch(port & 0x07) {
			case 2:
				//Clearing the not-reset line resets the controller
				if((_dor & DorNotReset) && !(value & DorNotReset)) {
					Reset();
				}
				_dor = value;
				_driveSel = value & DorSelectDrive;
				for(int i = 0; i < 4; i++) {
					_drives[i].Running = (value & (DorMotorA << i)) && _drives[i].Exists;
				}
				break;

			case 3:
				_sra &= ~SraIrq;
				break;

			case 5:
				if(!(_dor & DorNotReset) || _resetPin) {
					//Held in reset the controller latches nothing, so the ready flag has to
					//stay up: the phase handlers that would raise it again are not running
					//either, and software that sends a command without releasing reset would
					//otherwise wait for a ready that can never come back.
					break;
				}
				if((_msr & MsrReady) && !(_msr & MsrDirection)) {
					_msr &= ~MsrReady;
					_sra &= ~SraIrq;
					_latch = value;
				}
				break;

			case 7: {
				static constexpr int32_t rates[4] = { 500, 300, 250, 1000 };
				_dataRate = rates[value & 0x03];
				break;
			}
		}
	}

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

		//Geometry comes from the image size, the way the reference emulator does it. Several
		//images in circulation are truncated copies of a 1.44MB disk, so anything that is not
		//a recognised size is treated as one.
		Drive& d = _drives[0];
		switch(size) {
			case 160 * 1024: d.Cylinders = 40; d.Sides = 1; d.Sectors = 8; d.CorrectDataRate = 250; break;
			case 180 * 1024: d.Cylinders = 40; d.Sides = 1; d.Sectors = 9; d.CorrectDataRate = 250; break;
			case 320 * 1024: d.Cylinders = 40; d.Sides = 2; d.Sectors = 8; d.CorrectDataRate = 250; break;
			case 360 * 1024: d.Cylinders = 40; d.Sides = 2; d.Sectors = 9; d.CorrectDataRate = 250; break;
			case 720 * 1024: d.Cylinders = 80; d.Sides = 2; d.Sectors = 9; d.CorrectDataRate = 250; break;
			case 1200 * 1024: d.Cylinders = 80; d.Sides = 2; d.Sectors = 15; d.CorrectDataRate = 500; break;
			case 2880 * 1024: d.Cylinders = 80; d.Sides = 2; d.Sectors = 36; d.CorrectDataRate = 1000; break;
			default: d.Cylinders = 80; d.Sides = 2; d.Sectors = 18; d.CorrectDataRate = 500; break;
		}
		d.Inserted = true;
		d.WriteProtected = false;
		d.Changed = true;
		return true;
	}

	void EjectDisk()
	{
		SaveDiskImage();
		_diskData.clear();
		_diskFilename.clear();
		_dirty = false;
		Drive& d = _drives[0];
		if(d.Inserted) {
			d.Changed = true;
		}
		d.Inserted = false;
	}

	void SaveDiskImage()
	{
		if(!_dirty || _diskData.empty() || _diskFilename.empty()) {
			return;
		}
		ofstream file(_diskFilename, ios::out | ios::binary);
		if(file) {
			file.write((char*)_diskData.data(), _diskData.size());
			_dirty = false;
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_driveSel); SV(_dor); SV(_msr); SV(_st0); SV(_st1); SV(_st2); SV(_sra);
		SV(_config); SV(_latch); SV(_cylinder); SV(_head); SV(_sectorNumber);
		SV(_sectorSize); SV(_bytesLeft); SV(_dataPos); SV(_dataRate);
		SV(_resetPin); SV(_useDma); SV(_phase); SV(_commandLen); SV(_resultCount);
		SV(_resultPos); SV(_pollTimer); SV(_pollDrive); SV(_activityCycles); SV(_dirty);
		SVArray(_command, 10);
		SVArray(_results, 8);
		for(int i = 0; i < 4; i++) {
			SVI(_previouslyReady[i]);
			SVI(_drives[i].Inserted); SVI(_drives[i].Running); SVI(_drives[i].Changed);
			SVI(_drives[i].Cylinder); SVI(_drives[i].Cylinders);
			SVI(_drives[i].Sides); SVI(_drives[i].Sectors); SVI(_drives[i].CorrectDataRate);
		}
	}
};
