#pragma once
#include "pch.h"
#include "Utilities/Serializer.h"
#include "Shared/Emulator.h"
#include "Shared/NotificationManager.h"
#include "Shared/Interfaces/INotificationListener.h"
#include "NES/Mappers/Yuxing/YuxingServerData.h"
#include "NES/Mappers/Yuxing/YuxingWebBridge.h"

//The far end of the line when the online option is on: the dial-up service the floppy model's
//online program was written for. Dialling 163 reached China Telecom's access server, which asked
//for the account and password and then handed the line to YuXing's own Unix host
//(www.yuxing.cn.net); the program logs in there a second time and starts the YuXing service on
//it. Both are long gone, so this plays them.
//
//The program reads the other end by watching for fixed text, never by parsing it properly:
//- after CONNECT it waits for a colon with "login" or "rname" (Username) before it
//- after the account it waits for the first colon and wants a 'd' before it (Password:)
//- after the password, the second prompt must be "login:" - an 'n' two places before the colon
//- after the second password, the first colon must come after "\r\nLa" (the fourth character
//  is an 'a': Last login:), and then it waits for a '!'
//- it then types the service's name and waits for a colon with ']' before it - the prompt
//Anything else and it hangs up with "login failed". Some of those checks read a fixed place in its
//buffer, and once it has seen the colon it holds the line (RTS down, interrupts off) until it has
//sent its answer and cleared the buffer - so nothing may follow a prompt's colon, not even the
//space a person would type after: it would arrive first in the cleared buffer and shift the
//text the next check reads. Unix's own Password: has none. Its receive buffer is cleared after it has
//sent each line, so whatever answers a line has to come a moment later, as it did over a real
//line: an answer arriving while the line was still being sent would be thrown away with the echo.
//
//The service is a menu of one-letter commands, each answered by its prompt. Its files are in
//numbered directories, /p2u1/yux/<n>: "3" and one character pick one ('1' to '9', then 'a' for
//10 on) and "1" lists it. The list is caught whole until the first "n:" and read by position -
//the echoed "1\r\n", then per file a 30-byte title, the file's name and CR LF, and a Ctrl-Z after
//the last; the name is what follows the title. A file is fetched with "yes", "1", "1" and its
//path, which starts a ZMODEM send (sz) of it: see SendFile. The program's 裕兴世界 keeps its
//boards (公告栏 留言板 电脑学校 图书馆 裕兴超市) in 16, fetched by name and shown as text, and
//its 软件下载 (文件库管理) lists libraries 1 to 9, whose files go to a floppy.
//
//因特网 is the program's browser, a front end to lynx on the host: "2", "1", then the address as a line, after
//which the program sends lynx's own keys (keypad 2 / 8 for the next / previous link, Enter to
//follow it, 4 to go back, space / b to page, g for an address, m for the home page) and draws
//what comes back on an 80 x 24 terminal of its own (VT100 cursor moves and clears, no
//attributes), showing a 30 x 11 window of it. The current link is where the cursor is left.
//Every answer that loads a page starts with "~~" and a status in two hex digits, which picks
//the program's message: 01 one page, 02 more to come, 07 asks for an address (网址:), 09 asks
//whether to go to the home page. An answer to a key that only moves about must hold an ESC and
//no '~' at all. The program shows an answer once the line has been quiet for ~0.4s.
//Pages are laid out to fit the window: a title row and 8 rows of 30 columns. The pages come
//from the UI (see YuxingWebBridge), which fetches them and turns them into that layout.
//The program's second way of showing a page (分屏方式, chosen at the start: the same commands reach
//the host either way) splits each 80-column row into lines of 30, 30 and 20 and shows three
//rows at a time, so these pages come out sparse there, three lines to its screen - the first
//way (移屏方式) is the one they are laid out for. Its keys are the arrows for '8' / '2', and it
//waits for a status after a page turn and pages back with keypad 9.
class YuxingHost final : public ISerializable
{
private:
	enum class Stage : uint8_t
	{
		Offline,
		AccessUser,
		AccessPassword,
		HostUser,
		HostPassword,
		Shell,
		//A ZMODEM send, by what the program is to answer next
		ZmodemInit,
		ZmodemPosition,
		ZmodemEnd,
		ZmodemFinish,
		//因特网 (the browser): lynx's keys
		Browse,
		//网友天地: tin's keys
		Tin,
		//电子邮件: Unix mail's commands
		Mail,
		Menu
	};

	//Where tin is: its list of groups, its options screen (and how far the program's setting of
	//it has got), a group, or the question before quitting
	enum class TinScreen : uint8_t
	{
		Groups,
		Options,
		Group,
		Article,
		QuitAsk
	};

	//What a key in the browser is waiting for: nothing, the address after g, the answer to m
	enum class BrowseInput : uint8_t
	{
		Keys,
		Address,
		HomeConfirm
	};

	struct PageLink
	{
		uint32_t Row;
		uint32_t Column;
		string Url;
	};

	//Rows 2-9 only: the system disc's 分屏方式 reader takes a two-digit row in ESC[r;cH for its units digit
	static constexpr uint32_t PageRowsLater = 8;
	//The earliest program pages the screen itself, 3 rows at a time over rows 2-22: it gets one screen of 21
	static constexpr uint32_t PageRowsEarliest = 21;
	uint32_t PageRows() const { return _earliest ? PageRowsEarliest : PageRowsLater; }
	static constexpr uint32_t ScreenColumns = 30;
	//How long to wait for the UI to answer before giving up on a page: ~60s
	static constexpr uint32_t FetchTimeoutCycles = 1770000 * 60;
	static constexpr const char* HomeAddress = "www.yuxing.cn.net";

	static constexpr uint32_t OutSize = 16384;
	static constexpr uint32_t MaxLine = 255;
	//How long the other end takes to answer a line, in CPU cycles at ~1.77MHz: ~0.2s
	static constexpr uint32_t AnswerCycles = 1770000 / 5;
	static constexpr uint32_t SubpacketSize = 1024;

	static constexpr uint8_t Zdle = 0x18;
	static constexpr uint8_t Xon = 0x11;
	//ZMODEM frame types, and the subpacket ends: more to come / end of the file / the header's
	//subpacket, which wants an answer
	static constexpr uint8_t ZrqInit = 0, Zfile = 4, Zfin = 8, Zdata = 10, Zeof = 11;
	static constexpr uint8_t ZcrcE = 'h', ZcrcG = 'i', ZcrcW = 'k';

	Stage _stage = Stage::Offline;
	uint8_t _out[OutSize] = {};
	uint32_t _outHead = 0;
	uint32_t _outCount = 0;
	uint8_t _line[MaxLine + 1] = {};
	uint32_t _lineLength = 0;
	uint32_t _wait = 0;

	//The directory the program is looking at (picked by the character after "3"), whether that
	//character is still to come, and prompts still owed to a download's confirmation ("yes",
	//then two choices answered "1")
	uint8_t _dir = 16;
	bool _pickingDir = false;
	uint8_t _confirmations = 0;
	//The file being sent: its place in YuxingHostSections and index, and how much of it is queued
	uint8_t _sendSection = 0;
	uint8_t _sendFile = 0;
	uint32_t _sendPos = 0;
	bool _sendingData = false;
	//After a download the program types "2" on its way back to the menu it came from - that one
	//is not the "2" of 因特网 and 网友天地
	bool _afterDownload = false;
	bool _pageNext = false;
	bool _pageMore = false;
	bool _earliest = false;
	shared_ptr<const YuxingContent> _content;
	bool _overPort = false;

