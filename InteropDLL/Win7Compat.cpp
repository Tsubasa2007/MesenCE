#include <windows.h>

//The prebuilt static C++ runtime that ships with MSVC v145 imports two kernel32 functions that
//only exist on Windows 8 and later, so MesenCore.dll cannot be loaded on Windows 7 at all - it
//fails with "the specified procedure could not be found" before any of it runs. The functions
//are in Microsoft's own precompiled objects, so no compiler switch removes them.
//
//Supplying them here takes them out of the DLL's import table. The real function is still used
//whenever the OS has it, so nothing changes on Windows 8 and later; only Windows 7 ever reaches
//the fallback.

namespace
{
	template<typename T> T GetKernel32Function(const char* name)
	{
		HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
		return kernel32 ? reinterpret_cast<T>(GetProcAddress(kernel32, name)) : nullptr;
	}
}

extern "C"
{
	HANDLE WINAPI Win7Compat_CreateFile2(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode, DWORD creationDisposition, LPCREATEFILE2_EXTENDED_PARAMETERS params)
	{
		typedef HANDLE(WINAPI * CreateFile2Fn)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
		static CreateFile2Fn realFunction = GetKernel32Function<CreateFile2Fn>("CreateFile2");
		if(realFunction) {
			return realFunction(fileName, desiredAccess, shareMode, creationDisposition, params);
		}

		//CreateFile2 is CreateFileW with the trailing arguments moved into a struct
		DWORD flagsAndAttributes = 0;
		LPSECURITY_ATTRIBUTES security = nullptr;
		HANDLE templateFile = nullptr;
		if(params) {
			flagsAndAttributes = params->dwFileAttributes | params->dwFileFlags | params->dwSecurityQosFlags;
			security = params->lpSecurityAttributes;
			templateFile = params->hTemplateFile;
		}
		return CreateFileW(fileName, desiredAccess, shareMode, security, creationDisposition, flagsAndAttributes, templateFile);
	}

	void WINAPI Win7Compat_GetSystemTimePreciseAsFileTime(LPFILETIME fileTime)
	{
		typedef void(WINAPI * PreciseTimeFn)(LPFILETIME);
		static PreciseTimeFn realFunction = GetKernel32Function<PreciseTimeFn>("GetSystemTimePreciseAsFileTime");
		if(realFunction) {
			realFunction(fileTime);
		} else {
			//Windows 7 has no precise variant - this one ticks at the timer interval instead
			GetSystemTimeAsFileTime(fileTime);
		}
	}

	//The runtime reaches both functions through their import thunks, so pointing the thunks at
	//the wrappers above is what keeps the Windows 8 imports out of the import table.
	void* __imp_CreateFile2 = reinterpret_cast<void*>(&Win7Compat_CreateFile2);
	void* __imp_GetSystemTimePreciseAsFileTime = reinterpret_cast<void*>(&Win7Compat_GetSystemTimePreciseAsFileTime);
}
