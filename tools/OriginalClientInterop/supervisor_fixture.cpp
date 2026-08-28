#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{

LRESULT CALLBACK fixture_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(18, 36, 62));
        FillRect(dc, &client, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(236, 210, 125));
        constexpr wchar_t label[] = L"OpenMinerZ original-client supervisor fixture";
        DrawTextW(dc, label, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    constexpr wchar_t class_name[] = L"OpenMinerZInteropSupervisorFixture";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = fixture_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (!RegisterClassW(&window_class))
        return 2;
    HWND window = CreateWindowExW(0U, class_name, L"CastleMinerZ Fixture",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 360,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr)
        return 3;
    ShowWindow(window, show == 0 ? SW_SHOW : show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
