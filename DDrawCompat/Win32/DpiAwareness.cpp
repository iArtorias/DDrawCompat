#include <ShellScalingApi.h>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Config/Settings/DpiAwareness.h>
#include <Win32/DpiAwareness.h>

namespace
{
	DPI_AWARENESS_CONTEXT g_defaultContext = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;

	decltype(&AreDpiAwarenessContextsEqual) g_areDpiAwarenessContextsEqual = nullptr;
	decltype(&GetProcessDpiAwareness) g_getProcessDpiAwareness = nullptr;
	decltype(&GetThreadDpiAwarenessContext) g_getThreadDpiAwarenessContext = nullptr;
	decltype(&GetWindowDpiAwarenessContext) g_getWindowDpiAwarenessContext = nullptr;
	decltype(&IsValidDpiAwarenessContext) g_isValidDpiAwarenessContext = nullptr;
	decltype(&SetProcessDpiAwareness) g_setProcessDpiAwareness = nullptr;
	decltype(&SetProcessDpiAwarenessContext) g_setProcessDpiAwarenessContext = nullptr;
	decltype(&SetThreadDpiAwarenessContext) g_setThreadDpiAwarenessContext = nullptr;

	void logDpiAwareness(bool isSuccessful, DPI_AWARENESS_CONTEXT dpiAwareness, const char* funcName)
	{
		LOG_INFO << (isSuccessful ? "DPI awareness was successfully changed" : "Failed to change DPI awareness")
			<< " to \"" << dpiAwareness << "\" via " << funcName;
	}
}

namespace Win32
{
	ScopedDpiAwareness::ScopedDpiAwareness(DPI_AWARENESS_CONTEXT context)
		: m_prevContext(context ? DpiAwareness::setThreadContext(context) : nullptr)
	{
	}

	ScopedDpiAwareness::~ScopedDpiAwareness()
	{
		if (m_prevContext)
		{
			DpiAwareness::setThreadContext(m_prevContext);
		}
	}

	namespace DpiAwareness
	{
		DPI_AWARENESS_CONTEXT getDefaultContext()
		{
			return g_defaultContext;
		}

		DPI_AWARENESS_CONTEXT getThreadContext()
		{
			if (g_getThreadDpiAwarenessContext)
			{
				return g_getThreadDpiAwarenessContext();
			}

			if (g_getProcessDpiAwareness)
			{
				PROCESS_DPI_AWARENESS awareness = PROCESS_DPI_UNAWARE;
				if (SUCCEEDED(g_getProcessDpiAwareness(nullptr, &awareness)))
				{
					switch (awareness)
					{
					case PROCESS_DPI_UNAWARE:
						return DPI_AWARENESS_CONTEXT_UNAWARE;
					case PROCESS_SYSTEM_DPI_AWARE:
						return DPI_AWARENESS_CONTEXT_SYSTEM_AWARE;
					case PROCESS_PER_MONITOR_DPI_AWARE:
						return DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
					}
				}

			}

			return IsProcessDPIAware() ? DPI_AWARENESS_CONTEXT_SYSTEM_AWARE : DPI_AWARENESS_CONTEXT_UNAWARE;
		}

		void init()
		{
			LoadLibraryA("shcore");

			g_areDpiAwarenessContextsEqual = GET_PROC_ADDRESS(user32, AreDpiAwarenessContextsEqual);
			g_getProcessDpiAwareness = GET_PROC_ADDRESS(shcore, GetProcessDpiAwareness);
			g_getThreadDpiAwarenessContext = GET_PROC_ADDRESS(user32, GetThreadDpiAwarenessContext);
			g_getWindowDpiAwarenessContext = GET_PROC_ADDRESS(user32, GetWindowDpiAwarenessContext);
			g_isValidDpiAwarenessContext = GET_PROC_ADDRESS(user32, IsValidDpiAwarenessContext);
			g_setProcessDpiAwareness = GET_PROC_ADDRESS(shcore, SetProcessDpiAwareness);
			g_setProcessDpiAwarenessContext = GET_PROC_ADDRESS(user32, SetProcessDpiAwarenessContext);
			g_setThreadDpiAwarenessContext = GET_PROC_ADDRESS(user32, SetThreadDpiAwarenessContext);

			LOG_INFO << "Initial DPI awareness: " << getThreadContext();

			struct SetDefaultContext
			{
				~SetDefaultContext() { g_defaultContext = getThreadContext(); }
			};
			SetDefaultContext setDefaultContext;

			auto dpiAwareness = Config::dpiAwareness.get();
			if (!dpiAwareness)
			{
				return;
			}

			if (g_isValidDpiAwarenessContext && g_setProcessDpiAwarenessContext)
			{
				if (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == dpiAwareness &&
					!g_isValidDpiAwarenessContext(dpiAwareness))
				{
					dpiAwareness = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
				}

				if (DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED == dpiAwareness &&
					!g_isValidDpiAwarenessContext(dpiAwareness))
				{
					dpiAwareness = DPI_AWARENESS_CONTEXT_UNAWARE;
				}

				logDpiAwareness(g_setProcessDpiAwarenessContext(dpiAwareness), dpiAwareness, "SetProcessDpiAwarenessContext");
				return;
			}

			if (g_setProcessDpiAwareness)
			{
				HRESULT result = S_OK;
				if (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE == dpiAwareness ||
					DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == dpiAwareness)
				{
					dpiAwareness = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
					result = g_setProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
				}
				else if (DPI_AWARENESS_CONTEXT_SYSTEM_AWARE == dpiAwareness)
				{
					result = g_setProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);
				}
				else
				{
					dpiAwareness = DPI_AWARENESS_CONTEXT_UNAWARE;
					result = g_setProcessDpiAwareness(PROCESS_DPI_UNAWARE);
				}

				logDpiAwareness(SUCCEEDED(result), dpiAwareness, "SetProcessDpiAwareness");
				return;
			}

			if (DPI_AWARENESS_CONTEXT_UNAWARE == dpiAwareness ||
				DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED == dpiAwareness)
			{
				LOG_INFO << "DPI awareness was not changed";
			}
			else
			{
				logDpiAwareness(SetProcessDPIAware(), DPI_AWARENESS_CONTEXT_SYSTEM_AWARE, "SetProcessDPIAware");
			}
		}

		bool isEqual(DPI_AWARENESS_CONTEXT lhs, DPI_AWARENESS_CONTEXT rhs)
		{
			return g_areDpiAwarenessContextsEqual ? g_areDpiAwarenessContextsEqual(lhs, rhs) : (lhs == rhs);
		}

		bool isMixedModeSupported()
		{
			return g_setThreadDpiAwarenessContext;
		}

		bool isMixedWindow(HWND hwnd)
		{
			if (g_getWindowDpiAwarenessContext && g_areDpiAwarenessContextsEqual)
			{
				return !g_areDpiAwarenessContextsEqual(g_getWindowDpiAwarenessContext(hwnd), g_defaultContext);
			}
			return false;
		}

		DPI_AWARENESS_CONTEXT setThreadContext(DPI_AWARENESS_CONTEXT context)
		{
			return g_setThreadDpiAwarenessContext ? g_setThreadDpiAwarenessContext(context) : nullptr;
		}
	}
}