	//因特网: "2" then "1" lead to the address line; 网友天地: "2" then "3" start tin
	uint8_t _internetStep = 0;
	TinScreen _tinScreen = TinScreen::Groups;
	vector<uint8_t> _tinNumber;
	uint8_t _tinForum = 0;
	//电子邮件: "1" then "2" start mail; the letter mail is on
	bool _mailNext = false;
	uint8_t _mailCurrent = 0;
	//The page on show as the UI sent it (re-read after a state load), the addresses before it,
	//its first row on screen and the current link
	vector<uint8_t> _page;
	vector<uint8_t> _history;
	uint32_t _top = 0;
	int32_t _link = -1;
	BrowseInput _browseInput = BrowseInput::Keys;
	vector<uint8_t> _address;
	//A page asked of the UI and not come yet, and whether the address it replaces goes on the history
	bool _fetching = false;
	//Cycles, once the status of an 'm' has gone out, before its page is asked for
	static constexpr uint32_t HomeDelayCycles = 1770000 / 2;
	uint32_t _homeDelay = 0;
	//The earliest program's library (LS): a directory of the content
	uint8_t _library = 16;
	bool _fetchPushes = false;
	uint32_t _fetchId = 0;
	uint32_t _fetchCycles = 0;
	vector<uint8_t> _fetchUrl;
	//Not saved: a request is sent again after a state load
	bool _requestSent = false;
	Emulator* _emu = nullptr;

	//The page, read
	string _title;
	string _url;
	vector<string> _lines;
	vector<PageLink> _links;

	void SendByte(uint8_t value)
	{
		if(_outCount < OutSize) {
			_out[(_outHead + _outCount) % OutSize] = value;
			_outCount++;
		}
	}

	void SendBytes(const uint8_t* data, uint32_t size)
	{
		for(uint32_t i = 0; i < size; i++) {
			SendByte(data[i]);
		}
	}

	void Send(const char* text)
	{
		while(*text) {
			SendByte((uint8_t)*text++);
		}
	}

	void Prompt()
	{
		Send("\r\n[YuXing]:");
	}

	bool Echoes()
	{
		//Passwords are not echoed; everything else is, as a terminal line does. The earliest
		//program's menu system does echo (it waits for each character back)
		return _stage != Stage::HostPassword && (_stage != Stage::AccessPassword || _earliest);
	}

	static uint16_t Crc16(uint16_t crc, uint8_t value)
	{
		crc ^= (uint16_t)(value << 8);
		for(int i = 0; i < 8; i++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
		}
		return crc;
	}

	//What ZMODEM escapes: ZDLE, the flow control characters and their high-bit forms. The
	//program undoes an escape by taking 0x40 off, which is the escape's XOR for all of these.
	void SendEscaped(uint8_t value)
	{
		switch(value) {
			case 0x10: case 0x11: case 0x13: case 0x18:
			case 0x90: case 0x91: case 0x93: case 0x98:
				SendByte(Zdle);
				SendByte(value ^ 0x40);
				break;
			default:
				SendByte(value);
				break;
		}
	}

	void SendHexHeader(uint8_t type, uint32_t position, bool xon)
	{
		uint8_t header[5] = { type, (uint8_t)position, (uint8_t)(position >> 8), (uint8_t)(position >> 16), (uint8_t)(position >> 24) };
		uint16_t crc = 0;
		for(uint8_t value : header) {
			crc = Crc16(crc, value);
		}
		Send("**\x18" "B");
		char hex[16];
		for(uint8_t value : header) {
			snprintf(hex, sizeof(hex), "%02x", value);
			Send(hex);
		}
		snprintf(hex, sizeof(hex), "%02x%02x", crc >> 8, crc & 0xFF);
		Send(hex);
		Send("\r\n");
		if(xon) {
			SendByte(Xon);
		}
	}

	//The program skips a binary header by its length (10), so these must need no escapes - true
	//of every one sent here but ZEOF's, which comes when nothing reads it any more
	void SendBinaryHeader(uint8_t type, uint32_t position)
	{
		uint8_t header[5] = { type, (uint8_t)position, (uint8_t)(position >> 8), (uint8_t)(position >> 16), (uint8_t)(position >> 24) };
		uint16_t crc = 0;
		Send("*\x18" "A");
		for(uint8_t value : header) {
			crc = Crc16(crc, value);
			SendEscaped(value);
		}
		SendEscaped((uint8_t)(crc >> 8));
		SendEscaped((uint8_t)crc);
	}

	void SendSubpacket(const uint8_t* data, uint32_t size, uint8_t end)
	{
		uint16_t crc = 0;
		for(uint32_t i = 0; i < size; i++) {
			crc = Crc16(crc, data[i]);
			SendEscaped(data[i]);
		}
		crc = Crc16(crc, end);
		SendByte(Zdle);
		SendByte(end);
		SendEscaped((uint8_t)(crc >> 8));
		SendEscaped((uint8_t)crc);
	}

	//What this call serves. A state loaded from elsewhere can hold places in other content
	const YuxingContent& Content()
	{
		if(!_content) {
			_content = YuxingServerData::Get();
		}
		return *_content;
	}

	const YuxingDataFile& SentFile()
	{
		static const YuxingDataFile none;
		const YuxingContent& content = Content();
		if(_sendSection < content.Sections.size() && _sendFile < content.Sections[_sendSection].Files.size()) {
			return content.Sections[_sendSection].Files[_sendFile];
		}
		return none;
	}

	//sz, after the program has asked for a file: "rz" and ZRQINIT, then each step waits for the
	//program's answer (see ReceiveZmodem). It takes the ZFILE frame up to its first XON by
	//position - the name 10 bytes in - and the data by skipping the ZDATA header's 10 bytes.
	void SendFile(uint8_t section, uint8_t file)
	{
		_sendSection = section;
		_sendFile = file;
		_sendPos = 0;
		_sendingData = false;
		Send("rz\r");
		SendHexHeader(ZrqInit, 0, true);
		_stage = Stage::ZmodemInit;
	}

	void SendFileHeader()
	{
		const YuxingDataFile& file = SentFile();
		SendBinaryHeader(Zfile, 0);
		string info = file.Name + '\0' + std::to_string(file.Data.size()) + '\0';
		SendSubpacket((const uint8_t*)info.data(), (uint32_t)info.size(), ZcrcW);
		SendByte(Xon);
	}

	//The data goes out a subpacket at a time as the queue empties, so a file of any size fits
	void QueueData()
	{
		const YuxingDataFile& file = SentFile();
		uint32_t fileSize = (uint32_t)file.Data.size();
		while(_sendingData && _outCount < OutSize - SubpacketSize * 2 - 16) {
			uint32_t size = std::min(SubpacketSize, fileSize - _sendPos);
			bool last = _sendPos + size >= fileSize;
			SendSubpacket(file.Data.data() + _sendPos, size, last ? ZcrcE : ZcrcG);
			_sendPos += size;
			if(last) {
				SendBinaryHeader(Zeof, fileSize);
				_sendingData = false;
			}
		}
	}

