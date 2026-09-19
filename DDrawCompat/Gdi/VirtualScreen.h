#pragma once

#include <Windows.h>

#include <Common/CompatPtr.h>

namespace DDraw
{
	class PrimarySurface;
}

namespace Gdi
{
	class Region;

	namespace VirtualScreen
	{
		HDC createDc(bool useDefaultPalette);
		HBITMAP createDib(bool useDefaultPalette);
		HBITMAP createOffScreenDib(LONG width, LONG height, DWORD bpp);
		CompatPtr<IDirectDrawSurface7> createSurface(CompatWeakPtr<IDirectDraw7> dd, const RECT& rect);
		void deleteDc(HDC dc);

		RECT getBounds();
		HDC getDc();
		DDSURFACEDESC2 getSurfaceDesc(const RECT& rect);

		void init();
		void onReleasePrimary(DDraw::PrimarySurface& primary);
		void setFullscreenPrimary(DDraw::PrimarySurface& primary);
		bool update();
		void updatePalette(PALETTEENTRY(&palette)[256]);
	}
}
