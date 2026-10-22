#pragma once

#include <string>

#include <Windows.h>

#include <Common/CompatVtable.h>
#include <Common/Log.h>
#include <Common/ScopedCriticalSection.h>

namespace DirectShow
{
	template <auto memberPtr>
	std::string g_funcName;

	template <typename Vtable>
	class FuncNameVisitor
	{
	public:
		template <auto memberPtr>
		void visit( const char * funcName )
		{
			g_funcName<memberPtr> = Compat::getTypeName<Vtable>() + "::" + funcName;
		}
	};

	template <auto memberPtr>
	HRESULT logResult( HRESULT result )
	{
		if (FAILED( result ))
		{
			LOG_INFO << "DirectShow: " << g_funcName<memberPtr> << " failed: " << Compat::hex( result );
		}
		return result;
	}

	template <auto memberPtr, typename Interface, typename... Params>
	HRESULT STDMETHODCALLTYPE logFailure( Interface * This, Params... params )
	{
		return logResult<memberPtr>( (getOrigVtable( This ).*memberPtr)(This, params...) );
	}

	template <typename Vtable>
	void hookVtable( const Vtable & vtable )
	{
		static Compat::CriticalSection cs;
		static const Vtable * hookedVtable = nullptr;

		Compat::ScopedCriticalSection lock( cs );
		if (hookedVtable)
		{
			if (hookedVtable != &vtable)
			{
				LOG_ONCE( "DirectShow: Skipped hooking another " << Compat::getTypeName<Vtable>()
					<< " implementation: " << Compat::funcPtrToStr( &vtable ) );
			}
			return;
		}
		hookedVtable = &vtable;

		FuncNameVisitor<Vtable> funcNameVisitor;
		forEach<Vtable>( funcNameVisitor, 0 );

		class NullLock
		{};
		CompatVtable<Vtable>::template hookVtable<NullLock>( vtable );
	}
}