	//The program's ZMODEM answers are hex headers; only which one comes next matters, and each
	//ends with an XON but ZFIN, which ends its line
	void ReceiveZmodem(uint8_t value)
	{
		switch(_stage) {
			case Stage::ZmodemInit:
				//ZRINIT
				if(value == Xon) {
					_wait = AnswerCycles;
					SendFileHeader();
					_stage = Stage::ZmodemPosition;
				}
				break;

			case Stage::ZmodemPosition:
				//ZRPOS at 0
				if(value == Xon) {
					_wait = AnswerCycles;
					SendBinaryHeader(Zdata, 0);
					_sendingData = true;
					QueueData();
					_stage = Stage::ZmodemEnd;
				}
				break;

			case Stage::ZmodemEnd:
				//ZRINIT, after the end of the file
				if(value == Xon) {
					_wait = AnswerCycles;
					SendHexHeader(Zfin, 0, false);
					_stage = Stage::ZmodemFinish;
				}
				break;

			case Stage::ZmodemFinish:
				//ZFIN. sz's "OO" is left out: the program has stopped listening, and it would
				//only turn up in front of the next answer
				if(value == '\n') {
					_stage = Stage::Shell;
					_afterDownload = true;
					//The controller-port programs wait for the menu's message that follows a transfer,
					//by its last characters
					if(_overPort) {
						_wait = AnswerCycles;
						Send("\r\nBack to menu.\r\n");
					}
				}
				break;

			default:
				break;
		}
	}

	//The directory's place in the content's sections, or -1 for one that holds nothing
	int FindSection(int dir)
	{
		const YuxingContent& content = Content();
		for(int i = 0; i < (int)content.Sections.size(); i++) {
			if(content.Sections[i].Dir == dir) {
				return i;
			}
		}
		return -1;
	}

	void SendList()
	{
		int index = FindSection(_dir);
		if(index >= 0) {
			const YuxingDataSection& section = Content().Sections[index];
			for(const YuxingDataFile& file : section.Files) {
				SendBytes(file.Title.data(), (uint32_t)file.Title.size());
				Send(file.Name.c_str());
				//电脑学校's reader skips one more character after each line's CR LF
				Send(section.Dir >= 17 ? "\r\n " : "\r\n");
			}
		}
		SendByte(0x1A);
		Send("\r\nSelection:");
	}

	//股市传真 reads the file off the screen as more shows it: plain text up to the first ESC, a page
	//ending in the --More-- prompt's ESC [ m. The file's last ESC [ J it takes for an end with nothing
	//left to draw, so that comes after the Space that asks for more. There are no quotes here. It
	//leaves as it does after a download
	void SendPage(const string& line)
	{
		int section;
		int file;
		if(FindPath(line, section, file)) {
			//All of it but the ^Z that ends a file
			const vector<uint8_t>& data = Content().Sections[section].Files[file].Data;
			SendBytes(data.data(), (uint32_t)data.size() - (!data.empty() && data.back() == 0x1A ? 1 : 0));
		}
		Send("\r\n\x1B[7m--More--\x1B[m");
		_pageMore = true;
		_afterDownload = true;
	}

	static bool SameName(const string& a, const string& b)
	{
		if(a.size() != b.size()) {
			return false;
		}
		for(size_t i = 0; i < a.size(); i++) {
			if(toupper((uint8_t)a[i]) != toupper((uint8_t)b[i])) {
				return false;
			}
		}
		return true;
	}

	//The earliest program's file lists: "LS <library>" picks one (STOCK = 股市传真, the files of directory 0;
	//YUXING2 = the boards, directory 16), "LLCL" lists it. Its reader (bank $AEAB) skips to the line with a
	//'=', then takes each line not starting with a space: the name in columns 0-17, a description from
	//column 28, up to 11 bytes; an empty line ends the list
	void SendLibraryList()
	{
		Send("\r\nLibrary list\r\n==================== ========= ===========\r\n");
		int section = FindSection(_library);
		if(section >= 0) {
			for(const YuxingDataFile& file : Content().Sections[section].Files) {
				string line = Clip(file.Name, 18);
				line.resize(18, ' ');
				char size[16];
				snprintf(size, sizeof(size), " %8u ", (uint32_t)file.Data.size());
				line += size;
				string title(file.Title.begin(), file.Title.end());
				while(!title.empty() && title.back() == ' ') {
					title.pop_back();
				}
				line += Clip(title, 11);
				Send((line + "\r\n").c_str());
			}
		}
		Send("\r\nCommand:~\x08");
	}

	//The earliest program's 选择论坛 ("S?"): its reader ($8BD6) skips a line of '-', then takes each line up
	//to an empty one: the name in columns 0-14, 4 columns from 16, 8 from 31, a description from 41 (16 bytes)
	void SendForumList()
	{
		static const char* forums[] = {
			"\xD4\xA3\xD0\xCB\xC2\xDB\xCC\xB3",               //裕兴论坛
			"\xB9\xE3\xB6\xF8\xB8\xE6\xD6\xAE",               //广而告之
			"\xD0\xC2\xCA\xD6\xBC\xD3\xD3\xCD\xD5\xBE",       //新手加油站
			"\xD3\xA2\xD3\xEF\xC9\xB3\xC1\xFA",               //英语沙龙
			"\xB7\xC7\xB3\xA3\xD0\xA3\xD4\xB0",               //非常校园
			"\xCF\xA6\xD1\xF4\xBA\xEC",                       //夕阳红
			"\xCC\xE5\xD3\xFD",                               //体育
			"\xD0\xC2\xB8\xC9\xCF\xDF",                       //新干线
			"\xD3\xCE\xCF\xB7\xC3\xD4\xBE\xE3\xC0\xD6\xB2\xBF", //游戏迷俱乐部
			"\xBD\xA1\xBF\xB5\xC0\xD6\xD4\xB0",               //健康乐园
			"\xB9\xC9\xCA\xD0\xB7\xE7\xD4\xC6",               //股市风云
			"\xCD\xF8\xC2\xE7\xD7\xF7\xBC\xD2",               //网络作家
			"\xCC\xEC\xB5\xD8\xD0\xD0",                       //天地行
		};
		//The reader starts 128 bytes into what it took in: a heading of exactly that, then the dashes in one
		//run (it skips '-' only, up to the line's end)
		string heading = "\r\nForum list";
		heading.resize(124, ' ');      //(one byte more comes before it: land inside the dashes)
		Send((heading + "\r\n").c_str());
		Send("------------------------------------------------------------\r\n");
		for(const char* name : forums) {
			string line = name;
			line.resize(16, ' ');
			line += "   0";
			line.resize(31, ' ');
			line += "\xD4\xA3\xD0\xCB";      //主持人 (the moderator): 裕兴
			line.resize(41, ' ');
			line += name;
			Send((line + "\r\n").c_str());
		}
		Send("\r\nCommand:~\x08");
	}

