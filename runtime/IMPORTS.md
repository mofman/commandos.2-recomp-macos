# Import surface (comm2.exe retail, unwrapped)

208 imports. Each needs a host shim in runtime/.

## KERNEL32.dll (112)

- `GlobalUnlock` — IAT 0x00ba38e4
- `GetTickCount` — IAT 0x00ba38e8
- `GetCommandLineA` — IAT 0x00ba38ec
- `GlobalAlloc` — IAT 0x00ba38f0
- `GetFileAttributesA` — IAT 0x00ba38f4
- `SetFileAttributesA` — IAT 0x00ba38f8
- `GetLastError` — IAT 0x00ba38fc
- `CreateMutexA` — IAT 0x00ba3900
- `GetModuleHandleA` — IAT 0x00ba3904
- `ReleaseMutex` — IAT 0x00ba3908
- `QueryPerformanceCounter` — IAT 0x00ba390c
- `CopyFileA` — IAT 0x00ba3910
- `CreateEventA` — IAT 0x00ba3914
- `LocalFree` — IAT 0x00ba3918
- `FormatMessageA` — IAT 0x00ba391c
- `CloseHandle` — IAT 0x00ba3920
- `SetCurrentDirectoryA` — IAT 0x00ba3924
- `GetCurrentDirectoryA` — IAT 0x00ba3928
- `RaiseException` — IAT 0x00ba392c
- `GetStartupInfoA` — IAT 0x00ba3930
- `GetVersion` — IAT 0x00ba3934
- `QueryPerformanceFrequency` — IAT 0x00ba3938
- `ExitProcess` — IAT 0x00ba393c
- `GetCurrentProcess` — IAT 0x00ba3940
- `EnterCriticalSection` — IAT 0x00ba3944
- `LeaveCriticalSection` — IAT 0x00ba3948
- `HeapAlloc` — IAT 0x00ba394c
- `HeapFree` — IAT 0x00ba3950
- `GetLocalTime` — IAT 0x00ba3954
- `WideCharToMultiByte` — IAT 0x00ba3958
- `GetTimeZoneInformation` — IAT 0x00ba395c
- `SetConsoleCtrlHandler` — IAT 0x00ba3960
- `CreateDirectoryA` — IAT 0x00ba3964
- `InterlockedDecrement` — IAT 0x00ba3968
- `InterlockedIncrement` — IAT 0x00ba396c
- `FindFirstFileA` — IAT 0x00ba3970
- `FindNextFileA` — IAT 0x00ba3974
- `FindClose` — IAT 0x00ba3978
- `FileTimeToLocalFileTime` — IAT 0x00ba397c
- `GetFullPathNameA` — IAT 0x00ba3980
- `GetDriveTypeA` — IAT 0x00ba3984
- `InitializeCriticalSection` — IAT 0x00ba3988
- `DeleteCriticalSection` — IAT 0x00ba398c
- `FatalAppExitA` — IAT 0x00ba3990
- `TerminateProcess` — IAT 0x00ba3994
- `GetModuleFileNameA` — IAT 0x00ba3998
- `HeapSize` — IAT 0x00ba399c
- `GetProcAddress` — IAT 0x00ba39a0
- `UnhandledExceptionFilter` — IAT 0x00ba39a4
- `GlobalLock` — IAT 0x00ba39a8
- `FreeEnvironmentStringsA` — IAT 0x00ba39ac
- `FreeEnvironmentStringsW` — IAT 0x00ba39b0
- `GetEnvironmentStrings` — IAT 0x00ba39b4
- `GetEnvironmentStringsW` — IAT 0x00ba39b8
- `SetHandleCount` — IAT 0x00ba39bc
- `GetStdHandle` — IAT 0x00ba39c0
- `GetFileType` — IAT 0x00ba39c4
- `GetCurrentThreadId` — IAT 0x00ba39c8
- `TlsSetValue` — IAT 0x00ba39cc
- `TlsAlloc` — IAT 0x00ba39d0
- `TlsFree` — IAT 0x00ba39d4
- `SetLastError` — IAT 0x00ba39d8
- `TlsGetValue` — IAT 0x00ba39dc
- `GetCurrentThread` — IAT 0x00ba39e0
- `HeapDestroy` — IAT 0x00ba39e4
- `HeapCreate` — IAT 0x00ba39e8
- `VirtualFree` — IAT 0x00ba39ec
- `RtlUnwind` — IAT 0x00ba39f0
- `WriteFile` — IAT 0x00ba39f4
- `VirtualAlloc` — IAT 0x00ba39f8
- `IsBadWritePtr` — IAT 0x00ba39fc
- `ReadFile` — IAT 0x00ba3a00
- `SetFilePointer` — IAT 0x00ba3a04
- `SetStdHandle` — IAT 0x00ba3a08
- `Sleep` — IAT 0x00ba3a0c
- `MultiByteToWideChar` — IAT 0x00ba3a10
- `LCMapStringA` — IAT 0x00ba3a14
- `LCMapStringW` — IAT 0x00ba3a18
- `IsBadReadPtr` — IAT 0x00ba3a1c
- `IsBadCodePtr` — IAT 0x00ba3a20
- `OutputDebugStringA` — IAT 0x00ba3a24
- `GetACP` — IAT 0x00ba3a28
- `GetOEMCP` — IAT 0x00ba3a2c
- `LoadLibraryA` — IAT 0x00ba3a30
- `FlushFileBuffers` — IAT 0x00ba3a34
- `CompareStringA` — IAT 0x00ba3a38
- `CompareStringW` — IAT 0x00ba3a3c
- `SetEnvironmentVariableA` — IAT 0x00ba3a40
- `GetStringTypeA` — IAT 0x00ba3a44
- `GetStringTypeW` — IAT 0x00ba3a48
- `CreateFileA` — IAT 0x00ba3a4c
- `IsValidLocale` — IAT 0x00ba3a50
- `IsValidCodePage` — IAT 0x00ba3a54
- `GetLocaleInfoA` — IAT 0x00ba3a58
- `EnumSystemLocalesA` — IAT 0x00ba3a5c
- `GetUserDefaultLCID` — IAT 0x00ba3a60
- `GetVersionExA` — IAT 0x00ba3a64
- `GetExitCodeProcess` — IAT 0x00ba3a68
- `WaitForSingleObject` — IAT 0x00ba3a6c
- `CreateProcessA` — IAT 0x00ba3a70
- `SetEndOfFile` — IAT 0x00ba3a74
- `GetLocaleInfoW` — IAT 0x00ba3a78
- `SetUnhandledExceptionFilter` — IAT 0x00ba3a7c
- `HeapReAlloc` — IAT 0x00ba3a80
- `FileTimeToSystemTime` — IAT 0x00ba3a84
- `GetCPInfo` — IAT 0x00ba3a88
- `TerminateThread` — IAT 0x00ba3a8c
- `SuspendThread` — IAT 0x00ba3a90
- `CreateThread` — IAT 0x00ba3a94
- `ResumeThread` — IAT 0x00ba3a98
- `SleepEx` — IAT 0x00ba3a9c
- `SetThreadPriority` — IAT 0x00ba3aa0

