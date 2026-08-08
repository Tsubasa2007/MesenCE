#pragma once
#include "pch.h"
#include "Shared/MessageManager.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/Serializer.h"

//Parallel-port dot-matrix printer attached to the BBK learning machine's PC-card port
//($FF40 data, $FF48 status, $FF50 control). The word processing and drawing titles drive
//it directly with Epson ESC/P escape sequences.
//Ported from the VirtuaNES-BBK fork (NES/LPT.cpp, author fanoble).
//
//The printer only ever receives bit images: the machine has no Latin-only text mode
//worth using for Chinese, so the guest rasterizes everything itself and sends it as
//graphics. Rendering is therefore just "plot the bits" - there is no font handling, and
//printable characters sent outside a graphics block are discarded.
//
//The page is built at 200dpi A4 and written out as a PNG when the guest ends the job
//with a form feed (or resets the printer, or stops sending data long enough for the
//watchdog to fire). The original opened a device-selection dialog and could also print
//to a real Windows printer; that path was unreachable in practice (its printer
//enumeration read from a null pointer, so the list only ever held "PNG image"), so this
//port keeps the PNG output and drops the dialog.
//
//Neither the original nor this port has any notion of colour: the page is one bit deep
//and ESC r (select print colour) is not implemented. No BBK software seen so far sends it.
class BbkPrinter final : public ISerializable
{
private:
	//The sheet is A4 at 200dpi (exactly 1653.5 x 2338.6 dots; the fork rounded up and
	//aligned the width to 4, which is kept here so pages stay comparable).
	static constexpr int PageDpi = 200;
	static constexpr int PageWidth = 1660;
	static constexpr int PageHeight = 2340;
	static constexpr int LeftMargin = 60;
	static constexpr int TopMargin = 96;

	//The head position is tracked in 1/2160 inch, the coarsest unit that divides every
	//density ESC/P can select (60/72/80/90/120/144/180/216/240/360dpi as well as the
	//n/180 and n/216 line feeds), so nothing has to be rounded until a dot reaches the
	//page. The original counted whole dots at the current vertical density and truncated
	//every line feed, which cost ~9% of each advance - enough to overlap successive
	//8-pin bands and squash a full-page image by more than a tenth.
	static constexpr int UnitsPerInch = 2160;

	//Flush a job the guest never terminated. The original had the same 6-second watchdog
	//but never clocked it, so an unterminated job was silently lost - which is every job
	//in practice: none of the software seen so far ends its job with a form feed.
	static constexpr uint64_t TimeoutCycles = 6 * 1789773;

	enum class DataState : uint8_t
	{
		Idle = 0,
		Esc = 1,
		Param = 2,
		Data = 3,
		Skip = 4,     //Swallowing the payload of a command that is parsed but not drawn
		SkipToNul = 5 //Swallowing a NUL-terminated list (ESC B / ESC D / ESC b)
	};

	struct DotDensity
	{
		uint8_t Mode;
		int16_t DpiX;
		int16_t DpiY;
		uint8_t Dots;
	};

	//ESC * dot densities.
	static constexpr DotDensity DotDensityTable[] = {
		{ 0, 60, 72, 8 }, { 1, 120, 72, 8 }, { 2, 120, 72, 8 }, { 3, 240, 72, 8 },
		{ 4, 80, 72, 8 }, { 5, 72, 72, 8 }, { 6, 90, 72, 8 }, { 7, 144, 72, 8 },
		{ 32, 60, 180, 24 }, { 33, 120, 180, 24 }, { 38, 90, 180, 24 },
		{ 39, 180, 180, 24 }, { 40, 360, 180, 24 }
	};

	//Parameter counts for the ESC/P commands this printer does not act on. Knowing how
	//long they are is what keeps an unsupported command from desynchronizing the stream:
	//without this the parameter bytes get read back as data, and a stray $0A or $1B in
	//them corrupts everything that follows.
	struct EscParams
	{
		uint8_t Cmd;
		uint8_t Count;
	};

