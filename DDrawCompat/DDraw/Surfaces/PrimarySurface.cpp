#include <Common/CompatPtr.h>
#include <Common/CompatRef.h>
#include <Common/HResultException.h>
#include <Config/Settings/CompatFixes.h>
#include <D3dDdi/Device.h>
#include <D3dDdi/KernelModeThunks.h>
#include <D3dDdi/Resource.h>
#include <D3dDdi/ScopedCriticalSection.h>
#include <DDraw/DirectDraw.h>
#include <DDraw/DirectDrawSurface.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Surfaces/PrimarySurfaceImpl.h>
#include <DDraw/Surfaces/TagSurface.h>
#include <Gdi/Palette.h>
#include <Gdi/VirtualScreen.h>
#include <Win32/DisplayMode.h>

namespace
{
	bool checkMultiplePrimariesAcrossMonitors(DDraw::TagSurface& tagSurface)
	{
		const auto gdiResource = D3dDdi::Device::getGdiResource();
		if (!gdiResource)
		{
			return true;
		}

		const auto resourceHandle = DDraw::DirectDrawSurface::getDriverResourceHandle(*tagSurface.getDDS());
		if (D3dDdi::Device::findDeviceByResource(resourceHandle) != &gdiResource->getDevice())
		{
			LOG_ONCE("ERROR: multiple primary surfaces are not supported across different monitors");
			if (!Config::compatFixes.get().singlemonitor)
			{
				LOG_ONCE("Hint: try adding CompatFixes=singlemonitor to the configuration if experiencing display issues");
			}
			return false;
		}
		return true;
	}

	HPALETTE createPalette()
	{
		LOGPALETTE lp = {};
		lp.palVersion = 0x300;
		lp.palNumEntries = 1;
		HPALETTE palette = CreatePalette(&lp);
		ResizePalette(palette, 256);
		return palette;
	}
}

namespace DDraw
{
	PrimarySurface::PrimarySurface(DWORD origFlags, const Caps& origCaps, TagSurface& tagSurface)
		: Surface(origFlags, origCaps)
		, m_tagSurface(tagSurface)
	{
	}

	PrimarySurface::~PrimarySurface()
	{
		LOG_FUNC("PrimarySurface::~PrimarySurface");
		const auto gdiResource = D3dDdi::Device::getGdiResource();
		if (gdiResource)
		{
			gdiResource->prepareForCpuRead(0);
		}
		m_tagSurface.setPrimary(nullptr);
		Gdi::VirtualScreen::onReleasePrimary(*this);
	}

	template <typename TDirectDraw, typename TSurface, typename TSurfaceDesc>
	HRESULT PrimarySurface::create(CompatRef<TDirectDraw> dd, TSurfaceDesc desc, TSurface*& surface)
	{
		LOG_FUNC("PrimarySurface::create", &dd, desc, surface);

		auto tagSurface = TagSurface::get(DirectDraw::getInt(dd.get()).lpLcl);
		if (!tagSurface)
		{
			LOG_ONCE("ERROR: TagSurface not found");
			return DDERR_GENERIC;
		}

		if (!checkMultiplePrimariesAcrossMonitors(*tagSurface))
		{
			return DDERR_UNSUPPORTED;
		}

		const DWORD origCaps = desc.ddsCaps.dwCaps;
		auto privateData(std::make_unique<PrimarySurface>(desc.dwFlags, desc.ddsCaps, *tagSurface));
		auto primary = privateData.get();

		auto deviceName = D3dDdi::KernelModeThunks::getAdapterInfo(*CompatPtr<IDirectDraw7>::from(&dd)).deviceName;
		primary->m_monitorInfo = Win32::DisplayMode::getMonitorInfo(deviceName);

		try
		{
			primary->m_realPrimary.reset(new RealPrimarySurface(*CompatPtr<IDirectDraw>::from(&dd), *primary));
		}
		catch (const HResultException& e)
		{
			return LOG_RESULT(e.getResult());
		}

		desc.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
		desc.dwWidth = primary->m_monitorInfo.rcEmulated.right - primary->m_monitorInfo.rcEmulated.left;
		desc.dwHeight = primary->m_monitorInfo.rcEmulated.bottom - primary->m_monitorInfo.rcEmulated.top;
		desc.ddsCaps.dwCaps &= ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
			DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM);
		desc.ddsCaps.dwCaps |= DDSCAPS_OFFSCREENPLAIN;
		desc.ddpfPixelFormat = DirectDraw::getRgbPixelFormat(primary->m_monitorInfo.bpp);