## USER32.dll (69)

- `ReleaseCapture` — IAT 0x00ba3b28
- `SetCursorPos` — IAT 0x00ba3b2c
- `ClientToScreen` — IAT 0x00ba3b30
- `SetCursor` — IAT 0x00ba3b34
- `IsDialogMessageA` — IAT 0x00ba3b38
- `SendDlgItemMessageA` — IAT 0x00ba3b3c
- `LoadCursorA` — IAT 0x00ba3b40
- `UnregisterClassA` — IAT 0x00ba3b44
- `InvalidateRect` — IAT 0x00ba3b48
- `SetCapture` — IAT 0x00ba3b4c
- `GetFocus` — IAT 0x00ba3b50
- `BeginPaint` — IAT 0x00ba3b54
- `ShowCursor` — IAT 0x00ba3b58
- `GetWindowLongA` — IAT 0x00ba3b5c
- `MoveWindow` — IAT 0x00ba3b60
- `GetWindowRect` — IAT 0x00ba3b64
- `GetDesktopWindow` — IAT 0x00ba3b68
- `CreateDialogParamA` — IAT 0x00ba3b6c
- `EndDialog` — IAT 0x00ba3b70
- `GetDlgItem` — IAT 0x00ba3b74
- `CheckDlgButton` — IAT 0x00ba3b78
- `SetWindowLongA` — IAT 0x00ba3b7c
- `EndPaint` — IAT 0x00ba3b80
- `LoadIconA` — IAT 0x00ba3b84
- `GetDlgItemTextA` — IAT 0x00ba3b88
- `SetWindowPos` — IAT 0x00ba3b8c
- `GetDoubleClickTime` — IAT 0x00ba3b90
- `GetKeyState` — IAT 0x00ba3b94
- `AdjustWindowRectEx` — IAT 0x00ba3b98
- `GetMenu` — IAT 0x00ba3b9c
- `SystemParametersInfoA` — IAT 0x00ba3ba0
- `ReleaseDC` — IAT 0x00ba3ba4
- `GetDC` — IAT 0x00ba3ba8
- `MapWindowPoints` — IAT 0x00ba3bac
- `AdjustWindowRect` — IAT 0x00ba3bb0
- `UpdateWindow` — IAT 0x00ba3bb4
- `RegisterClassA` — IAT 0x00ba3bb8
- `MsgWaitForMultipleObjects` — IAT 0x00ba3bbc
- `GetAsyncKeyState` — IAT 0x00ba3bc0
- `wsprintfA` — IAT 0x00ba3bc4
- `SendMessageA` — IAT 0x00ba3bc8
- `FillRect` — IAT 0x00ba3bcc
- `CreateWindowExA` — IAT 0x00ba3bd0
- `ShowWindow` — IAT 0x00ba3bd4
- `SetFocus` — IAT 0x00ba3bd8
- `DestroyWindow` — IAT 0x00ba3bdc
- `GetClientRect` — IAT 0x00ba3be0
- `DefWindowProcA` — IAT 0x00ba3be4
- `CheckRadioButton` — IAT 0x00ba3be8
- `IsDlgButtonChecked` — IAT 0x00ba3bec
- `SetDlgItemTextA` — IAT 0x00ba3bf0
- `MessageBoxA` — IAT 0x00ba3bf4
- `SetRect` — IAT 0x00ba3bf8
- `PeekMessageA` — IAT 0x00ba3bfc
- `TranslateMessage` — IAT 0x00ba3c00
- `FindWindowA` — IAT 0x00ba3c04
- `SetActiveWindow` — IAT 0x00ba3c08
- `SetForegroundWindow` — IAT 0x00ba3c0c
- `PostQuitMessage` — IAT 0x00ba3c10
- `GetCursorPos` — IAT 0x00ba3c14
- `WindowFromPoint` — IAT 0x00ba3c18
- `GetCapture` — IAT 0x00ba3c1c
- `ScreenToClient` — IAT 0x00ba3c20
- `DispatchMessageA` — IAT 0x00ba3c24
- `CloseClipboard` — IAT 0x00ba3c28
- `EmptyClipboard` — IAT 0x00ba3c2c
- `GetClipboardData` — IAT 0x00ba3c30
- `OpenClipboard` — IAT 0x00ba3c34
- `SetClipboardData` — IAT 0x00ba3c38

