// PcLinkBridge: the PC end of the BBK's PC link cable, for PcLinkServer.exe (see ../../README.md).
// For a 5 V board: Arduino Uno, Nano, Pro Mini (ATmega328P - the pins are read and written a port
// at a time); other boards fall back to digitalRead/digitalWrite.
//
// Wiring, to a DB25 female socket, where the cable's PC end plugs in:
//   DB25 2 3 4 5   (D0-D3)        <- D2 D3 D4 D5   our nibble
//   DB25 6         (D4)           <- D6            our handshake
//   DB25 15 13 12 10 (S3-S6)      -> A0 A1 A2 A3   the BBK's nibble
//   DB25 11        (S7, Busy)     -> A4            the BBK's handshake
//   DB25 18-25     ground         -- GND
// A 330 ohm resistor in series with each of the ten lines guards both ends against a miswire.
//
// The serial speed must match PcLinkServer's (--baud).
#define BAUD 1000000

static const uint8_t outPins[5] = { 2, 3, 4, 5, 6 };
static const uint8_t inPins[5] = { A0, A1, A2, A3, A4 };

#if defined(__AVR_ATmega328P__)
static inline uint8_t bbkLines() { return PINC & 0x1F; }
static inline void pcLines(uint8_t v) { PORTD = (PORTD & 0x83) | ((v & 0x1F) << 2); }
#else
static inline uint8_t bbkLines()
{
	uint8_t v = 0;
	for(uint8_t i = 0; i < 5; i++) {
		if(digitalRead(inPins[i])) {
			v |= 1 << i;
		}
	}
	return v;
}

static inline void pcLines(uint8_t v)
{
	//The nibble first, the handshake after it, so the BBK never sees the handshake before the data
	for(uint8_t i = 0; i < 5; i++) {
		digitalWrite(outPins[i], (v >> i) & 1);
	}
}
#endif

static inline int serialGet() { return Serial.read(); }
static inline void serialPut(uint8_t b) { Serial.write(b); }
static inline void settle() { delayMicroseconds(1); }

#include "bridge.h"

void setup()
{
	for(uint8_t i = 0; i < 5; i++) {
		pinMode(outPins[i], OUTPUT);
		pinMode(inPins[i], INPUT_PULLUP);
	}
	Serial.begin(BAUD);
	bridgeInit();
}

void loop()
{
	bridgePoll();
}
