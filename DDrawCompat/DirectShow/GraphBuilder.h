#pragma once

#include <ddraw.h>
#include <strmif.h>

namespace DirectShow
{
	namespace GraphBuilder
	{
		void hookVtable( const IGraphBuilderVtbl & vtable );
	}
}