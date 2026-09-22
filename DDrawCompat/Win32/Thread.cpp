#include <bitset>
#include <map>
#include <sstream>
#include <vector>

#include <Windows.h>
#include <avrt.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Common/Path.h>
#include <Common/ScopedCriticalSection.h>
#include <Common/Time.h>
#include <Config/Settings/CpuAffinity.h>
#include <Config/Settings/CpuAffinityRotation.h>
#include <Config/Settings/ThreadPriorityBoost.h>
#include <Dll/Dll.h>
#include <Win32/Thread.h>

struct THREAD_BASIC_INFORMATION
{
	NTSTATUS  ExitStatus;
	PVOID     TebBaseAddress;
	CLIENT_ID ClientId;
	KAFFINITY AffinityMask;
	KPRIORITY Priority;
	KPRIORITY BasePriority;
};

NTSTATUS NTAPI NtSetInformationThread(
	HANDLE ThreadHandle,
	THREADINFOCLASS ThreadInformationClass,
	PVOID ThreadInformation,
	ULONG ThreadInformationLength);

namespace
{
	struct Breakpoint
	{
		void* address;
		unsigned length;
		Win32::Thread::BreakpointCondition condition;
	};

	struct DR7
	{
		uint32_t L0   : 1;  //     0 DR0 local enable
		uint32_t G0   : 1;  //     1 DR0 global enable
		uint32_t L1   : 1;  //     2 DR1 local enable
		uint32_t G1   : 1;  //     3 DR1 global enable
		uint32_t L2   : 1;  //     4 DR2 local enable
		uint32_t G2   : 1;  //     5 DR2 global enable
		uint32_t L3   : 1;  //     6 DR3 local enable
		uint32_t G3   : 1;  //     7 DR3 global enable
		uint32_t LE   : 1;  //     8 Local Exact Breakpoint Enable
		uint32_t GE   : 1;  //     9 Global Exact Breakpoint Enable
		uint32_t      : 3;  // 10-12 Reserved
		uint32_t GD   : 1;  //    13 General Detect Enable
		uint32_t      : 2;  // 14-15 Reserved
		uint32_t RW0  : 2;  // 16-17 DR0 condition
		uint32_t LEN0 : 2;  // 18-19 DR0 length
		uint32_t RW1  : 2;  // 20-21 DR1 condition
		uint32_t LEN1 : 2;  // 22-23 DR1 length
		uint32_t RW2  : 2;  // 24-25 DR2 condition
		uint32_t LEN2 : 2;  // 26-27 DR2 length
		uint32_t RW3  : 2;  // 28-29 DR3 condition
		uint32_t LEN3 : 2;  // 30-31 DR3 length
	};

	struct ThreadInfo
	{
		DWORD id;
		HANDLE handle;
		const void* startAddress;
		bool useCpuAffinity;
		bool debuggable;
	};

	class ThreadSuspender
	{
	public:
		ThreadSuspender(DWORD threadId, HANDLE thread, bool condition) : m_threadId(threadId), m_thread(nullptr)
		{
			if (condition)
			{
				if (-1 == SuspendThread(thread))
				{
					LOG_INFO << "ERROR: SuppressThread failed for thread ID "
						<< Compat::hex(threadId) << ": " << GetLastError();
				}
				else
				{
					m_thread = thread;
				}
			}
		}

		~ThreadSuspender()
		{
			if (m_thread)
			{
				if (-1 == ResumeThread(m_thread))
				{
					LOG_INFO << "ERROR: SuppressThread failed for thread ID "
						<< Compat::hex(m_threadId) << ": " << GetLastError();
				}
			}
		}

	private:
		DWORD m_threadId;
		HANDLE m_thread;
	};

	Compat::CriticalSection g_cs;
	DWORD g_cpuAffinity = 0;
	std::map<BYTE, BYTE> g_nextProcessor;
	std::map<DWORD, ThreadInfo> g_threads;

	Breakpoint g_breakpoints[4] = {};
	unsigned g_nextBreakpointSlot = 0;
	void* g_vectoredExceptionHandler = nullptr;

	HANDLE g_exclusiveModeMutex = nullptr;
	thread_local bool g_skipWaitingForExclusiveModeMutex = false;

	decltype(&NtQueryInformationThread) g_ntQueryInformationThread = nullptr;
	decltype(&NtSetInformationThread) g_ntSetInformationThread = nullptr;

