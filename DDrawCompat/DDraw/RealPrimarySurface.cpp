#include <memory>

#include <Windows.h>
#include <VersionHelpers.h>

#include <Common/CompatPtr.h>
#include <Common/Hook.h>
#include <Common/HResultException.h>
#include <Common/ScopedCriticalSection.h>
#include <Common/ScopedThreadPriority.h>
#include <Common/Time.h>
#include <Config/AtomicSetting.h>
#include <Config/Settings/FpsLimiter.h>
#include <Config/Settings/FullscreenMode.h>
#include <Config/Settings/GdiInterops.h>
#include <Config/Settings/PresentDelay.h>
#include <Config/Settings/VSync.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/KernelModeThunks.h>
#include <D3dDdi/Resource.h>
#include <D3dDdi/ScopedCriticalSection.h>
#include <D3dDdi/SurfaceRepository.h>
#include <DDraw/DirectDraw.h>
#include <DDraw/DirectDrawSurface.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/ScopedThreadLock.h>
#include <DDraw/Surfaces/OverlaySurface.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Surfaces/TagSurface.h>
#include <Gdi/Caret.h>
#include <Gdi/Cursor.h>
#include <Gdi/DcFunctions.h>
#include <Gdi/GuiThread.h>
#include <Gdi/VirtualScreen.h>
#include <Gdi/Window.h>
#include <Input/Input.h>
#include <Overlay/ConfigWindow.h>
#include <Overlay/StatsWindow.h>
#include <Overlay/Steam.h>
#include <Win32/DisplayMode.h>
#include <Win32/DpiAwareness.h>

namespace
{
	void updatePresentationParams();

	CompatWeakPtr<IDirectDrawClipper> g_clipper;

	bool g_isExclusiveFullscreen = false;
	bool g_suppressLost = false;

	Compat::CriticalSection g_presentCs;
	bool g_isOverlayUpdatePending = false;
	bool g_isUpdatePending = false;
	bool g_isUpdateReady = false;
	long long g_qpcUpdateStart = 0;
	long long g_qpcLastDelayedUpdate = 0;

	HWND g_presentationWindow = nullptr;
	HWND g_prevPresentationWindow = nullptr;
	long long g_qpcPrevPresentationWindow = 0;

	Config::AtomicSettingStore g_fpsLimiter(Config::fpsLimiter);

	UINT getFlipInterval(DWORD flags)
	{
		switch (Config::vSync.get())
		{
		case Config::Settings::VSync::OFF:
			return 0;
		case Config::Settings::VSync::WAIT:
		case Config::Settings::VSync::ON:
			return Config::vSync.getParam();
		}

		if (flags & DDFLIP_NOVSYNC)
		{
			return 0;
		}

		if (flags & (DDFLIP_INTERVAL2 | DDFLIP_INTERVAL3 | DDFLIP_INTERVAL4))
		{
			UINT flipInterval = (flags & (DDFLIP_INTERVAL2 | DDFLIP_INTERVAL3 | DDFLIP_INTERVAL4)) >> 24;
			if (flipInterval < 2 || flipInterval > 4)
			{
				flipInterval = 1;
			}
			return flipInterval;
		}

		return 1;
	}

