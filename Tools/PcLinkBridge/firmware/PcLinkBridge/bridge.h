// The PC end of the BBK's PC link cable, on a microcontroller. It does the 4-bit handshakes that
// PCSVER.EXE did on a PC's printer port, and passes the bytes over USB serial to PcLinkServer.exe,
// which answers PCLINK.CMD.
//
// The including file supplies the hardware:
//   uint8_t bbkLines()       the BBK's five lines: its nibble in bits 0-3, its handshake in bit 4
//   void pcLines(uint8_t v)  ours, the same way
//   int serialGet()          a byte from the PC, or -1
//   void serialPut(uint8_t)
//   void settle()            about a microsecond, between the two reads of a line
//
// A byte goes over as two nibbles, the BBK keeping time (PCLINK.CMD at $8282 and $82A7):
//   the BBK sends:    it puts its low nibble and raises its line; we take the nibble and raise
//                     ours; it puts the high nibble and drops its line; we take it and drop ours
//   the BBK receives: it raises its line; we put the low nibble and raise ours; it drops its
//                     line; we put the high nibble, then drop ours
// A raised line alone does not say which of the two the BBK is doing - only the server knows - so
// we act on its word: the bytes it has queued go first, then as many bytes as it has asked for.
// Until then the BBK waits, as it did while PCSVER was busy.
//
// Serial, PC to us:  'S' n b1..bn   n (1-255) bytes to send the BBK
//                    'R' hi lo      take this many more bytes from the BBK
//                    'I'            start over: drop what is queued, say hello
//         us to PC:  'D' b          a byte the BBK sent
//                    'K'            64 more of the queued bytes taken (the PC keeps at most
//                                   RING_SIZE bytes queued, counting 64 off per 'K')
//                    'H' 'B' 'B' 'K' version ring/64    hello, at power-on and on 'I'

#ifndef RING_SIZE
#define RING_SIZE 1024          // a power of two
#endif
#define BRIDGE_VERSION 1

static uint8_t ring[RING_SIZE];
static uint16_t ringHead, ringCount;
static uint32_t granted;        // bytes the server still wants from the BBK
static uint8_t takenSinceCredit;
static uint8_t parseState, parseLeft;
static uint16_t parseCount;
static bool aborted;            // an 'I' came in the middle of a byte
static uint32_t ringOverflows;  // bytes dropped for want of room (the server broke the window)

static void hello()
{
	serialPut('H');
	serialPut('B');
	serialPut('B');
	serialPut('K');
	serialPut(BRIDGE_VERSION);
	serialPut(RING_SIZE / 64);
}

static void startOver()
{
	ringHead = ringCount = 0;
	granted = 0;
	takenSinceCredit = 0;
	aborted = true;
	hello();
}

// Takes in what the PC has sent
static void pump()
{
	int c;
	while((c = serialGet()) >= 0) {
		uint8_t b = (uint8_t)c;
		switch(parseState) {
			case 0:
				if(b == 'S') {
					parseState = 1;
				} else if(b == 'R') {
					parseState = 3;
				} else if(b == 'I') {
					startOver();
				}
				break;
			case 1:
				parseLeft = b;
				parseState = b ? 2 : 0;
				break;
			case 2:
				if(ringCount < RING_SIZE) {
					ring[(ringHead + ringCount) & (RING_SIZE - 1)] = b;
					ringCount++;
				} else {
					ringOverflows++;
				}
				if(--parseLeft == 0) {
					parseState = 0;
				}
				break;
			case 3:
				parseCount = (uint16_t)(b << 8);
				parseState = 4;
				break;
			case 4:
				granted += parseCount | b;
				parseState = 0;
				break;
		}
	}
}

// Waits for the BBK's line to be up (or down), the same in two reads, as PCSVER did, and returns
// its lines - or 0xFF when an 'I' cuts the byte short
static uint8_t waitLine(bool up)
{
	for(;;) {
		uint8_t a = bbkLines();
		if(((a & 0x10) != 0) == up) {
			settle();
			if(bbkLines() == a) {
				return a;
			}
		}
		pump();
		if(aborted) {
			pcLines(0);
			return 0xFF;
		}
	}
}

static void sendByte()
{
	uint8_t b = ring[ringHead];
	ringHead = (ringHead + 1) & (RING_SIZE - 1);
	ringCount--;
	if(++takenSinceCredit == 64) {
		takenSinceCredit = 0;
		serialPut('K');
	}
	pcLines(b & 0x0F);
	pcLines((b & 0x0F) | 0x10);
	if(waitLine(false) == 0xFF) {
		return;
	}
	pcLines((b >> 4) | 0x10);
	pcLines(b >> 4);
}

static void receiveByte(uint8_t lines)
{
	uint8_t low = lines & 0x0F;
	granted--;
	pcLines(0x10);
	uint8_t high = waitLine(false);
	if(high == 0xFF) {
		return;
	}
	pcLines(0x00);
	serialPut('D');
	serialPut((uint8_t)(low | (high & 0x0F) << 4));
}

static void bridgeInit()
{
	pcLines(0);
	startOver();
}

static void bridgePoll()
{
	pump();
	aborted = false;
	if(!ringCount && !granted) {
		return;
	}
	uint8_t a = bbkLines();
	if(!(a & 0x10)) {
		return;
	}
	settle();
	if(bbkLines() != a) {
		return;
	}
	if(ringCount) {
		sendByte();
	} else {
		receiveByte(a);
	}
}