	bool useCpuAffinityRotation();
	std::string maskToString(ULONG_PTR mask);
	void registerThread(DWORD threadId, const char* msg);
	void setNextProcessorSet(DWORD fromSet, DWORD toSet);
	void updateThreadBreakpoints(DWORD threadId, HANDLE thread);
	bool useCpuAffinityForThread(const void* startAddress);

	HANDLE WINAPI ddrawOpenMutexW(DWORD dwDesiredAccess, BOOL bInheritHandle, LPCWSTR lpName)
	{
		LOG_FUNC("ddrawOpenMutexW", dwDesiredAccess, bInheritHandle, lpName);
		auto result = CALL_ORIG_FUNC(OpenMutexW)(dwDesiredAccess, bInheritHandle, lpName);
		if (SUCCEEDED(result) && lpName && 0 == lstrcmpW(lpName, L"Local\\__DDrawExclMode__"))
		{
			g_exclusiveModeMutex = result;
		}
		return LOG_RESULT(result);
	}

	DWORD WINAPI ddrawWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
	{
		LOG_FUNC("ddrawWaitForSingleObject", hHandle, dwMilliseconds);
		if (hHandle && hHandle == g_exclusiveModeMutex && g_skipWaitingForExclusiveModeMutex)
		{
			return WAIT_OBJECT_0;
		}
		return LOG_RESULT(WaitForSingleObject(hHandle, dwMilliseconds));
	}

