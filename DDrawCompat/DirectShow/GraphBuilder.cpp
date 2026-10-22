#include <Common/CompatVtable.h>
#include <Common/Log.h>
#include <Config/Settings/DirectShowAudioRenderer.h>
#include <DirectShow/GraphBuilder.h>
#include <DirectShow/VtableHook.h>
#include <DirectShow/Visitors/GraphBuilderVtblVisitor.h>

namespace
{
	const GUID CLSID_AUDIO_RENDERER =
	{ 0xE30629D1, 0x27E5, 0x11CE, { 0x87, 0x5D, 0x00, 0x60, 0x8C, 0xB7, 0x80, 0x66 } };
	const GUID IID_BASE_FILTER =
	{ 0x56A86895, 0x0AD4, 0x11CE, { 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70 } };

	IBaseFilter * addWaveOutRenderer( IGraphBuilder * graph )
	{
		IBaseFilter * filter = nullptr;
		HRESULT result = CoCreateInstance( CLSID_AUDIO_RENDERER, nullptr, CLSCTX_INPROC_SERVER, IID_BASE_FILTER,
			reinterpret_cast<void **>(&filter) );
		if (FAILED( result ) || !filter)
		{
			LOG_ONCE( "DirectShow: Failed to create the waveOut audio renderer: " << Compat::hex( result ) );
			return nullptr;
		}

		result = getOrigVtable( graph ).AddFilter( graph, filter, L"Audio Renderer" );
		if (FAILED( result ))
		{
			LOG_ONCE( "DirectShow: Failed to add the waveOut audio renderer: " << Compat::hex( result ) );
			filter->lpVtbl->Release( filter );
			return nullptr;
		}
		return filter;
	}

	bool isConnected( IBaseFilter * filter )
	{
		IEnumPins * enumPins = nullptr;
		if (FAILED( filter->lpVtbl->EnumPins( filter, &enumPins ) ) || !enumPins)
		{
			return false;
		}

		bool connected = false;
		IPin * pin = nullptr;
		while (!connected && S_OK == enumPins->lpVtbl->Next( enumPins, 1, &pin, nullptr ))
		{
			IPin * connectedPin = nullptr;
			if (SUCCEEDED( pin->lpVtbl->ConnectedTo( pin, &connectedPin ) ) && connectedPin)
			{
				connected = true;
				connectedPin->lpVtbl->Release( connectedPin );
			}
			pin->lpVtbl->Release( pin );
		}

		enumPins->lpVtbl->Release( enumPins );
		return connected;
	}

	HRESULT STDMETHODCALLTYPE AddSourceFilter( IGraphBuilder * This, LPCWSTR lpcwstrFileName,
		LPCWSTR lpcwstrFilterName, IBaseFilter ** ppFilter )
	{
		LOG_INFO << "DirectShow: Opening file: \"" << lpcwstrFileName << '"';
		return DirectShow::logResult<&IGraphBuilderVtbl::AddSourceFilter>(
			getOrigVtable( This ).AddSourceFilter( This, lpcwstrFileName, lpcwstrFilterName, ppFilter ) );
	}

	HRESULT STDMETHODCALLTYPE RenderFile( IGraphBuilder * This, LPCWSTR lpcwstrFile, LPCWSTR lpcwstrPlayList )
	{
		LOG_INFO << "DirectShow: Rendering file: \"" << lpcwstrFile << '"';

		IBaseFilter * waveOutRenderer = nullptr;
		if (Config::Settings::DirectShowAudioRenderer::WAVEOUT == Config::directShowAudioRenderer.get())
		{
			waveOutRenderer = addWaveOutRenderer( This );
		}

		const HRESULT result = getOrigVtable( This ).RenderFile( This, lpcwstrFile, lpcwstrPlayList );

		if (waveOutRenderer)
		{
			if (isConnected( waveOutRenderer ))
			{
				LOG_ONCE( "Using feature: waveOut audio renderer for DirectShow" );
			}
			else
			{
				getOrigVtable( This ).RemoveFilter( This, waveOutRenderer );
			}
			waveOutRenderer->lpVtbl->Release( waveOutRenderer );
		}

		return DirectShow::logResult<&IGraphBuilderVtbl::RenderFile>( result );
	}

	template<>
	constexpr void setCompatVtable( IGraphBuilderVtbl & vtable )
	{
		vtable.AddFilter = &DirectShow::logFailure<&IGraphBuilderVtbl::AddFilter>;
		vtable.RemoveFilter = &DirectShow::logFailure<&IGraphBuilderVtbl::RemoveFilter>;
		vtable.ConnectDirect = &DirectShow::logFailure<&IGraphBuilderVtbl::ConnectDirect>;
		vtable.Disconnect = &DirectShow::logFailure<&IGraphBuilderVtbl::Disconnect>;
		vtable.Connect = &DirectShow::logFailure<&IGraphBuilderVtbl::Connect>;
		vtable.Render = &DirectShow::logFailure<&IGraphBuilderVtbl::Render>;
		vtable.RenderFile = &RenderFile;
		vtable.AddSourceFilter = &AddSourceFilter;
	}
}

namespace DirectShow
{
	namespace GraphBuilder
	{
		void hookVtable( const IGraphBuilderVtbl & vtable )
		{
			DirectShow::hookVtable( vtable );
		}
	}
}