	UINT getMaxPresentDelay()
	{
		if (Config::Settings::PresentDelay::AUTO == Config::presentDelay.get())
		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			if (Time::qpcToMs(Time::queryPerformanceCounter() - g_qpcLastDelayedUpdate) > 500)
			{
				return 0;
			}
		}
		return Config::presentDelay.getParam();
	}

	void logBypassExclusiveModeCheck()
	{
		LOG_ONCE("Using feature: bypassing exclusive mode check for GOG.com DirectShow wrapper (dshow.dll)");
	}

	void presentationBlt(CompatRef<IDirectDrawSurface7> dst, CompatWeakPtr<IDirectDrawSurface7> src)
	{
		LOG_FUNC("RealPrimarySurface::presentationBlt", &dst.get(), src.get());
		D3dDdi::ScopedCriticalSection lock;
		auto srcResource = src ? D3dDdi::Device::findResource(
			DDraw::DirectDrawSurface::getDriverResourceHandle(*src)) : nullptr;
		auto dstResource = D3dDdi::Device::findResource(
			DDraw::DirectDrawSurface::getDriverResourceHandle(dst.get()));
		if (src && !srcResource || !dstResource)
		{
			return;
		}

		D3DDDIARG_BLT blt = {};
		if (srcResource)
		{
			blt.hSrcResource = *srcResource;
			blt.SrcSubResourceIndex = DDraw::DirectDrawSurface::getSubResourceIndex(*src);
			blt.SrcRect = srcResource->getRect(blt.SrcSubResourceIndex);
		}
		blt.hDstResource = *dstResource;
		blt.DstSubResourceIndex = DDraw::DirectDrawSurface::getSubResourceIndex(dst.get());
		blt.DstRect = dstResource->getRect(blt.DstSubResourceIndex);
		dstResource->presentationBlt(blt, srcResource);
	}

	void present(DDraw::PrimarySurface* primary, bool isOverlayOnly)
	{
		LOG_FUNC("RealPrimarySurface::present", primary, isOverlayOnly);

		Gdi::VirtualScreen::update();

		Gdi::GuiThread::execute([&]()
			{
				auto statsWindow = Gdi::GuiThread::getStatsWindow();
				if (statsWindow)
				{
					if (!isOverlayOnly)
					{
						statsWindow->m_presentCount++;
						statsWindow->m_present.add();
					}
					statsWindow->update();
				}

				auto configWindow = Gdi::GuiThread::getConfigWindow();
				if (configWindow)
				{
					configWindow->update();
				}

				auto capture = Input::getCaptureWindow();
				if (capture)
				{
					capture->update();
				}

				Input::updateCursor();
			});

		auto src = primary ? primary->getDDS() : nullptr;
		if (primary)
		{
			auto composedSrc = DDraw::OverlaySurface::compose( *primary );
			if (composedSrc)
			{
				src = composedSrc;
			}
		}

		const auto presentationWindow = g_presentationWindow ? g_presentationWindow : g_prevPresentationWindow;
		if (!src && !presentationWindow)
		{
			LOG_DEBUG << "Present mode: windowed GDI";
			Gdi::Window::present(nullptr);
			return;
		}

		auto realPrimary = primary ? primary->getRealPrimary() : nullptr;
		const bool useFlip = src && realPrimary && realPrimary->isFullscreen();
		Win32::DisplayMode::MonitorInfo mi = {};
		CompatWeakPtr<IDirectDrawSurface7> frontBuffer;
		CompatPtr<IDirectDrawSurface7> backBuffer;
		CompatPtr<IDirectDrawSurface7> windowedSrc;

		if (primary && realPrimary)
		{
			mi = primary->getMonitorInfo();
			frontBuffer = realPrimary->getSurface();
			if (realPrimary->isFullscreen())
			{
				backBuffer = realPrimary->getBackBuffer();
			}
		}
		else
		{
			mi = Win32::DisplayMode::getMonitorInfo(CALL_ORIG_FUNC(MonitorFromWindow)(presentationWindow, MONITOR_DEFAULTTOPRIMARY));
			if (!DDraw::TagSurface::findFullscreenWindow())
			{
				frontBuffer = D3dDdi::SurfaceRepository::getPrimaryRepo().getWindowedPrimary();
			}
		}

		if (presentationWindow && !backBuffer)
		{
			D3dDdi::SurfaceRepository* repo = nullptr;
			if (primary)
			{
				repo = DDraw::DirectDrawSurface::getSurfaceRepository(*primary->getDDS());
			}
			else
			{
				repo = &D3dDdi::SurfaceRepository::getPrimaryRepo();
				if (Config::gdiInterops.anyRedirects())
				{
					windowedSrc = repo->getWindowedSrc(mi.rcEmulated);
					if (!windowedSrc)
					{
						return;
					}
					src = windowedSrc;
				}
			}

			backBuffer = repo->getWindowedBackBuffer(
				mi.rcDpiAware.right - mi.rcDpiAware.left, mi.rcDpiAware.bottom - mi.rcDpiAware.top);
			if (!backBuffer)
			{
				return;
			}
		}

		Gdi::Region excludeRegion(mi.rcEmulated);
		Gdi::Window::present(excludeRegion);
		if (backBuffer)
		{
			presentationBlt(*backBuffer, src);
		}

		if (useFlip)
		{
			if (g_isExclusiveFullscreen)
			{
				LOG_DEBUG << "Present mode: exclusive fullscreen flip";
				const HRESULT result = frontBuffer->Flip(frontBuffer, backBuffer, DDFLIP_WAIT);
				if (FAILED(result))
				{
					LOG_DEBUG << "ERROR: exclusive fullscreen flip failed: " << Compat::hex(result);
				}
			}
			else
			{
				LOG_DEBUG << "Present mode: borderless fullscreen flip";
				*realPrimary->getDeviceWindowPtr() = presentationWindow;
				const HRESULT result = frontBuffer->Flip(frontBuffer, nullptr, DDFLIP_WAIT);
				*realPrimary->getDeviceWindowPtr() = realPrimary->getDeviceWindow();
				if (FAILED(result))
				{
					LOG_DEBUG << "ERROR: borderless fullscreen flip failed: " << Compat::hex(result);
				}
			}
		}
		else if (frontBuffer)
		{
			if (!g_clipper)
			{
				CALL_ORIG_PROC(DirectDrawCreateClipper)(0, &g_clipper.getRef(), nullptr);
			}
			frontBuffer->SetClipper(frontBuffer, g_clipper);

			if (presentationWindow)
			{
				LOG_DEBUG << "Present mode: windowed fullscreen blt";
				g_clipper->SetHWnd(g_clipper, 0, presentationWindow);
				const HRESULT result = frontBuffer->Blt(frontBuffer, nullptr, backBuffer, nullptr, DDBLT_WAIT, nullptr);
				if (FAILED(result))
				{
					LOG_DEBUG << "ERROR: windowed fullscreen blt failed: " << Compat::hex(result);
				}
			}
			else if (src)
			{
				LOG_DEBUG << "Present mode: windowed blt";
				Gdi::Window::present(*frontBuffer, *src, *g_clipper, mi.rcEmulated);
			}
		}
		else
		{
			LOG_DEBUG << "Present mode: windowed fullscreen GDI";
			HDC dstDc = GetWindowDC(presentationWindow);
			HDC srcDc = nullptr;
			D3dDdi::Resource::setReadOnlyLock(true);
			backBuffer->GetDC(backBuffer, &srcDc);
			D3dDdi::Resource::setReadOnlyLock(false);
			CALL_ORIG_FUNC(BitBlt)(dstDc, 0, 0, mi.rcDpiAware.right - mi.rcDpiAware.left, mi.rcDpiAware.bottom - mi.rcDpiAware.top,
				srcDc, 0, 0, SRCCOPY);
			backBuffer->ReleaseDC(backBuffer, srcDc);
			ReleaseDC(presentationWindow, dstDc);
		}
	}

	void setFullscreenPresentationMode(const Win32::DisplayMode::MonitorInfo& mi)
	{
		static Win32::DisplayMode::MonitorInfo prevMi = {};
		const bool isCursorEmulated = !IsRectEmpty(&mi.rcEmulated) && !Overlay::Steam::isOverlayOpen();
		Gdi::Cursor::setEmulated(isCursorEmulated);
		if (0 != memcmp(&mi, &prevMi, sizeof(mi)))
		{
			Gdi::Cursor::setMonitorClipRect(mi.rcEmulated);
			Gdi::DcFunctions::setFullscreenMonitorInfo(mi);
			Input::setFullscreenMonitorInfo(mi);
			prevMi = mi;
		}
		Gdi::Window::setFullscreenMode(0 != mi.cbSize);
	}

	void updateNow(DDraw::PrimarySurface* src, bool isOverlayOnly)
	{
		updatePresentationParams();

		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			g_isOverlayUpdatePending = false;
			g_isUpdatePending = false;
			g_isUpdateReady = false;
		}

		present(src, isOverlayOnly);
		D3dDdi::KernelModeThunks::setPresentEndVsyncCount();
	}

	void updatePresentationParams()
	{
		LOG_FUNC("RealPrimarySurface::updatePresentationParams");

		HWND fullscreenWindow = nullptr;
		if (DDraw::RealPrimarySurface::isProcessActive())
		{
			const auto mainPrimary = DDraw::TagSurface::getMainPrimary();
			const auto realPrimary = mainPrimary ? mainPrimary->getRealPrimary() : nullptr;
			if (realPrimary && realPrimary->isFullscreen() &&
				IsWindowVisible(realPrimary->getDeviceWindow()) && !IsIconic(realPrimary->getDeviceWindow()))
			{
				fullscreenWindow = realPrimary->getDeviceWindow();
			}
			else
			{
				fullscreenWindow = Gdi::Window::getFullscreenWindow();
			}
		}
		else
		{
			DDraw::PrimarySurface* fullscreenPrimary = nullptr;
			DDraw::TagSurface::enumTagSurfaces([&](DDraw::TagSurface& tagSurface)
				{
					auto primary = tagSurface.getPrimary();
					if (primary && primary->getRealPrimary() && primary->getRealPrimary()->isFullscreen())
					{
						fullscreenPrimary = primary;
						return false;
					}
					return true;
				});

			if (fullscreenPrimary)
			{
				setFullscreenPresentationMode({});
				if (g_prevPresentationWindow)
				{
					Gdi::GuiThread::destroyWindow(g_prevPresentationWindow);
					g_prevPresentationWindow = nullptr;
				}
				return;
			}
		}

		HWND fullscreenPresentationWindow = nullptr;
		if (fullscreenWindow)
		{
			Gdi::Window::setDpiAwareness(fullscreenWindow, true);
			fullscreenPresentationWindow = Gdi::Window::getPresentationWindow(fullscreenWindow);
		}

		g_presentationWindow = fullscreenPresentationWindow;

		static HWND prevFullscreenWindow = nullptr;
		if (prevFullscreenWindow != fullscreenWindow)
		{
			LOG_DEBUG << "Fullscreen window changed: " << fullscreenWindow;
			if (prevFullscreenWindow)
			{
				g_prevPresentationWindow = Gdi::Window::getPresentationWindow(prevFullscreenWindow);
				g_qpcPrevPresentationWindow = Time::queryPerformanceCounter();
				Gdi::Window::setDpiAwareness(prevFullscreenWindow, false);
				const HWND prevFullscreenPresentationWindow = Gdi::Window::getPresentationWindow(prevFullscreenWindow);
				if (prevFullscreenPresentationWindow)
				{
					Gdi::Window::updatePresentationWindowPos(prevFullscreenPresentationWindow, prevFullscreenWindow);
				}
			}
		}
		prevFullscreenWindow = fullscreenWindow;

		if (g_presentationWindow)
		{
			const auto mi = Win32::DisplayMode::getMonitorInfo(CALL_ORIG_FUNC(MonitorFromWindow)(fullscreenWindow, MONITOR_DEFAULTTOPRIMARY));
			const auto& mr = mi.rcDpiAware;

			Gdi::GuiThread::execute([&]()
				{
					Win32::ScopedDpiAwareness dpiAwareness;
					CALL_ORIG_FUNC(SetWindowPos)(g_presentationWindow, nullptr, mr.left, mr.top, 0, 0,
						SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | SWP_NOSIZE);
					CALL_ORIG_FUNC(SetWindowPos)(g_presentationWindow, nullptr, 0, 0, mr.right - mr.left, mr.bottom - mr.top,
						SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE);
				});

			setFullscreenPresentationMode(mi);
		}
		else
		{
			setFullscreenPresentationMode({});
		}

		if (g_prevPresentationWindow &&
			(g_presentationWindow || Time::qpcToMs(Time::queryPerformanceCounter() - g_qpcPrevPresentationWindow) >= 500))
		{
			Gdi::GuiThread::destroyWindow(g_prevPresentationWindow);
			g_prevPresentationWindow = nullptr;
			DDraw::RealPrimarySurface::scheduleOverlayUpdate();
		}
	}

	unsigned WINAPI updateThreadProc(LPVOID /*lpParameter*/)
	{
		int msUntilUpdateReady = 0;
		while (true)
		{
			if (msUntilUpdateReady > 0)
			{
				Sleep(1);
			}
			else
			{
				D3dDdi::KernelModeThunks::waitForPresentEnd();
			}

			DDraw::ScopedThreadLock lock;
			msUntilUpdateReady = DDraw::RealPrimarySurface::flush();
		}
	}
}