	Breakpoint getBreakpointInfo(const CONTEXT& ctx, unsigned slot)
	{
		const auto& dr7 = reinterpret_cast<const DR7&>(ctx.Dr7);
		Breakpoint bp = {};

#define GET_BP(slot) \
    bp.address = reinterpret_cast<void*>(ctx.Dr##slot); \
	bp.length = dr7.LEN##slot + 1; \
	bp.condition = static_cast<Win32::Thread::BreakpointCondition>(dr7.RW##slot)

		switch (slot)
		{
		case 0: GET_BP(0); break;
		case 1: GET_BP(1); break;
		case 2: GET_BP(2); break;
		case 3: GET_BP(3); break;
		}
#undef GET_BP

		return bp;
	}

	std::map<BYTE, std::vector<DWORD>> getProcessorSets()
	{
		std::map<BYTE, std::vector<DWORD>> result;
		DWORD primaryProcessorGroup = 0;
		auto getThreadIdealProcessorEx = reinterpret_cast<decltype(&GetThreadIdealProcessorEx)>(
			Compat::getProcAddress(GetModuleHandle("kernel32"), "GetThreadIdealProcessorEx"));
		if (getThreadIdealProcessorEx)
		{
			PROCESSOR_NUMBER pn = {};
			getThreadIdealProcessorEx(GetCurrentThread(), &pn);
			primaryProcessorGroup = pn.Group;
			LOG_INFO << "Primary CPU group number: " << primaryProcessorGroup;
		}

		auto getLogicalProcessorInformationEx = reinterpret_cast<decltype(&GetLogicalProcessorInformationEx)>(
			Compat::getProcAddress(GetModuleHandle("kernel32"), "GetLogicalProcessorInformationEx"));
		if (getLogicalProcessorInformationEx)
		{
			DWORD size = 0;
			std::vector<BYTE> buffer;
			getLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
			buffer.resize(size);
			getLogicalProcessorInformationEx(RelationProcessorCore,
				reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &size);

			DWORD offset = 0;
			while (offset < size)
			{
				auto& pi = *reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
				for (DWORD i = 0; i < pi.Processor.GroupCount; ++i)
				{
					if (primaryProcessorGroup == pi.Processor.GroupMask[i].Group)
					{
						result[pi.Processor.EfficiencyClass].push_back(pi.Processor.GroupMask[i].Mask);
					}
				}
				offset += pi.Size;
			}
		}
		else
		{
			DWORD size = 0;
			GetLogicalProcessorInformation(nullptr, &size);
			std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> processorInfo;
			processorInfo.resize(size / sizeof(processorInfo[0]));
			GetLogicalProcessorInformation(processorInfo.data(), &size);

			for (auto& pi : processorInfo)
			{
				if (RelationProcessorCore == pi.Relationship)
				{
					result[0].push_back(pi.ProcessorMask);
				}
			}
		}

		if (result.empty())
		{
			LOG_INFO << "ERROR: Failed to detect CPU topology!";
			return result;
		}

		for (auto it = result.rbegin(); it != result.rend(); ++it)
		{
			LOG_INFO << "Physical to logical CPU core mapping for efficiency class " << static_cast<DWORD>(it->first) << ':';
			DWORD core = 0;
			for (auto& set : it->second)
			{
				++core;
				LOG_INFO << "  Physical core #" << core << ": " << maskToString(set);
			}
		}
		return result;
	}

	void initNextProcessorMap(const std::map<BYTE, std::vector<DWORD>>& processorSets)
	{
		for (auto& ps : processorSets)
		{
			for (BYTE fromIndex = 0; fromIndex < ps.second.size(); ++fromIndex)
			{
				auto bitCount = std::bitset<32>(ps.second[fromIndex]).count();
				bool found = false;
				for (BYTE toIndex = fromIndex + 1; toIndex < ps.second.size() && !found; ++toIndex)
				{
					if (bitCount == std::bitset<32>(ps.second[toIndex]).count())
					{
						setNextProcessorSet(ps.second[fromIndex], ps.second[toIndex]);
						found = true;
					}
				}
				for (BYTE toIndex = 0; toIndex < fromIndex && !found; ++toIndex)
				{
					if (bitCount == std::bitset<32>(ps.second[toIndex]).count())
					{
						setNextProcessorSet(ps.second[fromIndex], ps.second[toIndex]);
						found = true;
					}
				}
				if (!found)
				{
					setNextProcessorSet(ps.second[fromIndex], ps.second[fromIndex]);
				}
			}
		}
	}

	void logThreadInfo(ThreadInfo threadInfo, const char* msg)
	{
		if (Config::logLevel.get() < Config::Settings::LogLevel::DEBUG)
		{
			return;
		}

		Compat::Log log(Config::Settings::LogLevel::DEBUG);
		log << msg << ": ID: " << std::hex << std::setw(4) << std::setfill('0') << threadInfo.id << std::dec
			<< ", use CPU affinity: " << threadInfo.useCpuAffinity
			<< ", debuggable: " << threadInfo.debuggable;
		if (threadInfo.startAddress)
		{
			log << " (" << Compat::funcPtrToStr(threadInfo.startAddress) << ')';
		}
	}

	std::string maskToString(ULONG_PTR mask)
	{
		std::ostringstream oss;
		for (BYTE i = 0; i < 32; ++i)
		{
			if (mask & (1U << i))
			{
				oss << i + 1 << ", ";
			}
		}
		return oss.str().empty() ? "null" : oss.str().substr(0, oss.str().length() - 2);
	}

	void registerExistingThreads()
	{
		LOG_FUNC("registerExistingThreads");
		const DWORD pid = GetCurrentProcessId();
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (INVALID_HANDLE_VALUE == snapshot)
		{
			LOG_INFO << "ERROR: CreateToolhelp32Snapshot failed: " << GetLastError();
			return;
		}

		THREADENTRY32 te = {};
		te.dwSize = sizeof(te);
		if (!Thread32First(snapshot, &te))
		{
			CloseHandle(snapshot);
			LOG_INFO << "ERROR: Thread32First failed: " << GetLastError();
			return;
		}

		do
		{
			if (pid == te.th32OwnerProcessID)
			{
				registerThread(te.th32ThreadID, "Thread running");
			}
		} while (Thread32Next(snapshot, &te));

		CloseHandle(snapshot);
	}

	void registerThread(DWORD threadId, const char* msg)
	{
		{
			Compat::ScopedCriticalSection lock(g_cs);
			if (g_threads.find(threadId) != g_threads.end())
			{
				return;
			}
		}

		ThreadInfo threadInfo = {};
		threadInfo.id = threadId;
		threadInfo.handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION |
			THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, threadId);
		threadInfo.debuggable = threadInfo.handle;
		if (!threadInfo.handle)
		{
			threadInfo.handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, FALSE, threadId);
			if (!threadInfo.handle)
			{
				LOG_ONCE("ERROR: OpenThread failed: " << GetLastError());
				return;
			}
		}

		if (g_ntQueryInformationThread)
		{
			const auto ThreadQuerySetWin32StartAddress = static_cast<THREADINFOCLASS>(9);
			g_ntQueryInformationThread(threadInfo.handle, ThreadQuerySetWin32StartAddress,
				&threadInfo.startAddress, sizeof(threadInfo.startAddress), nullptr);
		}

		threadInfo.useCpuAffinity = useCpuAffinityForThread(threadInfo.startAddress);

		{
			Compat::ScopedCriticalSection lock(g_cs);
			if (!g_threads.insert({ threadId, threadInfo }).second)
			{
				return;
			}
			if (threadInfo.debuggable)
			{
				updateThreadBreakpoints(threadId, threadInfo.handle);
			}
		}

		logThreadInfo(threadInfo, msg);

		if (threadInfo.useCpuAffinity)
		{
			SetThreadAffinityMask(threadInfo.handle, g_cpuAffinity);
		}
	}

