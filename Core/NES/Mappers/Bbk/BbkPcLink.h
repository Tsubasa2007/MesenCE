#pragma once
#include "pch.h"
#include <deque>
#include <map>
#include <functional>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <chrono>
#include "Shared/MessageManager.h"

//A PC on the other end of the BBK's parallel port, serving a host folder as a remote drive.
//
//The BBK side is PCLINK.CMD ("Parallel Port Disk Simulation Management Driver", Tang Ruichun,
//Tsinghua University, 1997), a BBGDOS driver that makes a drive letter of a disk on a PC. The PC
//side was PCSVER.EXE ("PC Remote Disk Simulation Server (for FXLINK Cable)"), a 16-bit DOS program
//that no current Windows runs. This is that server, rebuilt from both programs' code.
//
//The cable crosses the two ports' data lines over to the other side's status lines, LapLink
//style, so a byte goes over as two nibbles with a handshake each way. The BBK writes $FF40 with
//its nibble in bits 0-3 and its handshake in bit 4, and reads the PC's nibble in bits 3-6 of
//$FF48 and the PC's handshake, inverted, in bit 7.
//
//What goes over the wire is the BBK's own file-system requests. The BBK sends $01 and a 16-byte
//request: a command (1-24), the memory bank its buffers are in, two words, a 32-bit file
//position, a status byte and a handle. The PC then drives the BBK as a memory server - $08 n
//selects the bank the next transfer goes through, $0B len addr data writes the BBK's memory,
//$15 len addr reads it back (lengths and addresses high byte first) - and ends with $00 and the
//first 14 bytes of the request, updated. The commands, from PCSVER's dispatch table:
//  1 open (8.3 name)   2 read        3 write      4 seek       5 close        6 find first (8.3)
//  7 find next          8 create      10 delete    11 free space 12 set attributes and time
//  13 rename an open file              16 chdir     17 mkdir     18 rmdir       19 current directory
//  20 open (path)       21 create (path)            22 find first (path)       23 delete (path)
//  24 set attributes (path)
//The status codes are PCSVER's own, except where it passes on a DOS error.
class BbkPcLink
{
private:
	using Path = std::filesystem::path;

	//The wire
	bool _bbkStrobe = false;
	bool _pcStrobe = false;
	uint8_t _pcNibble = 0;
	uint8_t _rxLow = 0;
	std::deque<uint8_t> _tx;

	//The protocol
	bool _inRequest = false;
	vector<uint8_t> _req;
	size_t _rxWanted = 0;
	vector<uint8_t> _rxData;
	std::function<void(vector<uint8_t>&)> _onRx;

	//The drive
	Path _root;
	vector<string> _cwd;   //host names of the directories below the root
	struct OpenFile { std::fstream Stream; Path HostPath; };
	std::map<uint16_t, OpenFile> _files;
	uint16_t _nextHandle = 5;
	struct Entry { uint8_t Attr; uint16_t Time; uint16_t Date; uint32_t Size; string Name83; };
	struct Search { vector<Entry> Entries; size_t Next = 0; };
	std::map<uint16_t, Search> _searches;
	uint16_t _nextSearch = 0x100;

	enum Status : uint8_t { Ok = 0, BadCommand = 1, Failed = 2, NoPath = 3, Denied = 5, BadDrive = 0x0F, WriteFault = 0x1D, ReadFault = 0x1E, NoMoreFiles = 0x12 };

	uint16_t Word(int at) { return (uint16_t)(_req[at] | (_req[at + 1] << 8)); }
	void SetWord(int at, uint16_t v) { _req[at] = v & 0xFF; _req[at + 1] = v >> 8; }
	uint32_t Position() { return _req[7] | (_req[8] << 8) | (_req[9] << 16) | ((uint32_t)_req[10] << 24); }
	void SetPosition(uint32_t v) { for(int i = 0; i < 4; i++) { _req[7 + i] = (v >> (8 * i)) & 0xFF; } }
	void SetStatus(uint8_t s) { _req[11] = s; }

	void Send(uint8_t b) { _tx.push_back(b); }
	void Send16(uint16_t w) { Send(w >> 8); Send(w & 0xFF); }
	void Bank(uint8_t bank) { Send(0x08); Send(bank); }

