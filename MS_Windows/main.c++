// This program receives messages sent on a COM port from a device with a brightness knob and a volume knob and it forwards requests to change brightness/volume to "LGTV Companion" (https://github.com/JPersson77/LGTVCompanion).

#include <iostream>
#include <string>
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
        std::cerr << "CreateProcess() failed with error " << GetLastError() << std::endl;
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

#define COM_PORT "COM4"

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

static AutoHANDLE open_COM_port()
{
    auto hSerial = AutoHANDLE(CreateFileW(
        L"\\\\.\\" COM_PORT, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL));

    if (INVALID_HANDLE_VALUE == hSerial.get())
    {
        throw "Failed to open " COM_PORT;
    }

    DCB dcbSerialParams = { 0 };
    dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
    if (!GetCommState(hSerial.get(), &dcbSerialParams))
    {
        throw "Failed to get " COM_PORT " state";
    }
    dcbSerialParams.BaudRate = CBR_9600;
    dcbSerialParams.ByteSize = 8;
    dcbSerialParams.StopBits = ONESTOPBIT;
    dcbSerialParams.Parity = NOPARITY;

    if (!SetCommState(hSerial.get(), &dcbSerialParams))
    {
        throw "Failed to set " COM_PORT " parameters";
    }

    COMMTIMEOUTS timeouts = { 0 };
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial.get(), &timeouts);

    return hSerial;
}

static std::string read_line(const AutoHANDLE& COM_port)
{
    static std::string buffer;

    for (;; Sleep(10 /*ms*/))
    {
        char buf[128];
        DWORD bytes_read;

        if (ReadFile(COM_port.get(), buf, sizeof(buf), &bytes_read, nullptr))
        {
            buffer.append(buf, bytes_read);
            if (size_t pos = buffer.find('\n'); pos != std::string::npos)
            {
                std::string line = buffer.substr(0, pos);  // (no '\n')
                buffer.erase(0, pos + 1);
                return line;
            }
        }
        else
        {
            std::cerr << "ReadFile() failed. Error=" << GetLastError() << '\n';
        }
    }
}


int main()
try
{
    for (const auto COM_port = open_COM_port(); ;)
    {
        auto s = read_line(COM_port);
        std::cout << s << '\n';
        if (s.starts_with('b'))
        {
            set_brightness(s.substr(1));
        }
        else if (s.starts_with('v'))    
        {
            set_volume(s.substr(1));
        }
    }
}
catch (const char* e)
{
    std::cerr << e << '\n';
}