	DWORD rotateMask(DWORD mask)
	{
		DWORD result = 0;
		for (BYTE i = 0; i < 32; ++i)
		{
			if (mask & (1U << i))
			{
				auto it = g_nextProcessor.find(i);
				result |= 1U << (it != g_nextProcessor.end() ? it->second : i);
			}
		}
		return result;
	}

	void setNextProcessorSet(DWORD fromSet, DWORD toSet)
	{
		BYTE from = 0;
		BYTE to = 0;
		while (0 != fromSet)
		{
			while (!(fromSet & (1U << from)))
			{
				++from;
			}
			while (!(toSet & (1U << to)))
			{
				++to;
			}
			g_nextProcessor[from] = to;
			fromSet &= ~(1U << from);
			++from;
			++to;
		}
	}

	void setProcessAffinity()
	{
		if (0 == g_cpuAffinity)
		{
			return;
		}

		Compat::ScopedCriticalSection lock(g_cs);
		for (const auto& t : g_threads)
		{
			if (t.second.useCpuAffinity)
			{
				auto prev = SetThreadAffinityMask(t.second.handle, g_cpuAffinity);
				LOG_DEBUG << "Thread affinity changed from " << Compat::hex(prev) << " to " << Compat::hex(g_cpuAffinity)
					<< " for thread ID " << Compat::hex(t.second.id);
			}
		}
	}

	unsigned WINAPI threadManagerProc(LPVOID /*lpParameter*/)
	{
		DWORD taskIndex = 0;
		HANDLE task = AvSetMmThreadCharacteristicsA("Audio", &taskIndex);
		if (!task)
		{
			LOG_INFO << "ERROR: AvSetMmThreadCharacteristicsA failed: " << GetLastError();
		}
		else if (!AvSetMmThreadPriority(task, AVRT_PRIORITY_CRITICAL))
		{
			LOG_INFO << "ERROR: AvSetMmThreadPriority failed: " << GetLastError();
		}

		const bool rotate = useCpuAffinityRotation();

		while (true)
		{
			Sleep(100);

			{
				Compat::ScopedCriticalSection lock(g_cs);
				for (const auto& t : g_threads)
				{
					if (WAIT_OBJECT_0 == WaitForSingleObject(t.second.handle, 0))
					{
						logThreadInfo(t.second, "Thread terminated");
						CloseHandle(t.second.handle);
						g_threads.erase(t.first);
					}
				}

				if (rotate)
				{
					g_cpuAffinity = rotateMask(g_cpuAffinity);
					setProcessAffinity();
				}

				if (!g_ntQueryInformationThread || !g_ntSetInformationThread)
				{
					continue;
				}

				for (const auto& t : g_threads)
				{
					const auto ThreadBasicInformation = static_cast<THREADINFOCLASS>(0);
					THREAD_BASIC_INFORMATION tbi = {};
					g_ntQueryInformationThread(t.second.handle, ThreadBasicInformation, &tbi, sizeof(tbi), nullptr);

					if (tbi.Priority > THREAD_PRIORITY_TIME_CRITICAL)
					{
						const auto ThreadPriority = static_cast<THREADINFOCLASS>(2);
						ULONG prio = THREAD_PRIORITY_TIME_CRITICAL;
						g_ntSetInformationThread(t.second.handle, ThreadPriority, &prio, sizeof(prio));
					}
				}
			}

			if (rotate)
			{
				SetThreadAffinityMask(GetCurrentThread(), rotateMask(g_cpuAffinity));
			}
		}
	}

