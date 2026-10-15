//sim: PcLinkServer's loop and a BBK running PCLINK's byte routines on a simulated cable, the server's
//end being the bridge's firmware on a simulated serial line, then a PC's parallel port - a check of
//both wires (what the server answers is the emulator's, checked against PCLINK there). Exit code 0:
//passed.
#include "../server/serve.h"
#include "../server/lpt.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

//The cable: the BBK's five lines ($FF40 bits 0-4) and the bridge's
static std::atomic<uint8_t> bbkOut { 0x0F };
static std::atomic<uint8_t> pcOut { 0 };

//The serial line, each way
struct Line
{
	std::mutex m;
	std::condition_variable cv;
	std::deque<uint8_t> q;

	void Push(const uint8_t* d, size_t n)
	{
		{
			std::lock_guard<std::mutex> l(m);
			q.insert(q.end(), d, d + n);
		}
		cv.notify_all();
	}

	int Pop(int timeoutMs)
	{
		std::unique_lock<std::mutex> l(m);
		if(q.empty() && timeoutMs > 0) {
			cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [this] { return !q.empty(); });
		}
		if(q.empty()) {
			return -1;
		}
		uint8_t b = q.front();
		q.pop_front();
		return b;
	}
};
static Line toBridge, toServer;

static thread_local std::mt19937 rng;

static void Jitter(int oneIn, int maxUs)
{
	if(rng() % oneIn == 0) {
		std::this_thread::sleep_for(std::chrono::microseconds(rng() % maxUs));
	} else {
		std::this_thread::yield();
	}
}

//The firmware, on the simulated hardware (a small ring, so the window fills often)
static uint8_t bbkLines() { return bbkOut.load() & 0x1F; }
static void pcLines(uint8_t v) { pcOut.store(v & 0x1F); }
static int serialGet() { return toBridge.Pop(0); }
static void serialPut(uint8_t b) { toServer.Push(&b, 1); }
static void settle() { std::this_thread::yield(); }
#define RING_SIZE 256
#include "../firmware/PcLinkBridge/bridge.h"

struct SimPort : Port
{
	void Write(const uint8_t* data, size_t len) override { toBridge.Push(data, len); }
	int Read(int timeoutMs) override
	{
		Jitter(500, 3000);   //(USB's latency)
		return toServer.Pop(timeoutMs);
	}
};

//The BBK: PCLINK's byte routines, $8282 (send) and $82A7 (receive)
static void BbkWrite(uint8_t v) { bbkOut.store(v & 0x1F); }
static uint8_t BbkStatus()
{
	uint8_t p = pcOut.load();
	return (uint8_t)(((p & 0x10) ? 0x00 : 0x80) | ((p & 0x0F) << 3));
}

static void BbkSend(uint8_t a)
{
	BbkWrite(a | 0x10);
	while(BbkStatus() & 0x80) { Jitter(2000, 300); }
	BbkWrite(a >> 4);
	while(!(BbkStatus() & 0x80)) { Jitter(2000, 300); }
}

static uint8_t BbkReceive()
{
	BbkWrite(0x10);
	uint8_t s;
	while((s = BbkStatus()) & 0x80) { Jitter(2000, 300); }
	BbkWrite(0x0F);   //DEC $FF40
	uint8_t low = (s >> 3) & 0x0F;
	while(!((s = BbkStatus()) & 0x80)) { Jitter(2000, 300); }
	return (uint8_t)(((s << 1) & 0xF0) | low);
}

static uint16_t BbkReceive16()
{
	uint16_t hi = BbkReceive();
	return (uint16_t)((hi << 8) | BbkReceive());
}

static vector<uint8_t> mem[8];

static int failures = 0;
#define CHECK(c, ...) do { if(!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

//A request, and the server's memory commands until its answer: $08 bank, $0B write, $15 read, $00 done
static vector<uint8_t> Request(const vector<uint8_t>& req)
{
	BbkSend(0x01);
	for(uint8_t b : req) {
		BbkSend(b);
	}
	uint8_t bank = 0;
	for(;;) {
		uint8_t c = BbkReceive();
		if(c == 0x08) {
			bank = BbkReceive();
		} else if(c == 0x0B) {
			uint16_t len = BbkReceive16(), addr = BbkReceive16();
			for(uint32_t i = 0; i < len; i++) {
				mem[bank & 7][(uint16_t)(addr + i)] = BbkReceive();
			}
		} else if(c == 0x15) {
			uint16_t len = BbkReceive16(), addr = BbkReceive16();
			for(uint32_t i = 0; i < len; i++) {
				BbkSend(mem[bank & 7][(uint16_t)(addr + i)]);
			}
		} else if(c == 0x00) {
			vector<uint8_t> r(14);
			for(auto& b : r) {
				b = BbkReceive();
			}
			return r;
		} else {
			CHECK(false, "the server sent $%02X", c);
			return vector<uint8_t>(14, 0xFF);
		}
	}
}

