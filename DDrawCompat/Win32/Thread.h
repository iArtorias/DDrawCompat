#pragma once

namespace Win32
{
	namespace Thread
	{
		enum class BreakpointCondition
		{
			EXECUTION = 0,
			WRITE = 1,
			READWRITE = 3
		};

		void addBreakpoint(void* address, unsigned length, BreakpointCondition condition);
		void applyConfig();
		void installHooks();
		void dllThreadAttach();
		void dllThreadDetach();
		void skipWaitingForExclusiveModeMutex(bool skip);
	}
}