	void updateThreadBreakpoints(DWORD threadId, HANDLE thread)
	{
		ThreadSuspender suspender(threadId, thread, GetCurrentThreadId() != threadId);

		CONTEXT ctx = {};
		ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
		if (!GetThreadContext(thread, &ctx))
		{
			LOG_INFO << "ERROR: GetThreadContext failed for thread ID "
				<< Compat::hex(threadId) << ": " << GetLastError();
			return;
		}

		auto& dr7 = reinterpret_cast<DR7&>(ctx.Dr7);

#define SET_BP(slot) \
    ctx.Dr##slot = reinterpret_cast<DWORD>(g_breakpoints[slot].address); \
    dr7.L##slot = g_breakpoints[slot].address ? 1: 0; \
    dr7.G##slot = 0; \
    dr7.RW##slot = static_cast<unsigned>(g_breakpoints[slot].condition); \
    dr7.LEN##slot = g_breakpoints[slot].length - 1

		SET_BP(0);
		SET_BP(1);
		SET_BP(2);
		SET_BP(3);
#undef SET_BP

		if (!SetThreadContext(thread, &ctx))
		{
			LOG_DEBUG << "ERROR: SetThreadContext failed for thread ID "
				<< Compat::hex(threadId) << ": " << GetLastError();
		}
	}