	static constexpr EscParams IgnoredCommands[] = {
		//No parameters
		{ 0x0E, 0 }, { 0x0F, 0 }, { 0x12, 0 }, { 0x14, 0 }, //SO/SI/DC2/DC4 as ESC-prefixed
		{ 0x23, 0 }, { 0x30, 0 }, { 0x31, 0 }, { 0x32, 0 }, { 0x34, 0 }, { 0x35, 0 },
		{ 0x36, 0 }, { 0x37, 0 }, { 0x38, 0 }, { 0x39, 0 }, { 0x3D, 0 }, { 0x3E, 0 },
		{ 0x45, 0 }, { 0x46, 0 }, { 0x47, 0 }, { 0x48, 0 }, { 0x4D, 0 }, { 0x4F, 0 },
		{ 0x50, 0 }, { 0x54, 0 }, { 0x67, 0 },
		//One parameter
		{ 0x19, 1 }, { 0x21, 1 }, { 0x2D, 1 }, { 0x41, 1 }, { 0x49, 1 }, { 0x52, 1 },
		{ 0x51, 1 }, { 0x53, 1 }, { 0x55, 1 }, { 0x57, 1 }, { 0x61, 1 }, { 0x68, 1 },
		{ 0x6A, 1 }, { 0x6B, 1 }, { 0x6C, 1 }, { 0x70, 1 }, { 0x72, 1 }, { 0x73, 1 },
		{ 0x74, 1 }, { 0x77, 1 }, { 0x78, 1 },
		//Two parameters (nL nH)
		{ 0x24, 2 }, { 0x5C, 2 }
	};

	string _romName;

	//Protocol state
	DataState _state = DataState::Idle;
	bool _printing = false;
	uint8_t _escCmd = 0;
	int _paramCount = 0;
	int _dataLength = 0;
	uint8_t _params[4] = {};
	int _paramPos = 0;
	uint64_t _idleCycles = 0;
	int _unknownLogged = 0;

	//Control lines
	bool _reset = false;
	bool _selectPrinter = false;
	bool _lineFeed = false;
	bool _strobe = false;

	//Head position, in 1/2160 inch from the top-left of the printable area
	int _xUnits = 0;
	int _yUnits = 0;

	//Page state
	int _column = 0; //Byte index within a 24-pin column (0-2)
	int _dpiX = 0;
	int _dpiY = 0;
	//Line spacing is stored as a distance, not as the raw ESC 3 operand: the operand's
	//unit depends on which printer the guest thinks it is talking to, and that can change
	//between the command and the line feed that uses it.
	int _lineSpaceUnits = 0;
	int _bandPins = 24;
	bool _mode24p = false;
	bool _graphicsMode = false;
	bool _pageDirty = false;
	vector<uint8_t> _page;

	static int ToPixels(int units) { return (int)((int64_t)units * PageDpi / UnitsPerInch); }

	int PitchX() { return _dpiX > 0 ? UnitsPerInch / _dpiX : 0; }
	int PitchY() { return _dpiY > 0 ? UnitsPerInch / _dpiY : 0; }

	//ESC 3 / ESC J / LF measure in n/180 inch on a 24-pin printer and n/216 on a 9-pin one
	int LineFeedUnit() { return _mode24p ? UnitsPerInch / 180 : UnitsPerInch / 216; }

	void StartJob()
	{
		_page.assign((size_t)PageWidth * PageHeight, 0xFF);
		_xUnits = _yUnits = 0;
		_column = 0;
		_dpiX = _dpiY = 0;
		_mode24p = false;
		_graphicsMode = false;
		_pageDirty = false;
		_unknownLogged = 0;
		_bandPins = 24;
		_lineSpaceUnits = UnitsPerInch / 6;
	}

	//Writes the page out and feeds a fresh sheet, which is what a form feed does on a real
	//printer - the job itself carries on. The original ended the whole job here.
	void EjectPage()
	{
		if(_pageDirty && _page.size() == (size_t)PageWidth * PageHeight) {
			SavePage();
		}
		std::fill(_page.begin(), _page.end(), (uint8_t)0xFF);
		_xUnits = _yUnits = 0;
		_column = 0;
		_pageDirty = false;
	}

	void EndJob()
	{
		if(_printing) {
			EjectPage();
			_printing = false;
		}
		_page.clear();
		_page.shrink_to_fit();
		_state = DataState::Idle;
	}