	void WriteMemory(uint16_t addr, const uint8_t* data, size_t len)
	{
		if(len == 0) {
			return;
		}
		Send(0x0B);
		Send16((uint16_t)len);
		Send16(addr);
		for(size_t i = 0; i < len; i++) {
			Send(data[i]);
		}
	}

	void ReadMemory(uint16_t addr, uint16_t len, std::function<void(vector<uint8_t>&)> then)
	{
		Send(0x15);
		Send16(len);
		Send16(addr);
		_rxWanted = len;
		_rxData.clear();
		_onRx = std::move(then);
	}

	void Reply()
	{
		Send(0x00);
		for(int i = 0; i < 14; i++) {
			Send(_req[i]);
		}
	}

	static string Upper(string s)
	{
		for(char& c : s) {
			c = (char)toupper((unsigned char)c);
		}
		return s;
	}

	//"NAME    EXT" -> "NAME.EXT", the way PCSVER builds the name it opens
	string NameFrom83(int at)
	{
		string name;
		for(int i = 0; i < 8 && _req[at + i] != ' '; i++) { name += (char)_req[at + i]; }
		string ext;
		for(int i = 8; i < 11 && _req[at + i] != ' '; i++) { ext += (char)_req[at + i]; }
		return ext.empty() ? name : name + "." + ext;
	}

	//The 8.3 form of a host name, or "" when it has none
	static string To83(const string& hostName)
	{
		if(hostName == "." || hostName == "..") {
			string n = hostName;
			n.resize(11, ' ');
			return n;
		}
		string up = Upper(hostName);
		size_t dot = up.find('.');
		string base = up.substr(0, dot);
		string ext = dot == string::npos ? "" : up.substr(dot + 1);
		if(base.empty() || base.size() > 8 || ext.size() > 3 || ext.find('.') != string::npos) {
			return "";
		}
		for(char c : base + ext) {
			if((unsigned char)c < 0x21 || strchr("\"*+,/:;<=>?[\\]|", c)) {
				return "";
			}
		}
		base.resize(8, ' ');
		ext.resize(3, ' ');
		return base + ext;
	}

	//A DOS wildcard name ("*.TXT", "A?C.*") as an 11-character pattern
	static string Pattern83(const string& name)
	{
		string up = Upper(name);
		size_t dot = up.find('.');
		string parts[2] = { up.substr(0, dot), dot == string::npos ? "" : up.substr(dot + 1) };
		size_t sizes[2] = { 8, 3 };
		string out;
		for(int p = 0; p < 2; p++) {
			string f;
			for(char c : parts[p]) {
				if(c == '*') {
					while(f.size() < sizes[p]) { f += '?'; }
					break;
				}
				f += c;
			}
			f.resize(sizes[p], ' ');
			out += f.substr(0, sizes[p]);
		}
		return out;
	}

	static bool Match83(const string& pattern, const string& name83)
	{
		for(int i = 0; i < 11; i++) {
			if(pattern[i] != '?' && pattern[i] != name83[i]) {
				return false;
			}
		}
		return true;
	}

	Path Directory(const vector<string>& parts)
	{
		Path p = _root;
		for(const string& d : parts) {
			p /= std::filesystem::u8path(d);
		}
		return p;
	}

	//The host entry in a directory whose name is this one, ignoring case - or, for a new one, the
	//name in capitals, as DOS makes it
	string FindHostName(const Path& dir, const string& name)
	{
		std::error_code ec;
		string want = Upper(name);
		for(auto& e : std::filesystem::directory_iterator(dir, ec)) {
			string n = e.path().filename().u8string();
			if(Upper(n) == want || To83(n) == To83(name)) {
				return n;
			}
		}
		return Upper(name);
	}