	//The earliest program's boards: after "LS YUXING2", "LD <name>.TXT A" fetches one as text. It keeps
	//what comes from the first '*' to the '~' of the next prompt and the character after it, so the
	//text starts with a '*' and may hold no '~'. Its four names are the later program's boards
	void SendLibraryText(const string& request)
	{
		static const char* names[][2] = {
			{ "\xB9\xAB\xB8\xE6\xC0\xB8.TXT", "ggl.txt" },              //公告栏
			{ "\xB5\xE7\xC4\xD4\xBF\xCE\xCC\xC3.TXT", "dnxx.txt" },      //电脑课堂
			{ "\xCD\xBC\xCA\xE9\xB9\xDD.TXT", "tsg.txt" },               //图书馆
			{ "\xC8\xED\xBC\xFE\xB3\xAC\xCA\xD0.TXT", "yxcs.txt" },      //软件超市
		};
		string name = request.substr(0, request.find(' '));
		const vector<uint8_t>* data = nullptr;
		int section = FindSection(16);
		for(auto& entry : names) {
			if(section >= 0 && name == entry[0]) {
				for(const YuxingDataFile& file : Content().Sections[section].Files) {
					if(file.Name == entry[1]) {
						data = &file.Data;
					}
				}
			}
		}
		//Or a file of the library chosen with LS, by its name in any case
		section = FindSection(_library);
		if(!data && section >= 0) {
			for(const YuxingDataFile& file : Content().Sections[section].Files) {
				if(SameName(file.Name, name)) {
					data = &file.Data;
				}
			}
		}
		Send("\r\n*");
		if(data) {
			for(uint8_t value : *data) {
				if(value != 0x1A) {
					SendByte(value == '~' ? '-' : value);
				}
			}
		} else {
			Send((name + ": not found").c_str());
		}
		Send("\r\nCommand:~\x08");
	}

	//A path the program asks for, /p2u1/yux/<directory>/<name>, or else the page saying the host
	//does not have it: directory 0's first file
	bool FindPath(const string& line, int& section, int& file)
	{
		static const string root = "/p2u1/yux/";
		size_t slash = line.find('/', root.size());
		if(line.compare(0, root.size(), root) == 0 && slash != string::npos) {
			section = FindSection(atoi(line.substr(root.size(), slash - root.size()).c_str()));
			string name = line.substr(slash + 1);
			while(!name.empty() && name.back() == ' ') {
				name.pop_back();
			}
			if(section >= 0) {
				const vector<YuxingDataFile>& files = Content().Sections[section].Files;
				for(file = 0; file < (int)files.size(); file++) {
					if(name == files[file].Name) {
						return true;
					}
				}
			}
		}
		section = FindSection(0);
		file = 0;
		return section >= 0 && !Content().Sections[section].Files.empty();
	}

	//The program has no way back from an answer that is not a transfer
	bool SendPath(const string& line)
	{
		int section;
		int file;
		if(!FindPath(line, section, file)) {
			return false;
		}
		SendFile((uint8_t)section, (uint8_t)file);
		return true;
	}

	//Columns a GB2312 string takes: a lead byte from $A1 starts a two-column character
	static uint32_t Width(const string& text)
	{
		uint32_t width = 0;
		for(size_t i = 0; i < text.size(); i++) {
			if((uint8_t)text[i] >= 0xA1 && i + 1 < text.size()) {
				i++;
				width += 2;
			} else {
				width++;
			}
		}
		return width;
	}

	static string Clip(const string& text, uint32_t columns)
	{
		string out;
		uint32_t width = 0;
		for(size_t i = 0; i < text.size(); i++) {
			bool wide = (uint8_t)text[i] >= 0xA1 && i + 1 < text.size();
			if(width + (wide ? 2 : 1) > columns) {
				break;
			}
			out += text[i];
			if(wide) {
				out += text[++i];
			}
			width += wide ? 2 : 1;
		}
		return out;
	}

	//The UI's page: lines of "T" the title, "U" the address it came from, "P" a row of text,
	//"K<row> <column> <address>" a link
	void ReadPage()
	{
		_title.clear();
		_url.clear();
		_lines.clear();
		_links.clear();
		string text(_page.begin(), _page.end());
		size_t start = 0;
		while(start < text.size()) {
			size_t end = text.find('\n', start);
			if(end == string::npos) {
				end = text.size();
			}
			string line = text.substr(start, end - start);
			start = end + 1;
			if(line.empty()) {
				continue;
			}
			string value = line.substr(1);
			switch(line[0]) {
				case 'T': _title = value; break;
				case 'U': _url = value; break;
				case 'P': _lines.push_back(value); break;
				case 'K': {
					uint32_t row = 0, column = 0;
					int used = 0;
					if(sscanf_s(value.c_str(), "%u %u %n", &row, &column, &used) >= 2 && row < _lines.size()) {
						_links.push_back({ row, column, value.substr(used) });
					}
					break;
				}
			}
		}
	}

	void RequestPage(const string& url, bool pushHistory)
	{
		_fetching = true;
		_fetchPushes = pushHistory;
		_fetchUrl.assign(url.begin(), url.end());
		_fetchId++;
		_fetchCycles = 0;
		_requestSent = false;
	}

