#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <string>
#include <vector>

#include "PolicyConfig.h"
#include "../resources/resource.h"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Propsys.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Dwmapi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr WCHAR APP_CLASS[]    = L"OutputSwitchClass";
constexpr WCHAR OSD_CLASS[]    = L"OutputSwitchOsdClass";
constexpr WCHAR APP_TITLE[]    = L"OutputSwitch";
constexpr WCHAR NOTIFICATION_TITLE[] = L"Sound Device Switcher";
constexpr WCHAR MUTEX_NAME[]   = L"Local\\OutputSwitch_8D44B8CB";
constexpr WCHAR INI_FILE[]     = L"OutputSwitch.ini";
constexpr WCHAR CFG_SECTION[]  = L"Hotkey";
constexpr WCHAR CFG_MODS[]     = L"Modifiers";
constexpr WCHAR CFG_KEY[]      = L"Key";
constexpr WCHAR CFG_UI[]       = L"UI";
constexpr WCHAR CFG_NOTIF[]    = L"Notifications";

constexpr WCHAR STARTUP_REG_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr WCHAR STARTUP_REG_VAL[] = L"OutputSwitch";

constexpr WCHAR DEFAULT_MODS[] = L"Ctrl+Alt";
constexpr WCHAR DEFAULT_KEY[]  = L"S";
constexpr int   DEFAULT_NOTIF  = 1;
constexpr UINT_PTR NOTIFICATION_TIMER_ID = 1;
constexpr UINT NOTIFICATION_DEBOUNCE_MS = 500;
constexpr UINT_PTR OSD_HIDE_TIMER_ID = 1;
constexpr UINT_PTR OSD_FADE_TIMER_ID = 2;
constexpr UINT OSD_VISIBLE_MS = 1600;
constexpr UINT OSD_FADE_INTERVAL_MS = 30;
constexpr int OSD_WIDTH = 340;
constexpr int OSD_HEIGHT = 142;
constexpr int OSD_ALPHA = 235;

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct AppState {
    HWND  hwnd        = nullptr;
    HWND  hOsd        = nullptr;
    HMENU hTrayMenu   = nullptr;
    UINT  uTmsgTaskbar = 0;
    UINT  modifiers    = 0;
    UINT  vk           = 0;
    BOOL  notifications = TRUE;
    BOOL  hotkeyReg    = FALSE;
    BOOL  startupOn    = FALSE;
    WCHAR exePath[MAX_PATH]  = {};
    WCHAR iniPath[MAX_PATH]  = {};
    WCHAR deviceName[256]    = {};
    WCHAR osdText[256]       = {};
    WCHAR shortcutText[128]  = {};
    int   osdAlpha           = OSD_ALPHA;
};

AppState g_state;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void GetExeDir(WCHAR* buf, DWORD size)
{
    GetModuleFileNameW(nullptr, buf, size);
    WCHAR* last = wcsrchr(buf, L'\\');
    if (last) *last = L'\0';
}

void GetExeFullName(WCHAR* buf, DWORD size)
{
    GetModuleFileNameW(nullptr, buf, size);
}

// ---------------------------------------------------------------------------
// Convert a virtual key code to a human-readable name
// ---------------------------------------------------------------------------
bool VkToString(UINT vk, std::wstring& out)
{
    if (vk >= L'A' && vk <= L'Z') { out = (WCHAR)vk; return true; }
    if (vk >= L'0' && vk <= L'9') { out = (WCHAR)vk; return true; }
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t buf[8];
        swprintf_s(buf, L"F%u", vk - VK_F1 + 1);
        out = buf;
        return true;
    }
    switch (vk) {
    case VK_SPACE:   out = L"Space";    return true;
    case VK_TAB:     out = L"Tab";      return true;
    case VK_HOME:    out = L"Home";     return true;
    case VK_END:     out = L"End";      return true;
    case VK_INSERT:  out = L"Insert";   return true;
    case VK_DELETE:  out = L"Delete";   return true;
    case VK_PRIOR:   out = L"PageUp";   return true;
    case VK_NEXT:    out = L"PageDown"; return true;
    case VK_LEFT:    out = L"Left";     return true;
    case VK_RIGHT:   out = L"Right";    return true;
    case VK_UP:      out = L"Up";       return true;
    case VK_DOWN:    out = L"Down";     return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Build a display string like "Shortcut: Ctrl + Alt + S"
// ---------------------------------------------------------------------------
void BuildShortcutString(UINT mods, UINT vk, WCHAR* out, size_t outSize)
{
    std::wstring s = L"Shortcut: ";
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT)     s += L"Alt + ";
    if (mods & MOD_SHIFT)   s += L"Shift + ";
    if (mods & MOD_WIN)     s += L"Win + ";

    std::wstring keyName;
    if (!VkToString(vk, keyName)) keyName = L"?";
    s += keyName;

    wcsncpy_s(out, outSize, s.c_str(), _TRUNCATE);
}

