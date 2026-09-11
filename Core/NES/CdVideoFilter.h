#pragma once
#include "pch.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "NES/Mappers/CdVideoPlayer.h"

//The picture while a disc's video is on screen.
//
//Every console here hands its own kind of buffer to a filter of its own, and the NES one is
//palette indices - one word per pixel, turned into colour by the palette. A decoded picture
//is already colour and is a different size, so it needs a filter of its own rather than a
//way of pretending it is a PPU frame.
//
//Nothing is filtered as such: the frame arrives as the colour it will be shown in, and this
//only carries it across and says how big it is.
class CdVideoFilter : public BaseVideoFilter
{
private:
	CdVideoPlayer* _player = nullptr;

protected:
	//The size comes from the frame itself, which the base class has already been told, rather
	//than from asking the player: the player is on the emulation thread and this is on the one
	//that draws, so between one question and the next a video can have ended and the machine's
	//own frame arrived in its place.

	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		//The size the buffer below was made for
		FrameInfo size = _frameInfo;
		uint32_t* out = GetOutputBuffer();
		//The pipeline is typed for the PPU's buffer, so the picture arrives cast to it
		uint32_t* picture = (uint32_t*)ppuOutputBuffer;
		if(!out) {
			return;
		}

		//A frame is handed over on one thread and drawn on another, so this can be asked to
		//draw the machine's own frame on the way in or out of a video: same call, but a buffer
		//of palette indices rather than colour - half as wide in memory as this would read it.
		//The frame's size does not tell the two apart, since the machine's own is what this
		//filter answers with once a video has ended, so the buffer is asked for by name: only
		//a picture the player itself decoded is copied out, and anything else is left black -
		//one dark frame at the join, instead of reading twice past the end of the PPU's.
		if(!_player || !_player->OwnsPicture(picture)) {
			std::fill(out, out + (size_t)size.Width * size.Height, 0xFF000000);
			return;
		}

		std::copy(picture, picture + (size_t)size.Width * size.Height, out);
	}

public:
	CdVideoFilter(Emulator* emu, CdVideoPlayer* player) : BaseVideoFilter(emu)
	{
		_player = player;
	}

	OverscanDimensions GetOverscan() override
	{
		//A video is shown whole; the overscan the machine's own picture is cropped by has
		//nothing to say about it
		return {};
	}
};
