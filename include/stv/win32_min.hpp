#pragma once
// Deliberately small Windows x64 bootstrap declarations. Only used without SDK headers.
// Native source-lab frontend; no Windows SDK headers are required by this bootstrap.
// Layouts checked at compile time for the Windows x64 ABI.
#if !defined(_WIN32) || !defined(_WIN64)
#error This header is only for Windows x64
#endif
#include "base.hpp"
namespace win {
using BOOL=int;using DWORD=unsigned long;using UINT=unsigned int;using WORD=unsigned short;using LONG=long;
using HANDLE=void*;using HWND=HANDLE;using HINSTANCE=HANDLE;using HICON=HANDLE;using HCURSOR=HANDLE;
using HBRUSH=HANDLE;using HDC=HANDLE;using HMENU=HANDLE;using HGDIOBJ=HANDLE;using HDROP=HANDLE;
using WPARAM=stv::u64;using LPARAM=stv::i64;using LRESULT=stv::i64;using UINT_PTR=stv::u64;
using SIZE_T=stv::usize;using LPCWSTR=const wchar_t*;using LPWSTR=wchar_t*;
using WNDPROC=LRESULT(__stdcall*)(HWND,UINT,WPARAM,LPARAM);
using FARPROC=stv::i64(__stdcall*)();
struct POINT {LONG x,y;};struct RECT {LONG left,top,right,bottom;};
struct MSG {HWND hwnd;UINT message;WPARAM wParam;LPARAM lParam;DWORD time;POINT pt;DWORD lPrivate;};
struct WNDCLASSEXW {UINT cbSize,style;WNDPROC lpfnWndProc;int cbClsExtra,cbWndExtra;HINSTANCE hInstance;HICON hIcon;HCURSOR hCursor;HBRUSH hbrBackground;LPCWSTR lpszMenuName,lpszClassName;HICON hIconSm;};
struct PAINTSTRUCT {HDC hdc;BOOL fErase;RECT rcPaint;BOOL fRestore,fIncUpdate;unsigned char rgbReserved[32];};
struct OPENFILENAMEW {DWORD lStructSize;HWND hwndOwner;HINSTANCE hInstance;LPCWSTR lpstrFilter;LPWSTR lpstrCustomFilter;DWORD nMaxCustFilter,nFilterIndex;LPWSTR lpstrFile;DWORD nMaxFile;LPWSTR lpstrFileTitle;DWORD nMaxFileTitle;LPCWSTR lpstrInitialDir,lpstrTitle;DWORD Flags;WORD nFileOffset,nFileExtension;LPCWSTR lpstrDefExt;LPARAM lCustData;void* lpfnHook;LPCWSTR lpTemplateName;void* pvReserved;DWORD dwReserved,FlagsEx;};
struct BITMAPINFOHEADER {DWORD biSize;LONG biWidth,biHeight;WORD biPlanes,biBitCount;DWORD biCompression,biSizeImage;LONG biXPelsPerMeter,biYPelsPerMeter;DWORD biClrUsed,biClrImportant;};
struct BITMAPINFO {BITMAPINFOHEADER bmiHeader;DWORD bmiColors[1];};
struct MONITORINFO {DWORD cbSize;RECT rcMonitor,rcWork;DWORD dwFlags;};
static_assert(sizeof(BITMAPINFOHEADER)==40 && sizeof(MONITORINFO)==40);
struct XINPUT_GAMEPAD {WORD wButtons;unsigned char bLeftTrigger,bRightTrigger;short sThumbLX,sThumbLY,sThumbRX,sThumbRY;};
struct XINPUT_STATE {DWORD dwPacketNumber;XINPUT_GAMEPAD Gamepad;};
using XINPUT_GET_STATE=DWORD(__stdcall*)(DWORD,XINPUT_STATE*);
static_assert(sizeof(DWORD)==4 && sizeof(wchar_t)==2 && sizeof(MSG)==48 && sizeof(WNDCLASSEXW)==80 && sizeof(PAINTSTRUCT)==72 && sizeof(OPENFILENAMEW)==152 && sizeof(XINPUT_STATE)==16);
#define WIN_API extern "C" __declspec(dllimport)
WIN_API HINSTANCE __stdcall GetModuleHandleW(LPCWSTR);
WIN_API void __stdcall ExitProcess(UINT);
WIN_API void* __stdcall VirtualAlloc(void*,SIZE_T,DWORD,DWORD);
WIN_API BOOL __stdcall VirtualFree(void*,SIZE_T,DWORD);
WIN_API DWORD __stdcall GetLastError();
WIN_API HANDLE __stdcall CreateFileW(LPCWSTR,DWORD,DWORD,void*,DWORD,DWORD,HANDLE);
WIN_API BOOL __stdcall ReadFile(HANDLE,void*,DWORD,DWORD*,void*);
WIN_API BOOL __stdcall WriteFile(HANDLE,const void*,DWORD,DWORD*,void*);
WIN_API BOOL __stdcall FlushFileBuffers(HANDLE);
WIN_API BOOL __stdcall CloseHandle(HANDLE);
WIN_API BOOL __stdcall GetFileSizeEx(HANDLE,stv::i64*);
WIN_API BOOL __stdcall DeleteFileW(LPCWSTR);
WIN_API BOOL __stdcall MoveFileExW(LPCWSTR,LPCWSTR,DWORD);
WIN_API HANDLE __stdcall LoadLibraryExW(LPCWSTR,HANDLE,DWORD);
WIN_API FARPROC __stdcall GetProcAddress(HANDLE,const char*);
WIN_API BOOL __stdcall FreeLibrary(HANDLE);
WIN_API WORD __stdcall RegisterClassExW(const WNDCLASSEXW*);
WIN_API HWND __stdcall CreateWindowExW(DWORD,LPCWSTR,LPCWSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,void*);
WIN_API BOOL __stdcall ShowWindow(HWND,int);
WIN_API BOOL __stdcall UpdateWindow(HWND);
WIN_API BOOL __stdcall GetMessageW(MSG*,HWND,UINT,UINT);
WIN_API BOOL __stdcall TranslateMessage(const MSG*);
WIN_API LRESULT __stdcall DispatchMessageW(const MSG*);
WIN_API LRESULT __stdcall DefWindowProcW(HWND,UINT,WPARAM,LPARAM);
WIN_API void __stdcall PostQuitMessage(int);
WIN_API BOOL __stdcall DestroyWindow(HWND);
WIN_API HDC __stdcall BeginPaint(HWND,PAINTSTRUCT*);
WIN_API BOOL __stdcall EndPaint(HWND,const PAINTSTRUCT*);
WIN_API BOOL __stdcall GetClientRect(HWND,RECT*);
WIN_API int __stdcall DrawTextW(HDC,LPCWSTR,int,RECT*,UINT);
WIN_API int __stdcall FillRect(HDC,const RECT*,HBRUSH);
WIN_API BOOL __stdcall InvalidateRect(HWND,const RECT*,BOOL);
WIN_API HCURSOR __stdcall LoadCursorW(HINSTANCE,LPCWSTR);
WIN_API int __stdcall MessageBoxW(HWND,LPCWSTR,LPCWSTR,UINT);
WIN_API UINT_PTR __stdcall SetTimer(HWND,UINT_PTR,UINT,void*);
WIN_API BOOL __stdcall KillTimer(HWND,UINT_PTR);
WIN_API short __stdcall GetAsyncKeyState(int);
WIN_API HWND __stdcall GetForegroundWindow();
WIN_API HBRUSH __stdcall CreateSolidBrush(DWORD);
WIN_API BOOL __stdcall DeleteObject(HGDIOBJ);
WIN_API DWORD __stdcall SetTextColor(HDC,DWORD);
WIN_API int __stdcall SetBkMode(HDC,int);
WIN_API BOOL __stdcall GetOpenFileNameW(OPENFILENAMEW*);
WIN_API BOOL __stdcall GetSaveFileNameW(OPENFILENAMEW*);
WIN_API void __stdcall DragAcceptFiles(HWND,BOOL);
WIN_API UINT __stdcall DragQueryFileW(HDROP,UINT,LPWSTR,UINT);
WIN_API void __stdcall DragFinish(HDROP);
WIN_API BOOL __stdcall QueryPerformanceCounter(stv::i64*);
WIN_API BOOL __stdcall QueryPerformanceFrequency(stv::i64*);
WIN_API DWORD __stdcall GetModuleFileNameW(HINSTANCE,LPWSTR,DWORD);
WIN_API DWORD __stdcall GetEnvironmentVariableW(LPCWSTR,LPWSTR,DWORD);
WIN_API BOOL __stdcall CreateDirectoryW(LPCWSTR,void*);
WIN_API UINT __stdcall GetPrivateProfileIntW(LPCWSTR,LPCWSTR,int,LPCWSTR);
WIN_API BOOL __stdcall WritePrivateProfileStringW(LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR);
WIN_API BOOL __stdcall GetWindowRect(HWND,RECT*);
WIN_API stv::i64 __stdcall GetWindowLongPtrW(HWND,int);
WIN_API stv::i64 __stdcall SetWindowLongPtrW(HWND,int,stv::i64);
WIN_API BOOL __stdcall SetWindowPos(HWND,HWND,int,int,int,int,UINT);
WIN_API HANDLE __stdcall MonitorFromWindow(HWND,DWORD);
WIN_API BOOL __stdcall GetMonitorInfoW(HANDLE,MONITORINFO*);
WIN_API int __stdcall SetStretchBltMode(HDC,int);
WIN_API int __stdcall StretchDIBits(HDC,int,int,int,int,int,int,int,int,const void*,const BITMAPINFO*,UINT,DWORD);
#undef WIN_API
}
