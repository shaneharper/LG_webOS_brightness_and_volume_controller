// This program receives messages sent on a COM port from a device with a brightness knob and a volume knob and it forwards requests to change brightness/volume to "LGTV Companion" (https://github.com/JPersson77/LGTVCompanion).

#include <cstdint>
#include <cwchar>
#include <mutex>
#include <optional>
#include <sal.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>
#include <corecrt.h>
#include <devguid.h>
#include <setupapi.h>
#pragma comment(lib, "setupapi.lib")


static void run_command(const std::string& command_line)
{
    STARTUPINFOA startup_info = { sizeof(STARTUPINFOA) };
    PROCESS_INFORMATION process_info;

    if (not CreateProcessA(
        nullptr,     // Application name (nullptr means use command line)
        std::string(command_line).data(),  // (command_line is copied. Note that CreateProcessA() may insert '\0's -?)
        nullptr,     // Process security attributes
        nullptr,     // Thread security attributes
        FALSE,       // Inherit handles
        0,           // Creation flags
        nullptr,     // Environment
        nullptr,     // Current directory
        &startup_info, &process_info))
    {
        // XXX std::cerr << "CreateProcess() failed with error " << GetLastError() << '.' << std::endl;
        return;
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
}

#define LGTV_COMPANION_PATHNAME "\"C:\\Program Files\\LGTV Companion\\LGTV Companion.exe\""

static void set_brightness(const std::string& s)  // XXX XXX This should be ...(unsigned percent)
{
    run_command(LGTV_COMPANION_PATHNAME " --backlight "  // "--backlight" sets the brightness even on an OLED display which won't have a backlight.
        + s);
}

static void set_volume(const std::string& s)  // XXX XXX This should be ...(unsigned percent)
{
    run_command(LGTV_COMPANION_PATHNAME" --volume " + s);
}

struct AutoHANDLE
{
    explicit AutoHANDLE(HANDLE handle) noexcept : handle(handle) {}
    ~AutoHANDLE() { close(); }


    AutoHANDLE(AutoHANDLE&& o) noexcept : handle(o.handle)
    {
        o.handle = INVALID_HANDLE_VALUE;
    }

    AutoHANDLE(const AutoHANDLE&) = delete;
    AutoHANDLE& operator=(const AutoHANDLE&) = delete;

    HANDLE get() const noexcept { return handle; }

    std::optional<std::string> read(DWORD max_bytes_to_read = 128) const
    {
        std::string result(max_bytes_to_read, '\0');
        DWORD bytes_read;
        if (!ReadFile(handle, result.data(), max_bytes_to_read, &bytes_read, nullptr))
            return std::nullopt;
        result.resize(bytes_read);
        return result;
    }

private:
    HANDLE handle;

    void close() noexcept
    {
        if (handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
    }
};

struct AutoHKEY
{
    explicit AutoHKEY(HKEY key) noexcept : key(key) {}
    ~AutoHKEY() { if (*this) RegCloseKey(key); }

    AutoHKEY(const AutoHKEY&) = delete;
    AutoHKEY& operator=(const AutoHKEY&) = delete;

    explicit operator bool() const noexcept { return key != INVALID_HANDLE_VALUE; }
    HKEY get() const noexcept { return key; }

private:
    HKEY key;
};

static bool hardware_id_could_be_arduino(const std::string& hardware_id)
{
    for (const auto* fragment : {
        "VID_1A86&PID_7523",  // CH340 (cheap Nano clones; also used by many ESP32 dev boards)
        "VID_0403&PID_6001",  // FTDI FT232R (older genuine Nanos; also generic FTDI USB-serial cables)
        "VID_2341",           // Arduino LLC (any PID - covers official boards using ATmega16U2)
        "VID_2A03",           // Arduino SA (any PID - the post-2015 official VID)
    })
        if (hardware_id.find(fragment) != std::string::npos) return true;
    return false;
}

static std::string read_device_hardware_id(HDEVINFO devs, const SP_DEVINFO_DATA& dev)
{
    // SPDRP_HARDWAREID is a REG_MULTI_SZ (a sequence of null-terminated
    // strings); the std::string constructor stops at the first '\0', which is
    // exactly the substring (e.g. "USB\VID_1A86&PID_7523&REV_0254") we want to
    // match against.
    char hardware_id[512] = {};
    return SetupDiGetDeviceRegistryPropertyA(devs, const_cast<PSP_DEVINFO_DATA>(&dev) /* input-only */,
                                             SPDRP_HARDWAREID, nullptr,
                                             reinterpret_cast<PBYTE>(hardware_id),
                                             sizeof(hardware_id) - 1, nullptr)
               ? hardware_id : std::string{};
}

static AutoHKEY open_device_reg_key(HDEVINFO devs, const SP_DEVINFO_DATA& dev)
{
    return AutoHKEY(SetupDiOpenDevRegKey(devs, const_cast<PSP_DEVINFO_DATA>(&dev) /* input-only */,
                                         DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ));
}

static std::string read_device_COM_port_name(HDEVINFO devs, const SP_DEVINFO_DATA& dev)
{
    const AutoHKEY key = open_device_reg_key(devs, dev);
    if (!key) return {};

    char port_name[32] = {};
    DWORD size = sizeof(port_name) - 1;
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExA(key.get(), "PortName", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(port_name), &size);

    return (status == ERROR_SUCCESS && type == REG_SZ) ? port_name : std::string{};
}

static std::vector<std::string> enumerate_possibly_Arduino_COM_ports()
{
    std::vector<std::string> ports;

    const HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return ports;

    SP_DEVINFO_DATA dev = { sizeof(SP_DEVINFO_DATA) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i)
    {
        if (!hardware_id_could_be_arduino(read_device_hardware_id(devs, dev))) continue;
        if (auto port_name = read_device_COM_port_name(devs, dev); !port_name.empty())
            ports.emplace_back(std::move(port_name));
    }

    SetupDiDestroyDeviceInfoList(devs);
    return ports;
}

static void apply_9600_8N1(HANDLE port)
{
    DCB dcb = { 0 };
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port, &dcb)) throw "GetCommState() failed.";
    dcb.BaudRate = CBR_9600;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;
    if (!SetCommState(port, &dcb)) throw "SetCommState() failed.";
}