	void SavePage()
	{
		string name = _romName.empty() ? "bbk" : _romName;
		string baseFilename = FolderUtilities::CombinePath(FolderUtilities::GetScreenshotFolder(), name + "_print");
		string filename;
		for(int counter = 0; ; counter++) {
			string counterStr = std::to_string(counter);
			while(counterStr.size() < 3) {
				counterStr = "0" + counterStr;
			}
			filename = baseFilename + "_" + counterStr + ".png";
			ifstream file(filename, ios::in);
			if(!file) {
				break;
			}
		}

		vector<uint32_t> pixels((size_t)PageWidth * PageHeight);
		for(size_t i = 0; i < pixels.size(); i++) {
			pixels[i] = _page[i] ? 0xFFFFFF : 0x000000;
		}

		if(PNGHelper::WritePNG(filename, pixels.data(), PageWidth, PageHeight)) {
			MessageManager::DisplayMessage("BBK", "Printed page saved: " + FolderUtilities::GetFilename(filename, true));
		} else {
			MessageManager::DisplayMessage("BBK", "Failed to save printed page");
		}
	}

	//Paints one pin's dot at the head's current position. The dot covers its own pitch, so
	//a 180dpi dot is 1.11 page pixels across and a 120dpi one 1.67 - taking the span
	//between two rounded edges rather than rounding a size keeps the columns gapless.
	void PlotDot(int pin)
	{
		int pitchX = PitchX();
		int pitchY = PitchY();
		if(pitchX <= 0 || pitchY <= 0) {
			return;
		}

		int x0 = LeftMargin + ToPixels(_xUnits);
		int x1 = LeftMargin + ToPixels(_xUnits + pitchX);
		int y0 = TopMargin + ToPixels(_yUnits + pin * pitchY);
		int y1 = TopMargin + ToPixels(_yUnits + (pin + 1) * pitchY);

		x1 = std::max(x1, x0 + 1);
		y1 = std::max(y1, y0 + 1);
		x0 = std::max(x0, 0);
		y0 = std::max(y0, 0);
		x1 = std::min(x1, PageWidth);
		y1 = std::min(y1, PageHeight);

		for(int y = y0; y < y1; y++) {
			uint8_t* row = _page.data() + (size_t)y * PageWidth;
			for(int x = x0; x < x1; x++) {
				row[x] = 0;
			}
			_pageDirty = true;
		}
	}

	void DrawBits(uint8_t data)
	{
		if(_page.empty()) {
			return;
		}

		for(int i = 0; i < 8; i++) {
			if(data & (0x80 >> i)) {
				PlotDot(_column * 8 + i);
			}
		}

		_column++;
		if(_graphicsMode || !_mode24p || _column == 3) {
			_column = 0;
			_xUnits += PitchX();
		}
	}

	//Advances down the page, breaking to a new sheet when there is no longer room for a
	//full band. The original had no page break at all: anything past the bottom edge was
	//written past the end of the page buffer.
	void AdvanceY(int units)
	{
		_yUnits += units;

		int reserve = ToPixels(PitchY() * _bandPins);
		if(TopMargin + ToPixels(_yUnits) + reserve > PageHeight) {
			EjectPage();
		}
	}

	void BeginData(int columns, bool graphics)
	{
		_column = 0;
		if(columns <= 0) {
			//An empty image block: undo the mode the command just selected rather than
			//leaving the state machine parked waiting for bytes that never come
			_graphicsMode = false;
			_state = DataState::Idle;
			return;
		}

		_bandPins = _mode24p && !graphics ? 24 : 8;
		_dataLength = _bandPins == 24 ? columns * 3 : columns;
		_graphicsMode = graphics;
		_state = DataState::Data;
	}

	//The 8-pin graphics commands, which differ only in horizontal density. Their pins are
	//spaced 1/60 inch on a 24-pin printer and 1/72 on a 9-pin one.
	void Begin8PinGraphics(int dpiX)
	{
		_dpiX = dpiX;
		_dpiY = _mode24p ? 60 : 72;
		BeginData(_params[1] * 256 + _params[0], true);
	}