static vector<uint8_t> Req(uint8_t cmd, uint8_t bank)
{
	vector<uint8_t> r(16, 0);
	r[0] = cmd;
	r[2] = bank;
	return r;
}
static void Put16(vector<uint8_t>& r, int at, uint16_t v) { r[at] = v & 0xFF; r[at + 1] = v >> 8; }
static void Put32(vector<uint8_t>& r, int at, uint32_t v) { for(int i = 0; i < 4; i++) { r[at + i] = (v >> (8 * i)) & 0xFF; } }
static uint16_t Get16(const vector<uint8_t>& r, int at) { return (uint16_t)(r[at] | (r[at + 1] << 8)); }
static uint32_t Get32(const vector<uint8_t>& r, int at) { return r[at] | (r[at + 1] << 8) | (r[at + 2] << 16) | ((uint32_t)r[at + 3] << 24); }
static void Name83(vector<uint8_t>& r, const char* n) { memcpy(&r[5], n, 11); }
static void PutString(int bank, uint16_t addr, const char* s) { memcpy(&mem[bank][addr], s, strlen(s) + 1); }

static void Session(const std::filesystem::path& root, int round)
{
	const uint32_t size = 20000 + round * 3001;
	vector<uint8_t> data(size);
	for(auto& b : data) {
		b = (uint8_t)rng();
	}
	memcpy(&mem[2][0x1000], data.data(), size);

	//create, write in pieces, close
	auto q = Req(8, 0);
	Name83(q, "TEST    BIN");
	auto r = Request(q);
	CHECK(r[11] == 0, "create: status $%02X", r[11]);
	uint16_t h = Get16(r, 12);
	for(uint32_t pos = 0; pos < size;) {
		uint16_t n = (uint16_t)std::min<uint32_t>(5000, size - pos);
		q = Req(3, 2);
		Put16(q, 3, (uint16_t)(0x1000 + pos));
		Put16(q, 5, n);
		Put32(q, 7, pos);
		Put16(q, 12, h);
		r = Request(q);
		CHECK(r[11] == 0 && Get32(r, 7) == pos + n, "write at %u: status $%02X, position %u", pos, r[11], Get32(r, 7));
		pos += n;
	}
	q = Req(5, 0);
	Put16(q, 12, h);
	Request(q);
	{
		std::ifstream f(root / "TEST.BIN", std::ios::binary);
		vector<uint8_t> got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		CHECK(got == data, "the PC's file differs (%zu bytes, want %u)", got.size(), size);
	}

	//open, read in pieces, close
	q = Req(1, 0);
	Name83(q, "TEST    BIN");
	r = Request(q);
	CHECK(r[11] == 0, "open: status $%02X", r[11]);
	h = Get16(r, 12);
	uint32_t pos = 0;
	for(;;) {
		q = Req(2, 3);
		Put16(q, 3, (uint16_t)(0x2000 + pos));
		Put16(q, 5, 8000);
		Put32(q, 7, pos);
		Put16(q, 12, h);
		r = Request(q);
		CHECK(r[11] == 0, "read at %u: status $%02X", pos, r[11]);
		uint16_t got = Get16(r, 5);
		if(got == 0 || r[11] != 0) {
			break;
		}
		pos += got;
	}
	CHECK(pos == size, "read %u bytes, want %u", pos, size);
	CHECK(memcmp(&mem[3][0x2000], data.data(), size) == 0, "the BBK's copy differs");
	q = Req(5, 0);
	Put16(q, 12, h);
	Request(q);

	//md by path, then the listing
	PutString(5, 0x0400, "SUB");
	q = Req(17, 5);
	Put16(q, 3, 0x0400);
	r = Request(q);
	CHECK(r[11] == 0 && std::filesystem::is_directory(root / "SUB"), "md: status $%02X", r[11]);

	PutString(4, 0x0500, "*.*");
	q = Req(22, 4);   //(find first by path, as DIR does)
	Put16(q, 3, 0x0300);
	Put16(q, 5, 0x0500);
	q[7] = 0x10;
	r = Request(q);
	int files = 0, dirs = 0;
	while(r[11] == 0) {
		const uint8_t* e = &mem[4][0x0300];
		string name((const char*)e + 9, 11);
		uint32_t len = e[5] | (e[6] << 8) | (e[7] << 16) | ((uint32_t)e[8] << 24);
		if(name == "TEST    BIN") {
			CHECK(len == size && !(e[0] & 0x10), "listed TEST.BIN: %u bytes, attributes $%02X", len, e[0]);
			files++;
		} else if(name == "SUB        ") {
			CHECK(e[0] & 0x10, "listed SUB: attributes $%02X", e[0]);
			dirs++;
		} else {
			CHECK(false, "listed '%s'", name.c_str());
		}
		uint16_t id = Get16(r, 7);
		q = Req(7, 4);
		Put16(q, 3, 0x0300);
		Put16(q, 7, id);
		r = Request(q);
	}
	CHECK(r[11] == 0x12 && files == 1 && dirs == 1, "listing: status $%02X, %d files, %d directories", r[11], files, dirs);

	//rd and delete by path
	q = Req(18, 5);
	Put16(q, 3, 0x0400);
	r = Request(q);
	CHECK(r[11] == 0 && !std::filesystem::exists(root / "SUB"), "rd: status $%02X", r[11]);
	PutString(5, 0x0400, "TEST.BIN");
	q = Req(23, 5);
	Put16(q, 3, 0x0400);
	r = Request(q);
	CHECK(r[11] == 0 && !std::filesystem::exists(root / "TEST.BIN"), "delete: status $%02X", r[11]);
}