	LONG WINAPI vectoredExceptionHandler(EXCEPTION_POINTERS* ExceptionInfo)
	{
		if (!ExceptionInfo || !ExceptionInfo->ExceptionRecord || !ExceptionInfo->ContextRecord ||
			EXCEPTION_SINGLE_STEP != ExceptionInfo->ExceptionRecord->ExceptionCode ||
			!(ExceptionInfo->ContextRecord->Dr6 & 0xF))
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		auto& ctx = *ExceptionInfo->ContextRecord;
		for (unsigned slot = 0; slot < 4; ++slot)
		{
			if (!(ctx.Dr6 & (1 << slot)))
			{
				continue;
			}

			const auto bp = getBreakpointInfo(ctx, slot);
			Compat::Log log(Config::Settings::LogLevel::INFO);
			log << (Win32::Thread::BreakpointCondition::EXECUTION == bp.condition ? "Code" : "Data") << " breakpoint:"
				<< std::setfill('0') << std::hex
				<< " address=" << std::setw(8) << reinterpret_cast<DWORD>(bp.address)
				<< " eip=" << std::setw(8) << ctx.Eip
				<< " eax=" << std::setw(8) << ctx.Eax
				<< " ebx=" << std::setw(8) << ctx.Ebx
				<< " ecx=" << std::setw(8) << ctx.Ecx
				<< " edx=" << std::setw(8) << ctx.Edx
				<< " esi=" << std::setw(8) << ctx.Esi
				<< " edi=" << std::setw(8) << ctx.Edi
				<< " ebp=" << std::setw(8) << ctx.Ebp
				<< " esp=" << std::setw(8) << ctx.Esp
				<< std::dec << std::setfill(' ');

			if (Win32::Thread::BreakpointCondition::EXECUTION != bp.condition)
			{
				log << " value=";
				for (int i = bp.length - 1; i >= 0; --i)
				{
					log << Compat::HexByte(static_cast<BYTE*>(bp.address)[i]);
				}
			}
		}

		ctx.Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	bool useCpuAffinityForThread(const void* startAddress)
	{
		if (0 == g_cpuAffinity)
		{
			return false;
		}

		if (!startAddress)
		{
			return true;
		}

		HMODULE mod = Compat::getModuleHandleFromAddress(startAddress);
		if (!mod)
		{
			return true;
		}

		if (mod == Dll::g_currentModule)
		{
			return false;
		}

		auto path = Compat::getModulePath(mod);
		auto filename = path.filename();
		return !Compat::isEqual("dinput.dll", filename) &&
			!Compat::isEqual("dinput8.dll", filename) &&
			!Compat::isEqual("dsound.dll", filename) &&
			!Compat::isPrefix(Compat::getSystemPath() / "DriverStore" / "", path);
	}

	bool useCpuAffinityRotation()
	{
		Compat::ScopedCriticalSection lock(g_cs);
		return Config::cpuAffinityRotation.get() && rotateMask(g_cpuAffinity) != g_cpuAffinity;
	}

	PVOID WINAPI addVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
	{
		LOG_FUNC("AddVectoredExceptionHandler", First, Handler);
		const auto result = CALL_ORIG_FUNC(AddVectoredExceptionHandler)(First, Handler);
		if (result && First)
		{
			Compat::ScopedCriticalSection lock(g_cs);
			if (g_vectoredExceptionHandler)
			{
				const auto veh = CALL_ORIG_FUNC(AddVectoredExceptionHandler)(1, vectoredExceptionHandler);
				if (!veh)
				{
					LOG_ONCE("ERROR: Failed to reinstall vectored exception handler: " << GetLastError());
					return result;
				}
				RemoveVectoredExceptionHandler(g_vectoredExceptionHandler);
				g_vectoredExceptionHandler = veh;
			}
		}
		return result;
	}

	HANDLE WINAPI createThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
		LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
	{
		LOG_FUNC("CreateThread", lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
		if (0 == g_cpuAffinity)
		{
			return LOG_RESULT(CALL_ORIG_FUNC(CreateThread)(
				lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId));
		}

		const auto thread = CALL_ORIG_FUNC(CreateThread)(
			lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags | CREATE_SUSPENDED, lpThreadId);
		if (thread)
		{
			registerThread(GetThreadId(thread), "Thread started");
			if (!(dwCreationFlags & CREATE_SUSPENDED))
			{
				ResumeThread(thread);
			}
		}
		return LOG_RESULT(thread);
	}

	BOOL WINAPI setProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask)
	{
		LOG_FUNC("SetProcessAffinityMask", hProcess, Compat::hex(dwProcessAffinityMask));
		if (0 != Config::cpuAffinity.get())
		{
			return LOG_RESULT(TRUE);
		}
		return LOG_RESULT(CALL_ORIG_FUNC(SetProcessAffinityMask)(hProcess, dwProcessAffinityMask));
	}

	BOOL WINAPI setProcessPriorityBoost(HANDLE hProcess, BOOL bDisablePriorityBoost)
	{
		LOG_FUNC("SetProcessPriorityBoost", hProcess, bDisablePriorityBoost);
		if (Config::Settings::ThreadPriorityBoost::APP != Config::threadPriorityBoost.get())
		{
			return LOG_RESULT(TRUE);
		}
		return LOG_RESULT(CALL_ORIG_FUNC(SetProcessPriorityBoost)(hProcess, bDisablePriorityBoost));
	}

	BOOL WINAPI setThreadPriorityBoost(HANDLE hThread, BOOL bDisablePriorityBoost)
	{
		LOG_FUNC("SetThreadPriorityBoost", hThread, bDisablePriorityBoost);
		if (Config::Settings::ThreadPriorityBoost::APP != Config::threadPriorityBoost.get())
		{
			return LOG_RESULT(TRUE);
		}
		return LOG_RESULT(CALL_ORIG_FUNC(SetThreadPriorityBoost)(hThread, bDisablePriorityBoost));
	}
}

namespace Win32
{
	namespace Thread
	{
		void addBreakpoint(void* address, unsigned length, BreakpointCondition condition)
		{
			if (!address)
			{
				LOG_INFO << "ERROR: invalid address for breakpoint: " << address;
				return;
			}

			if (Win32::Thread::BreakpointCondition::EXECUTION == condition)
			{
				if (1 != length)
				{
					LOG_INFO << "ERROR: invalid length for code breakpoint: " << length;
					return;
				}
			}
			else if (4 != length && 2 != length && 1 != length)
			{
				LOG_INFO << "ERROR: invalid length for data breakpoint: " << length;
				return;
			}

			Compat::ScopedCriticalSection lock(g_cs);
			for (unsigned slot = 0; slot < 4; ++slot)
			{
				if (address == g_breakpoints[slot].address)
				{
					return;
				}
			}

			g_breakpoints[g_nextBreakpointSlot].address = address;
			g_breakpoints[g_nextBreakpointSlot].length = length;
			g_breakpoints[g_nextBreakpointSlot].condition = condition;
			g_nextBreakpointSlot = (g_nextBreakpointSlot + 1) % 4;

			for (const auto& threadInfo : g_threads)
			{
				if (threadInfo.second.debuggable)
				{
					updateThreadBreakpoints(threadInfo.first, threadInfo.second.handle);
				}
			}

			static bool vehAdded = false;
			if (!vehAdded)
			{
				g_vectoredExceptionHandler = CALL_ORIG_FUNC(AddVectoredExceptionHandler)(1, vectoredExceptionHandler);
				if (!g_vectoredExceptionHandler)
				{
					LOG_ONCE("ERROR: Failed to add vectored exception handler: " << GetLastError());
				}
				vehAdded = true;
			}
		}

