// This program receives messages sent on a COM port from a device with a brightness knob and a volume knob and it forwards requests to change brightness/volume to "LGTV Companion" (https://github.com/JPersson77/LGTVCompanion).

#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>


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
        std::cerr << "CreateProcess() failed with error " << GetLastError() << '.' << std::endl;
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
    //operator HANDLE() const noexcept { return handle; }

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

static std::vector<std::string> enumerate_COM_ports()
{
    std::vector<std::string> ports;

    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return ports;
    }

    for (DWORD index = 0; ; ++index)
    {
        char value_name[256];
        DWORD value_name_size = sizeof(value_name);
        char data[256];
        DWORD data_size = sizeof(data);
        DWORD type;
        if (RegEnumValueA(key, index, value_name, &value_name_size, nullptr,
                          &type, reinterpret_cast<LPBYTE>(data), &data_size) != ERROR_SUCCESS)
        {
            break;
        }
        if (type == REG_SZ)
        {
            ports.emplace_back(data);
        }
    }

    RegCloseKey(key);
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

static void apply_short_read_timeouts(HANDLE port)
{
    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    if (!SetCommTimeouts(port, &timeouts)) throw "SetCommTimeouts() failed.";
}

static AutoHANDLE try_open_COM_port(const std::string& port_name)
{
    const std::wstring path = L"\\\\.\\" + std::wstring(port_name.begin(), port_name.end());
    auto port = AutoHANDLE(CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL));

    if (port.get() == INVALID_HANDLE_VALUE) return port;

    try
    {
        apply_9600_8N1(port.get());
        apply_short_read_timeouts(port.get());
    }
    catch (const char* e)
    {
        const auto win32_error = GetLastError();
        std::cerr << "Failed to configure " << port_name << ": " << e << " (Win32 error " << win32_error << ").\n";
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
        std::cerr << "WriteFile() failed. Error=" << GetLastError() << ".\n";
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
static bool COM_port_appears_to_be_arduino(HANDLE port)
{
    const auto deadline = GetTickCount64() + 3000 /*ms*/;

    std::string buffer;
    while (GetTickCount64() < deadline)
    {
        send_query(port);

        char buf[128];
        DWORD bytes_read;
        if (!ReadFile(port, buf, sizeof(buf), &bytes_read, nullptr)) return false;
        buffer.append(buf, bytes_read);

        if (arduino_response_seen_in(buffer)) return true;
    }
    return false;
}

static AutoHANDLE open_COM_port()
{
    const auto port_names = enumerate_COM_ports();
    if (port_names.empty()) throw "No COM ports were found.";

    for (const auto& port_name : port_names)
    {
        std::cout << "Probing " << port_name << "...\n";
        auto port = try_open_COM_port(port_name);
        if (port.get() == INVALID_HANDLE_VALUE) continue;
        if (COM_port_appears_to_be_arduino(port.get()))
        {
            std::cout << "Connected to Arduino on " << port_name << ".\n";
            return port;
        }
    }

    throw "Failed to find an Arduino on any COM port.";
}

namespace
{
    HANDLE active_COM_port = INVALID_HANDLE_VALUE;
    std::mutex active_COM_port_mutex;
    bool last_HDR_state_seen = false;  // only touched by the system-event thread
}

static void send_query_to_active_COM_port()
{
    std::lock_guard lock(active_COM_port_mutex);
    if (active_COM_port != INVALID_HANDLE_VALUE) send_query(active_COM_port);
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

// This window receives system events that should trigger us to push current brightness/volume to
// the TV again:
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
static LRESULT CALLBACK system_event_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
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
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static void run_system_event_loop()
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = system_event_wndproc;
    wc.lpszClassName = L"LGwebOSBrightnessVolumeSystemEvents";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassExW(&wc);

    // Top-level (parent = nullptr) rather than HWND_MESSAGE so the window receives broadcast messages
    // such as WM_SETTINGCHANGE. The window has no WS_VISIBLE style and is never shown.
    CreateWindowExW(0, wc.lpszClassName, nullptr, 0,
                    0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);

    last_HDR_state_seen = is_HDR_enabled();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static std::string read_line(const AutoHANDLE& COM_port)
{
    static std::string buffer;

    for (;; Sleep(10 /*ms*/))
    {
        char buf[128];
        DWORD bytes_read;

        if (!ReadFile(COM_port.get(), buf, sizeof(buf), &bytes_read, nullptr))
        {
            buffer.clear();
            throw "Arduino disconnected. Searching for it again...";
        }

        buffer.append(buf, bytes_read);
        if (size_t pos = buffer.find('\n'); pos != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            return line;
        }
    }
}

static void dispatch_arduino_message(const std::string& line)
{
    if (line.starts_with('b'))
    {
        set_brightness(line.substr(1));
    }
    else if (line.starts_with('v'))
    {
        set_volume(line.substr(1));
    }
}

static void run_arduino_session()
{
    const auto COM_port = open_COM_port();
    ScopedActiveCOMPort active_port(COM_port.get());

    send_query(COM_port.get());

    for (;;)
    {
        auto line = read_line(COM_port);
        std::cout << line << '\n';
        dispatch_arduino_message(line);
    }
}

int main()
{
    std::thread(run_system_event_loop).detach();

    for (;;)
    {
        try
        {
            run_arduino_session();
        }
        catch (const char* e)
        {
            std::cerr << e << '\n';
        }
        Sleep(2000);
    }
}
