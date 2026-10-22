#pragma once

#include <ddraw.h>

#include <DDraw/Surfaces/SurfaceImpl.h>

namespace DDraw
{
	class OverlaySurface;

	template <typename TSurface>
	class OverlaySurfaceImpl : public SurfaceImpl<TSurface>
	{
	public:
		using SurfaceImpl<TSurface>::TDdsCaps;
		using SurfaceImpl<TSurface>::TSurfaceDesc;
		using SurfaceImpl<TSurface>::TUnlockParam;

		OverlaySurfaceImpl( Surface * data );

		virtual HRESULT AddOverlayDirtyRect( TSurface * This, LPRECT lpRect ) override;
		virtual HRESULT Blt( TSurface * This, LPRECT lpDestRect, TSurface * lpDDSrcSurface, LPRECT lpSrcRect,
			DWORD dwFlags, LPDDBLTFX lpDDBltFx ) override;
		virtual HRESULT BltFast( TSurface * This, DWORD dwX, DWORD dwY,
			TSurface * lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans ) override;
		virtual HRESULT Flip( TSurface * This, TSurface * lpDDSurfaceTargetOverride, DWORD dwFlags ) override;
		virtual HRESULT GetAttachedSurface( TSurface * This, TDdsCaps * lpDDSCaps, TSurface ** lplpDDAttachedSurface ) override;
		virtual HRESULT GetCaps( TSurface * This, TDdsCaps * lpDDSCaps ) override;
		virtual HRESULT GetFlipStatus( TSurface * This, DWORD dwFlags ) override;
		virtual HRESULT GetOverlayPosition( TSurface * This, LPLONG lplX, LPLONG lplY ) override;
		virtual HRESULT GetPixelFormat( TSurface * This, LPDDPIXELFORMAT lpDDPixelFormat ) override;
		virtual HRESULT GetSurfaceDesc( TSurface * This, TSurfaceDesc * lpDDSurfaceDesc ) override;
		virtual HRESULT Lock( TSurface * This, LPRECT lpDestRect, TSurfaceDesc * lpDDSurfaceDesc,
			DWORD dwFlags, HANDLE hEvent ) override;
		virtual HRESULT ReleaseDC( TSurface * This, HDC hDC ) override;
		virtual HRESULT SetOverlayPosition( TSurface * This, LONG lX, LONG lY ) override;
		virtual HRESULT Unlock( TSurface * This, TUnlockParam lpRect ) override;
		virtual HRESULT UpdateOverlay( TSurface * This, LPRECT lpSrcRect, TSurface * lpDDDestSurface,
			LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx ) override;
		virtual HRESULT UpdateOverlayDisplay( TSurface * This, DWORD dwFlags ) override;

	private:
		OverlaySurface & getOverlay() const
		{
			return *static_cast<OverlaySurface *>(this->m_data);
		}
		void fixDesc( TSurfaceDesc & desc ) const;
	};
}