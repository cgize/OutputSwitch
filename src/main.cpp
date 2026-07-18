#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <climits>
#include <cwchar>

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
    HWND  hwnd;
    HWND  hOsd;
    HMENU hTrayMenu;
    UINT  uTmsgTaskbar;
    UINT  modifiers;
    UINT  vk;
    BOOL  notifications;
    BOOL  hotkeyReg;
    BOOL  startupOn;
    WCHAR exePath[MAX_PATH];
    WCHAR iniPath[MAX_PATH];
    WCHAR deviceName[256];
    WCHAR shortcutText[128];
    int   osdAlpha;
};

AppState g_state = {};

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
bool TokenEquals(LPCWSTR text, size_t length, LPCWSTR expected)
{
    for (size_t i = 0; i < length; ++i) {
        if (!expected[i] || text[i] != expected[i]) return false;
    }
    return expected[length] == L'\0';
}

int ParseInteger(LPCWSTR text)
{
    while (*text == L' ' || (*text >= L'\t' && *text <= L'\r')) ++text;
    int sign = 1;
    if (*text == L'-') { sign = -1; ++text; }
    else if (*text == L'+') { ++text; }

    unsigned long value = 0;
    unsigned long limit = sign > 0 ? INT_MAX : static_cast<unsigned long>(INT_MAX) + 1;
    while (*text >= L'0' && *text <= L'9') {
        unsigned long digit = static_cast<unsigned long>(*text++ - L'0');
        if (value > (limit - digit) / 10) return sign > 0 ? INT_MAX : INT_MIN;
        value = value * 10 + digit;
    }
    if (sign > 0) return static_cast<int>(value);
    if (value == static_cast<unsigned long>(INT_MAX) + 1) return INT_MIN;
    return -static_cast<int>(value);
}

void FormatHex(HRESULT value, WCHAR (&out)[9])
{
    constexpr WCHAR digits[] = L"0123456789ABCDEF";
    unsigned long number = static_cast<unsigned long>(value);
    for (int i = 7; i >= 0; --i) {
        out[i] = digits[number & 0xF];
        number >>= 4;
    }
    out[8] = L'\0';
}

bool VkToString(UINT vk, WCHAR* out, size_t outSize)
{
    if (vk >= L'A' && vk <= L'Z') {
        out[0] = static_cast<WCHAR>(vk);
        out[1] = L'\0';
        return true;
    }
    if (vk >= L'0' && vk <= L'9') {
        out[0] = static_cast<WCHAR>(vk);
        out[1] = L'\0';
        return true;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        UINT number = vk - VK_F1 + 1;
        out[0] = L'F';
        if (number >= 10) {
            out[1] = static_cast<WCHAR>(L'0' + number / 10);
            out[2] = static_cast<WCHAR>(L'0' + number % 10);
            out[3] = L'\0';
        } else {
            out[1] = static_cast<WCHAR>(L'0' + number);
            out[2] = L'\0';
        }
        return true;
    }
    LPCWSTR name = nullptr;
    switch (vk) {
    case VK_SPACE:   name = L"Space";    break;
    case VK_TAB:     name = L"Tab";      break;
    case VK_HOME:    name = L"Home";     break;
    case VK_END:     name = L"End";      break;
    case VK_INSERT:  name = L"Insert";   break;
    case VK_DELETE:  name = L"Delete";   break;
    case VK_PRIOR:   name = L"PageUp";   break;
    case VK_NEXT:    name = L"PageDown"; break;
    case VK_LEFT:    name = L"Left";     break;
    case VK_RIGHT:   name = L"Right";    break;
    case VK_UP:      name = L"Up";       break;
    case VK_DOWN:    name = L"Down";     break;
    }
    if (!name) return false;
    wcsncpy_s(out, outSize, name, _TRUNCATE);
    return true;
}

