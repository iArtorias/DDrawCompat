#include <Common/CompatPtr.h>
#include <Config/Settings/PresentDelay.h>
#include <Config/Settings/FpsLimiter.h>
#include <Config/Settings/GdiInterops.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/Resource.h>
#include <D3dDdi/ScopedCriticalSection.h>
#include <DDraw/DirectDrawClipper.h>
#include <DDraw/DirectDrawPalette.h>
#include <DDraw/DirectDrawSurface.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Surfaces/PrimarySurfaceImpl.h>
#include <Dll/Dll.h>
#include <Gdi/Gdi.h>
#include <Gdi/GuiThread.h>
#include <Gdi/Region.h>
#include <Gdi/VirtualScreen.h>
#include <Overlay/StatsWindow.h>

namespace
{
	template <typename TSurface>
	void bltToGdi(TSurface* This, LPRECT lpDestRect, TSurface* lpDDSrcSurface, LPRECT lpSrcRect,
		DWORD dwFlags, LPDDBLTFX lpDDBltFx, const RECT& monitorRect)
	{
		CompatPtr<IDirectDrawClipper> clipper;
		CompatVtable<Vtable<TSurface>>::s_origVtable.GetClipper(This, &clipper.getRef());
		if (!clipper)
		{
			return;
		}

		D3dDdi::ScopedCriticalSection lock;
		Gdi::Region clipRgn(DDraw::DirectDrawClipper::getClipRgn(*clipper));
		RECT virtualScreenBounds = Gdi::VirtualScreen::getBounds();
		clipRgn.offset(monitorRect.left, monitorRect.top);
		clipRgn &= virtualScreenBounds;
		clipRgn -= monitorRect;
		if (clipRgn.isEmpty())
		{
			return;
		}

		auto repo = DDraw::DirectDrawSurface::getSurfaceRepository(*This);
		if (!repo)
		{
			return;
		}

		auto dd(repo->getDirectDraw());
		if (!dd)
		{
			return;
		}

		auto gdiSurface(Gdi::VirtualScreen::createSurface(dd, virtualScreenBounds));
		if (!gdiSurface)
		{
			return;
		}

		CompatPtr<IDirectDrawClipper> gdiClipper;
		CALL_ORIG_PROC(DirectDrawCreateClipper)(0, &gdiClipper.getRef(), nullptr);
		if (!gdiClipper)
		{
			return;
		}

		RECT dstRect = *lpDestRect;
		OffsetRect(&dstRect, monitorRect.left - virtualScreenBounds.left, monitorRect.top - virtualScreenBounds.top);
		clipRgn.offset(-virtualScreenBounds.left, -virtualScreenBounds.top);
		DDraw::DirectDrawClipper::setClipRgn(*gdiClipper, clipRgn);

		auto srcSurface(CompatPtr<IDirectDrawSurface7>::from(lpDDSrcSurface));
		gdiSurface->SetClipper(gdiSurface, gdiClipper);
		gdiSurface.get()->lpVtbl->Blt(gdiSurface, &dstRect, srcSurface, lpSrcRect, dwFlags, lpDDBltFx);
		gdiSurface->SetClipper(gdiSurface, nullptr);
	}

	bool isFsBlt(const RECT& monitorRect, int cx, int cy)
	{
		return monitorRect.right - monitorRect.left == cx &&
			monitorRect.bottom - monitorRect.top == cy;
	}

	bool isFsBlt(const RECT& monitorRect, LPRECT lpDestRect)
	{
		return !lpDestRect || 0 == lpDestRect->left && 0 == lpDestRect->top &&
			isFsBlt(monitorRect, lpDestRect->right, lpDestRect->bottom);
	}

	bool isFsBlt(int& cache, const RECT& monitorRect, LPRECT lpDestRect)
	{
		if (0 == cache)
		{
			cache = isFsBlt(monitorRect, lpDestRect) ? 1 : -1;
		}
		return 1 == cache;
	}

	template <typename TSurface>
	bool isFsBltFast(const RECT& monitorRect, DWORD dwX, DWORD dwY, TSurface* lpDDSrcSurface, LPRECT lpSrcRect)
	{
		if (0 != dwX || 0 != dwY || !lpDDSrcSurface)
		{
			return false;
		}

		if (lpSrcRect)
		{
			return isFsBlt(monitorRect, lpSrcRect->right - lpSrcRect->left, lpSrcRect->bottom - lpSrcRect->top);
		}

		typename DDraw::Types<TSurface>::TSurfaceDesc desc = {};
		desc.dwSize = sizeof(desc);
		getOrigVtable(lpDDSrcSurface).GetSurfaceDesc(lpDDSrcSurface, &desc);
		return isFsBlt(monitorRect, desc.dwWidth, desc.dwHeight);
	}

