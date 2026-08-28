#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr std::wstring_view endpoint_file_name = L"OpenMinerZInterop.endpoint.json";
constexpr std::wstring_view capture_directory_name = L"InteropCaptures";
constexpr std::wstring_view managed_state_host_name = L"OpenMinerZ.OriginalStateHost.exe";
constexpr std::size_t maximum_command_bytes = 512U;

std::filesystem::path module_directory()
{
    std::wstring path(32768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size())
        return {};
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

struct PipeSecurity final
{
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor{};

    PipeSecurity()
    {
        HANDLE token{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return;
        DWORD bytes{};
        static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &bytes));
        std::vector<std::uint8_t> storage(bytes);
        if (bytes == 0U ||
            !GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes))
        {
            CloseHandle(token);
            return;
        }
        CloseHandle(token);
        const auto *user = reinterpret_cast<const TOKEN_USER *>(storage.data());
        LPWSTR sid{};
        if (!ConvertSidToStringSidW(user->User.Sid, &sid))
            return;
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring{sid} + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return;
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }

    ~PipeSecurity()
    {
        if (descriptor != nullptr)
            LocalFree(descriptor);
    }

    PipeSecurity(const PipeSecurity &) = delete;
    PipeSecurity &operator=(const PipeSecurity &) = delete;

    [[nodiscard]] bool valid() const noexcept
    {
        return descriptor != nullptr;
    }
};

struct WindowSearch final
{
    DWORD process_id{};
    HWND window{};
};