	static int LookupIgnoredParams(uint8_t cmd)
	{
		for(const EscParams& entry : IgnoredCommands) {
			if(entry.Cmd == cmd) {
				return entry.Count;
			}
		}
		return -1;
	}

	void ExecuteCommand()
	{
		switch(_escCmd) {
			case 0x2A: {
				//ESC * m nL nH - select bit image
				const DotDensity* density = nullptr;
				for(const DotDensity& entry : DotDensityTable) {
					if(entry.Mode == _params[0]) {
						density = &entry;
						break;
					}
				}

				if(density) {
					_dpiX = density->DpiX;
					_dpiY = density->DpiY;
					_mode24p = density->Dots == 24;
				} else {
					_dpiX = 180;
					_dpiY = 180;
					_mode24p = true;
				}

				BeginData(_params[2] * 256 + _params[1], false);
				break;
			}

			case 0x4B: Begin8PinGraphics(60); break;  //ESC K - single density
			case 0x4C: Begin8PinGraphics(120); break; //ESC L - double density
			case 0x59: Begin8PinGraphics(120); break; //ESC Y - double density, double speed
			case 0x5A: Begin8PinGraphics(240); break; //ESC Z - quadruple density

			case 0x33:
				//ESC 3 n - line spacing of n/180 (24-pin) or n/216 (9-pin) inch
				_lineSpaceUnits = _params[0] * LineFeedUnit();
				break;

			case 0x43:
				//ESC C n - page length in lines, or ESC C 0 n - page length in inches.
				//The page size is fixed here; only the parameter count matters.
				break;

			case 0x4A:
				//ESC J n - advance n/180 or n/216 inch without a carriage return
				AdvanceY(_params[0] * LineFeedUnit());
				break;

			case 0x4E:
				//ESC N n - bottom margin in lines. The page size is fixed here.
				break;

			case 0x28:
				//ESC ( <cmd> nL nH <data> - the ESC/P2 extension format. None of these are
				//implemented, but the length is self-describing so they can be skipped.
				_dataLength = _params[2] * 256 + _params[1];
				_state = _dataLength > 0 ? DataState::Skip : DataState::Idle;
				break;

			default:
				break;
		}
	}

public:
	BbkPrinter() {}

	~BbkPrinter()
	{
		EndJob();
	}

	void SetRomName(string romName)
	{
		_romName = romName;
	}

	void Reset()
	{
		EndJob();
		_idleCycles = 0;
		_reset = false;
		_selectPrinter = false;
		_lineFeed = false;
		_strobe = false;
	}

	bool IsPrinting() { return _printing; }

	//$FF48 - status port. The lines are tied to "ready, online, paper loaded": there is no
	//host-side printer that could be busy, and the guest polls these before every byte.
	uint8_t ReadStatus()
	{
		uint8_t status = 0;
		status |= 0x08; //not error
		status |= 0x10; //selected
		//0x20 - paper out: never
		//0x40 - not ack: always acknowledged
		status |= 0x80; //not busy
		return status;
	}

