# PcLinkBridge

A BBK running **PCLINK.CMD** (清华 唐瑞春, 1997) uses a disk on a PC as a drive, over a cable
between the two parallel ports. The PC end was **PCSVER.EXE**, a 16-bit DOS program that drives
the printer port's pins directly - which no current Windows lets a program do, and USB printer
adapters cannot do at all.

PcLinkBridge replaces the PC's printer port with a small 5 V microcontroller board (an Arduino
Uno or Nano) on USB:

    BBK ──PC link cable──> DB25 socket ── Arduino ──USB──> PC: PcLinkServer.exe ── a folder

A PC that still has a real parallel port can do without the Arduino: see
[Without the Arduino](#without-the-arduino-a-real-parallel-port).

- `firmware/PcLinkBridge/` - the Arduino sketch. It does the 4-bit handshakes on the cable,
  exactly as PCSVER did, and passes the bytes over USB serial.
- `server/` - **PcLinkServer.exe**, which answers PCLINK's requests with a folder on the PC. It
  is built from the emulator's own PC link server (`Core/NES/Mappers/Bbk/BbkPcLink.h`, checked
  against PCLINK in MesenCE), so it behaves as the emulator's BbkPcLink setting does.
- `test/sim.cpp` - the server loop and a BBK running PCLINK's byte routines on a simulated
  cable, the server's end being the firmware on a simulated serial line, then a simulated
  parallel port: files written and read back in pieces, md, dir, rd, delete, and a BBK that
  stops in the middle of a request.

中文说明：[使用说明.md](使用说明.md).

> **Not yet tried on real hardware.** Both ways are checked in simulation (above) and the
> sketch compiles; the first real test is the one in [Trying it](#trying-it).

## Parts

- An Arduino **Uno**, **Nano** or **Pro Mini (5 V)** - ATmega328P, 5 V. Another 5 V board works
  too, a little slower (the sketch falls back to `digitalRead`/`digitalWrite`). A 3.3 V board
  (a Pico, an ESP32) needs level shifters on all ten lines.
- A **DB25 female** socket (a breakout board, or a socket and wires), for the cable's PC end.
- Ten **330 Ω** resistors, one in series with each signal line. They protect both ends if
  something is miswired.
- The PC link cable that went with PCSVER. It is a LapLink-style cable: each side's data pins
  2-6 go to the other side's status pins 15, 13, 12, 10, 11.

## Wiring

The Arduino stands in for the PC's printer port:

| DB25 pin | PC port signal | Arduino | What it carries |
| --- | --- | --- | --- |
| 2 | D0 | D2 | our nibble, bit 0 |
| 3 | D1 | D3 | bit 1 |
| 4 | D2 | D4 | bit 2 |
| 5 | D3 | D5 | bit 3 |
| 6 | D4 | D6 | our handshake |
| 15 | Error (S3) | A0 | the BBK's nibble, bit 0 |
| 13 | Select (S4) | A1 | bit 1 |
| 12 | Paper out (S5) | A2 | bit 2 |
| 10 | Ack (S6) | A3 | bit 3 |
| 11 | Busy (S7) | A4 | the BBK's handshake |
| 18-25 | ground | GND | |

Leave the other DB25 pins unconnected. Before wiring the cable to the Arduino, check it with a
meter: with the cable on the BBK and PCLINK loaded and idle, the cable's PC-end plug should
read about 5 V on pins 15, 13, 12 and 10 and about 0 V on pin 11, against pin 25. (PCLINK
starts by writing $0F to its port: the nibble high, the handshake low.) The sketch assumes
5 V logic, like a PC's port.

## Flashing the sketch

Open `firmware/PcLinkBridge/PcLinkBridge.ino` in the Arduino IDE. Choose the board (and, for a
Nano with an old bootloader, *ATmega328P (Old Bootloader)*), then Upload.

## Running the server

Build it with `build.bat` (needs the Visual Studio C++ tools), or use a built
`PcLinkServer.exe`:

    PcLinkServer.exe [folder] [--port COMn] [--baud n]

- `folder` is what the BBK sees as drive C:. The default is `BbkPcLink` in the current
  directory; it is made if missing.
- Without `--port`, every serial port is tried until one answers as the bridge.
- `--baud` must match `BAUD` in the sketch (both default to 1000000).

It prints a line per file operation (open, create, close, dir, cd, md, rd, delete, rename).
Ctrl+C stops it.

## Trying it

1. Plug the Arduino into the PC and start `PcLinkServer.exe`. It should print
   `COMn: PcLinkBridge 1, 1000000 baud` and then `Ready`.
2. Plug the PC link cable into the BBK and into the DB25 socket. Load PCLINK on the BBK as
   before.
3. On the BBK: `c:`, then `dir`. The listing should show the folder's files with the label
   `PCLINK`. Then try `type` on a text file and `copy` a file each way.

If `dir` hangs, the bridge is waiting on the cable: check the wiring and the grounds. If the
server prints `! ... starting over` again and again, try a lower speed: set `BAUD` in the
sketch to 115200, upload it, and run the server with `--baud 115200`.

## Without the Arduino: a real parallel port

`PcLinkServer.exe --lpt` does the handshakes itself, on the PC's own parallel port, as PCSVER did.
The cable plugs straight into the port.

- **The port.** A motherboard port (or a header with a bracket) or a PCIe parallel card. USB
  printer adapters cannot be used. In the BIOS, set the port's mode to **SPP** (Normal,
  Standard); ECP and EPP modes do not drive the pins directly.
