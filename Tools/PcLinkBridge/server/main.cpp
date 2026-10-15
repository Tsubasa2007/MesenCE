//PcLinkServer: serves a folder on this PC as a drive to a BBK running PCLINK.CMD - what PCSVER.EXE did
//on a PC's printer port. The wire is PcLinkBridge (a microcontroller on the PC link cable,
//../firmware) on USB serial, or a real parallel port through the InpOut driver.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <cstdio>
#include <filesystem>
#include "serve.h"
#include "lpt.h"

class ComPort : public Port
{
private:
	HANDLE _h = INVALID_HANDLE_VALUE;
	uint8_t _buf[4096];
	DWORD _len = 0;
	DWORD _pos = 0;
	int _timeout = -1;

	[[noreturn]] static void Fail() { throw std::runtime_error("the serial port stopped working (the bridge unplugged?)"); }

public:
	~ComPort()
	{
		if(_h != INVALID_HANDLE_VALUE) {
			CloseHandle(_h);
		}
	}

	bool Open(const string& name, DWORD baud)
	{
		string path = "\\\\.\\" + name;
		_h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if(_h == INVALID_HANDLE_VALUE) {
			return false;
		}
		SetupComm(_h, 1 << 16, 1 << 16);
		DCB dcb = {};
		dcb.DCBlength = sizeof(dcb);
		if(!GetCommState(_h, &dcb)) {
			return false;
		}
		dcb.BaudRate = baud;
		dcb.ByteSize = 8;
		dcb.Parity = NOPARITY;
		dcb.StopBits = ONESTOPBIT;
		dcb.fBinary = TRUE;
		dcb.fParity = FALSE;
		dcb.fOutxCtsFlow = FALSE;
		dcb.fOutxDsrFlow = FALSE;
		dcb.fDsrSensitivity = FALSE;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;   //(an Uno or a Nano restarts as the port opens)
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		dcb.fOutX = FALSE;
		dcb.fInX = FALSE;
		dcb.fErrorChar = FALSE;
		dcb.fNull = FALSE;
		dcb.fAbortOnError = FALSE;
		if(!SetCommState(_h, &dcb)) {
			return false;
		}
		PurgeComm(_h, PURGE_RXCLEAR | PURGE_TXCLEAR);
		return true;
	}

	void Write(const uint8_t* data, size_t len) override
	{
		while(len) {
			DWORD written = 0;
			if(!WriteFile(_h, data, (DWORD)len, &written, nullptr) || written == 0) {
				Fail();
			}
			data += written;
			len -= written;
		}
	}

	int Read(int timeoutMs) override
	{
		if(_pos < _len) {
			return _buf[_pos++];
		}
		if(timeoutMs != _timeout) {
			//Back as soon as anything has come, or after the timeout
			COMMTIMEOUTS t = {};
			t.ReadIntervalTimeout = MAXDWORD;
			t.ReadTotalTimeoutMultiplier = MAXDWORD;
			t.ReadTotalTimeoutConstant = timeoutMs > 0 ? (DWORD)timeoutMs : 1;
			t.WriteTotalTimeoutConstant = 2000;
			SetCommTimeouts(_h, &t);
			_timeout = timeoutMs;
		}
		_pos = 0;
		_len = 0;
		if(!ReadFile(_h, _buf, sizeof(_buf), &_len, nullptr)) {
			Fail();
		}
		return _len ? _buf[_pos++] : -1;
	}
};

//The ports through InpOut (inpoutx64.dll in a 64-bit build, inpout32.dll in a 32-bit one, next to
//the program or on the path; from https://www.highrez.co.uk/downloads/inpout32/). The first run on
//a PC must be as administrator: that is when the DLL installs its driver.
class InpOut : public PortIo
{
private:
	typedef UCHAR(__stdcall* ReadFn)(USHORT);
	typedef void(__stdcall* WriteFn)(USHORT, UCHAR);
	typedef BOOL(__stdcall* OpenFn)();
	HMODULE _dll = nullptr;
	ReadFn _read = nullptr;
	WriteFn _write = nullptr;

public:
#ifdef _WIN64
	static constexpr const wchar_t* DllName = L"inpoutx64.dll";
#else
	static constexpr const wchar_t* DllName = L"inpout32.dll";
#endif

