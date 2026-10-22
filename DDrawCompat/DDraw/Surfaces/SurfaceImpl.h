#pragma once

#include <functional>

#include <ddraw.h>

#include <Common/CompatVtable.h>
#include <DDraw/Types.h>

struct _D3DDDIARG_BLT;

namespace DDraw
{
	class Surface;

	template <typename TSurface>
	class SurfaceImpl
	{
	public:
		typedef typename Types<TSurface>::TSurfaceDesc TSurfaceDesc;
		typedef typename Types<TSurface>::TDdsCaps TDdsCaps;
		typedef typename Types<TSurface>::TUnlockParam TUnlockParam;

		SurfaceImpl(Surface* data);
		virtual ~SurfaceImpl();

		virtual HRESULT AddAttachedSurface(TSurface* This, TSurface* lpDDSAttachedSurface);
		virtual HRESULT AddOverlayDirtyRect( TSurface * This, LPRECT lpRect );
		virtual HRESULT Blt(TSurface* This, LPRECT lpDestRect, TSurface* lpDDSrcSurface, LPRECT lpSrcRect,
			DWORD dwFlags, LPDDBLTFX lpDDBltFx);
		virtual HRESULT BltFast(TSurface* This, DWORD dwX, DWORD dwY,
			TSurface* lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwTrans);
		virtual HRESULT Flip(TSurface* This, TSurface* lpDDSurfaceTargetOverride, DWORD dwFlags);
		virtual HRESULT GetAttachedSurface(TSurface* This, TDdsCaps* lpDDSCaps, TSurface** lplpDDAttachedSurface);
		virtual HRESULT GetCaps(TSurface* This, TDdsCaps* lpDDSCaps);
		virtual HRESULT GetColorKey( TSurface * This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey );
		virtual HRESULT GetDC(TSurface* This, HDC* lphDC);
		virtual HRESULT GetFlipStatus( TSurface * This, DWORD dwFlags );
		virtual HRESULT GetOverlayPosition( TSurface * This, LPLONG lplX, LPLONG lplY );
		virtual HRESULT GetPalette(TSurface* This, LPDIRECTDRAWPALETTE* lplpDDPalette);
		virtual HRESULT GetPixelFormat( TSurface * This, LPDDPIXELFORMAT lpDDPixelFormat );
		virtual HRESULT GetSurfaceDesc(TSurface* This, TSurfaceDesc* lpDDSurfaceDesc);
		virtual HRESULT IsLost(TSurface* This);
		virtual HRESULT Lock(TSurface* This, LPRECT lpDestRect, TSurfaceDesc* lpDDSurfaceDesc,
			DWORD dwFlags, HANDLE hEvent);
		virtual HRESULT QueryInterface(TSurface* This, REFIID riid, LPVOID* obp);
		virtual HRESULT ReleaseDC(TSurface* This, HDC hDC);
		virtual HRESULT Restore(TSurface* This);
		virtual HRESULT SetColorKey( TSurface * This, DWORD dwFlags, LPDDCOLORKEY lpDDColorKey );
		virtual HRESULT SetOverlayPosition( TSurface * This, LONG lX, LONG lY );
		virtual HRESULT SetPalette(TSurface* This, LPDIRECTDRAWPALETTE lpDDPalette);
		virtual HRESULT SetSurfaceDesc(TSurface* This, TSurfaceDesc* lpddsd, DWORD dwFlags);
		virtual HRESULT Unlock(TSurface* This, TUnlockParam lpRect);
		virtual HRESULT UpdateOverlay( TSurface * This, LPRECT lpSrcRect, TSurface * lpDDDestSurface,
			LPRECT lpDestRect, DWORD dwFlags, LPDDOVERLAYFX lpDDOverlayFx );
		virtual HRESULT UpdateOverlayDisplay( TSurface * This, DWORD dwFlags );

	protected:
		Surface* m_data;

	private:
		void restoreOrigCaps(DWORD& caps);
		void waitForFlip(CompatWeakPtr<IDirectDrawSurface7> surface);
	};
}
