#pragma once
//The emulator's log, for BbkPcLink.h outside it: the console
#include <cstdio>
#include <string>

class MessageManager
{
public:
	static void Log(const std::string& message = "")
	{
		printf("%s\n", message.c_str());
		fflush(stdout);
	}
};
