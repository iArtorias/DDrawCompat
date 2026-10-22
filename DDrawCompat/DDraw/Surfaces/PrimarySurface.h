#pragma once

#include <memory>

#include <ddraw.h>

#include <Common/CompatPtr.h>
#include <Common/CompatRef.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/Surfaces/Surface.h>
#include <Win32/DisplayMode.h>

namespace D3dDdi
{
	class Device;
	class Resource;
}

namespace DDraw
{
	class TagSurface;

	class PrimarySurface : public Surface
	{
	public:
		PrimarySurface(DWORD origFlags, const Caps& origCaps, TagSurface& tagSurface);
		virtual ~PrimarySurface();

		template <typename TDirectDraw, typename TSurface, typename TSurfaceDesc>
		static HRESULT create(CompatRef<TDirectDraw> dd, TSurfaceDesc desc, TSurface*& surface);

		HRESULT flipToGdiSurface();
		CompatPtr<IDirectDrawSurface7> getGdiSurface();
		CompatPtr<IDirectDrawSurface7> getBackBuffer();
		CompatPtr<IDirectDrawSurface7> getLastSurface();
		const Win32::DisplayMode::MonitorInfo& getMonitorInfo() { return m_monitorInfo; }
		CompatWeakPtr<IDirectDrawPalette> getPalette() { return m_palette; }
		const DDCOLORKEY * getDestOverlayColorKey() const;
		HANDLE getFrontResource();
		DWORD getOrigCaps();
		RealPrimarySurface* getRealPrimary() { return m_realPrimary.get(); }
		TagSurface& getTagSurface() { return m_tagSurface; }
		void onLost();
		HRESULT resetRealPrimary();
		void setAsRenderTarget();
		void setDestOverlayColorKey( DWORD flags, const DDCOLORKEY * colorKey );
		void setPalette(IDirectDrawPalette* palette) { m_palette = palette; }
		void setWindowedCooperativeLevel();
		void updateFrontResource();
		void updatePalette();
		void waitForIdle();

		virtual void onRestore() override;

	private:
		virtual void createImpl() override;

		TagSurface& m_tagSurface;

		D3dDdi::Device* m_device = nullptr;
		HWND m_deviceWindow = nullptr;
		D3dDdi::Resource* m_frontResource = nullptr;
		UINT m_frontResourceIndex = 0;
		Win32::DisplayMode::MonitorInfo m_monitorInfo = {};
		DWORD m_origCaps = 0;
		DDCOLORKEY m_destOverlayColorKey = {};
		bool m_hasDestOverlayColorKey = false;
		CompatWeakPtr<IDirectDrawPalette> m_palette;
		std::unique_ptr<RealPrimarySurface> m_realPrimary;
	};
}