static void apply_short_timeouts(HANDLE port)
{
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 500;
    timeouts.WriteTotalTimeoutMultiplier = 100;
    if (!SetCommTimeouts(port, &timeouts)) throw "SetCommTimeouts() failed.";
}

// XXX: hang risk. CreateFileW on a COM port, and the GetCommState/SetCommState/ SetCommTimeouts
// IOCTLs called below, accept no caller-side timeout. A broken driver - a flaky CH340 clone driver
// after suspend/resume, a half-removed Bluetooth port - could leave the worker thread blocked here
// indefinitely, and the tooltip would sit at "Trying COMx." forever. The Win32 API has no
// synchronous-timeout knob for these calls; the fix is to run try_open_COM_port (or just the
// CreateFileW) on a watchdog thread, wait on it with WaitForSingleObject + a few-second timeout, and
// detach/abandon it if it overruns. Worth doing only if this is actually observed in practice -
// VID:PID filtering already keeps us off most strange devices.
static AutoHANDLE try_open_COM_port(const std::string& port_name)
{
    const std::wstring path = L"\\\\.\\" + std::wstring(port_name.begin(), port_name.end());
    auto port = AutoHANDLE(CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL));

    if (port.get() == INVALID_HANDLE_VALUE) return port;

    try
    {
        apply_9600_8N1(port.get());
        apply_short_timeouts(port.get());
    }
    catch (const char*)
    {
        // XXX const auto win32_error = GetLastError();
        // XXX std::cerr << "Failed to configure " << port_name << ": " << e << " (Win32 error " << win32_error << ").\n";
        return AutoHANDLE(INVALID_HANDLE_VALUE);
    }
    return port;
}

static void send_query(HANDLE COM_port)
{
    const char query = '?';
    DWORD bytes_written;
    if (!WriteFile(COM_port, &query, 1, &bytes_written, nullptr))
    {
        // XXX std::cerr << "WriteFile() failed. Error=" << GetLastError() << ".\n";
    }
}

static bool arduino_response_seen_in(const std::string& buffer)
{
    for (size_t start = 0; ; )
    {
        const size_t newline = buffer.find('\n', start);
        if (newline == std::string::npos) return false;
        if (newline > start && (buffer[start] == 'b' || buffer[start] == 'v')) return true;
        start = newline + 1;
    }
}

// Opening the COM port pulses DTR, which resets the Arduino. The bootloader takes ~1-2 seconds before the sketch runs.
static bool device_runs_brightness_and_volume_app(const AutoHANDLE& port)
{
    const auto deadline = GetTickCount64() + 3000 /*ms*/;

    std::string buffer;
    while (GetTickCount64() < deadline)
    {
        send_query(port.get());

        const auto s = port.read();
        if (!s) return false;
        buffer += *s;

        if (arduino_response_seen_in(buffer)) return true;
    }
    return false;
}

