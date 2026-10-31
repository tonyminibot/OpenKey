// Per-website memory: the host of the active tab of the browser in front, read through UI Automation.
#pragma once
#include <windows.h>
#include <string>
namespace vikey { namespace site {
bool isBrowser(const std::wstring& exe);
/// Posted to the owner window when the site in front changes: lParam = new std::wstring (the host; the receiver deletes it).
const UINT WM_SITE = WM_APP + 6;
/// Starts the UI Automation thread: it polls every second (the active tab can change without any event we see).
/// UI Automation calls into other apps can block for seconds; on the hook thread every mouse move would wait with them.
void start(HWND owner);
/// True while a password field has the focus (checked by the UI Automation thread).
bool focusedIsPassword();
/// Re-checks the focused control soon (call on focus changes); does not block.
void pollPassword();
}}
