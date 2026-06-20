#pragma once

#include <Windows.h>

const LONG DCX_USESTYLE = 0x10000;

namespace Gdi
{
	const ATOM MENU_ATOM = 0x8000;
	const ATOM DIALOG_ATOM = 0x8002;

	void checkDesktopComposition();
	void dllThreadDetach();
	ATOM getClassAtom(HINSTANCE hInstance, const wchar_t* className);
	ATOM getComboLBoxAtom();
	ATOM getSysShadowAtom();
	ATOM getUserAdapterWindowClassAtom();
	void installHooks();
	bool isDisplayDc(HDC dc);
	bool isRedirected(HWND hwnd);
	bool isRedirected(HDC dc);
	void onRegisterClass(const std::wstring& className, ATOM atom);
};
