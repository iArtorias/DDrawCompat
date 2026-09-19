#pragma once

#include <ddraw.h>

#include <Common/CompatPtr.h>
#include <Common/CompatRef.h>
#include <Config/AtomicSetting.h>

namespace DDraw
{
	class PrimarySurface;
	class Surface;

	class RealPrimarySurface
	{
	public:
		RealPrimarySurface(CompatRef<IDirectDraw> dd, PrimarySurface& primary);

		void flip(CompatPtr<IDirectDrawSurface7> surfaceTargetOverride, DWORD flags);
		CompatPtr<IDirectDrawSurface7> getBackBuffer();
		HWND getDeviceWindow() { return m_deviceWindow; }
		HWND* getDeviceWindowPtr() { return m_deviceWindowPtr; }
		HRESULT getGammaRamp(DDGAMMARAMP* rampData);
		CompatPtr<IDirectDrawSurface7> getLastSurface();
		CompatWeakPtr<IDirectDrawSurface7> getSurface();
		bool isDShowPrimary();
		bool isFullscreen();
		bool isLost();
		HRESULT setGammaRamp(DDGAMMARAMP* rampData);
		void waitForFlip(CompatWeakPtr<IDirectDrawSurface7> surface);

		static int flush();
		static Config::AtomicSetting getFpsLimiter();
		static HWND getPresentationWindow();
		static HWND getTopmost();
		static void init();
		static bool isExclusiveFullscreen();
		static bool isProcessActive();
		static void scheduleOverlayUpdate();
		static void scheduleUpdate(bool allowFlush = false);
		static void scheduleUpdateWithPresentDelay(bool allowFlush = false);
		static void setPresentationWindowTopmost();
		static void setUpdateReady();
		static void suppressLost(bool suppress);
		static void updateFpsLimiter();
		static void waitForFlipFpsLimit(unsigned fpsLimit, bool doFlush = true);

	private:
		PrimarySurface& m_primary;
		CompatPtr<IDirectDrawSurface7> m_frontBuffer;
		HWND m_deviceWindow = nullptr;
		HWND* m_deviceWindowPtr = nullptr;
		Surface* m_lastFlipSurface = nullptr;
		bool m_isFullscreen = false;
		bool m_isDShowPrimary = false;
	};
}