	void SendRequest()
	{
		_requestSent = true;
		if(!_emu) {
			return;
		}
		string url(_fetchUrl.begin(), _fetchUrl.end());
		MessageManager::Log("[YuXing] Browser fetching " + url);
		YuxingWebRequestParams request = { _fetchId, url.c_str() };
		_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::YuxingWebRequest, &request);
	}

	void PageArrived(vector<uint8_t>& page)
	{
		_fetching = false;
		if(_fetchPushes && !_url.empty()) {
			_history.insert(_history.end(), _url.begin(), _url.end());
			_history.push_back('\n');
		}
		_page = std::move(page);
		ReadPage();
		_top = 0;
		_link = FirstLinkOnPage();
		ShowPage(_lines.size() > PageRows() ? 2 : 1);
	}

	int32_t FirstLinkOnPage()
	{
		for(size_t i = 0; i < _links.size(); i++) {
			if(_links[i].Row >= _top && _links[i].Row < _top + PageRows()) {
				return (int32_t)i;
			}
		}
		return -1;
	}

	//The whole window again; a page that has just come is preceded by its status
	void ShowPage(uint8_t status)
	{
		_wait = AnswerCycles;
		char text[32];
		//The earliest program takes the status at the end (see below)
		if(status && !_earliest) {
			snprintf(text, sizeof(text), "~~%02x", status);
			Send(text);
		}
		Send("\x1b[H\x1b[2J");

		uint32_t pages = std::max<uint32_t>(1, ((uint32_t)_lines.size() + PageRows() - 1) / PageRows());
		string count;
		if(pages > 1) {
			snprintf(text, sizeof(text), " %u/%u", _top / PageRows() + 1, pages);
			count = text;
		}
		string title = Clip(_title, ScreenColumns - (uint32_t)count.size());
		title += string(ScreenColumns - (uint32_t)count.size() - Width(title), ' ') + count;
		Send("\x1b[1;1H");
		Send(title.c_str());

		for(uint32_t row = 0; row < PageRows() && _top + row < _lines.size(); row++) {
			snprintf(text, sizeof(text), "\x1b[%u;1H", row + 2);
			Send(text);
			Send(_lines[_top + row].c_str());
		}

		//The later programs' 分屏方式 reader splits lynx's screen into 7 screens of 3 rows, each starting
		//where its first row starts in what came in; one with no row of its own starts at the end, and
		//draws what an earlier, longer answer left behind it. It takes a row only as one digit in an
		//ESC sequence, but also counts line feeds: empty rows down to row 21 (the window of the first
		//way ends far above them)
		if(!_earliest) {
			Send("\x1b[9;1H\n\n\n\n\n\n\n\n\n\n\n\n");
		}

		//The cursor on the current link marks it. With none on screen it goes to the top left, not
		//below row 21 as lynx's would: the program sends nothing for the link keys from there,
		//and the next link could not be reached
		if(_link >= 0 && _links[_link].Row >= _top && _links[_link].Row < _top + PageRows()) {
			snprintf(text, sizeof(text), "\x1b[%u;%uH", _links[_link].Row - _top + 2, _links[_link].Column + 1);
		} else {
			snprintf(text, sizeof(text), "\x1b[2;1H");
		}
		Send(text);
		//The earliest program re-reads the whole screen from its capture buffer for each of its pages
		//of three rows: once up to a zero byte, which leaves the cursor where the link is, and once to
		//draw, up to a "~~" - a status, which it would take for the next answer's. That pass reads on
		//into whatever an earlier, longer answer left in the buffer, so the cursor goes below the 22
		//rows it draws before then
		//
		//So its status ends the page: the drawing pass of each 3-row page stops at the first "~~", which
		//before the page left the first one empty; a zero byte before it ends the counting pass with
		//the cursor still on the link
		if(_earliest) {
			SendByte(0);
			snprintf(text, sizeof(text), "~~%02x", status ? status : (_top + PageRows() < _lines.size() ? 2 : 1));
			Send(text);
		}
	}

	void MoveLink(int direction)
	{
		int32_t next = _link + direction;
		if(_link < 0) {
			//No link on this page: the next one after it, or the last one before it
			next = -1;
			for(size_t i = 0; i < _links.size(); i++) {
				if(direction > 0 && _links[i].Row >= _top + PageRows()) {
					next = (int32_t)i;
					break;
				}
				if(direction < 0 && _links[i].Row < _top) {
					next = (int32_t)i;
				}
			}
		}
		if(next >= 0 && next < (int32_t)_links.size()) {
			_link = next;
			_top = _links[_link].Row / PageRows() * PageRows();
		} else if(direction > 0 && _top + PageRows() < _lines.size()) {
			_top += PageRows();
			_link = FirstLinkOnPage();
		} else if(direction < 0 && _top >= PageRows()) {
			_top -= PageRows();
			_link = FirstLinkOnPage();
		}
		ShowPage(0);
	}

	//With a status: the program's second way of showing pages (分屏方式) waits for one after a
	//page turn, and the first ignores a status that comes before the redraw's first ESC
	void TurnPage(int direction)
	{
		if(direction > 0 && _top + PageRows() < _lines.size()) {
			_top += PageRows();
		} else if(direction < 0 && _top >= PageRows()) {
			_top -= PageRows();
		}
		_link = FirstLinkOnPage();
		ShowPage(_top + PageRows() < _lines.size() ? 2 : 1);
	}

	void GoBack()
	{
		if(_history.size() < 2) {
			ShowPage(1);
			return;
		}
		//The last address on the history, without its newline
		size_t end = _history.size() - 1;
		size_t start = end;
		while(start > 0 && _history[start - 1] != '\n') {
			start--;
		}
		string url(_history.begin() + start, _history.begin() + end);
		_history.resize(start);
		RequestPage(url, false);
	}

	void LeaveBrowser()
	{
		_stage = Stage::Shell;
		_fetching = false;
		_browseInput = BrowseInput::Keys;
		_wait = AnswerCycles;
		if(_earliest) {
			//The earliest program waits for a status after its Ctrl-C, then asks for a new address
			//itself (请输入新的网址, EXIT = leave): what it sends is an address, or "exit" - also sent
			//for Esc - which leaves the host (see ReceiveBrowse)
			_stage = Stage::Browse;
			_browseInput = BrowseInput::Address;
			_address.clear();
			Send("\x1b[24;1H~~01");
			return;
		}
		Prompt();
	}

	void ReceiveBrowse(uint8_t value)
	{
		//Ctrl-C leaves, waiting or not
		if(value == 0x03) {
			LeaveBrowser();
			return;
		}
		if(_fetching) {
			return;
		}

		switch(_browseInput) {
			case BrowseInput::Address:
				if(value == '\r') {
					_browseInput = BrowseInput::Keys;
					string url(_address.begin(), _address.end());
					_address.clear();
					if(_earliest && SameName(url, "exit")) {
						//Out of lynx and off the host, back to the menu system, whose prompt it waits for
						_stage = Stage::Menu;
						_wait = AnswerCycles;
						Send("\r\nCommand:~\x08");
					} else if(url.empty()) {
						ShowPage(1);
					} else {
						RequestPage(url, true);
					}
				} else if(value == 8 || value == 0x7F) {
					if(!_address.empty()) {
						_address.pop_back();
					}
				} else if(value >= 0x20 && _address.size() < MaxLine) {
					_address.push_back(value);
					//The earliest program edits the address itself and sends it a character at a time,
					//each once the one before has come back, as lynx's prompt echoes them
					if(_earliest) {
						SendByte(value);
					}
				}
				return;

			case BrowseInput::HomeConfirm:
				if(value == 'y' || value == 'Y') {
					_browseInput = BrowseInput::Keys;
					RequestPage(HomeAddress, true);
				} else if(value == 'n' || value == 'N') {
					//The system disc's program waits for an answer either way: the page is still on its
					//screen, and it has only the status to take back. The floppies' programs go straight
					//back to their keys
					_browseInput = BrowseInput::Keys;
					if(!_overPort) {
						Send(_top + PageRows() < _lines.size() ? "\x1b[24;1H~~02" : "\x1b[24;1H~~01");
					}
				}
				return;

			default:
				break;
		}

		switch(value) {
			case '2': MoveLink(1); break;
			case '8': MoveLink(-1); break;
			//lynx's keypad: 6 (right) follows the link, as Enter, and 3 (PgDn) turns the page. The
			//earliest program sends these
			case '6': ReceiveBrowse('\r'); break;
			case '3':
			case ' ': TurnPage(1); break;
			//Back a page: 'b' from 移屏方式, keypad 9 (PgUp) from 分屏方式
			case 'b': case '9': TurnPage(-1); break;
			case '4': GoBack(); break;

			case '\r':
				//The program waits for a page after Enter: with no link, the same one again
				if(_link >= 0) {
					RequestPage(_links[_link].Url, true);
				} else {
					ShowPage(_lines.size() > PageRows() ? 2 : 1);
				}
				break;

			case 'g':
				_browseInput = BrowseInput::Address;
				_address.clear();
				_wait = AnswerCycles;
				Send("\x1b[24;1H~~07");
				break;

			case 'm':
				//Home, without lynx's question (status 09): after its "n" the programs go back to their keys
				//still in status 09, which nothing sent to them can change - the system disc's ignores all
				//but g and m, the controller-port one indexes its key table past the end and crashes. With any
				//other status they wait for a second one and the page; that wait clears the receive buffer
				//first, so the page comes a moment after the status has gone out (_homeDelay)
				_wait = AnswerCycles;
				if(_earliest) {
					//The earliest program does ask: it shows the question for lynx's status 09 and waits for
					//Y or N, then sends 'y' or 'n' (and after 'n' goes back to its keys)
					Send("\x1b[24;1H~~09");
					_browseInput = BrowseInput::HomeConfirm;
					break;
				}
				Send("\x1b[24;1H~~02");
				_homeDelay = HomeDelayCycles;
				break;
		}
	}

	//网友天地 is a front end to tin, the newsreader, whose forums are its own list of twelve: it
	//reads tin's screens by fixed marks, so this answers each with one. Starting, and leaving a
	//group or the options: "***" and three characters. 'M' (the options), then "11" Enter, Space
	//until an answer starts ESC 'F', Enter, 'q': "save." after 'M' and after the Enter, "& <" after
	//the option number. A group by its number: tin's header, whose "(<n>T" is the number of
	//threads, then ')' ESC and later ESC "[J". 'q' from the groups: a "(y/n):" question, then
	//Enter returns to the service. The program's twelve 'c' (catch up) want no answer. The
	//forums hold the content's threads (a group with none would show garbage: the program
	//takes 0 threads as a full page).

	//A group's thread list as tin draws it, read by position: the header's "(<n>T" and ')' ESC,
	//then per thread "ESC[<row>;1H " and two spaces, the number in 4, two spaces, 3 columns, a
	//space, the subject as hex digits of its bytes (the program decodes them in place) and a
	//space, padding to column 60, the author up to an ESC
	void SendThreadList()
	{
		const vector<YuxingDataThread>& threads = Content().Forums[_tinForum].Threads;
		uint32_t count = (uint32_t)threads.size();
		char text[64];
		snprintf(text, sizeof(text), "\x1b[H\x1b[2J  Group %u (%uT %uA 0K 0H R)\x1b[K", _tinForum + 1, count, count);
		Send(text);
		for(uint32_t i = 0; i < count; i++) {
			const YuxingDataThread& thread = threads[i];
			string line;
			snprintf(text, sizeof(text), "\x1b[%u;1H   %4u      ", i + 3, i + 1);
			line = text;
			for(uint8_t value : thread.Subject) {
				snprintf(text, sizeof(text), "%02X", value);
				line += text;
			}
			//The author at the 60th column after "H " (the subject's hex ends by its 40th)
			line.resize(line.find('H') + 2 + 60, ' ');
			line.append(thread.Author.begin(), thread.Author.end());
			line += "\x1b[K";
			Send(line.c_str());
		}
		Send("\x1b[J");
	}

	//An article as tin shows it, read by position: after "Reading... " and 12 more bytes, a
	//30-byte field and a second one on the first line; a line with four ESCs and 6 bytes before
	//its field; a 30-byte field line; a blank line; the body, lines under 78 bytes; and tin's
	//"-- " line at the end
	void SendArticle(uint32_t index)
	{
		const YuxingDataThread& thread = Content().Forums[_tinForum].Threads[index];
		//The date fills 30 columns and two more are passed over; after the group's CR LF the program
		//passes two characters (its add takes the carry its compare set), after the subject's one
		Send("Reading... \x1b[2J\x1b[1;1H\r\n");
		Send("Sat, 31 Oct 1998 12:00:00       ");
		Send("yuxing.forum\r\n  ");
		Send("\x1b" "A\x1b" "A\x1b" "A\x1b" "123456");
		SendBytes(thread.Subject.data(), (uint32_t)thread.Subject.size());
		Send("\r\n ");
		string author(thread.Author.begin(), thread.Author.end());
		author.resize(30, ' ');
		Send(author.c_str());
		Send("\r\n\r\n ");
		SendBytes(thread.Body.data(), (uint32_t)thread.Body.size());
		Send("\r\n-- Last response --");
	}

	void StartTin()
	{
		_stage = Stage::Tin;
		_tinScreen = TinScreen::Groups;
		_tinNumber.clear();
		Send("\r\ntin 1.4 PL5 [UNIX]\r\n***END");
	}

	void ReceiveTin(uint8_t value)
	{
		if(value >= '0' && value <= '9') {
			if(_tinNumber.size() < 8) {
				_tinNumber.push_back(value);
			}
			return;
		}

		_wait = AnswerCycles;
		bool number = !_tinNumber.empty();
		string digits(_tinNumber.begin(), _tinNumber.end());
		if(value == '\r') {
			_tinNumber.clear();
		}

		switch(_tinScreen) {
			case TinScreen::Groups:
				if(value == '\r' && number) {
					int group = atoi(digits.c_str());
					if(group >= 1 && group <= (int)Content().Forums.size()) {
						_tinForum = (uint8_t)(group - 1);
						_tinScreen = TinScreen::Group;
						SendThreadList();
					}
				} else if(value == 'M') {
					_tinScreen = TinScreen::Options;
					Send("\x1b[H\x1b[2J  Options Menu\r\n\r\n Select option number before text or use arrows to highlight then <CR> to\r\n set, q to quit & save.");
				} else if(value == 'q') {
					_tinScreen = TinScreen::QuitAsk;
					Send("\r\nDo you really want to quit? (y/n):");
				} else if(value == 'H') {
					Send("\r\n***END");
				}
				break;

			case TinScreen::Options:
				if(value == '\r' && number) {
					Send("\r\n<SPACE> toggles & <CR> sets.");
				} else if(value == ' ') {
					Send("\x1b" "F");
				} else if(value == '\r') {
					Send("\r\nq to quit & save.");
				} else if(value == 'q') {
					_tinScreen = TinScreen::Groups;
					Send("\x1b[H\x1b[2J\r\n***END");
				}
				break;

			case TinScreen::Group:
				if(value == 'q') {
					_tinScreen = TinScreen::Groups;
					Send("\x1b[H\x1b[2J\r\n***END");
				} else if(value == '\r' && number) {
					uint32_t thread = (uint32_t)atoi(digits.c_str());
					if(thread >= 1 && thread <= Content().Forums[_tinForum].Threads.size()) {
						_tinScreen = TinScreen::Article;
						SendArticle(thread - 1);
					}
				}
				break;

			case TinScreen::Article:
				//Back to the threads
				if(value == 'q') {
					_tinScreen = TinScreen::Group;
					SendThreadList();
				}
				break;

			case TinScreen::QuitAsk:
				if(value == '\r' || value == 'y') {
					_stage = Stage::Shell;
					Prompt();
				} else if(value == 'n') {
					_tinScreen = TinScreen::Groups;
					Send("\r\n***END");
				}
				break;
		}
	}

	//电子邮件 drives Unix mail, a line at a time, and reads it by fixed marks: "\n?" (mail's prompt,
	//nothing after it) once mail has started means there is mail - the service's "]:" means none.
	//"p$" prints the last letter, caught to the next prompt and read by its "From ", "From:",
	//"Date:", "To:" and "Subject:" lines and the body after the first blank line; "new" keeps it
	//unread; "-" / "+" print the letter before / after, or start "R" (Referencing ...) when there is
	//none; "q" returns to the service.
	void MailPrompt()
	{
		Send("\r\n?");
	}

	void PrintMail(uint8_t index)
	{
		_mailCurrent = index;
		const YuxingDataMail& mail = Content().Mails[index];
		char text[64];
		snprintf(text, sizeof(text), "Message %u:\r\n", index + 1);
		Send(text);
		const string& from = mail.From;
		Send(("From " + from.substr(0, from.find('@')) + " Sat Oct 31 12:00:00 1998\r\n").c_str());
		Send("Date: Sat, 31 Oct 1998 12:00:00 +0800\r\n");
		Send(("From: " + from + "\r\n").c_str());
		Send("To: user@yuxing.cn.net\r\nSubject: ");
		SendBytes(mail.Subject.data(), (uint32_t)mail.Subject.size());
		Send("\r\n\r\n");
		SendBytes(mail.Body.data(), (uint32_t)mail.Body.size());
		Send("\r\n");
		MailPrompt();
	}

	void StartMail()
	{
		const vector<YuxingDataMail>& mails = Content().Mails;
		uint32_t count = (uint32_t)mails.size();
		if(count == 0) {
			//The program takes the service's prompt for an empty mailbox
			Send("No mail.\r\n");
			Prompt();
			return;
		}
		_stage = Stage::Mail;
		_mailCurrent = 0;
		char text[96];
		snprintf(text, sizeof(text), "Mail [5.2 UCB]  Type ? for help.\r\n\"/var/mail/user\": %u message%s %u new\r\n",
			count, count == 1 ? "" : "s", count);
		Send(text);
		for(uint32_t i = 0; i < count; i++) {
			string from = mails[i].From.substr(0, mails[i].From.find('@'));
			snprintf(text, sizeof(text), "%sN %2u %-14s Sat Oct 31 12:00  20/600   ", i == 0 ? ">" : " ", i + 1, from.c_str());
			Send(text);
			SendBytes(mails[i].Subject.data(), (uint32_t)mails[i].Subject.size());
			Send("\r\n");
		}
		Send("?");
	}

	void RunMail(const string& line)
	{
		if(line == "q" || line == "x") {
			_stage = Stage::Shell;
			Prompt();
		} else if(line == "p$") {
			PrintMail((uint8_t)(Content().Mails.size() - 1));
		} else if(line == "p" || line == "P" || line == ".") {
			PrintMail(_mailCurrent);
		} else if(line == "-") {
			if(_mailCurrent == 0) {
				Send("Referencing before first message");
				MailPrompt();
			} else {
				PrintMail(_mailCurrent - 1);
			}
		} else if(line == "+") {
			if(_mailCurrent + 1u >= Content().Mails.size()) {
				Send("Referencing beyond last message");
				MailPrompt();
			} else {
				PrintMail(_mailCurrent + 1);
			}
		} else {
			MailPrompt();
		}
	}

	void RunCommand(const string& line)
	{
		//电子邮件: "1" (into the mail menu), then "2" reads the mail
		bool mailNext = _mailNext;
		_mailNext = false;
		bool afterDownload = _afterDownload;
		//(leaving more it sends a Ctrl-C first)
		if(!line.empty() && (uint8_t)line[0] >= 0x20) {
			_afterDownload = false;
		}
		if(afterDownload && line == "2") {
			_internetStep = 0;
			Prompt();
			return;
		}
		if(mailNext && line == "2" && _confirmations == 0 && !_pickingDir) {
			_internetStep = 0;
			StartMail();
			return;
		}
		//网友天地: "2", "3"
		if(_internetStep == 1 && line == "3") {
			_internetStep = 0;
			StartTin();
			return;
		}
		//因特网: "2", "1", then the address; the earliest program starts lynx itself, on a home page
		//that is gone with its server
		if(line.compare(0, 5, "lynx ") == 0) {
			//Its own first one is for the service's page; one typed in after leaving lynx is taken as
			//it is
			string url = line.substr(5);
			_internetStep = 2;
			RunCommand(url == "www.bta.net.cn" ? "" : url);
			return;
		}
		if(_internetStep == 2) {
			_internetStep = 0;
			_stage = Stage::Browse;
			_browseInput = BrowseInput::Keys;
			_history.clear();
			_url.clear();
			RequestPage(line.empty() ? HomeAddress : line, false);
			return;
		} else if(_internetStep == 1 && line == "1") {
			_internetStep = 2;
			Prompt();
			return;
		}
		_internetStep = line == "2" ? 1 : 0;

		//A download is "yes", the choices, then the path; 股市传真 answers one choice fewer
		if(!line.empty() && line[0] == '/') {
			_confirmations = 0;
		}
		if(_confirmations > 0) {
			//"2" asks for the file on the screen, as more shows it, instead of a transfer
			_pageNext = _confirmations == 2 && line == "2";
			_confirmations--;
			Prompt();
		} else if(line.empty()) {
			Prompt();
		} else if(_pickingDir) {
			//The directory after "3": '1' to '9', then 'a' for 10 on, as the program counts
			_pickingDir = false;
			char c = line[0];
			if(c >= '1' && c <= '9') {
				_dir = (uint8_t)(c - '0');
			} else if(c >= 'a' && c <= 'z') {
				_dir = (uint8_t)(c - 'a' + 10);
			}
			Prompt();
		} else if(line == "3") {
			_pickingDir = true;
			Prompt();
		} else if(line == "1") {
			SendList();
			_mailNext = true;
		} else if(line == "yes") {
			_confirmations = 2;
			Prompt();
		} else if(line[0] == '/' && _pageNext) {
			_pageNext = false;
			SendPage(line);
		} else if(line[0] == '/') {
			if(!SendPath(line)) {
				Send((line + ": No such file or directory").c_str());
				Prompt();
			}
		} else if(line == "y`.']x" || line.size() == 1) {
			//The service's own command (typed after the login banner) and its other menus
			Prompt();
		} else {
			Send((line + ": Command not found.").c_str());
			Prompt();
		}
	}

	void RunLine(const string& line)
	{
		_wait = AnswerCycles;
		switch(_stage) {
			case Stage::AccessUser:
				Send(_earliest ? "\r\nPassword:~\x08" : "Password:");
				_stage = Stage::AccessPassword;
				break;

			//The earliest program's menu: "T <host>" goes on to the login, and it starts lynx itself
			case Stage::Menu:
				if(line.compare(0, 2, "T ") == 0) {
					Send("\r\nlogin: ");
					_stage = Stage::HostUser;
				} else if(line.compare(0, 3, "LD ") == 0) {
					SendLibraryText(line.substr(3));
				} else if(line.compare(0, 3, "LS ") == 0) {
					_library = line.compare(3, 5, "STOCK") == 0 ? 0 : 16;
					Send("\r\nCommand:~\x08");
				} else if(line == "L") {
					//软件下载 (文件库管理): its reader ($AE4D) waits for "he " and takes the library's name up to
					//a space, then lists it with LCL. The software, directory 1
					_library = 1;
					Send("\r\nYou are now in the YUXING library.\r\n\r\nCommand:~\x08");
				} else if(line == "LLCL" || line == "LCL") {
					SendLibraryList();
				} else if(line == "F") {
					//时空论坛: the forum's name after a ':' and a space, up to 16 bytes, then the prompt ($8B5F;
					//without a name line it waits for its CR for ever)
					Send("\r\nForum: \xD4\xA3\xD0\xCB\xC2\xDB\xCC\xB3\r\n\r\nCommand:~\x08");   //裕兴论坛
				} else if(line == "RF") {
					//读消息: its reader ($8545) takes a reply starting "Sorry" as no new messages
					Send("\r\nSorry, there are no new messages.\r\n\r\nCommand:~\x08");
				} else if(line == "FLFBF") {
					//线索: its reader ($A97E) looks for '#' entries, or "End" for none
					Send("\r\nEnd of list.\r\n\r\nCommand:~\x08");
				} else if(line == "S?") {
					SendForumList();
				} else {
					Send("\r\nCommand:~\x08");
				}
				break;

			case Stage::AccessPassword:
				if(_earliest) {
					Send("\r\nMain Menu (or Exit):~\x08");
					_stage = Stage::Menu;
					break;
				}
				Send("\r\n\r\nConnected to yxmail.yuxing.cn.net\r\n\r\nSunOS 5.5.1\r\n\r\nlogin: ");
				_stage = Stage::HostUser;
				break;

			case Stage::HostUser:
				Send("Password:");
				_stage = Stage::HostPassword;
				break;

			case Stage::HostPassword:
				//The earliest program goes on at the first colon, and what is left of a banner
				//would be taken for the start of its first page
				if(_earliest) {
					Send("\r\nyxmail:");
					_stage = Stage::Shell;
					break;
				}
				Send("\r\nLast login: Fri Oct 30 21:14:07 from bta.net.cn\r\n");
				Send("Sun Microsystems Inc.   SunOS 5.5.1       Generic May 1996\r\n\r\n");
				Send("        Welcome to YuXing Network Service!\r\n\r\n");
				Send("yxmail% ");
				_stage = Stage::Shell;
				break;

			case Stage::Shell:
				RunCommand(line);
				break;

			case Stage::Mail:
				RunMail(line);
				break;

			default:
				break;
		}
	}

	bool IsZmodem()
	{
		return _stage >= Stage::ZmodemInit && _stage <= Stage::ZmodemFinish;
	}

