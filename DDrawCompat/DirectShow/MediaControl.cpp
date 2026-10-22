#include <Common/CompatVtable.h>
#include <Common/Log.h>
#include <DirectShow/MediaControl.h>
#include <DirectShow/VtableHook.h>
#include <DirectShow/Visitors/MediaControlVtblVisitor.h>

namespace
{
	template<>
	constexpr void setCompatVtable( IMediaControlVtbl & vtable )
	{
		vtable.Run = &DirectShow::logFailure<&IMediaControlVtbl::Run>;
		vtable.Pause = &DirectShow::logFailure<&IMediaControlVtbl::Pause>;
		vtable.Stop = &DirectShow::logFailure<&IMediaControlVtbl::Stop>;
	}
}

namespace DirectShow
{
	namespace MediaControl
	{
		void hookVtable( const IMediaControlVtbl & vtable )
		{
			DirectShow::hookVtable( vtable );
		}
	}
}