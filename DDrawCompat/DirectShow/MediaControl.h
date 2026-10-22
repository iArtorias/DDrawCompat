#pragma once

#include <control.h>

namespace DirectShow
{
	namespace MediaControl
	{
		void hookVtable( const IMediaControlVtbl & vtable );
	}
}