//A PC's parallel port, on the same cable: the data register drives our lines, the status register
//reads the BBK's (its nibble in bits 3-6, its handshake inverted in bit 7)
struct SimIo : PortIo
{
	uint8_t data = 0, control = 0x0C;

	uint8_t In(uint16_t port) override
	{
		switch(port) {
			case 0x378: return data;
			case 0x379: {
				uint8_t b = bbkOut.load();
				return (uint8_t)(((b & 0x0F) << 3) | ((b & 0x10) ? 0x00 : 0x80) | 0x07);
			}
			case 0x37A: return control;
		}
		return 0xFF;
	}

	void Out(uint16_t port, uint8_t value) override
	{
		if(port == 0x378) {
			data = value;
			pcOut.store(value & 0x1F);
		} else if(port == 0x37A) {
			control = value;
		}
	}
};

//The sessions over a wire, then a BBK that restarts in the middle of a request: the server gives
//up on it and starts over, and the next request is answered
static void Run(const char* title, Wire& wire, std::atomic<bool>& stop, std::function<bool()> ready)
{
	printf("%s\n", title);
	bbkOut = 0x0F;
	stop = false;
	auto root = std::filesystem::temp_directory_path() / ("PcLinkSim-" + std::to_string((long long)time(nullptr)) + "-" + std::to_string(rng() % 10000));
	std::filesystem::create_directories(root);
	BbkPcLink link;
	link.SetRoot(root.u8string());
	vector<string> log;
	std::mutex logLock;
	std::thread serverThread([&] {
		rng.seed(2);
		if(!ready()) {
			printf("FAIL: the wire is not ready\n");
			failures++;
			return;
		}
		Serve(wire, link, stop, [&](const string& line) {
			std::lock_guard<std::mutex> l(logLock);
			printf("  %s\n", line.c_str());
			log.push_back(line);
		});
	});

	auto t0 = std::chrono::steady_clock::now();
	for(int round = 0; round < 3; round++) {
		Session(root, round);
	}
	BbkSend(0x01);
	for(int i = 0; i < 7; i++) {
		BbkSend(0x00);
	}
	BbkWrite(0x0F);
	std::this_thread::sleep_for(std::chrono::milliseconds(wire.Patience * 3));
	Session(root, 3);

	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	stop = true;
	serverThread.join();
	bool restarted = false;
	for(auto& l : log) {
		restarted |= l.find("starting over") != string::npos;
	}
	CHECK(restarted, "the server never started over");
	printf("  %llu bytes to the BBK, %llu from it, %.1f s\n", (unsigned long long)wire.Sent, (unsigned long long)wire.Received, secs);
}

int main()
{
	for(auto& m : mem) {
		m.assign(0x10000, 0);
	}
	rng.seed(3);
	std::atomic<bool> stop { false };
	{
		std::atomic<bool> bridgeStop { false };
		std::thread bridgeThread([&] {
			rng.seed(1);
			bridgeInit();
			while(!bridgeStop) {
				bridgePoll();
				std::this_thread::yield();
			}
		});
		SimPort port;
		Bridge bridge(port, stop);
		bridge.Patience = 1000;   //(a slow build machine must not stall that long by accident)
		Run("PcLinkBridge on USB serial:", bridge, stop, [&] { return bridge.AwaitHello(2000); });
		bridgeStop = true;
		bridgeThread.join();
		CHECK(ringOverflows == 0, "the bridge dropped %u bytes", (unsigned)ringOverflows);
	}
	{
		SimIo io;
		LptWire lpt(io, 0x378, stop);
		lpt.Patience = 1000;
		CHECK(lpt.Check(), "the port check failed");
		CHECK(!(io.control & 0x20), "the data lines are not outputs");
		Run("A parallel port:", lpt, stop, [] { return true; });
	}
	printf(failures ? "FAILED (%d)\n" : "passed\n", failures);
	return failures ? 1 : 0;
}
