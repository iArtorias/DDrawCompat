#pragma once

#include <control.h>

#include <Common/VtableVisitor.h>

template <>
struct VtableForEach<IMediaControlVtbl>
{
	template <typename Vtable, typename Visitor>
	static void forEach( Visitor & visitor, UINT version )
	{
		VtableForEach<IUnknownVtbl>::forEach<Vtable>( visitor, version );

		DD_VISIT( GetTypeInfoCount );
		DD_VISIT( GetTypeInfo );
		DD_VISIT( GetIDsOfNames );
		DD_VISIT( Invoke );

		DD_VISIT( Run );
		DD_VISIT( Pause );
		DD_VISIT( Stop );
		DD_VISIT( GetState );
		DD_VISIT( RenderFile );
		DD_VISIT( AddSourceFilter );
		DD_VISIT( get_FilterCollection );
		DD_VISIT( get_RegFilterCollection );
		DD_VISIT( StopWhenReady );
	}
};