#pragma once

#include <vector>

#include <ddraw.h>

#include <Common/CompatPtr.h>
#include <Common/CompatRef.h>
#include <DDraw/Surfaces/Surface.h>

namespace DDraw
{
	class PrimarySurface;

	class OverlaySurface : public Surface
	{
	public:
		OverlaySurface( DWORD origFlags, const Caps & origCaps );
		virtual ~OverlaySurface() override;

		template <typename TDirectDraw, typename TSurface, typename TSurfaceDesc>
		static HRESULT create( CompatRef<TDirectDraw> dd, TSurfaceDesc desc, TSurface *& surface );

		static void addCaps( DDCAPS & caps );
		static void addFourCCCodes( std::vector<DWORD> & codes );
		static CompatWeakPtr<IDirectDrawSurface7> compose( PrimarySurface & primary );
		static void enableEmulation( IUnknown * dd );
		static bool isEmulationEnabled();

		DWORD getBackBufferCount() const
		{
			return m_backBufferCount;
		}
		HRESULT getPosition( LONG & x, LONG & y ) const;
		const DDPIXELFORMAT & getPixelFormat() const
		{
			return m_pixelFormat;
		}
		bool isVisible() const
		{
			return m_isVisible;
		}
		void onContentChanged();
		HRESULT setPosition( LONG x, LONG y );
		HRESULT update( const RECT * srcRect, Surface * destSurface, const RECT * destRect, DWORD flags, const DDOVERLAYFX * fx );

	private:
		virtual void createImpl() override;

		void blendTo( const DDSURFACEDESC2 & dstDesc );

		DDPIXELFORMAT m_pixelFormat = {};
		DWORD m_backBufferCount = 0;
		bool m_isVisible = false;
		RECT m_srcRect = {};
		RECT m_destRect = {};
		bool m_isDestColorKeyEnabled = false;
		DDCOLORKEY m_destColorKey = {};
	};
}