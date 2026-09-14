//The implementation half of the single-header MPEG-1 decoder in pl_mpeg.h, which has to be
//compiled in exactly one translation unit. Third-party (MIT); see the header for its terms.
//
//The discs these learning machines read keep their pictures as MPEG-1 video and MP2 audio in
//an MPEG program stream, which is what this decodes.
//The header uses FILE and the C string/allocation calls but leaves including their headers
//to whoever compiles it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//Left exactly as it comes from upstream so it can be replaced wholesale later; the size_t
//narrowing it does on 64-bit is its own and is silenced here rather than in the file.
#ifdef _MSC_VER
	#pragma warning(push)
	#pragma warning(disable: 4267 4244 4305)
#endif

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#ifdef _MSC_VER
	#pragma warning(pop)
#endif