		if (!(desc.dwFlags & DDSD_BACKBUFFERCOUNT))
		{
			desc.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE;
		}

		D3dDdi::Device::setGdiResourceCreationInProgress(true);
		const HRESULT result = Surface::create(dd, desc, surface, std::move(privateData));
		D3dDdi::Device::setGdiResourceCreationInProgress(false);
		if (FAILED(result))
		{
			return LOG_RESULT(result);
		}

		primary->m_origCaps = origCaps;
		primary->m_deviceWindow = *DDraw::DirectDraw::getDeviceWindowPtr(dd.get());

		tagSurface->setPrimary(primary);
		primary->m_device = D3dDdi::Device::findDeviceByResource(DirectDrawSurface::getDriverResourceHandle(*surface));
		primary->onRestore();
		D3dDdi::Device::updateAllConfig();
		return LOG_RESULT(DD_OK);
	}

	template HRESULT PrimarySurface::create(
		CompatRef<IDirectDraw> dd, DDSURFACEDESC desc, IDirectDrawSurface*& surface);
	template HRESULT PrimarySurface::create(
		CompatRef<IDirectDraw2> dd, DDSURFACEDESC desc, IDirectDrawSurface*& surface);
	template HRESULT PrimarySurface::create(
		CompatRef<IDirectDraw4> dd, DDSURFACEDESC2 desc, IDirectDrawSurface4*& surface);
	template HRESULT PrimarySurface::create(
		CompatRef<IDirectDraw7> dd, DDSURFACEDESC2 desc, IDirectDrawSurface7*& surface);

	void PrimarySurface::createImpl()
	{
		m_impl.reset(new PrimarySurfaceImpl<IDirectDrawSurface>(this));
		m_impl2.reset(new PrimarySurfaceImpl<IDirectDrawSurface2>(this));
		m_impl3.reset(new PrimarySurfaceImpl<IDirectDrawSurface3>(this));
		m_impl4.reset(new PrimarySurfaceImpl<IDirectDrawSurface4>(this));
		m_impl7.reset(new PrimarySurfaceImpl<IDirectDrawSurface7>(this));
	}

	HRESULT PrimarySurface::flipToGdiSurface()
	{
		auto gdiSurface(getGdiSurface());
		if (!gdiSurface)
		{
			return DDERR_NOTFOUND;
		}
		if (gdiSurface == m_surface)
		{
			return DD_OK;
		}
		return m_surface.get()->lpVtbl->Flip(m_surface, gdiSurface, DDFLIP_WAIT);
	}

	CompatPtr<IDirectDrawSurface7> PrimarySurface::getGdiSurface()
	{
		const auto gdiResource = D3dDdi::Device::getGdiResource();
		if (!gdiResource)
		{
			return nullptr;
		}

		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_FLIP;
		auto surface(m_surface);

		do
		{
			if (*gdiResource == DirectDrawSurface::getDriverResourceHandle(*surface))
			{
				return CompatPtr<IDirectDrawSurface7>::from(surface.get());
			}

			if (FAILED(surface->GetAttachedSurface(surface, &caps, &surface.getRef())))
			{
				return nullptr;
			}
			surface->Release(surface);
		} while (surface != m_surface);

		return nullptr;
	}

	CompatPtr<IDirectDrawSurface7> PrimarySurface::getBackBuffer()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_BACKBUFFER;
		CompatPtr<IDirectDrawSurface7> backBuffer;
		m_surface->GetAttachedSurface(m_surface, &caps, &backBuffer.getRef());
		return backBuffer;
	}

	CompatPtr<IDirectDrawSurface7> PrimarySurface::getLastSurface()
	{
		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_FLIP;
		auto surface(CompatPtr<IDirectDrawSurface7>::from(m_surface.get()));
		CompatPtr<IDirectDrawSurface7> nextSurface;

		while (SUCCEEDED(surface->GetAttachedSurface(surface, &caps, &nextSurface.getRef())) &&
			nextSurface != m_surface)
		{
			surface.swap(nextSurface);
			nextSurface.release();
		}

		return surface;
	}

	HANDLE PrimarySurface::getFrontResource()
	{
		return *m_frontResource;
	}

	DWORD PrimarySurface::getOrigCaps()
	{
		return m_origCaps;
	}

	void PrimarySurface::onLost()
	{
		if (m_palette)
		{
			Gdi::Palette::setHardwarePalette(Gdi::Palette::getSystemPalette().data());
		}
		Gdi::VirtualScreen::onReleasePrimary(*this);
	}

	void PrimarySurface::onRestore()
	{
		LOG_FUNC("PrimarySurface::onRestore");

		updatePalette();
		if (m_realPrimary->isFullscreen() || m_realPrimary->isDShowPrimary())
		{
			Gdi::VirtualScreen::setFullscreenPrimary(*this);
		}

		updateFrontResource();
		D3dDdi::Device::setGdiResource();

		DDSCAPS2 caps = {};
		caps.dwCaps = DDSCAPS_FLIP;
		auto surface(CompatPtr<IDirectDrawSurface7>::from(m_surface.get()));
		HRESULT result = S_OK;
		do
		{
			auto surf = DDraw::Surface::getSurface(*surface);
			if (surf)
			{
				surf->setAsPrimary();
			}

			CompatPtr<IDirectDrawSurface7> next;
			result = surface->GetAttachedSurface(surface, &caps, &next.getRef());
			next.swap(surface);
		} while (SUCCEEDED(result) && surface != m_surface);

		Surface::onRestore();
	}

	HRESULT PrimarySurface::resetRealPrimary()
	{
		LOG_FUNC("PrimarySurface::resetRealPrimary");
		if (m_realPrimary && m_realPrimary->isFullscreen())
		{
			auto dd(m_tagSurface.getDD());
			if (FAILED(dd->TestCooperativeLevel(dd)))
			{
				return LOG_RESULT(DDERR_NOEXCLUSIVEMODE);
			}
		}

		auto frontBuffer = m_realPrimary->getSurface();
		if (frontBuffer)
		{
			frontBuffer->Restore(frontBuffer);
		}

		DDGAMMARAMP ramp = {};
		const HRESULT gammaResult = m_realPrimary->getGammaRamp(&ramp);

		HRESULT realPrimaryResult = DD_OK;
		CompatPtr<IDirectDraw> dd(m_tagSurface.getDD());
		try
		{
			m_realPrimary.reset();
			m_realPrimary.reset(new RealPrimarySurface(*dd, *this));
		}
		catch (const HResultException& e)
		{
			realPrimaryResult = e.getResult();
		}

		if (SUCCEEDED(realPrimaryResult) && SUCCEEDED(gammaResult))
		{
			m_realPrimary->setGammaRamp(&ramp);
		}
		return LOG_RESULT(realPrimaryResult);
	}

	void PrimarySurface::setAsRenderTarget()
	{
		m_origCaps |= DDSCAPS_3DDEVICE;
		D3dDdi::ScopedCriticalSection lock;
		auto resource = D3dDdi::Device::findResource(DirectDrawSurface::getDriverResourceHandle(*m_surface));
		if (resource)
		{
			resource->setAsRenderTarget();
		}
	}

	void PrimarySurface::setWindowedCooperativeLevel()
	{
		LOG_FUNC("PrimarySurface::setWindowedCooperativeLevel");
		resetRealPrimary();

		if (SUCCEEDED(m_surface->IsLost(m_surface)))
		{
			onLost();
			onRestore();
		}
	}

	void PrimarySurface::updateFrontResource()
	{
		m_frontResource = D3dDdi::Device::findResource(DirectDrawSurface::getDriverResourceHandle(*m_surface));
		m_frontResourceIndex = DirectDrawSurface::getSubResourceIndex(*m_surface);
	}

	void PrimarySurface::updatePalette()
	{
		if (!m_palette)
		{
			if (m_realPrimary->isFullscreen())
			{
				Gdi::Palette::setHardwarePalette(Gdi::Palette::getSystemPalette().data());
			}
			return;
		}

		PALETTEENTRY entries[256] = {};
		m_palette->GetEntries(m_palette, 0, 0, 256, entries);

		if (m_realPrimary->isFullscreen() && SUCCEEDED(m_surface->IsLost(m_surface)))
		{
			Gdi::Palette::setHardwarePalette(entries);
		}
		else
		{
			static HPALETTE palette = createPalette();
			SetPaletteEntries(palette, 0, 256, entries);
			HDC dc = GetDC(m_deviceWindow);
			HPALETTE oldPal = SelectPalette(dc, palette, FALSE);
			RealizePalette(dc);
			SelectPalette(dc, oldPal, FALSE);
			ReleaseDC(m_deviceWindow, dc);
		}

		RealPrimarySurface::scheduleUpdate(true);
	}

	void PrimarySurface::waitForIdle()
	{
		m_device->waitForIdle();
	}
}