		void applyConfig()
		{
			initNextProcessorMap(getProcessorSets());

			DWORD processMask = 0;
			DWORD systemMask = 0;
			GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
			if (0 == systemMask)
			{
				LOG_INFO << "ERROR: Failed to determine system CPU affinity mask";
			}

			g_cpuAffinity = Config::cpuAffinity.get();
			if (0 != g_cpuAffinity)
			{
				for (BYTE i = 0; i < 32; ++i)
				{
					if (g_nextProcessor.find(i) == g_nextProcessor.end())
					{
						g_cpuAffinity &= ~(1U << i);
					}
				}

				if (0 == g_cpuAffinity)
				{
					LOG_INFO << "Warning: CPU affinity setting doesn't match any existing logical cores, using default";
					g_cpuAffinity = 1;
				}
			}
			else
			{
				g_cpuAffinity = systemMask;
			}

			switch (Config::threadPriorityBoost.get())
			{
			case Config::Settings::ThreadPriorityBoost::OFF:
				CALL_ORIG_FUNC(SetProcessPriorityBoost)(GetCurrentProcess(), TRUE);
				break;

			case Config::Settings::ThreadPriorityBoost::ON:
				CALL_ORIG_FUNC(SetProcessPriorityBoost)(GetCurrentProcess(), FALSE);
				break;

			case Config::Settings::ThreadPriorityBoost::MAIN:
				CALL_ORIG_FUNC(SetProcessPriorityBoost)(GetCurrentProcess(), TRUE);
				CALL_ORIG_FUNC(SetThreadPriorityBoost)(GetCurrentThread(), FALSE);
				break;
			}

			LOG_INFO << "Process priority class: " << GetPriorityClass(GetCurrentProcess());
			LOG_INFO << "Current CPU affinity: " << maskToString(processMask);
			if (0 != g_cpuAffinity)
			{
				LOG_INFO << "Applying configured CPU affinity: " << maskToString(g_cpuAffinity);
				CALL_ORIG_FUNC(SetProcessAffinityMask)(GetCurrentProcess(), systemMask);
			}

			LOG_INFO << "CPU affinity rotation is " << (useCpuAffinityRotation() ? "enabled" : "disabled");

			registerExistingThreads();
			setProcessAffinity();
			Dll::createThread(&threadManagerProc, nullptr, THREAD_PRIORITY_TIME_CRITICAL);
		}

		void dllThreadAttach()
		{
			if (0 != g_cpuAffinity)
			{
				registerThread(GetCurrentThreadId(), "Thread started");
			}
		}

		void dllThreadDetach()
		{
			if (0 == g_cpuAffinity)
			{
				return;
			}

			Compat::ScopedCriticalSection lock(g_cs);
			auto it = g_threads.find(GetCurrentThreadId());
			if (it != g_threads.end())
			{
				logThreadInfo(it->second, "Thread stopped");
				CloseHandle(it->second.handle);
				g_threads.erase(it);
			}
		}

		void installHooks()
		{
			HOOK_FUNCTION(kernel32, AddVectoredExceptionHandler, addVectoredExceptionHandler);
			HOOK_FUNCTION(kernel32, CreateThread, createThread);
			HOOK_FUNCTION(kernel32, SetProcessAffinityMask, setProcessAffinityMask);
			HOOK_FUNCTION(kernel32, SetProcessPriorityBoost, setProcessPriorityBoost);
			HOOK_FUNCTION(kernel32, SetThreadPriorityBoost, setThreadPriorityBoost);

			Compat::hookIatFunction(Dll::g_origDDrawModule, "OpenMutexW", ddrawOpenMutexW);
			Compat::hookIatFunction(Dll::g_origDDrawModule, "WaitForSingleObject", ddrawWaitForSingleObject);

			g_ntQueryInformationThread = GET_PROC_ADDRESS(ntdll, NtQueryInformationThread);
			g_ntSetInformationThread = GET_PROC_ADDRESS(ntdll, NtSetInformationThread);
		}

		void skipWaitingForExclusiveModeMutex(bool skip)
		{
			g_skipWaitingForExclusiveModeMutex = skip;
		}
	}
}