	template <typename TSurface>
	bool isFsBltFast(int& cache, const RECT& monitorRect, DWORD dwX, DWORD dwY, TSurface* lpDDSrcSurface, LPRECT lpSrcRect)
	{
		if (0 == cache)
		{
			cache = isFsBltFast(monitorRect, dwX, dwY, lpDDSrcSurface, lpSrcRect) ? 1 : -1;
		}
		return 1 == cache;
	}

	void restorePrimaryCaps(DWORD& caps)
	{
		caps &= ~DDSCAPS_OFFSCREENPLAIN;
		caps |= DDSCAPS_PRIMARYSURFACE | DDSCAPS_VISIBLE;
	}
}

namespace DDraw
{
	template <typename TSurface>
	PrimarySurfaceImpl<TSurface>::PrimarySurfaceImpl(Surface* data)
		: SurfaceImpl<TSurface>(data)
	{
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::AddAttachedSurface(TSurface* This, TSurface* lpDDSAttachedSurface)
	{
		HRESULT result = getOrigVtable(This).AddAttachedSurface(This, lpDDSAttachedSurface);
		if (SUCCEEDED(result) && !(getPrimary().getOrigCaps() & DDSCAPS_3DDEVICE))
		{
			TDdsCaps caps = {};
			getOrigVtable(This).GetCaps(lpDDSAttachedSurface, &caps);
			if (caps.dwCaps & DDSCAPS_3DDEVICE)
			{
				getPrimary().setAsRenderTarget();
			}
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Blt(
		TSurface* This, LPRECT lpDestRect, TSurface* lpDDSrcSurface, LPRECT lpSrcRect,
		DWORD dwFlags, LPDDBLTFX lpDDBltFx)
	{
		if (FAILED(IsLost(This)))
		{
			return DDERR_SURFACELOST;
		}

		RealPrimarySurface::flush();
		const auto fpsLimiter = RealPrimarySurface::getFpsLimiter();
		int isFsBltCache = 0;
		if (Config::Settings::FpsLimiter::FLIPSTART == fpsLimiter.value &&
			isFsBlt(isFsBltCache, getPrimary().getMonitorInfo().rcEmulated, lpDestRect))
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}

		HRESULT result = DD_OK;
		bool isRedirected = true;

		CompatPtr<IDirectDrawClipper> clipper;
		getOrigVtable(This).GetClipper(This, &clipper.getRef());
		if (clipper)
		{
			HWND hwnd = nullptr;
			clipper->GetHWnd(clipper, &hwnd);
			isRedirected = !hwnd || Gdi::isRedirected(hwnd);
		}

		auto realPrimary = getPrimary().getRealPrimary();
		if (isRedirected || !realPrimary->isFullscreen())
		{
			result = SurfaceImpl<TSurface>::Blt(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx);
		}

		if (!isRedirected)
		{
			auto realPrimarySurface(realPrimary->getSurface());
			realPrimarySurface->SetClipper(realPrimarySurface, clipper);
			auto src(CompatPtr<IDirectDrawSurface7>::from(lpDDSrcSurface));
			result = realPrimarySurface->Blt(realPrimarySurface, lpDestRect, src, lpSrcRect, dwFlags, lpDDBltFx);
		}

		if (SUCCEEDED(result))
		{
			if (isRedirected && Config::gdiInterops.anyRedirects() &&
				lpDestRect && !realPrimary->isFullscreen())
			{
				bltToGdi(This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx,
					getPrimary().getMonitorInfo().rcEmulated);
			}

			auto statsWindow = Gdi::GuiThread::getStatsWindow();
			if (statsWindow)
			{
				statsWindow->m_blit.add();
			}

			if (Config::Settings::PresentDelay::AUTO == Config::presentDelay.get() &&
				isFsBlt(isFsBltCache, getPrimary().getMonitorInfo().rcEmulated, lpDestRect))
			{
				RealPrimarySurface::scheduleUpdate(true);
			}
			else
			{
				RealPrimarySurface::scheduleUpdateWithPresentDelay(true);
			}
		}

		if (Config::Settings::FpsLimiter::FLIPEND == fpsLimiter.value &&
			isFsBlt(isFsBltCache, getPrimary().getMonitorInfo().rcEmulated, lpDestRect))
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::BltFast(
		TSurface* This, DWORD dwX, DWORD dwY, TSurface* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans)
	{
		if (FAILED(IsLost(This)))
		{
			return DDERR_SURFACELOST;
		}

		RealPrimarySurface::flush();
		const auto fpsLimiter = RealPrimarySurface::getFpsLimiter();
		int isFsBltFastCache = 0;
		if (Config::Settings::FpsLimiter::FLIPSTART == fpsLimiter.value
			&& isFsBltFast(isFsBltFastCache, getPrimary().getMonitorInfo().rcEmulated, dwX, dwY, lpDDSrcSurface, lpSrcRect))
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}
		HRESULT result = SurfaceImpl<TSurface>::BltFast(This, dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans);
		if (SUCCEEDED(result))
		{
			auto statsWindow = Gdi::GuiThread::getStatsWindow();
			if (statsWindow)
			{
				statsWindow->m_blit.add();
			}
			if (Config::Settings::PresentDelay::AUTO == Config::presentDelay.get() &&
				isFsBltFast(isFsBltFastCache, getPrimary().getMonitorInfo().rcEmulated, dwX, dwY, lpDDSrcSurface, lpSrcRect))
			{
				RealPrimarySurface::scheduleUpdate(true);
			}
			else
			{
				RealPrimarySurface::scheduleUpdateWithPresentDelay(true);
			}
		}
		if (Config::Settings::FpsLimiter::FLIPEND == fpsLimiter.value
			&& isFsBltFast(isFsBltFastCache, getPrimary().getMonitorInfo().rcEmulated, dwX, dwY, lpDDSrcSurface, lpSrcRect))
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Flip(TSurface* This, TSurface* lpDDSurfaceTargetOverride, DWORD dwFlags)
	{
		if (FAILED(IsLost(This)))
		{
			return DDERR_SURFACELOST;
		}

		auto realPrimary = getPrimary().getRealPrimary();
		if (!realPrimary->isFullscreen())
		{
			return DDERR_NOEXCLUSIVEMODE;
		}

		RealPrimarySurface::setUpdateReady();
		RealPrimarySurface::flush();
		realPrimary->waitForFlip(this->m_data->getDDS());

		const auto fpsLimiter = RealPrimarySurface::getFpsLimiter();
		if (Config::Settings::FpsLimiter::FLIPSTART == fpsLimiter.value)
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}

		auto surfaceTargetOverride(CompatPtr<TSurface>::from(lpDDSurfaceTargetOverride));
		auto surface(surfaceTargetOverride);
		if (!surface)
		{
			TDdsCaps caps = {};
			caps.dwCaps = DDSCAPS_BACKBUFFER;
			getOrigVtable(This).GetAttachedSurface(This, &caps, &surface.getRef());
		}

		const bool isFlipEmulated = 0 != (getPrimary().getOrigCaps() & DDSCAPS_SYSTEMMEMORY);
		if (isFlipEmulated)
		{
			HRESULT result = SurfaceImpl<TSurface>::Blt(This, nullptr, surface.get(), nullptr, DDBLT_WAIT, nullptr);
			if (FAILED(result))
			{
				return result;
			}

			dwFlags = DDFLIP_NOVSYNC;
		}
		else
		{
			if (surface)
			{
				RECT emptyRect = {};
				getOrigVtable(surface.get()).BltFast(surface, 0, 0, surface, &emptyRect, DDBLTFAST_WAIT);
			}

			HRESULT result = SurfaceImpl<TSurface>::Flip(This, surfaceTargetOverride, DDFLIP_WAIT);
			if (FAILED(result))
			{
				return result;
			}
		}

		auto statsWindow = Gdi::GuiThread::getStatsWindow();
		if (statsWindow)
		{
			statsWindow->m_flip.add();
		}

		getPrimary().updateFrontResource();
		realPrimary->flip(surfaceTargetOverride, dwFlags);
		getPrimary().waitForIdle();

		if (Config::Settings::FpsLimiter::FLIPEND == fpsLimiter.value)
		{
			RealPrimarySurface::waitForFlipFpsLimit(fpsLimiter.param);
		}
		return DD_OK;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetAttachedSurface(TSurface* This, TDdsCaps* lpDDSCaps, TSurface** lplpDDAttachedSurface)
	{
		if (lpDDSCaps && (this->m_data->getOrigCaps().dwCaps & DDSCAPS_SYSTEMMEMORY))
		{
			TDdsCaps caps = *lpDDSCaps;
			caps.dwCaps &= ~DDSCAPS_SYSTEMMEMORY;
			return SurfaceImpl<TSurface>::GetAttachedSurface(This, &caps, lplpDDAttachedSurface);
		}
		return SurfaceImpl<TSurface>::GetAttachedSurface(This, lpDDSCaps, lplpDDAttachedSurface);
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetCaps(TSurface* This, TDdsCaps* lpDDSCaps)
	{
		HRESULT result = SurfaceImpl<TSurface>::GetCaps(This, lpDDSCaps);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSCaps->dwCaps);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetDC(TSurface* This, HDC* lphDC)
	{
		if (FAILED(IsLost(This)))
		{
			return DDERR_SURFACELOST;
		}

		RealPrimarySurface::flush();
		HRESULT result = SurfaceImpl<TSurface>::GetDC(This, lphDC);
		if (SUCCEEDED(result))
		{
			auto statsWindow = Gdi::GuiThread::getStatsWindow();
			if (statsWindow)
			{
				statsWindow->m_lock.add();
			}
			RealPrimarySurface::scheduleUpdateWithPresentDelay();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::GetSurfaceDesc(TSurface* This, TSurfaceDesc* lpDDSurfaceDesc)
	{
		HRESULT result = SurfaceImpl<TSurface>::GetSurfaceDesc(This, lpDDSurfaceDesc);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSurfaceDesc->ddsCaps.dwCaps);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::IsLost(TSurface* This)
	{
		HRESULT result = SurfaceImpl<TSurface>::IsLost(This);
		if (SUCCEEDED(result))
		{
			auto realPrimary = getPrimary().getRealPrimary();
			result = (!realPrimary || realPrimary->isLost()) ? DDERR_SURFACELOST : DD_OK;
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Lock(
		TSurface* This, LPRECT lpDestRect, TSurfaceDesc* lpDDSurfaceDesc,
		DWORD dwFlags, HANDLE hEvent)
	{
		if (FAILED(IsLost(This)))
		{
			return DDERR_SURFACELOST;
		}

		RealPrimarySurface::flush();
		HRESULT result = SurfaceImpl<TSurface>::Lock(This, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent);
		if (SUCCEEDED(result))
		{
			restorePrimaryCaps(lpDDSurfaceDesc->ddsCaps.dwCaps);
			auto statsWindow = Gdi::GuiThread::getStatsWindow();
			if (statsWindow)
			{
				statsWindow->m_lock.add();
			}
			RealPrimarySurface::scheduleUpdateWithPresentDelay();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::ReleaseDC(TSurface* This, HDC hDC)
	{
		HRESULT result = SurfaceImpl<TSurface>::ReleaseDC(This, hDC);
		if (SUCCEEDED(result))
		{
			RealPrimarySurface::scheduleUpdateWithPresentDelay(true);
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Restore(TSurface* This)
	{
		HRESULT result = IsLost(This);
		if (FAILED(result))
		{
			auto realPrimary = getPrimary().getRealPrimary();
			auto realPrimarySurface = realPrimary ? realPrimary->getSurface() : nullptr;
			result = (realPrimarySurface && SUCCEEDED(realPrimarySurface->IsLost(realPrimarySurface)))
				? DD_OK : getPrimary().resetRealPrimary();
			if (SUCCEEDED(result))
			{
				D3dDdi::Device::setGdiResourceCreationInProgress(true);
				result = SurfaceImpl<TSurface>::Restore(This);
				D3dDdi::Device::setGdiResourceCreationInProgress(false);
				D3dDdi::Device::setGdiResource();
				return result;
			}
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::SetPalette(TSurface* This, LPDIRECTDRAWPALETTE lpDDPalette)
	{
		if (lpDDPalette)
		{
			DirectDrawPalette::waitForNextUpdate();
		}

		HRESULT result = SurfaceImpl<TSurface>::SetPalette(This, lpDDPalette);
		if (SUCCEEDED(result))
		{
			getPrimary().setPalette(lpDDPalette);
			getPrimary().updatePalette();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT PrimarySurfaceImpl<TSurface>::Unlock(TSurface* This, TUnlockParam lpRect)
	{
		HRESULT result = SurfaceImpl<TSurface>::Unlock(This, lpRect);
		if (SUCCEEDED(result))
		{
			RealPrimarySurface::scheduleUpdateWithPresentDelay(true);
		}
		return result;
	}

	template PrimarySurfaceImpl<IDirectDrawSurface>;
	template PrimarySurfaceImpl<IDirectDrawSurface2>;
	template PrimarySurfaceImpl<IDirectDrawSurface3>;
	template PrimarySurfaceImpl<IDirectDrawSurface4>;
	template PrimarySurfaceImpl<IDirectDrawSurface7>;
}
