#pragma once

#include <ddraw.h>
#include <ddrawi.h>
#include <functional>

#include <Common/CompatRef.h>
#include <DDraw/Surfaces/Surface.h>

namespace DDraw
{
	class PrimarySurface;

	class TagSurface : public Surface
	{
	public:
		TagSurface(DWORD origFlags, const Caps& origCaps, DDRAWI_DIRECTDRAW_LCL* ddLcl);
		virtual ~TagSurface() override;

		static void enumTagSurfaces(std::function<bool(TagSurface&)> callback);
		static TagSurface* get(DDRAWI_DIRECTDRAW_LCL* ddLcl);
		static TagSurface* get(CompatRef<IDirectDraw> dd);
		static PrimarySurface* getMainPrimary();
		static TagSurface* findFullscreenWindow(HWND hwnd = nullptr);
		static bool inCreateSurface() { return s_inCreateSurface; }

		CompatPtr<IDirectDraw7> getDD();
		DWORD getExclusiveOwnerThreadId() const { return m_exclusiveOwnerThreadId; }
		PrimarySurface* getPrimary() const { return m_primary; }
		bool isFullscreen() const { return m_fullscreenWindow; }
		bool isOverlayEmulated() const { return m_isOverlayEmulated; }
		void setFullscreenWindow(HWND hwnd);
		void setOverlayEmulated( bool isEmulated ) { m_isOverlayEmulated = isEmulated; }
		void setPrimary(PrimarySurface* primary) { m_primary = primary; }
		LONG setWindowStyle(LONG style);
		LONG setWindowExStyle(LONG exStyle);

	private:
		static HRESULT create(CompatRef<IDirectDraw> dd);
		static PrimarySurface* getMainPrimary(bool fullscreen);

		DDRAWI_DIRECTDRAW_INT m_ddInt = {};
		DWORD m_exclusiveOwnerThreadId = 0;
		HWND m_fullscreenWindow = nullptr;
		LONG m_fullscreenWindowStyle = 0;
		LONG m_fullscreenWindowExStyle = 0;
		PrimarySurface* m_primary = nullptr;
		bool m_isOverlayEmulated = false;

		static bool s_inCreateSurface;
	};
}