// ---------------------------------------------------------------------------
// Build a display string like "Shortcut: Ctrl + Alt + S"
// ---------------------------------------------------------------------------
void BuildShortcutString(UINT mods, UINT vk, WCHAR* out, size_t outSize)
{
    wcscpy_s(out, outSize, L"Shortcut: ");
    if (mods & MOD_CONTROL) wcscat_s(out, outSize, L"Ctrl + ");
    if (mods & MOD_ALT)     wcscat_s(out, outSize, L"Alt + ");
    if (mods & MOD_SHIFT)   wcscat_s(out, outSize, L"Shift + ");
    if (mods & MOD_WIN)     wcscat_s(out, outSize, L"Win + ");

    WCHAR keyName[16];
    if (!VkToString(vk, keyName, _countof(keyName))) wcscpy_s(keyName, L"?");
    wcscat_s(out, outSize, keyName);
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
    LSTATUS st = RegQueryValueExW(hKey, APP_TITLE, nullptr,
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
        RegSetValueExW(hKey, APP_TITLE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(g_state.exePath),
                       static_cast<DWORD>((wcslen(g_state.exePath) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, APP_TITLE);
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
    LPCWSTR token = str;
    while (*token) {
        LPCWSTR end = token;
        while (*end && *end != L'+') ++end;
        size_t length = static_cast<size_t>(end - token);
        if (TokenEquals(token, length, L"Ctrl"))       mods |= MOD_CONTROL;
        else if (TokenEquals(token, length, L"Alt"))   mods |= MOD_ALT;
        else if (TokenEquals(token, length, L"Shift")) mods |= MOD_SHIFT;
        else if (TokenEquals(token, length, L"Win"))   mods |= MOD_WIN;
        else return false;
        if (!*end) break;
        token = end + 1;
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
    size_t len = wcslen(str);

    if (len == 1) {
        WCHAR c = str[0];
        if (c >= L'A' && c <= L'Z') { *out = (UINT)c; return true; }
        if (c >= L'0' && c <= L'9') { *out = (UINT)c; return true; }
    }
    if (str[0] == L'F' && len >= 2 && len <= 3) {
        int n = ParseInteger(str + 1);
        if (n >= 1 && n <= 24) { *out = VK_F1 + (UINT)(n - 1); return true; }
        return false;
    }
    if (TokenEquals(str, len, L"Space"))    { *out = VK_SPACE;   return true; }
    if (TokenEquals(str, len, L"Tab"))      { *out = VK_TAB;     return true; }
    if (TokenEquals(str, len, L"Home"))     { *out = VK_HOME;    return true; }
    if (TokenEquals(str, len, L"End"))      { *out = VK_END;     return true; }
    if (TokenEquals(str, len, L"Insert"))   { *out = VK_INSERT;  return true; }
    if (TokenEquals(str, len, L"Delete"))   { *out = VK_DELETE;  return true; }
    if (TokenEquals(str, len, L"PageUp"))   { *out = VK_PRIOR;   return true; }
    if (TokenEquals(str, len, L"PageDown")) { *out = VK_NEXT;    return true; }
    if (TokenEquals(str, len, L"Left"))     { *out = VK_LEFT;    return true; }
    if (TokenEquals(str, len, L"Right"))    { *out = VK_RIGHT;   return true; }
    if (TokenEquals(str, len, L"Up"))       { *out = VK_UP;      return true; }
    if (TokenEquals(str, len, L"Down"))     { *out = VK_DOWN;    return true; }

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
    *notif = ParseInteger(bufNotif) ? 1 : 0;
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
        DrawTextW(hdc, g_state.deviceName, -1, &textRect,
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

void ShowDeviceOsd()
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

bool GetFriendlyName(IMMDevice* device, WCHAR* out, size_t outSize)
{
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value;
        PropVariantInit(&value);
        HRESULT hr = store->GetValue(PKEY_Device_FriendlyName, &value);
        if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal && value.pwszVal[0]) {
            wcsncpy_s(out, outSize, value.pwszVal, _TRUNCATE);
            PropVariantClear(&value);
            return true;
        }
        PropVariantClear(&value);
    }
    wcsncpy_s(out, outSize, L"Unknown output device", _TRUNCATE);
    return false;
}

bool GetDefaultEndpoint(IMMDeviceEnumerator* enumerator,
                        ComPtr<IMMDevice>& device, LPWSTR* id)
{
    *id = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) return false;
    return SUCCEEDED(device->GetId(id));
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
        WCHAR hex[9];
        FormatHex(hr, hex);
        WCHAR msg[128];
        wcscpy_s(msg, L"CoCreateInstance(IPolicyConfig) failed: 0x");
        wcscat_s(msg, hex);
        MessageBoxW(nullptr, msg, L"Debug", MB_OK);
        return false;
    }

    bool multimediaOk = false;
    hr = pPolicy->SetDefaultEndpoint(deviceId, eMultimedia);
    if (SUCCEEDED(hr)) multimediaOk = true;
    else {
        WCHAR hex[9];
        FormatHex(hr, hex);
        WCHAR msg[256];
        wcscpy_s(msg, L"SetDefaultEndpoint(eMultimedia) failed: 0x");
        wcscat_s(msg, hex);
        wcscat_s(msg, L"\nDeviceId: ");
        wcsncat_s(msg, deviceId, _TRUNCATE);
        MessageBoxW(nullptr, msg, L"Debug", MB_OK);
    }

    pPolicy->SetDefaultEndpoint(deviceId, eConsole);
    pPolicy->SetDefaultEndpoint(deviceId, eCommunications);

    return multimediaOk;
}

// ---------------------------------------------------------------------------
// Rotate to the endpoint after the current default, wrapping to the first.
// ---------------------------------------------------------------------------
bool RotateToNextDevice(WCHAR* newName, size_t newNameSize)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        ShowNotification(NOTIFICATION_TITLE, L"No active output devices", NIIF_WARNING);
        return false;
    }

    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        ShowNotification(NOTIFICATION_TITLE, L"No active output devices", NIIF_WARNING);
        return false;
    }

    ComPtr<IMMDevice> defaultDevice;
    LPWSTR defaultId = nullptr;
    GetDefaultEndpoint(enumerator.Get(), defaultDevice, &defaultId);

    UINT count = 0;
    collection->GetCount(&count);
    ComPtr<IMMDevice> firstDevice;
    ComPtr<IMMDevice> selectedDevice;
    LPWSTR firstId = nullptr;
    LPWSTR selectedId = nullptr;
    bool selectNext = false;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;

        if (!firstId) {
            firstId = id;
            firstDevice = device;
            id = nullptr;
        }

        LPCWSTR currentId = id ? id : firstId;
        if (selectNext) {
            selectedId = id;
            selectedDevice = device;
            id = nullptr;
            break;
        }

        if (defaultId && wcscmp(currentId, defaultId) == 0) selectNext = true;
        if (id) CoTaskMemFree(id);
    }

    if (!firstId) {
        if (defaultId) CoTaskMemFree(defaultId);
        ShowNotification(NOTIFICATION_TITLE, L"No active output devices", NIIF_WARNING);
        return false;
    }

    if (!selectedId) {
        selectedId = firstId;
        selectedDevice = firstDevice;
        firstId = nullptr;
    }

    GetFriendlyName(selectedDevice.Get(), newName, newNameSize);
    bool changed = SetDefaultEndpointAll(selectedId);
    if (defaultId) CoTaskMemFree(defaultId);
    if (firstId) CoTaskMemFree(firstId);
    CoTaskMemFree(selectedId);

    if (!changed) {
        ShowNotification(NOTIFICATION_TITLE, L"Could not change the audio output device", NIIF_WARNING);
        return false;
    }
    return true;
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

