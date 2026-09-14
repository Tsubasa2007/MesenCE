#include "pch.h"
#include "SuperAcan/SacDefaultVideoFilter.h"
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

void SacDefaultVideoFilter::ApplyFilter(uint16_t* ppuOutputBuffer)
{
	uint32_t* out = GetOutputBuffer();
	FrameInfo size = _baseFrameInfo;

	for(uint32_t i = 0, len = size.Height * size.Width; i < len; i++) {
		out[i] = _calculatedPalette[ppuOutputBuffer[i] & 0x7FFF];
	}
}