	//$FF40 - data port. The byte is latched on the write itself, not on the strobe.
	void WriteData(uint8_t value)
	{
		_idleCycles = 0;

		switch(_state) {
			case DataState::Data:
				if(_dataLength > 0) {
					_dataLength--;
					DrawBits(value);
					if(_dataLength == 0) {
						_state = DataState::Idle;
						if(_graphicsMode) {
							_graphicsMode = false;
						} else {
							_xUnits = 0;
						}
					}
				}
				break;

			case DataState::Skip:
				if(_dataLength > 0 && --_dataLength == 0) {
					_state = DataState::Idle;
				}
				break;

			case DataState::SkipToNul:
				if(value == 0) {
					_state = DataState::Idle;
				}
				break;

			case DataState::Idle:
				switch(value) {
					case 0x0A: //LF
						AdvanceY(_lineSpaceUnits);
						break;

					case 0x0C: //FF - eject the sheet and carry on with the next one
						EjectPage();
						break;

					case 0x0D: //CR
						_xUnits = 0;
						break;

					case 0x1B: //ESC
						_state = DataState::Esc;
						break;

					default:
						break;
				}
				break;

			case DataState::Esc:
				_escCmd = value;
				_paramPos = 0;
				switch(value) {
					case 0x3C:
						//ESC < - return the head to the left margin
						_xUnits = 0;
						_state = DataState::Idle;
						break;

					case 0x40:
						//ESC @ - initialize. The first one starts the job; a later one
						//resets the print settings but does not move the paper.
						if(!_printing) {
							_printing = true;
							StartJob();
						} else {
							_lineSpaceUnits = UnitsPerInch / 6;
							_graphicsMode = false;
						}
						_xUnits = 0;
						_state = DataState::Idle;
						break;

					case 0x33: case 0x4A: case 0x4E:
						_paramCount = 1;
						_state = DataState::Param;
						break;

					case 0x43:
						//ESC C n is one parameter, ESC C 0 n is two - the parser extends
						//itself when the first byte turns out to be 0
						_paramCount = 1;
						_state = DataState::Param;
						break;

					case 0x4B: case 0x4C: case 0x59: case 0x5A:
						_paramCount = 2;
						_state = DataState::Param;
						break;

					case 0x2A: case 0x28:
						_paramCount = 3;
						_state = DataState::Param;
						break;

					case 0x42: case 0x44: case 0x62:
						//ESC B / ESC D / ESC b - NUL-terminated tab stop lists
						_state = DataState::SkipToNul;
						break;

					default: {
						int count = LookupIgnoredParams(value);
						if(count < 0) {
							//Genuinely unknown. The original left the state machine parked
							//in its Esc state here, which made it read the rest of the job
							//as a run of escape commands; a real printer ignores what it
							//does not know, and so does this - but its parameters cannot be
							//skipped, so say so rather than silently mangling the page.
							if(_unknownLogged < 8) {
								_unknownLogged++;
								MessageManager::Log("[BBK] Printer: unsupported escape sequence ESC $" + HexUtilities::ToHex(value));
							}
							_state = DataState::Idle;
						} else if(count == 0) {
							_state = DataState::Idle;
						} else {
							_paramCount = count;
							_state = DataState::Param;
						}
						break;
					}
				}
				break;

			case DataState::Param:
				if(_paramPos < (int)sizeof(_params)) {
					_params[_paramPos] = value;
				}
				_paramPos++;

				if(_escCmd == 0x43 && _paramPos == 1 && _params[0] == 0) {
					//ESC C 0 n - the page length is in inches, so one more byte follows
					_paramCount = 2;
				}

				if(_paramPos >= _paramCount) {
					_state = DataState::Idle;
					ExecuteCommand();
				}
				break;
		}
	}

	//$FF50 - control port, active-low lines. Only the reset line changes anything: the
	//data byte is already latched by the time the strobe arrives.
	void WriteControl(uint8_t value)
	{
		_idleCycles = 0;

		_strobe = !(value & 0x01);
		_lineFeed = !(value & 0x02);
		_selectPrinter = !(value & 0x08);

		bool reset = !(value & 0x04);
		if(_reset != reset) {
			if(!_reset) {
				EndJob();
			}
			_reset = reset;
		}
	}

	void Clock()
	{
		if(!_printing) {
			return;
		}

		_idleCycles++;
		if(_idleCycles > TimeoutCycles) {
			//The guest stopped mid-job: keep what it managed to print rather than losing it
			EndJob();
			_idleCycles = 0;
		}
	}

	void Serialize(Serializer& s) override
	{
		//The page bitmap is deliberately left out of save states - a print job is host-side
		//output, not machine state, and it is cheaper to let a loaded state keep printing
		//onto the page that is already on the platen.
		SV(_state); SV(_printing); SV(_escCmd); SV(_paramCount); SV(_dataLength); SV(_paramPos);
		SVArray(_params, 4);
		SV(_idleCycles); SV(_unknownLogged);
		SV(_reset); SV(_selectPrinter); SV(_lineFeed); SV(_strobe);
		SV(_xUnits); SV(_yUnits); SV(_column);
		SV(_dpiX); SV(_dpiY); SV(_lineSpaceUnits); SV(_bandPins); SV(_mode24p); SV(_graphicsMode);

		if(!s.IsSaving() && _printing && _page.empty()) {
			_page.assign((size_t)PageWidth * PageHeight, 0xFF);
			_pageDirty = false;
		}
	}
};