static void set_status_detail(const std::string& msg);  // defined further down once the tray state it touches is declared

static AutoHANDLE open_COM_port(std::string& out_port_name)
{
    const auto port_names = enumerate_possibly_Arduino_COM_ports();
    if (port_names.empty()) throw "No Arduino-like COM ports found.";

    for (const auto& port_name : port_names)
    {
        set_status_detail("Trying " + port_name + ".");
        auto port = try_open_COM_port(port_name);
        if (port.get() == INVALID_HANDLE_VALUE) continue;
        if (device_runs_brightness_and_volume_app(port))
        {
            out_port_name = port_name;
            return port;
        }
    }

    throw "Failed to find an Arduino running the brightness and volume app on any COM port.";
}

namespace
{
    HANDLE active_COM_port = INVALID_HANDLE_VALUE;
    std::mutex active_COM_port_mutex;
    bool last_HDR_state_seen = false;  // only touched by the system-event thread

    HWND main_window = nullptr;
    UINT taskbar_created_message = 0;

    std::mutex tray_state_mutex;
    std::string connected_port_name;       // empty => not connected
    std::string status_detail;             // tooltip's line 2 while not connected (e.g. "Trying COM4.")
    int last_brightness_percent = -1;      // -1 => not yet received
    int last_volume_percent = -1;

    HICON tray_icon = nullptr;
}

constexpr UINT WM_APP_TRAY_ICON         = WM_APP + 1;
constexpr UINT WM_APP_REFRESH_TOOLTIP   = WM_APP + 2;
constexpr UINT TRAY_ICON_ID             = 1;
constexpr UINT IDM_EXIT                 = 1001;

static void send_query_to_active_COM_port()
{
    std::lock_guard lock(active_COM_port_mutex);
    if (active_COM_port != INVALID_HANDLE_VALUE) send_query(active_COM_port);
}

static void set_status_detail(const std::string& msg)
{
    {
        std::lock_guard lock(tray_state_mutex);
        status_detail = msg;
    }
    PostMessageW(main_window, WM_APP_REFRESH_TOOLTIP, 0, 0);
}

struct ScopedActiveCOMPort
{
    explicit ScopedActiveCOMPort(HANDLE port)
    {
        std::lock_guard lock(active_COM_port_mutex);
        active_COM_port = port;
    }

    ~ScopedActiveCOMPort()
    {
        std::lock_guard lock(active_COM_port_mutex);
        active_COM_port = INVALID_HANDLE_VALUE;
    }

    ScopedActiveCOMPort(const ScopedActiveCOMPort&) = delete;
    ScopedActiveCOMPort& operator=(const ScopedActiveCOMPort&) = delete;
};

// DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 (Windows 11 24H2) exposes activeColorMode, which
// distinguishes SDR/WCG/HDR. The older v1 query's advancedColorEnabled flag conflates HDR with
// Wide Color Gamut and so can't reliably detect an HDR-on/off transition on a WCG-capable panel.
static bool is_HDR_enabled()
{
    UINT32 num_paths = 0, num_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) != ERROR_SUCCESS)
        return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(num_paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(num_modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths.data(),
                           &num_modes, modes.data(), nullptr) != ERROR_SUCCESS)
        return false;

    for (const auto& path : paths)
    {
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color_info = {
            .header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2,
                       .size = sizeof(color_info),
                       .adapterId = path.targetInfo.adapterId,
                       .id = path.targetInfo.id}};
        if (DisplayConfigGetDeviceInfo(&color_info.header) == ERROR_SUCCESS &&
            color_info.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR)
            return true;
    }
    return false;
}

// The TV stores separate brightness levels per colour profile, so when Windows switches between
// HDR and SDR the picture brightness would jump unless we re-push the current knob value. Sending
// a '?' to the Arduino makes it re-emit its last brightness (and volume); the main thread's read
// loop then dispatches that to LGTV Companion as if the knob had just been turned.
static void resend_brightness_if_HDR_changed()
{
    const bool now = is_HDR_enabled();
    if (now != last_HDR_state_seen)
    {
        last_HDR_state_seen = now;
        send_query_to_active_COM_port();
    }
}