	//"" when ready, else what is wrong
	string Load()
	{
		wchar_t exe[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::filesystem::path beside = std::filesystem::path(std::wstring(exe, n)).parent_path() / DllName;
		_dll = LoadLibraryW(beside.c_str());
		if(!_dll) {
			_dll = LoadLibraryW(DllName);
		}
		if(!_dll) {
			return "cannot load " + std::filesystem::path(DllName).u8string() + ": put it next to PcLinkServer.exe";
		}
		_read = (ReadFn)GetProcAddress(_dll, "DlPortReadPortUchar");
		_write = (WriteFn)GetProcAddress(_dll, "DlPortWritePortUchar");
		OpenFn open = (OpenFn)GetProcAddress(_dll, "IsInpOutDriverOpen");
		if(!_read || !_write || !open) {
			return "the InpOut DLL lacks DlPortReadPortUchar / DlPortWritePortUchar / IsInpOutDriverOpen";
		}
		if(!open()) {
			return "the InpOut driver is not running. Run PcLinkServer once as administrator (right-click,"
				" Run as administrator) so it can install it. If it still fails, Windows is refusing the"
				" driver: Memory Integrity (Windows Security > Device security > Core isolation) blocks it";
		}
		return string();
	}

	uint8_t In(uint16_t port) override { return _read(port); }
	void Out(uint16_t port, uint8_t value) override { _write(port, value); }
};

static vector<string> SerialPorts()
{
	vector<string> ports;
	HKEY key;
	if(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return ports;
	}
	for(DWORD i = 0;; i++) {
		char name[256], value[256];
		DWORD nameLen = sizeof(name), valueLen = sizeof(value), type;
		if(RegEnumValueA(key, i, name, &nameLen, nullptr, &type, (BYTE*)value, &valueLen) != ERROR_SUCCESS) {
			break;
		}
		if(type == REG_SZ) {
			ports.push_back(string(value, strnlen(value, valueLen)));
		}
	}
	RegCloseKey(key);
	return ports;
}

//The parallel ports Windows knows, with their I/O ranges (the first is the port's base)
struct ParallelPort
{
	string Name;
	vector<std::pair<uint16_t, uint16_t>> Ranges;
};

static vector<ParallelPort> ParallelPorts()
{
	vector<ParallelPort> ports;
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
	if(set == INVALID_HANDLE_VALUE) {
		return ports;
	}
	SP_DEVINFO_DATA dev = {};
	dev.cbSize = sizeof(dev);
	for(DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		char name[256] = {};
		SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)name, sizeof(name) - 1, nullptr);
		if(!strstr(name, "LPT")) {
			continue;
		}
		ParallelPort p;
		p.Name = name;
		LOG_CONF conf;
		if(CM_Get_First_Log_Conf(&conf, dev.DevInst, ALLOC_LOG_CONF) == CR_SUCCESS || CM_Get_First_Log_Conf(&conf, dev.DevInst, BOOT_LOG_CONF) == CR_SUCCESS) {
			RES_DES res, next;
			RESOURCEID type;
			if(CM_Get_Next_Res_Des(&res, conf, ResType_IO, &type, 0) == CR_SUCCESS) {
				for(;;) {
					ULONG size = 0;
					if(CM_Get_Res_Des_Data_Size(&size, res, 0) == CR_SUCCESS && size >= sizeof(IO_DES)) {
						vector<char> data(size);
						if(CM_Get_Res_Des_Data(res, data.data(), size, 0) == CR_SUCCESS) {
							IO_DES* io = (IO_DES*)data.data();
							p.Ranges.push_back({ (uint16_t)io->IOD_Alloc_Base, (uint16_t)io->IOD_Alloc_End });
						}
					}
					bool more = CM_Get_Next_Res_Des(&next, res, ResType_IO, &type, 0) == CR_SUCCESS;
					CM_Free_Res_Des_Handle(res);
					if(!more) {
						break;
					}
					res = next;
				}
			}
			CM_Free_Log_Conf_Handle(conf);
		}
		ports.push_back(p);
	}
	SetupDiDestroyDeviceInfoList(set);
	return ports;
}

static string Utf8(const wchar_t* s)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
	string out(n > 0 ? n - 1 : 0, '\0');
	if(n > 1) {
		WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
	}
	return out;
}

static bool ParseAddress(const string& s, uint16_t& address)
{
	string h = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? s.substr(2) : s;
	if(h.empty() || h.size() > 4 || h.find_first_not_of("0123456789abcdefABCDEF") != string::npos) {
		return false;
	}
	address = (uint16_t)strtoul(h.c_str(), nullptr, 16);
	return true;
}

static std::atomic<bool> stopping { false };

static BOOL WINAPI OnCtrl(DWORD)
{
	stopping = true;
	return TRUE;
}

static int Usage()
{
	printf("PcLinkServer [folder] [--port COMn] [--baud n]   through PcLinkBridge (USB serial)\n"
		"PcLinkServer [folder] --lpt [address]            through a parallel port (InpOut driver)\n"
		"PcLinkServer --list                              the serial and parallel ports\n"
		"  Serves the folder (default: BbkPcLink, here) as drive C: to a BBK running PCLINK.CMD.\n"
		"  Without --port, every serial port is tried. --baud must match the bridge's firmware\n"
		"  (default 1000000). --lpt without an address takes Windows' first parallel port, else\n"
		"  378; give it in hex (378, 0xD010).\n");
	return 2;
}