BOOL CALLBACK find_window_callback(HWND window, LPARAM parameter)
{
    auto &search = *reinterpret_cast<WindowSearch *>(parameter);
    DWORD process_id{};
    static_cast<void>(GetWindowThreadProcessId(window, &process_id));
    if (process_id == search.process_id && IsWindowVisible(window) &&
        GetWindow(window, GW_OWNER) == nullptr)
    {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

HWND find_game_window(DWORD process_id)
{
    WindowSearch search{process_id, nullptr};
    static_cast<void>(EnumWindows(find_window_callback, reinterpret_cast<LPARAM>(&search)));
    return search.window;
}

bool valid_capture_name(std::string_view name)
{
    if (name.size() < 5U || name.size() > 96U || !name.ends_with(".bmp"))
        return false;
    for (const unsigned char character : name)
        if (std::isalnum(character) == 0 && character != '.' && character != '_' &&
            character != '-')
            return false;
    return true;
}

bool capture_has_visual_detail(const void *pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0)
        return false;
    const auto *pixel = static_cast<const std::uint32_t *>(pixels);
    const int minimum_x = (std::min)(16, width / 4);
    const int maximum_x = (std::max)(minimum_x + 1, width - minimum_x);
    const int minimum_y = (std::min)(40, height / 4);
    const int maximum_y =
        (std::max)(minimum_y + 1, height - (std::min)(8, height / 4));
    const std::size_t area = static_cast<std::size_t>(maximum_x - minimum_x) *
                             static_cast<std::size_t>(maximum_y - minimum_y);
    std::array<std::uint32_t, 2U> colors{};
    std::size_t unique{};
    for (std::size_t sample{}; sample < area; ++sample)
    {
        const std::size_t local_width = static_cast<std::size_t>(maximum_x - minimum_x);
        const std::size_t index =
            (static_cast<std::size_t>(minimum_y) + sample / local_width) *
                static_cast<std::size_t>(width) +
            static_cast<std::size_t>(minimum_x) + sample % local_width;
        const std::uint32_t color = pixel[index] & 0x00FFFFFFU;
        bool seen{};
        for (std::size_t color_index{}; color_index < unique; ++color_index)
            seen |= colors[color_index] == color;
        if (!seen)
        {
            colors[unique++] = color;
            if (unique == colors.size())
                return true;
        }
    }
    return false;
}

bool write_window_bmp(HWND window, const std::filesystem::path &path)
{
    RECT bounds{};
    if (!GetWindowRect(window, &bounds))
        return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return false;

    HDC window_dc = GetWindowDC(window);
    if (window_dc == nullptr)
        return false;
    HDC memory_dc = CreateCompatibleDC(window_dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1U;
    info.bmiHeader.biBitCount = 32U;
    info.bmiHeader.biCompression = BI_RGB;
    void *pixels{};
    HBITMAP bitmap = CreateDIBSection(window_dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0U);
    if (memory_dc == nullptr || bitmap == nullptr || pixels == nullptr)
    {
        if (bitmap != nullptr)
            DeleteObject(bitmap);
        if (memory_dc != nullptr)
            DeleteDC(memory_dc);
        ReleaseDC(window, window_dc);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    constexpr UINT render_full_content = 0x00000002U;
    BOOL captured = PrintWindow(window, memory_dc, render_full_content);
    if (!captured || !capture_has_visual_detail(pixels, width, height))
        captured = BitBlt(memory_dc, 0, 0, width, height, window_dc, 0, 0,
                          SRCCOPY | CAPTUREBLT);
    if (!captured || !capture_has_visual_detail(pixels, width, height))
    {
        static_cast<void>(ShowWindow(window, SW_RESTORE));
        static_cast<void>(BringWindowToTop(window));
        static_cast<void>(SetForegroundWindow(window));
        Sleep(100U);
        HDC desktop_dc = GetForegroundWindow() == window ? GetDC(nullptr) : nullptr;
        captured = desktop_dc != nullptr &&
                   BitBlt(memory_dc, 0, 0, width, height, desktop_dc, bounds.left, bounds.top,
                          SRCCOPY | CAPTUREBLT);
        if (desktop_dc != nullptr)
            ReleaseDC(nullptr, desktop_dc);
    }
    captured = captured && capture_has_visual_detail(pixels, width, height);

    bool written{};
    if (captured)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!error)
        {
            const std::uint64_t pixel_bytes = static_cast<std::uint64_t>(width) * height * 4U;
            if (pixel_bytes <= 1'073'741'824ULL)
            {
                BITMAPFILEHEADER file_header{};
                file_header.bfType = 0x4D42U;
                file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
                file_header.bfSize = static_cast<DWORD>(file_header.bfOffBits + pixel_bytes);
                BITMAPINFOHEADER bitmap_header = info.bmiHeader;
                std::ofstream output{path, std::ios::binary | std::ios::out | std::ios::trunc};
                written = output &&
                          static_cast<bool>(output.write(
                              reinterpret_cast<const char *>(&file_header), sizeof(file_header))) &&
                          static_cast<bool>(output.write(
                              reinterpret_cast<const char *>(&bitmap_header),
                              sizeof(bitmap_header))) &&
                          static_cast<bool>(output.write(reinterpret_cast<const char *>(pixels),
                                                         static_cast<std::streamsize>(pixel_bytes)));
            }
        }
    }

    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(window, window_dc);
    return written;
}

std::string trim_command(std::string command)
{
    while (!command.empty() &&
           (command.back() == '\r' || command.back() == '\n' || command.back() == ' ' ||
            command.back() == '\t'))
        command.pop_back();
    std::size_t first{};
    while (first < command.size() && (command[first] == ' ' || command[first] == '\t'))
        ++first;
    return command.substr(first);
}

bool game_running(HANDLE process)
{
    DWORD exit_code{};
    return GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
}

std::wstring managed_state_pipe_path(DWORD process_id)
{
    return L"\\\\.\\pipe\\OpenMinerZ.OriginalState." + std::to_wstring(process_id);
}

bool managed_state_available(DWORD process_id)
{
    const std::wstring path = managed_state_pipe_path(process_id);
    if (WaitNamedPipeW(path.c_str(), 0U))
        return true;
    return GetLastError() == ERROR_PIPE_BUSY;
}

std::string forward_managed_state_command(std::string_view command, DWORD process_id)
{
    if (command.empty() || command.size() > maximum_command_bytes)
        return "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
               "\"code\":\"InvalidCommandFrame\"}";
    std::vector<char> response(16'385U, '\0');
    DWORD read{};
    const std::wstring path = managed_state_pipe_path(process_id);
    if (!CallNamedPipeW(path.c_str(), const_cast<char *>(command.data()),
                        static_cast<DWORD>(command.size()), response.data(),
                        static_cast<DWORD>(response.size() - 1U), &read, 3000U) || read == 0U ||
        read >= response.size())
        return "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
               "\"code\":\"StateHostUnavailable\"}";
    return std::string{response.data(), read};
}

std::string handle_command(std::string command, HANDLE game_process, DWORD game_process_id,
                           const std::filesystem::path &directory)
{
    command = trim_command(std::move(command));
    const bool running = game_running(game_process);
    HWND window = running ? find_game_window(game_process_id) : nullptr;
    if (command == "status")
        return std::string{"{\"schema\":\"openminerz.original-control.v1\",\"ok\":true,"}
               + "\"running\":" + (running ? "true" : "false") + ",\"window\":" +
               (window != nullptr ? "true" : "false") + ",\"state_host\":" +
               (running && managed_state_available(game_process_id) ? "true" : "false") + "}";
    if (command == "quit")
    {
        const bool posted = window != nullptr && PostMessageW(window, WM_CLOSE, 0U, 0U);
        return std::string{"{\"schema\":\"openminerz.original-control.v1\",\"ok\":"} +
               (posted ? "true" : "false") + ",\"command\":\"quit\"}";
    }
    constexpr std::string_view capture_prefix = "capture ";
    if (command.starts_with(capture_prefix))
    {
        const std::string name{std::string_view{command}.substr(capture_prefix.size())};
        if (window == nullptr || !valid_capture_name(name))
            return "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
                   "\"code\":\"CaptureUnavailableOrInvalidName\"}";
        const std::filesystem::path path =
            directory / capture_directory_name / std::filesystem::path{name};
        if (!write_window_bmp(window, path))
            return "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
                   "\"code\":\"CaptureFailed\"}";
        return std::string{"{\"schema\":\"openminerz.original-control.v1\",\"ok\":true,"}
               + "\"command\":\"capture\",\"name\":\"" + name + "\"}";
    }
    constexpr std::string_view state_prefix = "dump-state ";
    if (command.starts_with(state_prefix))
        return running ? forward_managed_state_command(command, game_process_id)
                       : "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
                         "\"code\":\"StateHostUnavailable\"}";
    return "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
           "\"code\":\"UnknownCommand\"}";
}

bool write_endpoint(const std::filesystem::path &path, std::string_view pipe_name,
                    DWORD supervisor_process_id, DWORD game_process_id)
{
    std::ofstream output{path, std::ios::binary | std::ios::out | std::ios::trunc};
    return output &&
           static_cast<bool>(output << "{\"schema\":\"openminerz.original-endpoint.v1\","
                                      "\"pipe\":\""
                                   << pipe_name << "\",\"supervisor_pid\":"
                                   << supervisor_process_id << ",\"game_pid\":"
                                   << game_process_id << "}\n"
                                   << std::flush);
}

void pipe_server(std::wstring pipe_path, const PipeSecurity &security, HANDLE game_process,
                 DWORD game_process_id, std::filesystem::path directory,
                 std::atomic_bool &stopping, HANDLE ready_event, std::atomic_bool &failed)
{
    bool first = true;
    while (!stopping.load())
    {
        HANDLE pipe = CreateNamedPipeW(
            pipe_path.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1U,
            4096U, 4096U, 0U, const_cast<SECURITY_ATTRIBUTES *>(&security.attributes));
        if (pipe == INVALID_HANDLE_VALUE)
        {
            failed.store(true);
            if (first)
                SetEvent(ready_event);
            return;
        }
        if (first)
        {
            SetEvent(ready_event);
            first = false;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                   ? TRUE
                                   : GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected && !stopping.load())
        {
            std::vector<char> command(maximum_command_bytes + 1U, '\0');
            DWORD read{};
            const BOOL received =
                ReadFile(pipe, command.data(), static_cast<DWORD>(maximum_command_bytes), &read,
                         nullptr);
            std::string response;
            if (!received || read == 0U || read > maximum_command_bytes)
                response = "{\"schema\":\"openminerz.original-control.v1\",\"ok\":false,"
                           "\"code\":\"InvalidCommandFrame\"}";
            else
                response = handle_command(std::string{command.data(), read}, game_process,
                                          game_process_id, directory);
            DWORD written{};
            static_cast<void>(WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()),
                                        &written, nullptr));
            static_cast<void>(FlushFileBuffers(pipe));
        }
        static_cast<void>(DisconnectNamedPipe(pipe));
        CloseHandle(pipe);
    }
}

void wake_pipe(std::wstring_view pipe_path)
{
    HANDLE pipe = CreateFileW(std::wstring{pipe_path}.c_str(), GENERIC_READ | GENERIC_WRITE, 0U,
                              nullptr, OPEN_EXISTING, 0U, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return;
    constexpr char stop[] = "status";
    DWORD written{};
    static_cast<void>(WriteFile(pipe, stop, sizeof(stop) - 1U, &written, nullptr));
    CloseHandle(pipe);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::filesystem::path directory = module_directory();
    if (directory.empty() || !SetCurrentDirectoryW(directory.c_str()))
        return 2;
    const std::filesystem::path original = directory / L"CastleMinerZ.exe";
    if (!std::filesystem::is_regular_file(original))
        return 3;

    // The wrapper hosts only process control. The original executable and its
    // Steam authentication path remain untouched and run from their own staged directory.
    SetEnvironmentVariableW(L"SteamAppId", L"253430");
    SetEnvironmentVariableW(L"SteamGameId", L"253430");
    const std::filesystem::path managed_host = directory / managed_state_host_name;
    const bool use_managed_host = std::filesystem::is_regular_file(managed_host);
    const std::filesystem::path executable = use_managed_host ? managed_host : original;
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0U, nullptr,
                        directory.c_str(), &startup, &process))
        return 4;
    CloseHandle(process.hThread);

    PipeSecurity security;
    if (!security.valid())
    {
        CloseHandle(process.hProcess);
        return 5;
    }
    const DWORD supervisor_process_id = GetCurrentProcessId();
    const std::string pipe_name = "OpenMinerZ.Interop." + std::to_string(supervisor_process_id);
    const std::wstring pipe_path = L"\\\\.\\pipe\\" +
                                   std::wstring{pipe_name.begin(), pipe_name.end()};
    HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_event == nullptr)
    {
        CloseHandle(process.hProcess);
        return 6;
    }
    std::atomic_bool stopping{};
    std::atomic_bool server_failed{};
    std::thread server{pipe_server, pipe_path, std::cref(security), process.hProcess,
                       process.dwProcessId, directory, std::ref(stopping), ready_event,
                       std::ref(server_failed)};
    const DWORD ready = WaitForSingleObject(ready_event, 5000U);
    CloseHandle(ready_event);
    const std::filesystem::path endpoint = directory / endpoint_file_name;
    if (ready != WAIT_OBJECT_0 || server_failed.load() ||
        !write_endpoint(endpoint, pipe_name, supervisor_process_id, process.dwProcessId))
    {
        stopping.store(true);
        wake_pipe(pipe_path);
        server.join();
        CloseHandle(process.hProcess);
        return 7;
    }

    static_cast<void>(WaitForSingleObject(process.hProcess, INFINITE));
    DWORD game_exit_code{};
    static_cast<void>(GetExitCodeProcess(process.hProcess, &game_exit_code));
    stopping.store(true);
    wake_pipe(pipe_path);
    server.join();
    CloseHandle(process.hProcess);
    std::error_code remove_error;
    std::filesystem::remove(endpoint, remove_error);
    return game_exit_code == STILL_ACTIVE ? 8 : static_cast<int>(game_exit_code);
}