// ---------------------------------------------------------------------------
// Read the "Start with Windows" status from HKCU\...\Run
// ---------------------------------------------------------------------------
bool IsStartupEnabled()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(hKey, STARTUP_REG_VAL, nullptr,
                                  &type, nullptr, nullptr);
    RegCloseKey(hKey);
    return st == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Enable / disable the "Start with Windows" entry in HKCU\...\Run
// (No administrator privileges required.)
// ---------------------------------------------------------------------------
void SetStartupEnabled(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0,
                      KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return;
    }

    if (enable) {
        RegSetValueExW(hKey, STARTUP_REG_VAL, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(g_state.exePath),
                       static_cast<DWORD>((wcslen(g_state.exePath) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, STARTUP_REG_VAL);
    }
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// Create OutputSwitch.ini with default values if it does not exist
// ---------------------------------------------------------------------------
void EnsureDefaultIniExists(LPCWSTR path)
{
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;

    const WCHAR content[] =
        L"[Hotkey]\r\n"
        L"Modifiers=Ctrl+Alt\r\n"
        L"Key=S\r\n"
        L"\r\n"
        L"[UI]\r\n"
        L"Notifications=1\r\n";

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD written = 0;
    WriteFile(h, L"\xFEFF", sizeof(WCHAR), &written, nullptr);
    WriteFile(h, content, sizeof(content) - sizeof(WCHAR), &written, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Parse modifier string like "Ctrl+Alt" -> MOD_CONTROL | MOD_ALT
// ---------------------------------------------------------------------------
bool ParseModifiers(LPCWSTR str, UINT* out)
{
    if (!str || !*str) return false;
    UINT mods = 0;
    std::wstring s(str);
    size_t pos = 0;
    while (pos < s.length()) {
        size_t end = s.find(L'+', pos);
        std::wstring token = (end == std::wstring::npos)
            ? s.substr(pos) : s.substr(pos, end - pos);
        if (token == L"Ctrl")  mods |= MOD_CONTROL;
        else if (token == L"Alt")   mods |= MOD_ALT;
        else if (token == L"Shift") mods |= MOD_SHIFT;
        else if (token == L"Win")   mods |= MOD_WIN;
        else return false;
        if (end == std::wstring::npos) break;
        pos = end + 1;
    }
    if (mods == 0) return false;
    *out = mods;
    return true;
}

// ---------------------------------------------------------------------------
// Parse key string -> virtual key
// ---------------------------------------------------------------------------
bool ParseKey(LPCWSTR str, UINT* out)
{
    if (!str || !*str) return false;
    std::wstring s(str);
    size_t len = s.length();

    if (len == 1) {
        WCHAR c = s[0];
        if (c >= L'A' && c <= L'Z') { *out = (UINT)c; return true; }
        if (c >= L'0' && c <= L'9') { *out = (UINT)c; return true; }
    }
    if (s[0] == L'F' && len >= 2 && len <= 3) {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) { *out = VK_F1 + (UINT)(n - 1); return true; }
        return false;
    }
    if (s == L"Space")     { *out = VK_SPACE;   return true; }
    if (s == L"Tab")       { *out = VK_TAB;     return true; }
    if (s == L"Home")      { *out = VK_HOME;    return true; }
    if (s == L"End")       { *out = VK_END;     return true; }
    if (s == L"Insert")    { *out = VK_INSERT;  return true; }
    if (s == L"Delete")    { *out = VK_DELETE;  return true; }
    if (s == L"PageUp")    { *out = VK_PRIOR;   return true; }
    if (s == L"PageDown")  { *out = VK_NEXT;    return true; }
    if (s == L"Left")      { *out = VK_LEFT;    return true; }
    if (s == L"Right")     { *out = VK_RIGHT;   return true; }
    if (s == L"Up")        { *out = VK_UP;      return true; }
    if (s == L"Down")      { *out = VK_DOWN;    return true; }

    return false;
}

// ---------------------------------------------------------------------------
// Read config from INI (does not apply hotkey)
// ---------------------------------------------------------------------------
bool ReadConfig(LPCWSTR iniPath, UINT* mods, UINT* key, int* notif)
{
    WCHAR bufMods[128] = {};
    WCHAR bufKey[128]  = {};
    WCHAR bufNotif[32] = {};

    GetPrivateProfileStringW(CFG_SECTION, CFG_MODS, DEFAULT_MODS,
                             bufMods, _countof(bufMods), iniPath);
    GetPrivateProfileStringW(CFG_SECTION, CFG_KEY, DEFAULT_KEY,
                             bufKey, _countof(bufKey), iniPath);
    GetPrivateProfileStringW(CFG_UI, CFG_NOTIF, L"1",
                             bufNotif, _countof(bufNotif), iniPath);

    if (!ParseModifiers(bufMods, mods)) return false;
    if (!ParseKey(bufKey, key)) return false;
    *notif = _wtoi(bufNotif) ? 1 : 0;
    return true;
}

// ---------------------------------------------------------------------------
// Show a message box (fatal errors) or tray notification
// ---------------------------------------------------------------------------
void ShowError(LPCWSTR msg)
{
    MessageBoxW(nullptr, msg, APP_TITLE, MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// Show tray notification (NIF_INFO balloon)
// ---------------------------------------------------------------------------
void ShowNotification(LPCWSTR title, LPCWSTR msg, DWORD infoFlags)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd  = g_state.hwnd;
    nid.uID   = 1;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, msg, _TRUNCATE);
    nid.dwInfoFlags = infoFlags;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

LRESULT CALLBACK OsdWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH background = CreateSolidBrush(RGB(18, 18, 20));
        FillRect(hdc, &rc, background);
        DeleteObject(background);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(250, 250, 250));
        HFONT iconFont = CreateFontW(-46, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
        HGDIOBJ oldFont = SelectObject(hdc, iconFont);
        RECT iconRect = { 0, 18, rc.right, 75 };
        DrawTextW(hdc, L"\xE767", 1, &iconRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        DeleteObject(iconFont);

        HFONT font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        oldFont = SelectObject(hdc, font);
        RECT textRect = { 24, 88, rc.right - 24, 119 };
        DrawTextW(hdc, g_state.osdText, -1, &textRect,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        DeleteObject(font);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == OSD_HIDE_TIMER_ID) {
            KillTimer(hwnd, OSD_HIDE_TIMER_ID);
            SetTimer(hwnd, OSD_FADE_TIMER_ID, OSD_FADE_INTERVAL_MS, nullptr);
            return 0;
        }
        if (wParam == OSD_FADE_TIMER_ID) {
            g_state.osdAlpha -= 24;
            if (g_state.osdAlpha <= 0) {
                KillTimer(hwnd, OSD_FADE_TIMER_ID);
                ShowWindow(hwnd, SW_HIDE);
            } else {
                SetLayeredWindowAttributes(hwnd, 0,
                    static_cast<BYTE>(g_state.osdAlpha), LWA_ALPHA);
            }
            return 0;
        }
        break;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowDeviceOsd(LPCWSTR deviceName)
{
    if (!g_state.hOsd) {
        g_state.hOsd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            OSD_CLASS, nullptr, WS_POPUP, 0, 0, OSD_WIDTH, OSD_HEIGHT,
            g_state.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!g_state.hOsd) return;

        DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_state.hOsd, DWMWA_WINDOW_CORNER_PREFERENCE,
                              &corners, sizeof(corners));
        COLORREF borderColor = RGB(55, 55, 60);
        DwmSetWindowAttribute(g_state.hOsd, DWMWA_BORDER_COLOR,
                              &borderColor, sizeof(borderColor));
    }

    wcsncpy_s(g_state.osdText, deviceName, _TRUNCATE);
    g_state.osdAlpha = OSD_ALPHA;
    KillTimer(g_state.hOsd, OSD_HIDE_TIMER_ID);
    KillTimer(g_state.hOsd, OSD_FADE_TIMER_ID);
    SetLayeredWindowAttributes(g_state.hOsd, 0, OSD_ALPHA, LWA_ALPHA);

    HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground ? foreground : g_state.hwnd,
                                         MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitorInfo = { sizeof(monitorInfo) };
    GetMonitorInfoW(monitor, &monitorInfo);
    const RECT& work = monitorInfo.rcWork;
    int x = work.left + ((work.right - work.left) - OSD_WIDTH) / 2;
    int y = work.bottom - OSD_HEIGHT - 72;

    InvalidateRect(g_state.hOsd, nullptr, TRUE);
    SetWindowPos(g_state.hOsd, HWND_TOPMOST, x, y, OSD_WIDTH, OSD_HEIGHT,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_state.hOsd, OSD_HIDE_TIMER_ID, OSD_VISIBLE_MS, nullptr);
}

void QueueDeviceNotification()
{
    if (g_state.notifications) {
        SetTimer(g_state.hwnd, NOTIFICATION_TIMER_ID, NOTIFICATION_DEBOUNCE_MS, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Enum active render endpoints -> vector of (id, friendlyName)
// ---------------------------------------------------------------------------
struct EndpointInfo {
    std::wstring id;
    std::wstring name;
};

bool EnumActiveEndpoints(std::vector<EndpointInfo>& endpoints)
{
    endpoints.clear();
    ComPtr<IMMDeviceEnumerator> pEnum;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return false;

    ComPtr<IMMDeviceCollection> pCollection;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) return false;

    UINT count = 0;
    pCollection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> pDevice;
        if (FAILED(pCollection->Item(i, &pDevice))) continue;

        LPWSTR pwszId = nullptr;
        if (FAILED(pDevice->GetId(&pwszId))) continue;
        std::wstring devId(pwszId);
        CoTaskMemFree(pwszId);

        ComPtr<IPropertyStore> pStore;
        std::wstring name;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &var))) {
                if (var.vt == VT_LPWSTR)
                    name = var.pwszVal;
                PropVariantClear(&var);
            }
        }
        if (name.empty()) name = L"Unknown output device";
        endpoints.push_back({ std::move(devId), std::move(name) });
    }
    return true;
}