static void List()
{
	printf("Serial ports:\n");
	for(const string& s : SerialPorts()) {
		printf("  %s\n", s.c_str());
	}
	printf("Parallel ports:\n");
	for(const ParallelPort& p : ParallelPorts()) {
		printf("  %s:", p.Name.c_str());
		for(auto& r : p.Ranges) {
			printf(" %X-%X", r.first, r.second);
		}
		printf("%s\n", p.Ranges.empty() ? " (no I/O range)" : "");
	}
}

//The wire through PcLinkBridge: the given port, else the first serial port that answers as it
static std::unique_ptr<Wire> OpenBridge(const string& portName, DWORD baud, std::unique_ptr<ComPort>& port)
{
	vector<string> ports = portName.empty() ? SerialPorts() : vector<string> { portName };
	if(ports.empty()) {
		printf("No serial ports - is the bridge plugged in?\n");
		return nullptr;
	}
	for(const string& name : ports) {
		auto p = std::make_unique<ComPort>();
		if(!p->Open(name, baud)) {
			printf("%s: cannot open it\n", name.c_str());
			continue;
		}
		printf("%s: looking for the bridge...\n", name.c_str());
		auto b = std::make_unique<Bridge>(*p, stopping);
		try {
			//Its hello as it restarts on the port opening, else an answer to 'I'
			if(b->AwaitHello(2500) || b->Start(1500)) {
				printf("%s: PcLinkBridge %d, %d baud\n", name.c_str(), b->Version, (int)baud);
				port = std::move(p);
				return b;
			}
		} catch(const std::exception&) {
		}
		printf("%s: no bridge there\n", name.c_str());
		if(stopping) {
			return nullptr;
		}
	}
	printf("No bridge found%s.\n", portName.empty() ? "" : " on that port");
	return nullptr;
}

static std::unique_ptr<Wire> OpenLpt(bool haveAddress, uint16_t address, InpOut& io)
{
	if(!haveAddress) {
		address = 0x378;
		for(const ParallelPort& p : ParallelPorts()) {
			if(!p.Ranges.empty()) {
				address = p.Ranges[0].first;
				printf("%s, at %X\n", p.Name.c_str(), address);
				break;
			}
		}
	}
	string err = io.Load();
	if(!err.empty()) {
		printf("%s.\n", err.c_str());
		return nullptr;
	}
	auto wire = std::make_unique<LptWire>(io, address, stopping);
	if(!wire->Check()) {
		printf("The parallel port at %X does not answer as a standard port: wrong address, or it is in ECP or\n"
			"EPP mode - set it to SPP (Normal, Standard) in the BIOS. --list shows the ports Windows knows.\n", address);
		return nullptr;
	}
	uint8_t lines = wire->BbkLines();
	printf("Parallel port %X. The BBK's lines: nibble %X, handshake %s%s\n", address, lines & 0x0F, (lines & 0x10) ? "up" : "down",
		lines == 0x0F ? " (PCLINK idle)" : lines == 0x1F ? " (nothing connected?)" : "");
	timeBeginPeriod(1);   //(millisecond sleeps while waiting)
	return wire;
}

int wmain(int argc, wchar_t** argv)
{
	SetConsoleOutputCP(CP_UTF8);
	string folder = "BbkPcLink";
	string portName;
	DWORD baud = 1000000;
	bool lpt = false, haveAddress = false;
	uint16_t address = 0;
	for(int i = 1; i < argc; i++) {
		string a = Utf8(argv[i]);
		if((a == "--port" || a == "-p") && i + 1 < argc) {
			portName = Utf8(argv[++i]);
		} else if((a == "--baud" || a == "-b") && i + 1 < argc) {
			baud = (DWORD)strtoul(Utf8(argv[++i]).c_str(), nullptr, 10);
		} else if(a == "--lpt") {
			lpt = true;
			if(i + 1 < argc && ParseAddress(Utf8(argv[i + 1]), address)) {
				haveAddress = true;
				i++;
			}
		} else if(a == "--list") {
			List();
			return 0;
		} else if(a.empty() || a[0] == '-' || a[0] == '/') {
			return Usage();
		} else {
			folder = a;
		}
	}
	SetConsoleCtrlHandler(OnCtrl, TRUE);

	std::unique_ptr<ComPort> port;
	InpOut io;
	std::unique_ptr<Wire> wire = lpt ? OpenLpt(haveAddress, address, io) : OpenBridge(portName, baud, port);
	if(!wire) {
		return 1;
	}

	std::error_code ec;
	auto root = std::filesystem::absolute(std::filesystem::u8path(folder), ec);
	BbkPcLink link;
	link.SetRoot(root.u8string());
	printf("Ready: run PCLINK on the BBK, then use drive C:. Ctrl+C stops.\n");
	fflush(stdout);

	try {
		Serve(*wire, link, stopping, [](const string& line) {
			printf("%s\n", line.c_str());
			fflush(stdout);
		});
	} catch(const std::exception& e) {
		printf("%s\n", e.what());
		return 1;
	}
	printf("Stopped.\n");
	return 0;
}
