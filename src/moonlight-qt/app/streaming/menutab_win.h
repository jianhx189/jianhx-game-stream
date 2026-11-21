#pragma once
#include <SDL_events.h>
#include <SDL_syswm.h>

// Owned by the SDL window. Windows removes it from view with its owner and
// routes clicks inside the small tab to this window instead of the game.
class StreamMenuTab {
public:
    explicit StreamMenuTab(SDL_Window *window, bool enabled) {
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if (!enabled || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return;
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = L"MiniStreamSessionMenuTab";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        handle = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                 L"MiniStream 串流菜单", WS_POPUP, 0, 0, 0, 0,
                                 info.info.win.window, nullptr, wc.hInstance, nullptr);
        update(false);
    }
    ~StreamMenuTab() { if (handle) DestroyWindow(handle); }
    void update(bool menuOpen) {
        if (!handle) return;
        HWND owner = GetWindow(handle, GW_OWNER);
        if (!owner || menuOpen || IsIconic(owner) || GetForegroundWindow() != owner) {
            ShowWindow(handle, SW_HIDE);
            return;
        }
        RECT bounds{};
        POINT origin{};
        if (!GetClientRect(owner, &bounds) || !ClientToScreen(owner, &origin)) return;
        HDC dc = GetDC(owner);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(owner, dc);
        int width = MulDiv(28, dpi, 96), height = MulDiv(56, dpi, 96);
        SetWindowPos(handle, HWND_TOP, origin.x, origin.y + (bounds.bottom-height)/2,
                     width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    static constexpr int openEvent = 106;
private:
    HWND handle = nullptr;
    static LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_LBUTTONUP) {
            SDL_Event event{};
            event.type = SDL_USEREVENT;
            event.user.code = openEvent;
            SDL_PushEvent(&event);
            return 0;
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            SetDCBrushColor(dc, RGB(27, 41, 66));
            FillRect(dc, &rect, (HBRUSH)GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc, RGB(102, 201, 242));
            int mid = rect.bottom/2;
            for (int dy : {-5, 0, 5}) {
                RECT line{rect.right/4, mid+dy, rect.right*3/4, mid+dy+2};
                FillRect(dc, &line, (HBRUSH)GetStockObject(DC_BRUSH));
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};