// ---------------------------------------------------------------------------
// Get the default multimedia endpoint ID
// ---------------------------------------------------------------------------
bool GetDefaultEndpointId(std::wstring& outId)
{
    outId.clear();
    ComPtr<IMMDeviceEnumerator> pEnum;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (FAILED(hr)) return false;

    ComPtr<IMMDevice> pDevice;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr)) return false;

    LPWSTR pwszId = nullptr;
    hr = pDevice->GetId(&pwszId);
    if (FAILED(hr)) return false;
    outId = pwszId;
    CoTaskMemFree(pwszId);
    return true;
}

// ---------------------------------------------------------------------------
// Set default endpoint for all three roles
// ---------------------------------------------------------------------------
bool SetDefaultEndpointAll(LPCWSTR deviceId)
{
    ComPtr<IPolicyConfig> pPolicy;
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&pPolicy));
    if (FAILED(hr)) {
        WCHAR msg[128];
        swprintf_s(msg, L"CoCreateInstance(IPolicyConfig) failed: 0x%08X", hr);
        MessageBoxW(nullptr, msg, L"Debug", MB_OK);
        return false;
    }

    bool multimediaOk = false;
    hr = pPolicy->SetDefaultEndpoint(deviceId, eMultimedia);
    if (SUCCEEDED(hr)) multimediaOk = true;
    else {
        WCHAR msg[256];
        swprintf_s(msg, L"SetDefaultEndpoint(eMultimedia) failed: 0x%08X\nDeviceId: %s", hr, deviceId);
        MessageBoxW(nullptr, msg, L"Debug", MB_OK);
    }

    pPolicy->SetDefaultEndpoint(deviceId, eConsole);
    pPolicy->SetDefaultEndpoint(deviceId, eCommunications);

    return multimediaOk;
}

