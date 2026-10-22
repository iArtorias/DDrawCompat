#pragma once

#include <ddraw.h>
#include <strmif.h>

#include <Common/VtableVisitor.h>

template <>
struct VtableForEach<IDDrawExclModeVideoVtbl>
{
	template <typename Vtable, typename Visitor>
	static void forEach( Visitor & visitor, UINT version )
	{
		VtableForEach<IUnknownVtbl>::forEach<Vtable>( visitor, version );

		DD_VISIT( SetDDrawObject );
		DD_VISIT( GetDDrawObject );
		DD_VISIT( SetDDrawSurface );
		DD_VISIT( GetDDrawSurface );
		DD_VISIT( SetDrawParameters );
		DD_VISIT( GetNativeVideoProps );
		DD_VISIT( SetCallbackInterface );
	}
};