static std::wstring build_tooltip_text()
{
    std::lock_guard lock(tray_state_mutex);

    std::wstring tip;
    if (connected_port_name.empty())
    {
        tip = L"Not connected.\n";
        tip.append(status_detail.begin(), status_detail.end());
    }
    else
    {
        tip.assign(connected_port_name.begin(), connected_port_name.end());
        tip += L"\nBrightness: ";
        tip += (last_brightness_percent < 0) ? L"-" : (std::to_wstring(last_brightness_percent) + L"%");
        tip += L", Volume: ";
        tip += (last_volume_percent < 0) ? L"-" : (std::to_wstring(last_volume_percent) + L"%");
    }
    return tip;
}

static NOTIFYICONDATAW make_nid()
{
    NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW) };
    nid.hWnd = main_window;
    nid.uID = TRAY_ICON_ID;
    return nid;
}

static void copy_tooltip_into(NOTIFYICONDATAW& nid)
{
    auto tip = build_tooltip_text();
    if (tip.size() >= ARRAYSIZE(nid.szTip)) tip.resize(ARRAYSIZE(nid.szTip) - 1);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
}

// Generate a tray icon at runtime: bold white "LG" on a solid black background. Drawing it with GDI
// avoids shipping a separate .ico resource and the size automatically tracks SM_CXSMICON/SM_CYSMICON.
static HICON create_LG_icon()
{
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);

    HDC mem_dc = CreateCompatibleDC(nullptr);
    if (!mem_dc) return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cx;
    bmi.bmiHeader.biHeight = cy;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* color_bits = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &color_bits, nullptr, 0);
    if (!color_bitmap)
    {
        DeleteDC(mem_dc);
        return nullptr;
    }
    HBITMAP old_bitmap = static_cast<HBITMAP>(SelectObject(mem_dc, color_bitmap));

    HFONT font = CreateFontW(
        -(cy - 4),                                 // negative => character height in pixels
        0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,                       // grayscale AA so RGB stays equal at the edges
        DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(mem_dc, font));

    SetBkMode(mem_dc, TRANSPARENT);
    SetTextColor(mem_dc, RGB(255, 255, 255));
    RECT rect = { 1, 1, cx-2, cy-2 };
    DrawTextW(mem_dc, L"LG", 2, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(mem_dc, old_font);
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(font);
    DeleteDC(mem_dc);

    // The DIB starts zeroed (black) and GDI text drawing leaves alpha at 0; force every pixel
    // fully opaque so the entire icon renders as white "LG" on a solid black square.
    auto* pixels = static_cast<uint32_t*>(color_bits);
    for (int i = 0; i < cx * cy; ++i) pixels[i] |= 0xFF000000u;

    // 1bpp AND mask, all zero. With a 32bpp color bitmap the alpha channel governs blending.
    const SIZE_T mask_stride = ((static_cast<SIZE_T>(cx) + 15) / 16) * 2;
    std::vector<BYTE> mask_bits(mask_stride * cy, 0);
    HBITMAP mask_bitmap = CreateBitmap(cx, cy, 1, 1, mask_bits.data());

    ICONINFO icon_info = {};
    icon_info.fIcon = TRUE;
    icon_info.hbmMask = mask_bitmap;
    icon_info.hbmColor = color_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);

    DeleteObject(color_bitmap);
    DeleteObject(mask_bitmap);
    return icon;
}

