#include "pch.h"
#include "SuperAcan/SacDefaultVideoFilter.h"
#include "SuperAcan/SacTypes.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/ColorUtilities.h"

SacDefaultVideoFilter::SacDefaultVideoFilter(Emulator* emu) : BaseVideoFilter(emu)
{
	_emu = emu;
	InitLookupTable();
}

//The palette RAM holds 15-bit colours with red in the low bits, the same layout as the SNES
void SacDefaultVideoFilter::InitLookupTable()
{
	VideoConfig config = _emu->GetSettings()->GetVideoConfig();

	InitConversionMatrix(config.Hue, config.Saturation);

	for(int color = 0; color < 0x8000; color++) {
		uint32_t argb = ColorUtilities::Rgb555ToArgb(color);
		if(config.Hue != 0 || config.Saturation != 0 || config.Brightness != 0 || config.Contrast != 0) {
			uint8_t r = (argb >> 16) & 0xFF;
			uint8_t g = (argb >> 8) & 0xFF;
			uint8_t b = argb & 0xFF;
			ApplyColorOptions(r, g, b, config.Brightness, config.Contrast);
			argb = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
		_calculatedPalette[color] = argb;
	}

	_videoConfig = config;
}

void SacDefaultVideoFilter::OnBeforeApplyFilter()
{
	VideoConfig config = _emu->GetSettings()->GetVideoConfig();
	if(_videoConfig.Hue != config.Hue || _videoConfig.Saturation != config.Saturation || _videoConfig.Contrast != config.Contrast || _videoConfig.Brightness != config.Brightness) {
		InitLookupTable();
	}
	_videoConfig = config;
}

//The frame comes in as 320x240 with the width and height it was drawn at stored after the pixels.
//The output is that width by 240 lines, with the 224-line mode in the middle: the window keeps the
//fixed 320x240 base size, and the renderer stretches either width across it.
FrameInfo SacDefaultVideoFilter::GetFrameInfo()
{
	FrameInfo size;
	size.Width = _ppuOutputBuffer && _ppuOutputBuffer[SacConstants::MaxPixelCount] == 256 ? 256 : SacConstants::MaxScreenWidth;
	size.Height = SacConstants::MaxScreenHeight;
	return size;
}

void SacDefaultVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	uint32_t* out = GetOutputBuffer();
	uint32_t width = _frameInfo.Width;
	uint32_t height = ppuOutputBuffer[SacConstants::MaxPixelCount + 1] == 224 ? 224 : SacConstants::MaxScreenHeight;
	uint32_t firstRow = (SacConstants::MaxScreenHeight - height) / 2;

	for(uint32_t y = 0; y < SacConstants::MaxScreenHeight; y++) {
		uint32_t* row = out + y * width;
		if(y < firstRow || y >= firstRow + height) {
			std::fill(row, row + width, 0xFF000000);
			continue;
		}

		uint16_t* src = ppuOutputBuffer + (y - firstRow) * width;
		for(uint32_t x = 0; x < width; x++) {
			row[x] = _calculatedPalette[src[x] & 0x7FFF];
		}
	}
}
