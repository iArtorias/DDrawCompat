#include <DDraw/Surfaces/OverlaySurface.h>
#include <DDraw/Surfaces/OverlaySurfaceImpl.h>

namespace
{
	void fixOverlayCaps( DWORD & caps, const DDraw::OverlaySurface & overlay )
	{
		caps &= ~(DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
		caps |= DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
		if (overlay.isVisible())
		{
			caps |= DDSCAPS_VISIBLE;
		}
		if (0 != overlay.getBackBufferCount())
		{
			caps |= DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_FRONTBUFFER;
		}
	}

	template <typename TSurface>
	bool isSameSurface( TSurface * surface1, TSurface * surface2 )
	{
		return surface1 && surface2 && DDraw::Surface::getSurface( *surface1 ) == DDraw::Surface::getSurface( *surface2 );
	}
}

namespace DDraw
{
	template <typename TSurface>
	OverlaySurfaceImpl<TSurface>::OverlaySurfaceImpl( Surface * data )
		: SurfaceImpl<TSurface>( data )
	{}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::AddOverlayDirtyRect( TSurface * /*This*/, LPRECT /*lpRect*/ )
	{
		getOverlay().onContentChanged();
		return DD_OK;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::Blt(
		TSurface * This, LPRECT lpDestRect, TSurface * lpDDSrcSurface, LPRECT lpSrcRect,
		DWORD dwFlags, LPDDBLTFX lpDDBltFx )
	{
		if (isSameSurface( This, lpDDSrcSurface ))
		{
			return DD_OK;
		}

		HRESULT result = SurfaceImpl<TSurface>::Blt( This, lpDestRect, lpDDSrcSurface, lpSrcRect, dwFlags, lpDDBltFx );
		if (SUCCEEDED( result ))
		{
			getOverlay().onContentChanged();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::BltFast(
		TSurface * This, DWORD dwX, DWORD dwY, TSurface * lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans )
	{
		if (isSameSurface( This, lpDDSrcSurface ))
		{
			return DD_OK;
		}

		HRESULT result = SurfaceImpl<TSurface>::BltFast( This, dwX, dwY, lpDDSrcSurface, lpSrcRect, dwTrans );
		if (SUCCEEDED( result ))
		{
			getOverlay().onContentChanged();
		}
		return result;
	}

	template <typename TSurface>
	void OverlaySurfaceImpl<TSurface>::fixDesc( TSurfaceDesc & desc ) const
	{
		fixOverlayCaps( desc.ddsCaps.dwCaps, getOverlay() );
		if (getOverlay().getPixelFormat().dwFlags & DDPF_FOURCC)
		{
			desc.ddpfPixelFormat = getOverlay().getPixelFormat();
		}
		if (0 != getOverlay().getBackBufferCount())
		{
			desc.dwFlags |= DDSD_BACKBUFFERCOUNT;
			desc.dwBackBufferCount = getOverlay().getBackBufferCount();
		}
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::Flip( TSurface * This, TSurface * lpDDSurfaceTargetOverride, DWORD dwFlags )
	{
		if (0 == getOverlay().getBackBufferCount())
		{
			return SurfaceImpl<TSurface>::Flip( This, lpDDSurfaceTargetOverride, dwFlags );
		}
		getOverlay().onContentChanged();
		return DD_OK;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetAttachedSurface(
		TSurface * This, TDdsCaps * lpDDSCaps, TSurface ** lplpDDAttachedSurface )
	{
		if (0 != getOverlay().getBackBufferCount() && lpDDSCaps && lplpDDAttachedSurface &&
			(lpDDSCaps->dwCaps & (DDSCAPS_BACKBUFFER | DDSCAPS_FLIP)))
		{
			getOrigVtable( This ).AddRef( This );
			*lplpDDAttachedSurface = This;
			return DD_OK;
		}
		return SurfaceImpl<TSurface>::GetAttachedSurface( This, lpDDSCaps, lplpDDAttachedSurface );
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetFlipStatus( TSurface * This, DWORD dwFlags )
	{
		if (0 == getOverlay().getBackBufferCount())
		{
			return SurfaceImpl<TSurface>::GetFlipStatus( This, dwFlags );
		}
		return DD_OK;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetCaps( TSurface * This, TDdsCaps * lpDDSCaps )
	{
		HRESULT result = SurfaceImpl<TSurface>::GetCaps( This, lpDDSCaps );
		if (SUCCEEDED( result ))
		{
			fixOverlayCaps( lpDDSCaps->dwCaps, getOverlay() );
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetOverlayPosition( TSurface * /*This*/, LPLONG lplX, LPLONG lplY )
	{
		if (!lplX || !lplY)
		{
			return DDERR_INVALIDPARAMS;
		}
		return getOverlay().getPosition( *lplX, *lplY );
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetPixelFormat( TSurface * /*This*/, LPDDPIXELFORMAT lpDDPixelFormat )
	{
		if (!lpDDPixelFormat)
		{
			return DDERR_INVALIDPARAMS;
		}
		*lpDDPixelFormat = getOverlay().getPixelFormat();
		return DD_OK;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::GetSurfaceDesc( TSurface * This, TSurfaceDesc * lpDDSurfaceDesc )
	{
		HRESULT result = SurfaceImpl<TSurface>::GetSurfaceDesc( This, lpDDSurfaceDesc );
		if (SUCCEEDED( result ))
		{
			fixDesc( *lpDDSurfaceDesc );
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::Lock(
		TSurface * This, LPRECT lpDestRect, TSurfaceDesc * lpDDSurfaceDesc,
		DWORD dwFlags, HANDLE hEvent )
	{
		HRESULT result = SurfaceImpl<TSurface>::Lock( This, lpDestRect, lpDDSurfaceDesc, dwFlags, hEvent );
		if (SUCCEEDED( result ))
		{
			fixDesc( *lpDDSurfaceDesc );
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::ReleaseDC( TSurface * This, HDC hDC )
	{
		HRESULT result = SurfaceImpl<TSurface>::ReleaseDC( This, hDC );
		if (SUCCEEDED( result ))
		{
			getOverlay().onContentChanged();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::SetOverlayPosition( TSurface * /*This*/, LONG lX, LONG lY )
	{
		return getOverlay().setPosition( lX, lY );
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::Unlock( TSurface * This, TUnlockParam lpRect )
	{
		HRESULT result = SurfaceImpl<TSurface>::Unlock( This, lpRect );
		if (SUCCEEDED( result ))
		{
			getOverlay().onContentChanged();
		}
		return result;
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::UpdateOverlay( TSurface * /*This*/, LPRECT lpSrcRect, TSurface * lpDDDestSurface,
		LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx )
	{
		return getOverlay().update( lpSrcRect, lpDDDestSurface ? Surface::getSurface( *lpDDDestSurface ) : nullptr,
			lpDestRect, dwFlags, lpDDOverlayFx );
	}

	template <typename TSurface>
	HRESULT OverlaySurfaceImpl<TSurface>::UpdateOverlayDisplay( TSurface * /*This*/, DWORD /*dwFlags*/ )
	{
		getOverlay().onContentChanged();
		return DD_OK;
	}

	template OverlaySurfaceImpl<IDirectDrawSurface>;
	template OverlaySurfaceImpl<IDirectDrawSurface2>;
	template OverlaySurfaceImpl<IDirectDrawSurface3>;
	template OverlaySurfaceImpl<IDirectDrawSurface4>;
	template OverlaySurfaceImpl<IDirectDrawSurface7>;
}