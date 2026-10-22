#pragma once

#include <ddraw.h>
#include <strmif.h>

namespace DirectShow
{
	namespace DDrawExclModeVideo
	{
		void hookVtable( const IDDrawExclModeVideoVtbl & vtable );
	}
}