static void add_tray_icon()
{
    NOTIFYICONDATAW nid = make_nid();
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY_ICON;
    nid.hIcon = tray_icon;
    copy_tooltip_into(nid);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void update_tray_tooltip()
{
    NOTIFYICONDATAW nid = make_nid();
    nid.uFlags = NIF_TIP;
    copy_tooltip_into(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void remove_tray_icon()
{
    NOTIFYICONDATAW nid = make_nid();
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void show_tray_context_menu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    // SetForegroundWindow before TrackPopupMenu so the menu dismisses correctly when focus is lost.
    SetForegroundWindow(main_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_window, nullptr);
    DestroyMenu(menu);
}

// This window receives system events that should trigger us to push current brightness/volume to
// the TV again, and also hosts the notification-area icon's callback messages.
//
//   PBT_APMRESUMEAUTOMATIC: PC has resumed from sleep. See "Why might USB resume not reset the
//     Arduino" below for why a query is needed.
//   WM_SETTINGCHANGE / "ImmersiveColorSet" and WM_DISPLAYCHANGE: Windows may have changed HDR/SDR
//     mode. Both messages are watched because neither fires reliably for HDR toggles across all
//     Windows builds and drivers.
//
// Why might USB resume not reset the Arduino: the Arduino's RESET pin is driven by DTR through a
// small capacitor, so only *transitions* on DTR cause a reset pulse - a steady DTR level (asserted
// or not) does nothing. If the host suspends the USB device rather than disconnecting it (typical
// when the motherboard maintains VBUS through sleep), USB suspend leaves control lines in their
// last-set state: DTR was asserted because the COM port was open, and it stays asserted through
// suspend and resume, so no edge occurs and the sketch keeps running with its last-sent values
// intact - which is when this query is required. If instead VBUS is cut, or the device re-enumerates
// on resume and the driver re-asserts DTR from scratch, the resulting edge resets the Arduino; the
// sketch restarts and emits fresh values on its first loop iteration, making the resume query
// redundant.
static LRESULT CALLBACK main_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMRESUMEAUTOMATIC) send_query_to_active_COM_port();
            break;

        case WM_DISPLAYCHANGE:
            resend_brightness_if_HDR_changed();
            break;

        case WM_SETTINGCHANGE:
            if (lparam && wcscmp(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") == 0)
                resend_brightness_if_HDR_changed();
            break;

        case WM_APP_TRAY_ICON:
            if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU)
                show_tray_context_menu();
            break;

        case WM_APP_REFRESH_TOOLTIP:
            update_tray_tooltip();
            break;

        case WM_COMMAND:
            if (LOWORD(wparam) == IDM_EXIT) DestroyWindow(hwnd);
            break;

        case WM_DESTROY:
            remove_tray_icon();
            if (tray_icon) { DestroyIcon(tray_icon); tray_icon = nullptr; }
            PostQuitMessage(0);
            break;

        default:
            // Re-add the icon if Explorer restarts.
            if (msg == taskbar_created_message) add_tray_icon();
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static std::string read_line(const AutoHANDLE& COM_port)
{
    static std::string buffer;

    for (;; Sleep(10 /*ms*/))
    {
        const auto s = COM_port.read();
        if (!s)
        {
            buffer.clear();
            throw "Arduino disconnected. Searching for it again...";
        }
        buffer += *s;
        if (size_t pos = buffer.find('\n'); pos != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            return line;
        }
    }
}

static int parse_percent(const std::string& s)
{
    try { return std::stoi(s); }
    catch (...) { return -1; }
}

static void dispatch_arduino_message(const std::string& line)
{
    if (line.starts_with('b'))
    {
        const auto value = line.substr(1);
        {
            std::lock_guard lock(tray_state_mutex);
            last_brightness_percent = parse_percent(value);
        }
        PostMessageW(main_window, WM_APP_REFRESH_TOOLTIP, 0, 0);
        set_brightness(value);
    }
    else if (line.starts_with('v'))
    {
        const auto value = line.substr(1);
        {
            std::lock_guard lock(tray_state_mutex);
            last_volume_percent = parse_percent(value);
        }
        PostMessageW(main_window, WM_APP_REFRESH_TOOLTIP, 0, 0);
        set_volume(value);
    }
}

static void run_arduino_session()
{
    std::string port_name;
    const auto COM_port = open_COM_port(port_name);
    {
        std::lock_guard lock(tray_state_mutex);
        connected_port_name = port_name;
    }
    PostMessageW(main_window, WM_APP_REFRESH_TOOLTIP, 0, 0);

    ScopedActiveCOMPort active_port(COM_port.get());

    send_query(COM_port.get());

    for (;;)
    {
        auto line = read_line(COM_port);
        dispatch_arduino_message(line);
    }
}

static void run_arduino_loop()
{
    for (;;)
    {
        try
        {
            run_arduino_session();
        }
        catch (const char* e)
        {
            {
                std::lock_guard lock(tray_state_mutex);
                connected_port_name.clear();
                status_detail = e;
            }
            PostMessageW(main_window, WM_APP_REFRESH_TOOLTIP, 0, 0);
        }

        Sleep(2000);
    }
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_ LPWSTR /*lpCmdLine*/,
                      _In_ int /*nShowCmd*/)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = main_wndproc;
    wc.lpszClassName = L"LGwebOSBrightnessVolumeSystemEvents";
    wc.hInstance = hInstance;
    RegisterClassExW(&wc);

    // Top-level (parent = nullptr) rather than HWND_MESSAGE so the window receives broadcast messages
    // such as WM_SETTINGCHANGE. The window has no WS_VISIBLE style and is never shown.
    main_window = CreateWindowExW(0, wc.lpszClassName, nullptr, 0,
                                  0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!main_window) return 1;

    taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    last_HDR_state_seen = is_HDR_enabled();

    tray_icon = create_LG_icon();
    add_tray_icon();

    std::thread(run_arduino_loop).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
