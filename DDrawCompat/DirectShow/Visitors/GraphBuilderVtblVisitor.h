#pragma once

#include <ddraw.h>
#include <strmif.h>

#include <Common/VtableVisitor.h>

template <>
struct VtableForEach<IGraphBuilderVtbl>
{
	template <typename Vtable, typename Visitor>
	static void forEach( Visitor & visitor, UINT version )
	{
		VtableForEach<IUnknownVtbl>::forEach<Vtable>( visitor, version );

		DD_VISIT( AddFilter );
		DD_VISIT( RemoveFilter );
		DD_VISIT( EnumFilters );
		DD_VISIT( FindFilterByName );
		DD_VISIT( ConnectDirect );
		DD_VISIT( Reconnect );
		DD_VISIT( Disconnect );
		DD_VISIT( SetDefaultSyncSource );
		DD_VISIT( Connect );
		DD_VISIT( Render );
		DD_VISIT( RenderFile );
		DD_VISIT( AddSourceFilter );
		DD_VISIT( SetLogFile );
		DD_VISIT( Abort );
		DD_VISIT( ShouldOperationContinue );
	}
};