// ---------------------------------------------------------------------------
// Rotate to next endpoint
// Returns the new device name on success, empty string on failure
// ---------------------------------------------------------------------------
std::wstring RotateToNextDevice()
{
    std::vector<EndpointInfo> endpoints;
    if (!EnumActiveEndpoints(endpoints) || endpoints.empty()) {
        ShowNotification(NOTIFICATION_TITLE, L"No active output devices", NIIF_WARNING);
        return {};
    }

    std::wstring defaultId;
    if (!GetDefaultEndpointId(defaultId)) {
        defaultId.clear();
    }

    size_t idx = 0;
    if (!defaultId.empty()) {
        bool found = false;
        for (size_t i = 0; i < endpoints.size(); ++i) {
            if (endpoints[i].id == defaultId) { idx = i; found = true; break; }
        }
        if (!found) idx = 0; // current default not in list: pick first
        else         idx = (idx + 1) % endpoints.size();
    } else {
        idx = 0; // no current default: pick first
    }

    if (!SetDefaultEndpointAll(endpoints[idx].id.c_str())) {
        ShowNotification(NOTIFICATION_TITLE, L"Could not change the audio output device", NIIF_WARNING);
        return {};
    }

    return endpoints[idx].name;
}

// ---------------------------------------------------------------------------
// Build the tray context menu
// ---------------------------------------------------------------------------
HMENU BuildTrayMenu(LPCWSTR deviceName)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return nullptr;

    MENUITEMINFOW mii = { sizeof(mii) };
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_STATE;
    mii.fType = MFT_STRING;
    mii.fState = MFS_DEFAULT;

    // Device name (grayed / disabled)
    mii.wID = IDM_DEVICE;
    mii.fState |= MFS_DISABLED;
    mii.dwTypeData = const_cast<LPWSTR>(deviceName);
    InsertMenuItemW(hMenu, 0, TRUE, &mii);

    // Separator
    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(hMenu, 1, TRUE, &mii);

    // Shortcut info (grayed / disabled, non-selectable)
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_STATE;
    mii.fType = MFT_STRING;
    mii.fState = MFS_DISABLED;
    mii.wID = IDM_SHORTCUT;
    mii.dwTypeData = const_cast<LPWSTR>(g_state.shortcutText);
    InsertMenuItemW(hMenu, 2, TRUE, &mii);

    // Start with Windows (checkable)
    mii.fState = MFS_ENABLED | (g_state.startupOn ? MFS_CHECKED : MFS_UNCHECKED);
    mii.wID = IDM_STARTUP;
    mii.dwTypeData = const_cast<LPWSTR>(L"Start with Windows");
    InsertMenuItemW(hMenu, 3, TRUE, &mii);

    // Separator
    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(hMenu, 4, TRUE, &mii);

    // Open configuration
    mii.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STRING;
    mii.fType = MFT_STRING;
    mii.fState = 0;
    mii.wID = IDM_OPEN_CONFIG;
    mii.dwTypeData = const_cast<LPWSTR>(L"Open configuration");
    InsertMenuItemW(hMenu, 5, TRUE, &mii);

    // Reload configuration
    mii.wID = IDM_RELOAD;
    mii.dwTypeData = const_cast<LPWSTR>(L"Reload configuration");
    InsertMenuItemW(hMenu, 6, TRUE, &mii);

    // Exit
    mii.wID = IDM_EXIT;
    mii.dwTypeData = const_cast<LPWSTR>(L"Exit");
    InsertMenuItemW(hMenu, 7, TRUE, &mii);

    return hMenu;
}