## COMCTL32.dll (1)

- `#17` — IAT 0x00ba3764

## ole32.dll (3)

- `CoUninitialize` — IAT 0x00ba3d00
- `CoInitialize` — IAT 0x00ba3d04
- `CoCreateInstance` — IAT 0x00ba3d08

## WINMM.dll (1)

- `timeGetTime` — IAT 0x00ba3c9c

## GDI32.dll (10)

- `GetStockObject` — IAT 0x00ba3854
- `BitBlt` — IAT 0x00ba3858
- `CreateFontIndirectA` — IAT 0x00ba385c
- `GetDeviceCaps` — IAT 0x00ba3860
- `GetObjectA` — IAT 0x00ba3864
- `StretchBlt` — IAT 0x00ba3868
- `SetStretchBltMode` — IAT 0x00ba386c
- `DeleteObject` — IAT 0x00ba3870
- `SelectObject` — IAT 0x00ba3874
- `CreateSolidBrush` — IAT 0x00ba3878

## comdlg32.dll (2)

- `ChooseFontA` — IAT 0x00ba3ccc
- `GetSaveFileNameA` — IAT 0x00ba3cd0

## ADVAPI32.dll (4)

- `RegQueryInfoKeyA` — IAT 0x00ba3728
- `RegCloseKey` — IAT 0x00ba372c
- `RegEnumValueA` — IAT 0x00ba3730
- `RegOpenKeyExA` — IAT 0x00ba3734

## DDRAW.dll (1)

- `DirectDrawCreate` — IAT 0x00ba3794

## DSOUND.dll (1)

- `#1` — IAT 0x00ba3824

## DPLAYX.dll (1)

- `#4` — IAT 0x00ba37f4

## DINPUT.dll (1)

- `DirectInputCreateEx` — IAT 0x00ba37c4

## IMM32.dll (2)

- `ImmSetOpenStatus` — IAT 0x00ba38b0
- `ImmGetContext` — IAT 0x00ba38b4