	//A BBK path ("C:\DIR\FILE", "..\X", "FILE") as host directory parts plus a last component
	bool Split(string bbkPath, vector<string>& dirs, string& last)
	{
		if(bbkPath.size() >= 2 && bbkPath[1] == ':') {
			bbkPath = bbkPath.substr(2);
		}
		dirs = (!bbkPath.empty() && (bbkPath[0] == '\\' || bbkPath[0] == '/')) ? vector<string>() : _cwd;
		vector<string> parts;
		string cur;
		for(char c : bbkPath) {
			if(c == '\\' || c == '/') {
				if(!cur.empty()) { parts.push_back(cur); }
				cur.clear();
			} else {
				cur += c;
			}
		}
		last = cur;
		for(const string& p : parts) {
			if(p == ".") {
				continue;
			} else if(p == "..") {
				if(!dirs.empty()) { dirs.pop_back(); }
			} else {
				dirs.push_back(FindHostName(Directory(dirs), p));
			}
		}
		return true;
	}

	Path Resolve(const string& bbkPath)
	{
		vector<string> dirs;
		string last;
		Split(bbkPath, dirs, last);
		if(last == "..") {
			if(!dirs.empty()) { dirs.pop_back(); }
			return Directory(dirs);
		}
		if(last.empty() || last == ".") {
			return Directory(dirs);
		}
		Path dir = Directory(dirs);
		return dir / std::filesystem::u8path(FindHostName(dir, last));
	}

