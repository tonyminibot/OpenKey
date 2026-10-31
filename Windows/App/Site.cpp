#undef WIN32_LEAN_AND_MEAN   // UIAutomation needs the OLE/RPC types windows.h leaves out (before any header pulls windows.h in)
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include "Site.h"
#include "Prefs.h"
#include <algorithm>
#include <atomic>
#include <thread>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace vikey { namespace site {

// Everything UI Automation runs on its own thread (_uia belongs to it). The keyboard and mouse hooks run on the main
// thread: a UIA call into a busy browser blocks for seconds, and the cursor would crawl until it returned.
static IUIAutomation* _uia = nullptr;
static HANDLE _wake = nullptr;   // pollPassword(): check now instead of at the next second

bool isBrowser(const std::wstring& exeIn) {
    std::wstring exe = exeIn; std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
    static const wchar_t* list[] = {L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"opera_gx.exe", L"firefox.exe", L"chromium.exe", L"arc.exe"};
    for (auto b : list) if (exe == b) return true;
    return false;
}

static bool looksLikeUrl(const std::wstring& v) {
    if (v.empty() || v.size() > 2048) return false;
    if (v.rfind(L"http://", 0) == 0 || v.rfind(L"https://", 0) == 0) return true;
    if (v.find(L' ') != std::wstring::npos) return false;
    size_t dot = v.find(L'.');
    return dot != std::wstring::npos && dot > 0 && dot + 1 < v.size();
}

static std::wstring hostOf(std::wstring v) {
    if (v.rfind(L"file:", 0) == 0) return L"";   // a local file has no site
    size_t p = v.find(L"://"); if (p != std::wstring::npos) v = v.substr(p + 3);
    size_t e = v.find_first_of(L"/?#:"); if (e != std::wstring::npos) v = v.substr(0, e);
    std::transform(v.begin(), v.end(), v.begin(), ::towlower);
    if (v.rfind(L"www.", 0) == 0) v = v.substr(4);
    if (v.find(L'.') == std::wstring::npos && v != L"localhost") return L"";   // "C:/vk/x.html", "edge", a search: not a site
    return v;
}

/// The exe of the window in front (not core::frontAppName(): that string is the main thread's).
static std::wstring frontExe(HWND w) {
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); if (!p) return L"";
    wchar_t path[MAX_PATH] = {0}; DWORD n = MAX_PATH; std::wstring name;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) { std::wstring s(path); size_t i = s.find_last_of(L'\\'); name = i == std::wstring::npos ? s : s.substr(i + 1); }
    CloseHandle(p);
    return name;
}

/// Reads the address bar of the foreground browser; empty when not a browser or no URL.
static std::wstring currentHost(HWND w) {
    if (!isBrowser(frontExe(w))) return L"";
    IUIAutomationElement* root = nullptr; if (FAILED(_uia->ElementFromHandle(w, &root)) || !root) return L"";
    std::wstring host;
    VARIANT vt; vt.vt = VT_I4; vt.lVal = UIA_EditControlTypeId;
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cond)) && cond) {
        IUIAutomationElementArray* arr = nullptr;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &arr)) && arr) {
            int n = 0; arr->get_Length(&n);
            for (int i = 0; i < n && n < 200 && host.empty(); i++) {
                IUIAutomationElement* el = nullptr; if (FAILED(arr->GetElement(i, &el)) || !el) continue;
                IUIAutomationValuePattern* vp = nullptr;
                if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern, (void**)&vp)) && vp) {
                    BSTR b = nullptr; if (SUCCEEDED(vp->get_CurrentValue(&b)) && b) { std::wstring v(b, SysStringLen(b)); SysFreeString(b); if (looksLikeUrl(v)) host = hostOf(v); }
                    vp->Release();
                }
                el->Release();
            }
            arr->Release();
        }
        cond->Release();
    }
    root->Release();
    return host;
}

/// Vikey's own windows are in front: UI Automation calls into them wait on the main thread (the main window took ~30 s
/// to show while it was busy), and there is no site or password field to read there.
static bool ownWindowInFront(HWND w) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }

/// The focused control is a password field (UIA IsPassword): its keys go through unchanged and the typing log must not record them.
static std::atomic<bool> _focusPassword{false};
bool focusedIsPassword() { return _focusPassword; }
static void checkPassword() {
    IUIAutomationElement* el = nullptr;
    if (FAILED(_uia->GetFocusedElement(&el)) || !el) { _focusPassword = false; return; }
    BOOL pw = FALSE; el->get_CurrentIsPassword(&pw); el->Release();
    _focusPassword = pw != FALSE;
}
void pollPassword() { if (_wake) SetEvent(_wake); }

static void run(HWND owner) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation2* uia2 = nullptr;   // Windows 8+: timeouts, so a hung app costs this thread 2 s, not the default 20
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia2)))) {
        uia2->put_ConnectionTimeout(1000); uia2->put_TransactionTimeout(2000); _uia = uia2;
    } else if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_uia)))) return;
    ULONGLONG next = GetTickCount64() + 1000;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD r = MsgWaitForMultipleObjects(1, &_wake, FALSE, next > now ? (DWORD)(next - now) : 0, QS_ALLINPUT);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);   // COM in an STA needs its messages
        bool focusOnly = r == WAIT_OBJECT_0;
        if (r == WAIT_OBJECT_0 + 1 && GetTickCount64() < next) continue;   // only messages
        if (!focusOnly) next = GetTickCount64() + 1000;
        HWND w = GetForegroundWindow();
        if (ownWindowInFront(w)) { _focusPassword = false; continue; }
        checkPassword();
        if (focusOnly) continue;
        std::wstring host = prefs::getInt(L"SmartSwitchPerSite", 1) ? currentHost(w) : L"";
        auto* posted = new std::wstring(host);   // the main thread's onSiteChanged skips an unchanged host
        if (!PostMessageW(owner, WM_SITE, 0, (LPARAM)posted)) delete posted;
    }
}

void start(HWND owner) {
    if (_wake) return;
    _wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(run, owner).detach();
}

}}