namespace DDraw
{
	RealPrimarySurface::RealPrimarySurface(CompatRef<IDirectDraw> dd, PrimarySurface& primary)
		: m_primary(primary)
	{
		LOG_FUNC("RealPrimarySurface::RealPrimarySurface", &dd, &primary);

		DDSURFACEDESC desc = {};
		desc.dwSize = sizeof(desc);
		desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
		desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
		desc.dwBackBufferCount = g_isExclusiveFullscreen ? 2 : 1;

		CompatPtr<IDirectDrawSurface> surface;
		HRESULT result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);

		if (DDERR_NOEXCLUSIVEMODE == result)
		{
			desc.dwFlags = DDSD_CAPS;
			desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
			desc.dwBackBufferCount = 0;
			result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);

			if (DDERR_NOEXCLUSIVEMODE == result && GetModuleHandle("dshow.dll"))
			{
				logBypassExclusiveModeCheck();
				auto ddLcl = DirectDraw::getInt(dd.get()).lpLcl;
				auto& exclusiveOwner = ddLcl->lpGbl->lpExclusiveOwner;
				auto prevExclusiveOwner = exclusiveOwner;
				exclusiveOwner = ddLcl;
				result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);
				exclusiveOwner = prevExclusiveOwner;
				m_isDShowPrimary = true;
			}
		}

		if (FAILED(result))
		{
			throw HResultException(result);
		}

		auto ddLcl = DDraw::DirectDraw::getInt(dd.get()).lpLcl;
		auto tagSurface = DDraw::TagSurface::get(ddLcl);
		if (!tagSurface)
		{
			LOG_ONCE("ERROR: TagSurface not found");
			throw HResultException(DDERR_GENERIC);
		}

		m_isFullscreen = 0 != desc.dwBackBufferCount;

		m_frontBuffer = CompatPtr<IDirectDrawSurface7>::from(surface.get());
		
		m_deviceWindowPtr = (0 != desc.dwBackBufferCount) ? DDraw::DirectDraw::getDeviceWindowPtr(dd.get()) : nullptr;
		m_deviceWindow = m_deviceWindowPtr ? *m_deviceWindowPtr : nullptr;

		if (g_isExclusiveFullscreen && (desc.ddsCaps.dwCaps & DDSCAPS_FLIP))
		{
			m_frontBuffer->Flip(m_frontBuffer, getLastSurface(), DDFLIP_WAIT);
			D3dDdi::KernelModeThunks::setPresentEndVsyncCount();
			D3dDdi::KernelModeThunks::waitForPresentEnd();
		}
	}

	void RealPrimarySurface::flip(CompatPtr<IDirectDrawSurface7> surfaceTargetOverride, DWORD flags)
	{
		const DWORD flipInterval = getFlipInterval(flags);
		for (DWORD i = flipInterval; i > 1; --i)
		{
			{
				Compat::ScopedCriticalSection lock(g_presentCs);
				g_isUpdatePending = true;
				g_isUpdateReady = true;
			}
			flush();
			D3dDdi::KernelModeThunks::waitForPresentEnd();
		}

		D3dDdi::KernelModeThunks::setFlipEndVsyncCount();

		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			scheduleUpdate();
			if (0 != flipInterval)
			{
				m_lastFlipSurface = Surface::getSurface(
					surfaceTargetOverride ? *surfaceTargetOverride : *m_primary.getLastSurface());
			}
			else
			{
				m_lastFlipSurface = nullptr;
			}
		}

		if (Config::Settings::VSync::WAIT == Config::vSync.get())
		{
			scheduleUpdate(true);
			D3dDdi::KernelModeThunks::waitForFlipEnd();
		}
		else if (0 == getMaxPresentDelay())
		{
			scheduleUpdate(true);
		}
	}

	int RealPrimarySurface::flush()
	{
		if (D3dDdi::KernelModeThunks::isPresentPending())
		{
			return -1;
		}

		static int lastOverlayCheckVsyncCount = 0;
		auto vsyncCount = D3dDdi::KernelModeThunks::getVsyncCounter();
		if (vsyncCount != lastOverlayCheckVsyncCount)
		{
			updatePresentationParams();
			Gdi::Cursor::update();
			Gdi::Caret::blink();
			auto statsWindow = Gdi::GuiThread::getStatsWindow();
			if (statsWindow && statsWindow->isVisible())
			{
				statsWindow->updateStats();
			}
			Overlay::Steam::flush();
			for (const auto& device : D3dDdi::Device::getDevices())
			{
				const auto repo = &device.second.getRepo();
				if (repo)
				{
					repo->clearReleasedSurfaces();
				}
			}
			lastOverlayCheckVsyncCount = vsyncCount;
		}

		bool isOverlayOnly = false;

		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			if (!g_isUpdateReady)
			{
				if (g_isUpdatePending)
				{
					auto msSinceUpdateStart = Time::qpcToMs(Time::queryPerformanceCounter() - g_qpcUpdateStart);
					const auto presentDelay = getMaxPresentDelay();
					if (msSinceUpdateStart < presentDelay)
					{
						return presentDelay - static_cast<int>(msSinceUpdateStart);
					}
					g_isUpdateReady = true;
				}
				else if (g_isOverlayUpdatePending)
				{
					g_isUpdateReady = true;
					isOverlayOnly = true;
				}
				else
				{
					return 1;
				}
			}
		}

		auto primarySurface = TagSurface::getMainPrimary();
		auto primary = primarySurface ? primarySurface->getDDS() : nullptr;
		auto realPrimary = primary ? primarySurface->getRealPrimary() : nullptr;
		const auto frontBuffer = realPrimary ? realPrimary->getSurface() : nullptr;
		if (!primary || FAILED(primary->IsLost(primary)) ||
			!frontBuffer || FAILED(frontBuffer->IsLost(frontBuffer)))
		{
			primarySurface = nullptr;
			primary = nullptr;
		}

		updateNow(primarySurface, isOverlayOnly);

		RECT emptyRect = {};
		HRESULT result = primary ? primary->BltFast(primary, 0, 0, primary, &emptyRect, DDBLTFAST_WAIT) : DD_OK;
		if (DDERR_SURFACEBUSY == result || DDERR_LOCKEDSURFACES == result)
		{
			scheduleUpdate();
		}

		return 1;
	}

	CompatPtr<IDirectDrawSurface7> RealPrimarySurface::getBackBuffer()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_BACKBUFFER;
		CompatPtr<IDirectDrawSurface7> backBuffer;
		if (m_frontBuffer)
		{
			m_frontBuffer->GetAttachedSurface(m_frontBuffer, &caps, &backBuffer.getRef());
		}
		return backBuffer;
	}

	CompatPtr<IDirectDrawSurface7> RealPrimarySurface::getLastSurface()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_FLIP;
		CompatPtr<IDirectDrawSurface7> backBuffer(getBackBuffer());
		CompatPtr<IDirectDrawSurface7> lastSurface;
		if (backBuffer)
		{
			backBuffer->GetAttachedSurface(backBuffer, &caps, &lastSurface.getRef());
		}
		return lastSurface;
	}

	Config::AtomicSetting RealPrimarySurface::getFpsLimiter()
	{
		return g_fpsLimiter.get();
	}

	HRESULT RealPrimarySurface::getGammaRamp(DDGAMMARAMP* rampData)
	{
		DDraw::ScopedThreadLock lock;
		auto gammaControl(CompatPtr<IDirectDrawGammaControl>::from(m_frontBuffer.get()));
		if (!gammaControl)
		{
			return DDERR_INVALIDPARAMS;
		}

		return gammaControl->GetGammaRamp(gammaControl, 0, rampData);
	}

	HWND RealPrimarySurface::getPresentationWindow()
	{
		return g_presentationWindow;
	}

	CompatWeakPtr<IDirectDrawSurface7> RealPrimarySurface::getSurface()
	{
		return m_frontBuffer;
	}

	HWND RealPrimarySurface::getTopmost()
	{
		return g_presentationWindow ? g_presentationWindow : (g_prevPresentationWindow ? g_prevPresentationWindow : HWND_TOPMOST);
	}

	void RealPrimarySurface::init()
	{
		g_isExclusiveFullscreen = Config::Settings::FullscreenMode::EXCLUSIVE == Config::fullscreenMode.get() ||
			!IsWindows8OrGreater();
		Dll::createThread(&updateThreadProc, nullptr, THREAD_PRIORITY_TIME_CRITICAL);
	}

	bool RealPrimarySurface::isDShowPrimary()
	{
		return m_isDShowPrimary;
	}

	bool RealPrimarySurface::isExclusiveFullscreen()
	{
		return g_isExclusiveFullscreen;
	}

	bool RealPrimarySurface::isFullscreen()
	{
		return m_isFullscreen;
	}

	bool RealPrimarySurface::isLost()
	{
		DDraw::ScopedThreadLock lock;
		return m_frontBuffer && !g_suppressLost && DDERR_SURFACELOST == m_frontBuffer->IsLost(m_frontBuffer);
	}

	bool RealPrimarySurface::isProcessActive()
	{
		const HWND foregroundWindow = GetForegroundWindow();
		if (foregroundWindow)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(foregroundWindow, &pid);
			return GetCurrentProcessId() == pid;
		}
		return false;
	}

	void RealPrimarySurface::scheduleOverlayUpdate()
	{
		Compat::ScopedCriticalSection lock(g_presentCs);
		g_isOverlayUpdatePending = true;
	}

	void RealPrimarySurface::scheduleUpdate(bool allowFlush)
	{
		const bool isPresentDelayEnabled = 0 != getMaxPresentDelay();

		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			if (!g_isUpdatePending)
			{
				g_qpcUpdateStart = Time::queryPerformanceCounter();
				g_isUpdatePending = true;
			}
			g_isUpdateReady = !isPresentDelayEnabled;
		}

		if (allowFlush && !isPresentDelayEnabled)
		{
			flush();
		}
	}

	void RealPrimarySurface::scheduleUpdateWithPresentDelay(bool allowFlush)
	{
		if (Config::Settings::PresentDelay::AUTO == Config::presentDelay.get())
		{
			Compat::ScopedCriticalSection lock(g_presentCs);
			g_qpcLastDelayedUpdate = Time::queryPerformanceCounter();
		}
		scheduleUpdate(allowFlush);
	}

	HRESULT RealPrimarySurface::setGammaRamp(DDGAMMARAMP* rampData)
	{
		DDraw::ScopedThreadLock lock;
		auto gammaControl(CompatPtr<IDirectDrawGammaControl>::from(m_frontBuffer.get()));
		if (!gammaControl)
		{
			return DDERR_INVALIDPARAMS;
		}

		D3dDdi::KernelModeThunks::enableWaitForGammaRamp(true);
		HRESULT result = gammaControl->SetGammaRamp(gammaControl, 0, rampData);
		D3dDdi::KernelModeThunks::enableWaitForGammaRamp(false);
		return result;
	}

	void RealPrimarySurface::setPresentationWindowTopmost()
	{
		const auto presentationWindow = g_presentationWindow ? g_presentationWindow : g_prevPresentationWindow;
		if (presentationWindow && IsWindowVisible(presentationWindow))
		{
			Gdi::GuiThread::execute([&]()
				{
					const bool isOwnerVisible = IsWindowVisible(CALL_ORIG_FUNC(GetWindow)(presentationWindow, GW_OWNER));
					CALL_ORIG_FUNC(SetWindowPos)(presentationWindow, HWND_TOPMOST, 0, 0, 0, 0,
						SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOOWNERZORDER |
						(isOwnerVisible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
				});
		}
	}

	void RealPrimarySurface::setUpdateReady()
	{
		Compat::ScopedCriticalSection lock(g_presentCs);
		if (g_isUpdatePending)
		{
			g_isUpdateReady = true;
		}
	}

	void RealPrimarySurface::suppressLost(bool suppress)
	{
		g_suppressLost = suppress;
	}

	void RealPrimarySurface::updateFpsLimiter()
	{
		g_fpsLimiter.update();
	}

	void RealPrimarySurface::waitForFlip(CompatWeakPtr<IDirectDrawSurface7> surface)
	{
		if (!surface || !m_lastFlipSurface ||
			surface != m_primary.getDDS() && surface != m_lastFlipSurface->getDDS() ||
			!D3dDdi::KernelModeThunks::isFlipPending())
		{
			return;
		}

		auto qpcStart = Time::queryPerformanceCounter();
		D3dDdi::KernelModeThunks::waitForFlipEnd();

		Compat::ScopedCriticalSection lock(g_presentCs);
		auto qpcEnd = Time::queryPerformanceCounter();
		if (g_isUpdatePending)
		{
			g_qpcUpdateStart += qpcEnd - qpcStart;
		}
	}

	void RealPrimarySurface::waitForFlipFpsLimit(unsigned fpsLimit, bool doFlush)
	{
		static long long g_qpcPrevWaitEnd = Time::queryPerformanceCounter() - Time::g_qpcFrequency;
		auto qpcNow = Time::queryPerformanceCounter();
		auto qpcWaitEnd = g_qpcPrevWaitEnd + Time::g_qpcFrequency / fpsLimit;
		if (qpcNow - qpcWaitEnd >= 0)
		{
			g_qpcPrevWaitEnd = qpcNow;
			return;
		}
		g_qpcPrevWaitEnd = qpcWaitEnd;

		Compat::ScopedThreadPriority prio(THREAD_PRIORITY_TIME_CRITICAL);
		while (Time::qpcToMs(qpcWaitEnd - qpcNow) > 0)
		{
			Time::waitForNextTick();
			if (doFlush)
			{
				flush();
			}
			qpcNow = Time::queryPerformanceCounter();
		}

		while (qpcWaitEnd - qpcNow > 0)
		{
			qpcNow = Time::queryPerformanceCounter();
		}
	}
}
