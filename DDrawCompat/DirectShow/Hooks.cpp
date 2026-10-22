#include <intrin.h>

#include <Windows.h>
#include <objbase.h>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <DirectShow/DDrawExclModeVideo.h>
#include <DirectShow/GraphBuilder.h>
#include <DirectShow/Hooks.h>
#include <DirectShow/MediaControl.h>

namespace
{
	const GUID CLSID_AM_MULTI_MEDIA_STREAM =
	{ 0x49C47CE5, 0x9BA4, 0x11D0, { 0x82, 0x12, 0x00, 0xC0, 0x4F, 0xC3, 0x2C, 0x45 } };
	const GUID CLSID_FILTER_GRAPH =
	{ 0xE436EBB3, 0x524F, 0x11CE, { 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };
	const GUID CLSID_FILTER_GRAPH_NO_THREAD =
	{ 0xE436EBB8, 0x524F, 0x11CE, { 0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };
	const GUID CLSID_OVERLAY_MIXER =
	{ 0xCD8743A1, 0x3736, 0x11D0, { 0x9E, 0x69, 0x00, 0xC0, 0x4F, 0xD7, 0xC1, 0x5B } };
	const GUID CLSID_VIDEO_RENDERER =
	{ 0x70E102B0, 0x5556, 0x11CE, { 0x97, 0xC0, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A } };
	const GUID CLSID_VIDEO_RENDERER_DEFAULT =
	{ 0x6BC1CFFA, 0x8FC1, 0x4261, { 0xAC, 0x22, 0xCF, 0xB4, 0xCC, 0x38, 0xDB, 0x50 } };

	const GUID IID_DDRAW_EXCL_MODE_VIDEO =
	{ 0x153ACC21, 0xD83B, 0x11D1, { 0x82, 0xBF, 0x00, 0xA0, 0xC9, 0x69, 0x6C, 0x8F } };
	const GUID IID_GRAPH_BUILDER =
	{ 0x56A868A9, 0x0AD4, 0x11CE, { 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };
	const GUID IID_MEDIA_CONTROL =
	{ 0x56A868B1, 0x0AD4, 0x11CE, { 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };

	const char * getClassName( REFCLSID rclsid )
	{
		if (CLSID_FILTER_GRAPH == rclsid || CLSID_FILTER_GRAPH_NO_THREAD == rclsid)
		{
			return "Filter graph";
		}
		if (CLSID_OVERLAY_MIXER == rclsid)
		{
			return "Overlay mixer";
		}
		if (CLSID_VIDEO_RENDERER == rclsid || CLSID_VIDEO_RENDERER_DEFAULT == rclsid)
		{
			return "Video renderer";
		}
		if (CLSID_AM_MULTI_MEDIA_STREAM == rclsid)
		{
			return "Multimedia stream";
		}
		return nullptr;
	}

	template <typename Interface, typename HookVtableFunc>
	void hookInterface( IUnknown * object, REFIID riid, HookVtableFunc hookVtable )
	{
		Interface * intf = nullptr;
		if (SUCCEEDED( object->lpVtbl->QueryInterface( object, riid, reinterpret_cast<void **>(&intf) ) ) && intf)
		{
			hookVtable( *intf->lpVtbl );
			intf->lpVtbl->Release( intf );
		}
	}

	void hookObject( REFCLSID rclsid, IUnknown * object )
	{
		if (CLSID_OVERLAY_MIXER == rclsid)
		{
			hookInterface<IDDrawExclModeVideo>( object, IID_DDRAW_EXCL_MODE_VIDEO,
				&DirectShow::DDrawExclModeVideo::hookVtable );
		}
		else if (CLSID_FILTER_GRAPH == rclsid || CLSID_FILTER_GRAPH_NO_THREAD == rclsid)
		{
			hookInterface<IGraphBuilder>( object, IID_GRAPH_BUILDER, &DirectShow::GraphBuilder::hookVtable );
			hookInterface<IMediaControl>( object, IID_MEDIA_CONTROL, &DirectShow::MediaControl::hookVtable );
		}
	}

	HRESULT WINAPI coCreateInstance( REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID * ppv )
	{
		LOG_FUNC( "CoCreateInstance", rclsid, pUnkOuter, Compat::hex( dwClsContext ), riid, ppv );
		HRESULT result = CALL_ORIG_FUNC( CoCreateInstance )(rclsid, pUnkOuter, dwClsContext, riid, ppv);

		const char * className = getClassName( rclsid );
		if (className)
		{
			if (FAILED( result ) || !ppv || !*ppv)
			{
				LOG_INFO << "DirectShow: Failed to create " << className << ": " << Compat::hex( result );
			}
			else
			{
				LOG_INFO << "DirectShow: Created " << className << " for " << Compat::funcPtrToStr( _ReturnAddress() );
				if (!pUnkOuter)
				{
					hookObject( rclsid, static_cast<IUnknown *>(*ppv) );
				}
			}
		}

		return LOG_RESULT( result );
	}
}

namespace DirectShow
{
	void installHooks()
	{
		HOOK_FUNCTION( ole32, CoCreateInstance, coCreateInstance );
	}
}