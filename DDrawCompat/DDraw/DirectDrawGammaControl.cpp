#include <Common/CompatVtable.h>
#include <DDraw/DirectDrawGammaControl.h>
#include <DDraw/RealPrimarySurface.h>
#include <DDraw/ScopedThreadLock.h>
#include <DDraw/Surfaces/PrimarySurface.h>
#include <DDraw/Visitors/DirectDrawGammaControlVtblVisitor.h>

namespace
{
	DDraw::PrimarySurface* getPrimary(IDirectDrawGammaControl* gamma)
	{
		return dynamic_cast<DDraw::PrimarySurface*>(DDraw::Surface::getSurface(*CompatPtr<IDirectDrawSurface7>::from(gamma)));
	}

	HRESULT STDMETHODCALLTYPE GetGammaRamp(
		IDirectDrawGammaControl* This, DWORD dwFlags, LPDDGAMMARAMP lpRampData)
	{
		auto primary = getPrimary(This);
		if (0 != dwFlags || !lpRampData || !primary)
		{
			return getOrigVtable(This).GetGammaRamp(This, dwFlags, lpRampData);
		}

		auto realPrimary = primary->getRealPrimary();
		if (!realPrimary)
		{
			return DDERR_SURFACELOST;
		}
		return realPrimary->getGammaRamp(lpRampData);
	}

	HRESULT STDMETHODCALLTYPE SetGammaRamp(
		IDirectDrawGammaControl* This, DWORD dwFlags, LPDDGAMMARAMP lpRampData)
	{
		auto primary = getPrimary(This);
		if ((0 != dwFlags && DDSGR_CALIBRATE != dwFlags) || !primary)
		{
			return getOrigVtable(This).SetGammaRamp(This, dwFlags, lpRampData);
		}

		auto realPrimary = primary->getRealPrimary();
		if (!realPrimary)
		{
			return DDERR_SURFACELOST;
		}
		return realPrimary->setGammaRamp(lpRampData);
	}

	template<>
	constexpr void setCompatVtable(IDirectDrawGammaControlVtbl& vtable)
	{
		vtable.GetGammaRamp = &GetGammaRamp;
		vtable.SetGammaRamp = &SetGammaRamp;
	}
}

namespace DDraw
{
	namespace DirectDrawGammaControl
	{
		void hookVtable(const IDirectDrawGammaControlVtbl& vtable)
		{
			CompatVtable<IDirectDrawGammaControlVtbl>::hookVtable<ScopedThreadLock>(vtable);
		}
	}
}