void SwitchToNextDevice()
{
    WCHAR newName[_countof(g_state.deviceName)];
    if (!RotateToNextDevice(newName, _countof(newName))) return;
    wcsncpy_s(g_state.deviceName, newName, _TRUNCATE);
    UpdateTrayTooltip(newName);
    QueueDeviceNotification();
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
        ShowError(L"Could not register the new hotkey.\n"
                  L"Keeping the current hotkey.");
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
            SwitchToNextDevice();
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == NOTIFICATION_TIMER_ID) {
            KillTimer(hwnd, NOTIFICATION_TIMER_ID);
            if (g_state.notifications && g_state.deviceName[0]) {
                ShowDeviceOsd();
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
                SwitchToNextDevice();
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
        ShowError(L"Could not register the hotkey.\n"
                  L"The shortcut may be in use by another application.");
        CoUninitialize();
        CloseHandle(hMutex);
        return 1;
    }
    g_state.hotkeyReg = TRUE;

    // 9. Get initial device name for tray
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                       CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            ComPtr<IMMDevice> device;
            LPWSTR id = nullptr;
            if (GetDefaultEndpoint(enumerator.Get(), device, &id)) {
                GetFriendlyName(device.Get(), g_state.deviceName,
                                _countof(g_state.deviceName));
                CoTaskMemFree(id);
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