	static uint8_t Attributes(const std::filesystem::directory_entry& e)
	{
		std::error_code ec;
		uint8_t attr = e.is_directory(ec) ? 0x10 : 0x20;
		auto perms = e.status(ec).permissions();
		if((perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
			attr |= 0x01;
		}
		return attr;
	}

	static void DosTime(const Path& p, uint16_t& time, uint16_t& date)
	{
		std::error_code ec;
		auto ft = std::filesystem::last_write_time(p, ec);
		time = 0;
		date = (1 << 5) | 1;
		if(ec) {
			return;
		}
		//The file clock and the system clock differ by an offset (C++17 has no clock_cast)
		auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - std::filesystem::file_time_type::clock::now());
		std::time_t t = std::chrono::system_clock::to_time_t(sys);
		std::tm tm = {};
#ifdef _WIN32
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		int year = std::max(0, tm.tm_year + 1900 - 1980);
		time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
		date = (uint16_t)((year << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
	}

	static void SetDosTime(const Path& p, uint16_t time, uint16_t date)
	{
		std::tm tm = {};
		tm.tm_year = ((date >> 9) & 0x7F) + 80;
		tm.tm_mon = ((date >> 5) & 0x0F) - 1;
		tm.tm_mday = date & 0x1F;
		tm.tm_hour = (time >> 11) & 0x1F;
		tm.tm_min = (time >> 5) & 0x3F;
		tm.tm_sec = (time & 0x1F) * 2;
		tm.tm_isdst = -1;
		std::time_t t = std::mktime(&tm);
		if(t == (std::time_t)-1) {
			return;
		}
		std::error_code ec;
		auto sys = std::chrono::system_clock::from_time_t(t);
		auto ft = std::filesystem::file_time_type::clock::now() + std::chrono::duration_cast<std::filesystem::file_time_type::duration>(sys - std::chrono::system_clock::now());
		std::filesystem::last_write_time(p, ft, ec);
	}

	static void SetReadOnly(const Path& p, bool readOnly)
	{
		std::error_code ec;
		std::filesystem::permissions(p, std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write,
			readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add, ec);
	}

	//Opens an existing file read/write, else read-only, as PCSVER does
	bool OpenExisting(const Path& p)
	{
		std::error_code ec;
		if(!std::filesystem::is_regular_file(p, ec)) {
			return false;
		}
		OpenFile f;
		f.HostPath = p;
		f.Stream.open(p, std::ios::in | std::ios::out | std::ios::binary);
		if(!f.Stream.is_open()) {
			f.Stream.open(p, std::ios::in | std::ios::binary);
		}
		if(!f.Stream.is_open()) {
			return false;
		}
		uint16_t h = NewHandle();
		_files[h] = std::move(f);
		SetWord(12, h);

		uint16_t time, date;
		DosTime(p, time, date);
		SetWord(3, date);
		SetWord(5, time);
		std::filesystem::directory_entry e(p, ec);
		_req[2] = Attributes(e);
		SetPosition(0);
		return true;
	}

	bool Create(const Path& p)
	{
		OpenFile f;
		f.HostPath = p;
		f.Stream.open(p, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		if(!f.Stream.is_open()) {
			return false;
		}
		uint16_t h = NewHandle();
		_files[h] = std::move(f);
		SetWord(12, h);
		SetPosition(0);
		return true;
	}

	uint16_t NewHandle()
	{
		while(_files.count(_nextHandle)) {
			_nextHandle = _nextHandle >= 0x7FFF ? 5 : _nextHandle + 1;
		}
		return _nextHandle++;
	}

	OpenFile* File(uint16_t h)
	{
		auto it = _files.find(h);
		return it == _files.end() ? nullptr : &it->second;
	}

	vector<Entry> List(const Path& dir, const string& pattern, uint8_t attr, bool atRoot)
	{
		vector<Entry> out;
		//A search with the volume label bit finds only the label, as on DOS - DIR asks for it first
		if(attr & 0x08) {
			Entry label = { 0x08, 0, 0, 0, "PCLINK     " };
			DosTime(dir, label.Time, label.Date);
			if(atRoot && Match83(pattern, label.Name83)) {
				out.push_back(label);
			}
			return out;
		}
		auto add = [&](const string& name83, uint8_t a, const Path& p, uint32_t size) {
			if(name83.empty() || !Match83(pattern, name83)) {
				return;
			}
			if((a & 0x10) && !(attr & 0x10)) {
				return;
			}
			Entry e;
			e.Attr = a;
			DosTime(p, e.Time, e.Date);
			e.Size = size;
			e.Name83 = name83;
			out.push_back(e);
		};
		if(!atRoot) {
			add(To83("."), 0x10, dir, 0);
			add(To83(".."), 0x10, dir.parent_path(), 0);
		}
		std::error_code ec;
		vector<std::filesystem::directory_entry> entries;
		for(auto& e : std::filesystem::directory_iterator(dir, ec)) {
			entries.push_back(e);
		}
		std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
		for(auto& e : entries) {
			std::error_code ec2;
			uint32_t size = e.is_regular_file(ec2) ? (uint32_t)std::min<uintmax_t>(e.file_size(ec2), 0xFFFFFFFF) : 0;
			add(To83(e.path().filename().u8string()), Attributes(e), e.path(), size);
		}
		return out;
	}

	//PCSVER's "found" step: the entry goes to the BBK's buffer as attribute, time, date, size and
	//the 8.3 name, 20 bytes; nothing found is the DOS error, "no more files"
	void SendEntry(uint16_t searchId)
	{
		auto it = _searches.find(searchId);
		if(it == _searches.end() || it->second.Next >= it->second.Entries.size()) {
			SetStatus(NoMoreFiles);
			if(it != _searches.end()) {
				_searches.erase(it);
			}
			return;
		}
		Entry& e = it->second.Entries[it->second.Next++];
		uint8_t rec[20];
		rec[0] = e.Attr;
		rec[1] = e.Time & 0xFF; rec[2] = e.Time >> 8;
		rec[3] = e.Date & 0xFF; rec[4] = e.Date >> 8;
		for(int i = 0; i < 4; i++) { rec[5 + i] = (e.Size >> (8 * i)) & 0xFF; }
		memcpy(rec + 9, e.Name83.data(), 11);
		Bank(_req[2]);
		WriteMemory(Word(3), rec, 20);
		SetStatus(Ok);
	}

	uint16_t StartSearch(const Path& dir, const string& pattern, uint8_t attr, bool atRoot)
	{
		uint16_t id = _nextSearch++;
		if(_nextSearch == 0) { _nextSearch = 0x100; }
		_searches[id] = { List(dir, pattern, attr, atRoot), 0 };
		//An abandoned search holds a directory listing; keep only the latest few
		while(_searches.size() > 16) {
			_searches.erase(_searches.begin());
		}
		return id;
	}

	string CurrentDirectory()
	{
		string s = "\\";
		for(size_t i = 0; i < _cwd.size(); i++) {
			s += Upper(_cwd[i]);
			if(i + 1 < _cwd.size()) { s += "\\"; }
		}
		return s;
	}

	void SendCurrentDirectory()
	{
		string cwd = CurrentDirectory();
		WriteMemory(0x0140, (const uint8_t*)cwd.c_str(), cwd.size() + 1);
	}

	static string CString(vector<uint8_t>& data)
	{
		string s;
		for(uint8_t c : data) {
			if(c == 0) { break; }
			s += (char)c;
		}
		return s;
	}

	//PCSVER fetches a path from the BBK's memory in an 80-byte block; addresses from $C000 up are
	//read through bank $FF, moved down by the window's offset
	void FetchPath(uint16_t addr, uint16_t highOffset, std::function<void(const string&)> then)
	{
		if(addr < 0xC000) {
			Bank(_req[2]);
		} else {
			Bank(0xFF);
			addr = (uint16_t)(addr - highOffset);
		}
		ReadMemory(addr, 80, [this, then](vector<uint8_t>& d) { then(CString(d)); });
	}

	void Handle()
	{
		uint8_t cmd = _req[0];
		switch(cmd) {
			case 1: SetStatus(OpenExisting(Resolve(NameFrom83(5))) ? Ok : Failed); if(_req[11] == Ok) { SetWord(9, 0); } Reply(); break;
			case 2: Read(); break;
			case 3: Write(); break;
			case 4: Seek(); Reply(); break;
			case 5: _files.erase(Word(12)); SetStatus(Ok); Reply(); break;
			case 6: {
				uint16_t id = StartSearch(Directory(_cwd), Pattern83(NameFrom83(5)), 0x1E, _cwd.empty());
				SendEntry(id);
				SetWord(7, id);
				Reply();
				break;
			}
			case 7: SendEntry(Word(7)); Reply(); break;
			case 8: SetStatus(Create(Resolve(NameFrom83(5))) ? Ok : Failed); Reply(); break;
			case 10: { std::error_code ec; SetStatus(std::filesystem::remove(Resolve(NameFrom83(5)), ec) ? Ok : Failed); Reply(); break; }
			case 11: FreeSpace(); Reply(); break;
			case 12: SetFileInfo(); Reply(); break;
			case 13: RenameOpen(); Reply(); break;
			case 16: case 17: case 18:
				Bank(_req[2]);
				ReadMemory(Word(3), 80, [this, cmd](vector<uint8_t>& d) { Directories(cmd, CString(d)); Reply(); });
				break;
			case 19: SetWord(3, 0x0140); SendCurrentDirectory(); SetStatus(Ok); Reply(); break;
			case 20:
				FetchPath(Word(3), 0x4000, [this](const string& p) { SetStatus(OpenExisting(Resolve(p)) ? Ok : Failed); Reply(); });
				break;
			case 21:
				FetchPath(Word(3), 0x4000, [this](const string& p) { SetStatus(Create(Resolve(p)) ? Ok : Denied); Reply(); });
				break;
			case 22:
				FetchPath(Word(5), 0x4000, [this](const string& p) { FindFirstPath(p); Reply(); });
				break;
			case 23:
				FetchPath(Word(3), 0x4000, [this](const string& p) { std::error_code ec; SetStatus(std::filesystem::remove(Resolve(p), ec) ? Ok : Failed); Reply(); });
				break;
			case 24:
				FetchPath(Word(3), 0x4000, [this](const string& p) {
					Path hp = Resolve(p);
					std::error_code ec;
					if(std::filesystem::exists(hp, ec)) {
						SetReadOnly(hp, (_req[5] & 0x01) != 0);
						SetStatus(Ok);
					} else {
						SetStatus(Failed);
					}
					Reply();
				});
				break;
			default: SetStatus(BadCommand); Reply(); break;
		}
	}

	void Read()
	{
		OpenFile* f = File(Word(12));
		uint32_t pos = Position();
		if(!f) {
			SetStatus(ReadFault);
			Reply();
			return;
		}
		f->Stream.clear();
		f->Stream.seekg(pos);
		uint16_t want = Word(5);
		SetWord(5, 0);
		Bank(_req[2]);
		uint16_t addr = Word(3);
		vector<uint8_t> buf(0x4000);
		uint16_t done = 0;
		while(done < want) {
			size_t chunk = std::min<size_t>(0x4000, want - done);
			f->Stream.read((char*)buf.data(), chunk);
			size_t got = (size_t)f->Stream.gcount();
			if(got == 0) {
				break;
			}
			WriteMemory(addr, buf.data(), got);
			addr += (uint16_t)got;
			done += (uint16_t)got;
			pos += (uint32_t)got;
		}
		SetWord(5, done);
		SetWord(3, addr);
		SetPosition(pos);
		SetStatus(Ok);
		Reply();
	}

	void Write()
	{
		OpenFile* f = File(Word(12));
		if(!f) {
			SetStatus(WriteFault);
			Reply();
			return;
		}
		uint32_t pos = Position();
		uint16_t count = Word(5);
		if(count == 0) {
			//DOS truncates a file written with no bytes at its current position
			f->Stream.close();
			std::error_code ec;
			std::filesystem::resize_file(f->HostPath, pos, ec);
			f->Stream.open(f->HostPath, std::ios::in | std::ios::out | std::ios::binary);
			SetStatus(ec ? WriteFault : Ok);
			Reply();
			return;
		}
		Bank(_req[2]);
		uint16_t handle = Word(12);
		ReadMemory(Word(3), count, [this, handle, pos](vector<uint8_t>& d) {
			OpenFile* f = File(handle);
			if(!f) {
				SetStatus(WriteFault);
				Reply();
				return;
			}
			f->Stream.clear();
			f->Stream.seekp(pos);
			f->Stream.write((const char*)d.data(), d.size());
			f->Stream.flush();
			if(!f->Stream) {
				SetStatus(WriteFault);
			} else {
				SetWord(3, (uint16_t)(Word(3) + d.size()));
				SetPosition(pos + (uint32_t)d.size());
				SetStatus(Ok);
			}
			Reply();
		});
	}

	void Seek()
	{
		OpenFile* f = File(Word(12));
		if(!f) {
			SetStatus(ReadFault);
			return;
		}
		std::error_code ec;
		uint32_t size = (uint32_t)std::min<uintmax_t>(std::filesystem::file_size(f->HostPath, ec), 0xFFFFFFFF);
		if(size <= Position()) {
			SetPosition(size);
		}
		SetStatus(Ok);
	}

	void FreeSpace()
	{
		std::error_code ec;
		auto sp = std::filesystem::space(_root, ec);
		if(ec) {
			SetStatus(BadDrive);
			return;
		}
		//PCSVER passes on what _dos_getdiskfree gives: sectors per cluster, bytes per sector, then free
		//and total clusters. 4KB clusters, at most 65535 of them: 256MB, about a PC disk of the time.
		uint32_t cluster = 512 * 8;
		SetWord(3, 8);
		SetWord(5, 512);
		SetWord(7, (uint16_t)std::min<uintmax_t>(sp.available / cluster, 0xFFFF));
		SetWord(9, (uint16_t)std::min<uintmax_t>(sp.capacity / cluster, 0xFFFF));
		SetStatus(Ok);
	}

	void SetFileInfo()
	{
		OpenFile* f = File(Word(12));
		if(!f) {
			SetStatus(Failed);
			return;
		}
		f->Stream.close();
		SetReadOnly(f->HostPath, (_req[2] & 0x01) != 0);
		SetDosTime(f->HostPath, Word(5), Word(3));
		f->Stream.open(f->HostPath, std::ios::in | std::ios::out | std::ios::binary);
		if(!f->Stream.is_open()) {
			f->Stream.open(f->HostPath, std::ios::in | std::ios::binary);
		}
		SetStatus(f->Stream.is_open() ? Ok : Failed);
	}

	void RenameOpen()
	{
		uint16_t h = Word(3);
		OpenFile* f = File(h);
		if(!f) {
			SetStatus(Failed);
			return;
		}
		Path from = f->HostPath;
		_files.erase(h);
		Path to = from.parent_path() / std::filesystem::u8path(NameFrom83(5));
		std::error_code ec;
		std::filesystem::rename(from, to, ec);
		if(ec) {
			SetStatus(Failed);
			return;
		}
		SetStatus(OpenExisting(to) ? Ok : Failed);
	}

	void Directories(uint8_t cmd, const string& bbkPath)
	{
		std::error_code ec;
		if(cmd == 16) {
			vector<string> dirs;
			string last;
			Split(bbkPath, dirs, last);
			if(last == "..") {
				if(!dirs.empty()) { dirs.pop_back(); }
			} else if(!last.empty() && last != ".") {
				dirs.push_back(FindHostName(Directory(dirs), last));
			}
			if(std::filesystem::is_directory(Directory(dirs), ec)) {
				_cwd = dirs;
				SetStatus(Ok);
			} else {
				SetStatus(NoPath);
			}
		} else if(cmd == 17) {
			Path p = Resolve(bbkPath);
			if(std::filesystem::exists(p, ec)) {
				SetStatus(Denied);
			} else {
				SetStatus(std::filesystem::create_directory(p, ec) ? Ok : NoPath);
			}
		} else {
			Path p = Resolve(bbkPath);
			if(!std::filesystem::is_directory(p, ec)) {
				SetStatus(NoPath);
			} else {
				SetStatus(std::filesystem::remove(p, ec) ? Ok : Denied);
			}
		}
	}

	void FindFirstPath(const string& bbkPath)
	{
		vector<string> dirs;
		string last;
		Split(bbkPath, dirs, last);
		uint16_t id = StartSearch(Directory(dirs), Pattern83(last.empty() ? "*.*" : last), _req[7], dirs.empty());
		SendEntry(id);
		//Then, as PCSVER does, the current directory at $0140 and its address and a directory
		//cluster after the entry
		SendCurrentDirectory();
		uint8_t at[2] = { 0x40, 0x01 };
		WriteMemory((uint16_t)(Word(3) + 0x15), at, 2);
		uint8_t cluster[2] = { 0, 0 };
		WriteMemory((uint16_t)(Word(3) + 0x17), cluster, 2);
		SetWord(7, id);
	}

	void Receive(uint8_t b)
	{
		if(_rxWanted) {
			_rxData.push_back(b);
			if(_rxData.size() >= _rxWanted) {
				_rxWanted = 0;
				auto then = std::move(_onRx);
				vector<uint8_t> data = std::move(_rxData);
				_rxData.clear();
				_onRx = nullptr;
				then(data);
			}
			return;
		}
		if(!_inRequest) {
			if(b == 0x01) {
				_inRequest = true;
				_req.clear();
			}
			return;
		}
		_req.push_back(b);
		if(_req.size() == 16) {
			_inRequest = false;
			Handle();
		}
	}

public:
	void SetRoot(const string& folder)
	{
		Path root = std::filesystem::u8path(folder);
		if(root == _root) {
			return;
		}
		std::error_code ec;
		std::filesystem::create_directories(root, ec);
		_root = root;
		_cwd.clear();
		_files.clear();
		_searches.clear();
		MessageManager::Log("[BBK] PC link serving " + folder);
	}

	void Reset()
	{
		_bbkStrobe = _pcStrobe = false;
		_pcNibble = 0;
		_tx.clear();
		_inRequest = false;
		_req.clear();
		_rxWanted = 0;
		_rxData.clear();
		_onRx = nullptr;
		_cwd.clear();
		_files.clear();
		_searches.clear();
	}

	//$FF40: the BBK's nibble in bits 0-3, its handshake in bit 4. The PC acts on each handshake
	//edge: answering with the nibble it is sending, or taking the one it is being sent.
	void WriteData(uint8_t value)
	{
		bool strobe = (value & 0x10) != 0;
		if(strobe == _bbkStrobe) {
			return;
		}
		_bbkStrobe = strobe;
		if(!_tx.empty()) {
			if(strobe) {
				_pcNibble = _tx.front() & 0x0F;
				_pcStrobe = true;
			} else {
				_pcNibble = _tx.front() >> 4;
				_pcStrobe = false;
				_tx.pop_front();
			}
		} else if(strobe) {
			_rxLow = value & 0x0F;
			_pcStrobe = true;
		} else {
			_pcStrobe = false;
			Receive((uint8_t)(_rxLow | ((value & 0x0F) << 4)));
		}
	}

	//$FF48: the PC's nibble in bits 3-6, its handshake inverted in bit 7
	uint8_t ReadStatus()
	{
		return (uint8_t)((_pcStrobe ? 0x00 : 0x80) | ((_pcNibble & 0x0F) << 3));
	}
};