public:
	//earliest: the first online program, which reached the service through a menu system whose
	//prompts all end in "~" and a backspace, and which it tells apart by their last characters
	void Connect(bool earliest, bool overPort)
	{
		Reset();
		//The UI answers this one before it returns, with the data file's texts
		if(_emu) {
			YuxingWebRequestParams request = { 0, "yuxing:data" };
			_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::YuxingWebRequest, &request);
		}
		_content = YuxingServerData::Get();
		_earliest = earliest;
		_overPort = overPort;
		_stage = Stage::AccessUser;
		_wait = AnswerCycles;
		if(earliest) {
			Send("\r\n\r\nUser-ID:~\x08");
		} else {
			//The earlier online programs count two colons up to the first prompt
			Send("\r\n\r\nCHINANET: User Access Verification\r\n\r\nUsername: ");
		}
	}

	void Reset()
	{
		_stage = Stage::Offline;
		_outHead = _outCount = 0;
		_lineLength = 0;
		_wait = 0;
		_dir = 16;
		_pickingDir = false;
		_confirmations = 0;
		_sendingData = false;
		_afterDownload = false;
		_pageNext = false;
		_homeDelay = 0;
		_library = 16;
		_internetStep = 0;
		_tinScreen = TinScreen::Groups;
		_tinNumber.clear();
		_mailNext = false;
		_mailCurrent = 0;
		_content.reset();
		_page.clear();
		_history.clear();
		_address.clear();
		_browseInput = BrowseInput::Keys;
		_fetching = false;
		_top = 0;
		_link = -1;
		ReadPage();
	}

	//Where to send the browser's page requests
	void SetEmulator(Emulator* emu) { _emu = emu; }

	bool IsConnected() { return _stage != Stage::Offline; }

	void Clock()
	{
		if(_wait) {
			_wait--;
		}
		if(_homeDelay && _outCount == 0 && --_homeDelay == 0) {
			RequestPage(HomeAddress, true);
		}
		if(_fetching) {
			if(!_requestSent) {
				SendRequest();
			}
			vector<uint8_t> page;
			if(YuxingWebBridge::Take(_fetchId, page)) {
				PageArrived(page);
			} else if(++_fetchCycles >= FetchTimeoutCycles) {
				string text = "T(no answer)\nPThe page did not come.\n";
				page.assign(text.begin(), text.end());
				PageArrived(page);
			}
		}
	}

	bool HasOutput() { return _outCount > 0 && _wait == 0; }

	uint8_t TakeOutput()
	{
		uint8_t value = _out[_outHead];
		_outHead = (_outHead + 1) % OutSize;
		_outCount--;
		if(_sendingData) {
			QueueData();
		}
		return value;
	}

	//A byte typed by the machine
	void Receive(uint8_t value)
	{
		if(_stage == Stage::Offline) {
			return;
		}
		if(_pageMore) {
			_pageMore = false;
			if(value == ' ') {
				_wait = AnswerCycles;
				Send("\x1B[J");
				return;
			}
		}
		if(IsZmodem()) {
			ReceiveZmodem(value);
			return;
		}
		if(_stage == Stage::Browse) {
			ReceiveBrowse(value);
			return;
		}
		if(_stage == Stage::Tin) {
			ReceiveTin(value);
			return;
		}

		if(value == '\r') {
			if(Echoes()) {
				Send("\r\n");
			}
			_line[_lineLength] = 0;
			string line((const char*)_line, _lineLength);
			_lineLength = 0;
			RunLine(line);
		} else if(value == 8 || value == 0x7F) {
			if(_lineLength > 0) {
				_lineLength--;
				if(Echoes()) {
					Send("\b \b");
				}
			}
		} else if(value >= 0x20 && _lineLength < MaxLine) {
			_line[_lineLength++] = value;
			if(Echoes()) {
				SendByte(value);
			}
		}
	}

	void Serialize(Serializer& s) override
	{
		SV(_stage);
		SVArray(_out, OutSize); SV(_outHead); SV(_outCount);
		SVArray(_line, MaxLine + 1); SV(_lineLength);
		SV(_wait);
		SV(_dir); SV(_pickingDir); SV(_confirmations); SV(_afterDownload); SV(_pageNext); SV(_pageMore); SV(_earliest); SV(_overPort); SV(_homeDelay); SV(_library);
		SV(_sendSection); SV(_sendFile); SV(_sendPos); SV(_sendingData);
		SV(_internetStep); SV(_tinScreen); SVVector(_tinNumber); SV(_tinForum);
		SV(_mailNext); SV(_mailCurrent);
		SVVector(_page); SVVector(_history); SV(_top); SV(_link);
		SV(_browseInput); SVVector(_address);
		SV(_fetching); SV(_fetchPushes); SV(_fetchId); SV(_fetchCycles); SVVector(_fetchUrl);
		if(!s.IsSaving()) {
			ReadPage();
			_requestSent = false;
		}
	}
};
