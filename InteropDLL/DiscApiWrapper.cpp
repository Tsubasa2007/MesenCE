#include "Common.h"
#include "Core/Shared/Emulator.h"
#include "Core/NES/Mappers/CdSegmentIndex.h"
#include "Core/NES/Mappers/CdStreamFile.h"

//The disc entry points, in a file of their own. EmuApiWrapper.cpp includes X11 on Linux,
//whose global Mask clashes with the NES register names the machines' disc code brings in,
//so whatever reaches into that code is kept here rather than there. What is left of it on
//this branch reads a named image and asks no drive, since the core now plays a disc's video.

extern unique_ptr<Emulator> _emu;

extern "C"
{
	//Write one of a disc's items out as an ordinary MPEG-1 file, for something other than
	//this to play - see CdStreamFile. `from` and `to` are counted in the sectors that carry
	//the stream, `to` of nought meaning the rest of the item. Returns how many sectors were
	//written.
	//
	//Reads the named image itself rather than the mounted one, so unlike its neighbours here
	//this may be called from any thread - and from a disc that is not mounted at all, which
	//is what the disc menu does.
	DllExport uint32_t __stdcall ExtractNesDiscItem(char* binPath, uint32_t lba, uint32_t sectors, uint32_t from, uint32_t to, bool retime, char* outPath)
	{
		return CdStreamFile::Write(binPath, lba, sectors, from, to, retime, outPath);
	}

	//The pack clock at a sector of a named image, or at the nearest one either side of it.
	//Reads the named image itself, so this too may be called from any thread.
	DllExport bool __stdcall GetNesDiscPackClock(char* binPath, uint32_t lba, bool searchBack, double* clock)
	{
		CdImageFile image;
		if(!image.Open(binPath)) {
			return false;
		}

		double value = CdSegmentIndex::ClockNear(image, lba, searchBack);
		if(value < 0) {
			return false;
		}

		*clock = value;
		return true;
	}
}
