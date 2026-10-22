#include <Common/CompatVtable.h>
#include <Common/Log.h>
#include <DDraw/Surfaces/OverlaySurface.h>
#include <DirectShow/DDrawExclModeVideo.h>
#include <DirectShow/VtableHook.h>
#include <DirectShow/Visitors/DDrawExclModeVideoVtblVisitor.h>

namespace
{
	HRESULT STDMETHODCALLTYPE GetNativeVideoProps( IDDrawExclModeVideo * This, LPDWORD pdwVideoWidth,
		LPDWORD pdwVideoHeight, LPDWORD pdwPictAspectRatioX, LPDWORD pdwPictAspectRatioY )
	{
		HRESULT result = getOrigVtable( This ).GetNativeVideoProps(
			This, pdwVideoWidth, pdwVideoHeight, pdwPictAspectRatioX, pdwPictAspectRatioY );
		if (SUCCEEDED( result ) && pdwVideoWidth && pdwVideoHeight)
		{
			LOG_INFO << "DirectShow: Native video size: " << *pdwVideoWidth << 'x' << *pdwVideoHeight;
		}
		return DirectShow::logResult<&IDDrawExclModeVideoVtbl::GetNativeVideoProps>( result );
	}

	HRESULT STDMETHODCALLTYPE SetDDrawObject( IDDrawExclModeVideo * This, IDirectDraw * pDDrawObject )
	{
		LOG_INFO << "DirectShow: Overlay mixer received DirectDraw object: " << pDDrawObject;
		DDraw::OverlaySurface::enableEmulation( reinterpret_cast<IUnknown *>(pDDrawObject) );
		return DirectShow::logResult<&IDDrawExclModeVideoVtbl::SetDDrawObject>(
			getOrigVtable( This ).SetDDrawObject( This, pDDrawObject ) );
	}

	HRESULT STDMETHODCALLTYPE SetDDrawSurface( IDDrawExclModeVideo * This, IDirectDrawSurface * pDDrawSurface )
	{
		LOG_INFO << "DirectShow: Overlay mixer received DirectDraw surface: " << pDDrawSurface;
		return DirectShow::logResult<&IDDrawExclModeVideoVtbl::SetDDrawSurface>(
			getOrigVtable( This ).SetDDrawSurface( This, pDDrawSurface ) );
	}

	HRESULT STDMETHODCALLTYPE SetDrawParameters( IDDrawExclModeVideo * This, const RECT * prcSource, const RECT * prcTarget )
	{
		if (prcTarget)
		{
			LOG_INFO << "DirectShow: Overlay mixer target rect: " << *prcTarget;
		}
		return DirectShow::logResult<&IDDrawExclModeVideoVtbl::SetDrawParameters>(
			getOrigVtable( This ).SetDrawParameters( This, prcSource, prcTarget ) );
	}

	template<>
	constexpr void setCompatVtable( IDDrawExclModeVideoVtbl & vtable )
	{
		vtable.SetDDrawObject = &SetDDrawObject;
		vtable.SetDDrawSurface = &SetDDrawSurface;
		vtable.SetDrawParameters = &SetDrawParameters;
		vtable.GetNativeVideoProps = &GetNativeVideoProps;
		vtable.SetCallbackInterface = &DirectShow::logFailure<&IDDrawExclModeVideoVtbl::SetCallbackInterface>;
	}
}

namespace DirectShow
{
	namespace DDrawExclModeVideo
	{
		void hookVtable( const IDDrawExclModeVideoVtbl & vtable )
		{
			DirectShow::hookVtable( vtable );
		}
	}
}