- **The driver.** Windows lets no program touch the port's pins, so this uses the InpOut driver.
  Download it from <https://www.highrez.co.uk/downloads/inpout32/> and put `inpoutx64.dll` next
  to `PcLinkServer.exe` (a 32-bit Windows: `bin\x86\PcLinkServer.exe` and `inpout32.dll`).
  **Run it the first time as administrator**: that is when the DLL installs its driver. If
  the driver still does not load, Windows is refusing it: turn off Memory Integrity (Windows
  Security > Device security > Core isolation details) and restart.
- **Windows.** Built with Visual Studio 2026 (`build.bat` on a current PC), it needs Windows 8
  or later. For Windows 7, use the build from GitHub: the workflow
  `.github/workflows/pclinkbridge.yml` builds with Visual Studio 2022 and fails if the program
  uses anything Windows 7 lacks. Windows XP is not covered.

Then:

    PcLinkServer.exe --list                 the ports Windows knows, with their addresses
    PcLinkServer.exe [folder] --lpt         Windows' first parallel port (else 378)
    PcLinkServer.exe [folder] --lpt D010    the port at that address (hex), e.g. a PCIe card's

On start it checks the port (the data lines must read back what it writes; otherwise it says so)
and reports what the BBK's lines read. With the cable on the BBK and PCLINK loaded, that should
be `nibble F, handshake down (PCLINK idle)`. `nibble F, handshake up` means nothing is driving
them: the cable or the BBK is not connected. Then try `c:` and `dir` on the BBK, as below.

## How it works

A byte goes over as two nibbles with a handshake each way, the BBK keeping time (PCLINK's
routines at $8282 and $82A7; PCSVER's at $02C2 and $0377). A raised handshake from the BBK can
mean "here is a nibble" or "send me one". Only the server knows which, so the bridge acts only
on its word. The server queues the bytes to send (`S`), then says how many bytes to take from the
BBK (`R`). Until it does, the BBK waits, as it did while PCSVER was busy with the disk. The
bridge sends back each byte it took (`D`) and a credit (`K`) for every 64 queued bytes it has
sent, so the server never queues more than the bridge holds. The details are at the top of
`firmware/PcLinkBridge/bridge.h`.

If a side stops in the middle of a request for 5 seconds, the server drops the request and
starts the bridge over, keeping the open files and the current directory. (That is for a BBK
that was reset or unplugged. A BBK still inside that request would be out of step; load
PCLINK again.)

## Building and testing

    build.bat          builds bin\PcLinkServer.exe and bin\x86\PcLinkServer.exe (32-bit)
    build.bat test     also runs bin\sim.exe: the simulation, over the bridge and over a
                       parallel port (prints "passed")
