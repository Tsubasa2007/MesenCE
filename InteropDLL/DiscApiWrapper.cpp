#include "Common.h"
#include "Core/Shared/Emulator.h"
#include "Core/NES/NesConsole.h"
#include "Core/NES/Mappers/Bung/DrPcJrMapper.h"
#include "Core/NES/Mappers/Yuxing/YuxingMapper.h"
#include "Core/NES/Mappers/CdSegmentIndex.h"
#include "Core/NES/Mappers/CdStreamFile.h"

//The disc drives' entry points, in a file of their own. Asking a drive about its disc needs
//the machines' mapper headers, and through them NesPpu.h, whose register names are in the
//global namespace - one of them is Mask, which X11 declares too. EmuApiWrapper.cpp includes
//X11 on Linux, so the two cannot share a file: they met there and the Linux build stopped.

extern unique_ptr<Emulator> _emu;

extern "C"
{
	//How long an item on the mounted disc is - see CdSegmentIndex::MeasureItem. Must be
	//called from the thread the machine runs on: it reads the mounted image, whose sector
	//cache belongs to the emulation and to nobody else.
	DllExport bool __stdcall GetNesDiscItemSeconds(uint32_t lba, uint32_t sectors, double* discSeconds, double* streamSeconds)
	{
		NesConsole* nes = dynamic_cast<NesConsole*>(_emu->GetConsole().get());
		if(!nes) {
			return false;
		}

		//Which drive holds the disc is the mapper's business, not the console's, so the
		//question is asked here rather than adding another disc-shaped method to NesConsole
		CdImageFile* image = nullptr;
		if(DrPcJrMapper* pcjr = dynamic_cast<DrPcJrMapper*>(nes->GetMapper())) {
			image = &pcjr->GetDiscImage();
		} else if(YuxingMapper* yuxing = dynamic_cast<YuxingMapper*>(nes->GetMapper())) {
			image = &yuxing->GetDiscImage();
		}
		if(!image || !image->IsOpen()) {
			return false;
		}

		CdSegmentIndex::MeasureItem(*image, lba, sectors, *discSeconds, *streamSeconds);
		return true;
	}

	//Where one of the mounted disc's videos is - see CdSegmentIndex::ReadTracks. The number
	//is the one a title names, which is one less than the disc's own numbering. Reads the
	//mounted image, so from the thread the machine runs on only.
	DllExport bool __stdcall GetNesDiscTrackExtent(uint32_t video, uint32_t* lba, uint32_t* sectors)
	{
		NesConsole* nes = dynamic_cast<NesConsole*>(_emu->GetConsole().get());
		if(!nes) {
			return false;
		}

		//Which drive holds the disc is the mapper's business, not the console's
		const vector<CdTrack>* tracks = nullptr;
		CdImageFile* image = nullptr;
		if(DrPcJrMapper* pcjr = dynamic_cast<DrPcJrMapper*>(nes->GetMapper())) {
			tracks = &pcjr->GetDiscTracks();
			image = &pcjr->GetDiscImage();
		} else if(YuxingMapper* yuxing = dynamic_cast<YuxingMapper*>(nes->GetMapper())) {
			tracks = &yuxing->GetDiscTracks();
			image = &yuxing->GetDiscImage();
		}

		CdTrack found = {};
		if(!tracks || !image || !image->IsOpen() || !CdSegmentIndex::FindVideoTrack(*tracks, video, found)) {
			return false;
		}

		//What the track names is not what lies in it - see CdSegmentIndex::FindItem
		return CdSegmentIndex::FindItem(*image, found.Lba, found.Sectors, *lba, *sectors);
	}

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