// ---------------------------------------------------------------------------
// Update tray tooltip
// ---------------------------------------------------------------------------
void UpdateTrayTooltip(LPCWSTR text)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd  = g_state.hwnd;
    nid.uID   = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---------------------------------------------------------------------------
// Add the tray icon (or re-add after Explorer restart)
// ---------------------------------------------------------------------------
void AddTrayIcon()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd         = g_state.hwnd;
    nid.uID          = 1;
    nid.uFlags       = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = UWM_TRAYICON;
    nid.hIcon        = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON));
    wcsncpy_s(nid.szTip, L"OutputSwitch", _TRUNCATE);

    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

// ---------------------------------------------------------------------------
// Remove tray icon
// ---------------------------------------------------------------------------
void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_state.hwnd;
    nid.uID  = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// ---------------------------------------------------------------------------
// Show the tray context menu
// ---------------------------------------------------------------------------
void ShowTrayMenu()
{
    if (g_state.hTrayMenu) {
        DestroyMenu(g_state.hTrayMenu);
        g_state.hTrayMenu = nullptr;
    }
    g_state.hTrayMenu = BuildTrayMenu(g_state.deviceName[0]
                                        ? g_state.deviceName : L"(no device)");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_state.hwnd);
    TrackPopupMenu(g_state.hTrayMenu,
                   TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, g_state.hwnd, nullptr);
    PostMessageW(g_state.hwnd, WM_NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// Reload hotkey and settings from INI
// ---------------------------------------------------------------------------
void ReloadConfig()
{
    UINT newMods = 0, newVk = 0;
    int  newNotif = 0;

    if (!ReadConfig(g_state.iniPath, &newMods, &newVk, &newNotif)) {
        ShowError(L"OutputSwitch.ini contains invalid values.\n"
                  L"Keeping the previous configuration.");
        return;
    }

    g_state.notifications = (newNotif != 0);
    if (!g_state.notifications) {
        KillTimer(g_state.hwnd, NOTIFICATION_TIMER_ID);
    }

    bool hotkeyChanged = (newMods != g_state.modifiers) ||
                         (newVk    != g_state.vk);

    if (!hotkeyChanged) return;

    if (g_state.hotkeyReg) {
        UnregisterHotKey(g_state.hwnd, HOTKEY_ID);
        g_state.hotkeyReg = FALSE;
    }

    if (!RegisterHotKey(g_state.hwnd, HOTKEY_ID,
                        newMods | MOD_NOREPEAT, newVk)) {
        // Try to restore old hotkey
        WCHAR msg[256];
        swprintf_s(msg, L"Could not register the new hotkey.\n"
                   L"Keeping the current hotkey.");
        ShowError(msg);
        if (g_state.modifiers && g_state.vk) {
            RegisterHotKey(g_state.hwnd, HOTKEY_ID,
                           g_state.modifiers | MOD_NOREPEAT, g_state.vk);
            g_state.hotkeyReg = TRUE;
        }
        return;
    }

    g_state.modifiers  = newMods;
    g_state.vk         = newVk;
    g_state.hotkeyReg  = TRUE;

    BuildShortcutString(g_state.modifiers, g_state.vk,
                        g_state.shortcutText, _countof(g_state.shortcutText));
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        g_state.hwnd = hwnd;
        AddTrayIcon();
        return 0;
    }

    case WM_HOTKEY: {
        if (wParam == HOTKEY_ID) {
            std::wstring newName = RotateToNextDevice();
            if (!newName.empty()) {
                wcsncpy_s(g_state.deviceName, newName.c_str(), _TRUNCATE);
                UpdateTrayTooltip(newName.c_str());
                QueueDeviceNotification();
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == NOTIFICATION_TIMER_ID) {
            KillTimer(hwnd, NOTIFICATION_TIMER_ID);
            if (g_state.notifications && g_state.deviceName[0]) {
                ShowDeviceOsd(g_state.deviceName);
            }
            return 0;
        }
        break;

    case WM_COMMAND: {
        WORD id = LOWORD(wParam);
        switch (id) {
        case IDM_OPEN_CONFIG:
            EnsureDefaultIniExists(g_state.iniPath);
            ShellExecuteW(hwnd, L"open", g_state.iniPath, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDM_STARTUP: {
            bool enable = !g_state.startupOn;
            SetStartupEnabled(enable);
            g_state.startupOn = (IsStartupEnabled() != FALSE);
            return 0;
        }
        case IDM_RELOAD:
            ReloadConfig();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, NOTIFICATION_TIMER_ID);
        if (g_state.hOsd) {
            DestroyWindow(g_state.hOsd);
            g_state.hOsd = nullptr;
        }
        RemoveTrayIcon();
        if (g_state.hTrayMenu) {
            DestroyMenu(g_state.hTrayMenu);
            g_state.hTrayMenu = nullptr;
        }
        if (g_state.hotkeyReg) {
            UnregisterHotKey(hwnd, HOTKEY_ID);
            g_state.hotkeyReg = FALSE;
        }
        PostQuitMessage(0);
        return 0;

    default: {
        if (msg == g_state.uTmsgTaskbar) {
            AddTrayIcon();
            return 0;
        }
        if (msg == UWM_TRAYICON) {
            switch (LOWORD(lParam)) {
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                ShowTrayMenu();
                return 0;
            case WM_LBUTTONDBLCLK: {
                std::wstring newName = RotateToNextDevice();
                if (!newName.empty()) {
                    wcsncpy_s(g_state.deviceName, newName.c_str(), _TRUNCATE);
                    UpdateTrayTooltip(newName.c_str());
                    QueueDeviceNotification();
                }
                return 0;
            }
            }
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Register window class
// ---------------------------------------------------------------------------
bool RegisterAppClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW osdClass = { sizeof(osdClass) };
    osdClass.lpfnWndProc   = OsdWndProc;
    osdClass.hInstance     = hInst;
    osdClass.lpszClassName = OSD_CLASS;
    osdClass.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    return RegisterClassExW(&osdClass) != 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // 1. Instance check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"OutputSwitch is already running.",
                    APP_TITLE, MB_OK | MB_ICONINFORMATION);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // 2. Init COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        ShowError(L"Could not initialize COM.");
        CloseHandle(hMutex);
        return 1;
    }

    // 3. Get INI path and exe path
    GetExeFullName(g_state.exePath, MAX_PATH);
    GetExeDir(g_state.iniPath, MAX_PATH);
    wcsncat_s(g_state.iniPath, L"\\", _TRUNCATE);
    wcsncat_s(g_state.iniPath, INI_FILE, _TRUNCATE);

    // 4. Read config (OutputSwitch.ini is optional: when the file is
    //    missing, Windows returns the default values silently.)
    int notif = DEFAULT_NOTIF;
    if (!ReadConfig(g_state.iniPath, &g_state.modifiers, &g_state.vk, &notif)) {
        ShowError(L"OutputSwitch.ini has an invalid format.\n"
                  L"Default values will be used.");
        g_state.modifiers = MOD_CONTROL | MOD_ALT;
        g_state.vk        = L'S';
        notif             = 1;
    }
    g_state.notifications = (notif != 0);
    BuildShortcutString(g_state.modifiers, g_state.vk,
                        g_state.shortcutText, _countof(g_state.shortcutText));

    // Auto-start status from HKCU\...\Run (no admin rights required)
    g_state.startupOn = IsStartupEnabled();

    // 5. Register window class
    if (!RegisterAppClass(hInstance)) {
        ShowError(L"Could not register the window class.");
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    // 6. Register TaskbarCreated message
    g_state.uTmsgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    // 7. Create hidden window
    HWND hwnd = CreateWindowExW(0, APP_CLASS, APP_TITLE,
                                0, 0, 0, 0, 0, nullptr, nullptr,
                                hInstance, nullptr);
    if (!hwnd) {
        ShowError(L"Could not create the window.");
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }

    // 8. Register hotkey
    if (!RegisterHotKey(hwnd, HOTKEY_ID,
                        g_state.modifiers | MOD_NOREPEAT, g_state.vk)) {
        WCHAR msg[256];
        swprintf_s(msg, L"Could not register the hotkey.\n"
                   L"The shortcut may be in use by another application.");
        ShowError(msg);
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }
    g_state.hotkeyReg = TRUE;

    // 9. Get initial device name for tray
    {
        std::wstring defId;
        if (GetDefaultEndpointId(defId)) {
            std::vector<EndpointInfo> endpoints;
            if (EnumActiveEndpoints(endpoints)) {
                for (auto& ep : endpoints) {
                    if (ep.id == defId) {
                        wcsncpy_s(g_state.deviceName, ep.name.c_str(), _TRUNCATE);
                        break;
                    }
                }
            }
        }
    }

    // 10. Message loop
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 11. Cleanup
    CoUninitialize();
    CloseHandle(hMutex);
    return (int)msg.wParam;
}
