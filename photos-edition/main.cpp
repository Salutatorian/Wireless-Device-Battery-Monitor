// Native Win32 BatteryHub Photos. The taskbar shows each mouse photo beside its
// percentage. The window's close button exits the process.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>

#include <setupapi.h>
extern "C" {
#include <hidclass.h>
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr COLORREF kBg = RGB(19, 21, 25);
constexpr COLORREF kWhite = RGB(241, 241, 237);
constexpr COLORREF kMuted = RGB(153, 160, 172);
constexpr COLORREF kLow = RGB(236, 78, 135);
constexpr UINT WM_APP_READINGS = WM_APP + 1;
constexpr UINT_PTR kTick = 1;

struct Rgba {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;
};

struct Mouse {
    USHORT pid = 0;
    std::wstring name;
    int percent = -1;
    bool charging = false;
    bool stale = false;
    bool present = false;
    std::wstring detail;
};

struct Settings {
    int refresh_seconds = 60;
    bool always_on_top = false;
    bool compact = false;
    bool close_to_tray = false;
    bool taskbar_photos = true;
    bool startup = false;
};

struct Hit {
    RECT rect;
    int id;
};

HWND g_window = nullptr;
Settings g_settings;
bool g_show_settings = false;
std::vector<Hit> g_hits;
HWND g_strip = nullptr;
HANDLE g_stop = nullptr;
std::thread g_poll;
std::mutex g_lock;
std::vector<Mouse> g_mice;
Rgba g_a9;
Rgba g_ghost;
bool g_strip_dirty = true;
int g_strip_h = 0;
UINT g_strip_dpi = 0;

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

Rgba LoadPng(const std::wstring& path) {
    Rgba image;
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return image;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    if (SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        image.w = (int)w;
        image.h = (int)h;
        image.px.resize((size_t)w * h * 4);
        converter->CopyPixels(nullptr, w * 4, (UINT)image.px.size(), image.px.data());
    }
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    factory->Release();
    return image;
}

Rgba Trim(const Rgba& source) {
    int left = source.w, top = source.h, right = -1, bottom = -1;
    for (int y = 0; y < source.h; ++y) {
        for (int x = 0; x < source.w; ++x) {
            if (source.px[((size_t)y * source.w + x) * 4 + 3] > 8) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (right < left || bottom < top) return {};
    Rgba out;
    out.w = right - left + 1;
    out.h = bottom - top + 1;
    out.px.resize((size_t)out.w * out.h * 4);
    for (int y = 0; y < out.h; ++y) {
        std::memcpy(&out.px[(size_t)y * out.w * 4], &source.px[((size_t)(y + top) * source.w + left) * 4], (size_t)out.w * 4);
    }
    return out;
}

Rgba Fit(const Rgba& source, int max_w, int max_h) {
    if (source.w <= 0 || source.h <= 0 || max_w <= 0 || max_h <= 0) return {};
    const double scale = std::min((double)max_w / source.w, (double)max_h / source.h);
    const int w = std::max(1, (int)(source.w * scale + 0.5));
    const int h = std::max(1, (int)(source.h * scale + 0.5));
    Rgba out;
    out.w = w;
    out.h = h;
    out.px.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; ++y) {
        const int y0 = y * source.h / h;
        const int y1 = std::max(y0 + 1, (y + 1) * source.h / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = x * source.w / w;
            const int x1 = std::max(x0 + 1, (x + 1) * source.w / w);
            int sum[4] = {};
            int count = 0;
            for (int yy = y0; yy < y1 && yy < source.h; ++yy) {
                for (int xx = x0; xx < x1 && xx < source.w; ++xx) {
                    const uint8_t* p = &source.px[((size_t)yy * source.w + xx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++count;
                }
            }
            uint8_t* dest = &out.px[((size_t)y * w + x) * 4];
            if (count) {
                for (int c = 0; c < 4; ++c) dest[c] = (uint8_t)(sum[c] / count);
            }
        }
    }
    return out;
}

void Blend(std::vector<uint8_t>& canvas, int width, int height, int x, int y, const uint8_t rgb[3], uint8_t alpha) {
    if (x < 0 || y < 0 || x >= width || y >= height || alpha == 0) return;
    uint8_t* pixel = &canvas[((size_t)y * width + x) * 4];
    const uint32_t a = alpha;
    const uint32_t inverse = 255 - a;
    const uint8_t src[3] = {rgb[2], rgb[1], rgb[0]};
    for (int c = 0; c < 3; ++c) pixel[c] = (uint8_t)((src[c] * a + pixel[c] * inverse) / 255);
    pixel[3] = (uint8_t)(a + pixel[3] * inverse / 255);
}

void DrawPhoto(std::vector<uint8_t>& canvas, int width, int height, int x, int y, const Rgba& photo, bool present) {
    for (int yy = 0; yy < photo.h; ++yy) {
        for (int xx = 0; xx < photo.w; ++xx) {
            const uint8_t* p = &photo.px[((size_t)yy * photo.w + xx) * 4];
            const uint8_t alpha = present ? p[3] : (uint8_t)(p[3] * 140 / 255);
            const uint8_t rgb[3] = {p[0], p[1], p[2]};
            Blend(canvas, width, height, x + xx, y + yy, rgb, alpha);
        }
    }
}

SIZE MeasureText(const wchar_t* text, int font_h, const wchar_t* face, int weight) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-font_h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{0, font_h};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
    SelectObject(dc, old);
    DeleteObject(font);
    DeleteDC(dc);
    return size;
}

void DrawTextAlpha(std::vector<uint8_t>& canvas, int canvas_w, int canvas_h, const wchar_t* text, int x, int y, int w, int h, int font_h, COLORREF color, uint8_t alpha, const wchar_t* face, int weight) {
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    std::memset(bits, 0, (size_t)w * h * 4);
    HFONT font = CreateFontW(-font_h, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rect{0, 0, w, h};
    DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    const uint8_t rgb[3] = {GetRValue(color), GetGValue(color), GetBValue(color)};
    const auto* source = static_cast<const uint8_t*>(bits);
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            const size_t offset = ((size_t)yy * w + xx) * 4;
            const uint32_t coverage = (source[offset] + source[offset + 1] + source[offset + 2]) / 3;
            Blend(canvas, canvas_w, canvas_h, x + xx, y + yy, rgb, (uint8_t)(coverage * alpha / 255));
        }
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

struct Metrics {
    int width = 1;
    int height = 48;
    int icon = 16;
    int pad = 6;
    int gap = 4;
    int text_w = 46;
    int font = 13;
    int bolt = 14;
    int between = 10;
    int stride = 76;
};

Metrics StripMetrics(int count, int height, UINT dpi, int limit) {
    const float scale = dpi / 96.f;
    int icon = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    if (icon < 12) icon = (int)(16 * scale);
    icon = std::min(icon, std::max(12, height - 8));
    Metrics m;
    m.height = height;
    m.icon = icon;
    m.pad = std::max(2, (int)(6 * scale));
    m.gap = std::max(2, (int)(4 * scale));
    m.text_w = std::max(28, (int)(40 * scale));
    m.font = std::max(12, (int)(13 * scale));
    m.bolt = std::max(12, (int)(14 * scale));
    m.between = std::max(6, (int)(10 * scale));
    const int between = m.between;
    m.stride = icon + m.gap + m.text_w + between;
    m.width = count <= 0 ? 1 : std::max(1, m.pad + count * m.stride - between + m.pad);
    m.width = std::min(m.width, std::max(1, limit));
    return m;
}

void DrawLevel(std::vector<uint8_t>& canvas, int width, int height, int x, int y, int w, int h, int percent, bool charging, bool full) {
    const uint8_t track[3] = {255, 255, 255};
    uint8_t fill[3] = {255, 255, 255};
    if (full) {
        fill[0] = 108;
        fill[1] = 203;
        fill[2] = 95;
    } else if (charging) {
        fill[0] = 96;
        fill[1] = 205;
        fill[2] = 255;
    }
    const int filled = std::clamp(percent, 0, 100) * w / 100;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            const bool on = xx < filled;
            uint8_t alpha = on ? 230 : 50;
            if (charging && on) {
                const int sweep = (int)((GetTickCount() / 14) % (UINT)(w + 24)) - 12;
                if (std::abs(xx - sweep) <= 6) alpha = 255;
            }
            Blend(canvas, width, height, x + xx, y + yy, on ? fill : track, alpha);
        }
    }
}

int StripWidth(const std::vector<Mouse>& mice, const Metrics& m, int limit) {
    if (mice.empty()) return 1;
    int width = m.pad;
    for (size_t i = 0; i < mice.size(); ++i) {
        width += m.icon + m.gap + m.text_w + (mice[i].charging ? m.bolt : 0);
        if (i + 1 < mice.size()) width += m.between;
    }
    return std::min(width + m.pad, std::max(1, limit));
}

bool PaintStrip(const std::vector<Mouse>& mice) {
    if (!g_strip || mice.empty()) return false;
    RECT bar{};
    GetClientRect(GetParent(g_strip), &bar);
    const int height = std::clamp((int)(bar.bottom - bar.top), 24, 200);
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(GetParent(g_strip)));
    const Metrics m = StripMetrics((int)mice.size(), height, dpi, bar.right - bar.left);
    const int width = StripWidth(mice, m, bar.right - bar.left);
    std::vector<uint8_t> canvas((size_t)width * m.height * 4, 0);
    for (size_t i = 3; i < canvas.size(); i += 4) canvas[i] = 1;
    const bool light = [] {
        DWORD value = 0, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value != 0;
    }();
    const COLORREF foreground = light ? RGB(25, 27, 31) : RGB(255, 255, 255);
    std::wstring tip;
    int cursor = m.pad;
    for (size_t i = 0; i < mice.size(); ++i) {
        const Mouse& mouse = mice[i];
        const int x = cursor;
        cursor += m.icon + m.gap + m.text_w + (mouse.charging ? m.bolt : 0) + (i + 1 < mice.size() ? m.between : 0);
        const Rgba* source = mouse.pid == 0x11d8 ? &g_ghost : &g_a9;
        const Rgba photo = Fit(*source, m.icon, m.icon);
        if (photo.w > 0) {
            DrawPhoto(canvas, width, m.height, x + (m.icon - photo.w) / 2, (m.height - photo.h) / 2, photo, mouse.present);
        }
        std::wstring label = mouse.percent < 0 ? L"—" : (std::to_wstring(mouse.percent) + L"%" + (mouse.stale ? L"*" : L""));
        const bool full = mouse.charging && mouse.percent >= 100;
        const COLORREF color = mouse.percent >= 0 && mouse.percent <= 15 ? kLow : foreground;
        const SIZE extent = MeasureText(label.c_str(), m.font, L"Segoe UI Variable Text", FW_NORMAL);
        const int bar_h = 2;
        const int bar_gap = 2;
        const int block = extent.cy + (mouse.percent >= 0 ? bar_gap + bar_h : 0);
        const int top = std::max(0, (m.height - block) / 2);
        const int text_x = x + m.icon + m.gap;
        DrawTextAlpha(canvas, width, m.height, label.c_str(), text_x, top, extent.cx + 1, extent.cy, m.font, color, mouse.stale ? 170 : 255, L"Segoe UI Variable Text", FW_NORMAL);
        if (mouse.percent >= 0) {
            DrawLevel(canvas, width, m.height, text_x, top + extent.cy + bar_gap, extent.cx, bar_h, mouse.percent, mouse.charging, full);
        }
        if (mouse.charging) {
            const wchar_t mark[] = {full ? L'\xE73E' : L'\xE945', 0};
            const COLORREF mark_color = full ? RGB(108, 203, 95) : RGB(96, 205, 255);
            DrawTextAlpha(canvas, width, m.height, mark, text_x + extent.cx + 3, top, m.bolt, extent.cy, m.bolt, mark_color, mouse.stale ? 170 : 255, L"Segoe MDL2 Assets", FW_NORMAL);
        }
        if (!tip.empty()) tip += L" | ";
        tip += mouse.name + L": " + (mouse.percent < 0 ? L"battery unavailable" : std::to_wstring(mouse.percent) + L"%");
        if (mouse.charging) tip += full ? L" (full)" : L" (charging)";
        if (mouse.stale) tip += L" (last known)";
    }
    SetWindowTextW(g_strip, tip.c_str());
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -m.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    std::memcpy(bits, canvas.data(), canvas.size());
    HGDIOBJ old = SelectObject(mem, bitmap);
    SIZE size{width, m.height};
    POINT origin{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok = UpdateLayeredWindow(g_strip, screen, nullptr, &size, mem, &origin, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    g_strip_h = height;
    g_strip_dpi = dpi;
    return ok != FALSE;
}

void PlaceStrip(const std::vector<Mouse>& mice) {
    HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_settings.taskbar_photos || !bar || mice.empty()) {
        if (g_strip) ShowWindow(g_strip, SW_HIDE);
        return;
    }
    HWND tray = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT br{}, tr{};
    if (!tray || !GetWindowRect(bar, &br) || !GetWindowRect(tray, &tr) || br.bottom - br.top > br.right - br.left) {
        if (g_strip) ShowWindow(g_strip, SW_HIDE);
        return;
    }
    if (!g_strip || !IsWindow(g_strip) || GetParent(g_strip) != bar) {
        if (g_strip) DestroyWindow(g_strip);
        WNDCLASSW cls = {};
        cls.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_LBUTTONUP || message == WM_RBUTTONUP) {
                if (g_window) {
                    ShowWindow(g_window, SW_RESTORE);
                    SetForegroundWindow(g_window);
                }
                return 0;
            }
            if (message == WM_ERASEBKGND) return 1;
            return DefWindowProcW(window, message, wp, lp);
        };
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"BatteryHubPhotoStrip";
        cls.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassW(&cls);
        g_strip = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, cls.lpszClassName, L"BatteryHub", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, bar, nullptr, cls.hInstance, nullptr);
        g_strip_dirty = true;
    }
    if (!g_strip) return;
    const int height = std::clamp((int)(br.bottom - br.top), 24, 200);
    const UINT dpi = std::max<UINT>(96, GetDpiForWindow(bar));
    if (g_strip_dirty || height != g_strip_h || dpi != g_strip_dpi) {
        if (!PaintStrip(mice)) return;
        g_strip_dirty = false;
    }
    const Metrics m = StripMetrics((int)mice.size(), height, dpi, br.right - br.left);
    const int width = StripWidth(mice, m, br.right - br.left);
    POINT anchor{tr.left, br.top};
    ScreenToClient(bar, &anchor);
    SetWindowPos(g_strip, HWND_TOP, std::max(0L, anchor.x - width - 4), 0, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

uint8_t Checksum(const uint8_t* data, int count) {
    uint8_t sum = 0;
    for (int i = 0; i < count; ++i) sum = (uint8_t)(sum + data[i]);
    return (uint8_t)(0x55 - sum);
}

bool ValidFrame(const uint8_t* frame, int count, uint8_t command) {
    return count >= 17 && frame[0] == 8 && frame[1] == command && frame[16] == Checksum(frame, 16);
}

std::wstring Hex(const uint8_t* data, int count) {
    std::wstring text;
    for (int i = 0; i < count && i < 20; ++i) {
        wchar_t byte[4];
        swprintf(byte, 4, L"%02X", data[i]);
        if (!text.empty()) text += L" ";
        text += byte;
    }
    return text;
}

bool TakeReport(const std::vector<uint8_t>& buffer, DWORD got, uint8_t command, std::vector<uint8_t>& frame) {
    if (got < 17) return false;
    const uint8_t* body = buffer.data();
    int count = (int)got;
    if (body[0] == 0 && count > 17) {
        ++body;
        --count;
    }
    for (int offset = 0; offset + 17 <= count; ++offset) {
        if (ValidFrame(body + offset, count - offset, command)) {
            frame.assign(body + offset, body + offset + 17);
            return true;
        }
    }
    return false;
}

bool Query(HANDLE device, USHORT out_len, USHORT in_len, uint8_t command, std::vector<uint8_t>& frame, std::wstring& why) {
    const USHORT report_out = out_len < 17 ? 17 : out_len;
    const USHORT report_in = in_len < 17 ? 17 : in_len;
    std::vector<uint8_t> request(report_out, 0);
    request[0] = 8;
    request[1] = command;
    request[16] = Checksum(request.data(), 16);
    HidD_SetNumInputBuffers(device, 64);
    std::vector<uint8_t> buffer(report_in, 0);
    OVERLAPPED read_ov = {};
    read_ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD got = 0;
    BOOL read = ReadFile(device, buffer.data(), report_in, &got, &read_ov);
    const DWORD read_error = read ? 0 : GetLastError();
    OVERLAPPED write_ov = {};
    write_ov.hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD written = 0;
    BOOL wrote = WriteFile(device, request.data(), report_out, &written, &write_ov);
    DWORD write_error = wrote ? 0 : GetLastError();
    if (!wrote && write_error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(write_ov.hEvent, 400) == WAIT_OBJECT_0)
            wrote = GetOverlappedResult(device, &write_ov, &written, FALSE);
        else {
            CancelIoEx(device, &write_ov);
            GetOverlappedResult(device, &write_ov, &written, TRUE);
        }
        write_error = wrote ? 0 : GetLastError();
    }
    CloseHandle(write_ov.hEvent);
    const wchar_t* how = L"write";
    if (!wrote) {
        wrote = HidD_SetOutputReport(device, request.data(), report_out);
        how = L"set";
        write_error = wrote ? 0 : GetLastError();
    }
    if (!wrote) {
        CancelIoEx(device, &read_ov);
        CloseHandle(read_ov.hEvent);
        why = L"write err " + std::to_wstring(write_error);
        return false;
    }
    if (!read && read_error == ERROR_IO_PENDING) {
        if (WaitForSingleObject(read_ov.hEvent, 650) == WAIT_OBJECT_0)
            read = GetOverlappedResult(device, &read_ov, &got, FALSE);
        else {
            CancelIoEx(device, &read_ov);
            GetOverlappedResult(device, &read_ov, &got, TRUE);
        }
    }
    CloseHandle(read_ov.hEvent);
    if (read && TakeReport(buffer, got, command, frame)) return true;
    why = std::wstring(how) + L" " + std::to_wstring(written) + L" out " + std::to_wstring(report_out) + L" in " + std::to_wstring(report_in) + L" got " + std::to_wstring(got) + L" " + (got ? Hex(buffer.data(), (int)got) : L"none");
    return false;
}

std::vector<Mouse> ReadBatteries() {
    std::vector<Mouse> found;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (list == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(list, nullptr, &guid, index, &iface); ++index) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(list, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> buffer(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(list, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes{sizeof(attributes)};
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        const bool attrs = HidD_GetAttributes(probe, &attributes) != FALSE;
        if (HidD_GetPreparsedData(probe, &preparsed)) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        CloseHandle(probe);
        const bool supported = attrs && attributes.VendorID == 0x373b && (attributes.ProductID == 0x1278 || attributes.ProductID == 0x11d8) &&
                               caps.UsagePage == 0xff02 && caps.Usage == 2;
        if (!supported) continue;
        Mouse mouse;
        mouse.pid = attributes.ProductID;
        mouse.name = mouse.pid == 0x11d8 ? L"ATK Ghost" : L"ATK Dragonfly A9 Series";
        mouse.present = true;
        HANDLE device = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        std::vector<uint8_t> frame;
        std::wstring why;
        if (device != INVALID_HANDLE_VALUE) {
            Query(device, caps.OutputReportByteLength, caps.InputReportByteLength, 3, frame, why);
            frame.clear();
            if (Query(device, caps.OutputReportByteLength, caps.InputReportByteLength, 4, frame, why) && frame.size() >= 17 && frame[6] <= 100) {
                mouse.percent = frame[6];
                mouse.charging = frame[7] != 0;
                mouse.stale = false;
                mouse.detail = mouse.charging ? (mouse.percent >= 100 ? L"Full" : L"Charging") : L"Live";
            } else {
                mouse.stale = true;
                mouse.detail = L"Battery update pending";
            }
            CloseHandle(device);
        } else {
            mouse.stale = true;
            mouse.detail = L"Receiver detected; battery update pending";
        }
        found.push_back(std::move(mouse));
    }
    SetupDiDestroyDeviceInfoList(list);
    std::sort(found.begin(), found.end(), [](const Mouse& a, const Mouse& b) { return a.pid < b.pid; });
    return found;
}

void Remember(std::vector<Mouse> fresh) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (Mouse& mouse : fresh) {
        if (mouse.percent >= 0) continue;
        for (const Mouse& old : g_mice) {
            if (old.pid == mouse.pid && old.percent >= 0) {
                mouse.percent = old.percent;
                mouse.charging = old.charging;
                mouse.stale = true;
                mouse.detail = old.charging ? L"Charging" : L"Last known";
            }
        }
    }
    g_mice = std::move(fresh);
    g_strip_dirty = true;
}

void LoadCachedReadings() {
    wchar_t local[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring path = std::wstring(local) + L"\\BatteryHubPhotos\\data\\last-readings.json";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(file, nullptr);
    std::string json(size, '\0');
    DWORD read = 0;
    ReadFile(file, json.data(), size, &read, nullptr);
    CloseHandle(file);
    json.resize(read);
    std::vector<Mouse> cached;
    for (size_t pos = 0; (pos = json.find("\"pid\":", pos)) != std::string::npos; pos += 6) {
        const int pid = std::atoi(json.c_str() + pos + 6);
        if (pid != 0x1278 && pid != 0x11d8) continue;
        const size_t next = json.find("\"pid\":", pos + 6);
        const size_t region_end = next == std::string::npos ? json.size() : next;
        const size_t mark = json.find("\"percent\":", pos);
        if (mark == std::string::npos || mark > region_end) continue;
        const char* value = json.c_str() + mark + 10;
        while (*value == ' ') ++value;
        if (std::strncmp(value, "null", 4) == 0) continue;
        Mouse mouse;
        mouse.pid = (USHORT)pid;
        mouse.name = pid == 0x11d8 ? L"ATK Ghost" : L"ATK Dragonfly A9 Series";
        mouse.percent = std::atoi(value);
        const size_t charge = json.find("\"charging\":", mark);
        if (charge != std::string::npos && charge < region_end) {
            const char* flag = json.c_str() + charge + 11;
            while (*flag == ' ') ++flag;
            mouse.charging = std::strncmp(flag, "true", 4) == 0;
        }
        mouse.stale = true;
        mouse.present = true;
        mouse.detail = mouse.charging ? L"Charging" : L"Last known";
        cached.push_back(mouse);
    }
    if (!cached.empty()) {
        std::lock_guard<std::mutex> guard(g_lock);
        g_mice = std::move(cached);
        g_strip_dirty = true;
    }
}

std::wstring SettingsDir() {
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return std::wstring(local) + L"\\BatteryHubPhotos\\data";
}

bool JsonBool(const std::string& json, const char* key, bool fallback) {
    const std::string token = std::string("\"") + key + "\":";
    const size_t pos = json.find(token);
    if (pos == std::string::npos) return fallback;
    const char* value = json.c_str() + pos + token.size();
    while (*value == ' ') ++value;
    if (std::strncmp(value, "true", 4) == 0) return true;
    if (std::strncmp(value, "false", 5) == 0) return false;
    return fallback;
}

void LoadSettings() {
    const std::wstring path = SettingsDir() + L"\\settings.json";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(file, nullptr);
    std::string json(size == INVALID_FILE_SIZE ? 0 : size, '\0');
    DWORD read = 0;
    if (!json.empty()) ReadFile(file, json.data(), (DWORD)json.size(), &read, nullptr);
    CloseHandle(file);
    json.resize(read);
    g_settings.taskbar_photos = JsonBool(json, "taskbar_photos", g_settings.taskbar_photos);
    g_settings.always_on_top = JsonBool(json, "always_on_top", g_settings.always_on_top);
    g_settings.compact = JsonBool(json, "compact", g_settings.compact);
    g_settings.close_to_tray = JsonBool(json, "close_to_tray", g_settings.close_to_tray);
    const std::string token = "\"refresh_seconds\":";
    const size_t pos = json.find(token);
    if (pos != std::string::npos) {
        const int seconds = std::atoi(json.c_str() + pos + token.size());
        if (seconds >= 30 && seconds <= 300) g_settings.refresh_seconds = seconds;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        g_settings.startup = RegQueryValueExW(key, L"BatteryHubPhotos", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
}

void SaveSettings() {
    const std::wstring dir = SettingsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::string json = std::string("{\n  \"devices\": {},\n  \"refresh_seconds\": ") + std::to_string(g_settings.refresh_seconds) +
                             ",\n  \"always_on_top\": " + (g_settings.always_on_top ? "true" : "false") +
                             ",\n  \"compact\": " + (g_settings.compact ? "true" : "false") +
                             ",\n  \"close_to_tray\": " + (g_settings.close_to_tray ? "true" : "false") +
                             ",\n  \"tray_percentages\": true,\n  \"show_unsupported\": false,\n  \"taskbar_photos\": " +
                             (g_settings.taskbar_photos ? "true" : "false") + "\n}\n";
    const std::wstring path = dir + L"\\settings.json";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, json.data(), (DWORD)json.size(), &written, nullptr);
    CloseHandle(file);
}

void SetStartup(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring value = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, L"BatteryHubPhotos", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"BatteryHubPhotos");
    }
    RegCloseKey(key);
}

void ApplyWindow() {
    if (!g_window) return;
    SetWindowPos(g_window, g_settings.always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, g_settings.compact ? 480 : 760, g_settings.compact ? 390 : 520, SWP_NOMOVE | SWP_NOACTIVATE);
    g_strip_dirty = true;
}

void PollLoop() {
    do {
        Remember(ReadBatteries());
        if (g_window) PostMessageW(g_window, WM_APP_READINGS, 0, 0);
    } while (WaitForSingleObject(g_stop, (DWORD)std::max(30, g_settings.refresh_seconds) * 1000) == WAIT_TIMEOUT);
}

void AddHit(HDC dc, RECT rect, int id) {
    g_hits.push_back({rect, id});
    (void)dc;
}

void CheckRow(HDC dc, HFONT text_font, HFONT icon_font, int x, int y, int width, const wchar_t* label, bool on, int id) {
    RECT box{x, y, x + 18, y + 18};
    HBRUSH brush = CreateSolidBrush(on ? kLow : RGB(39, 43, 50));
    FillRect(dc, &box, brush);
    DeleteObject(brush);
    if (on) {
        SelectObject(dc, icon_font);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT mark = box;
        DrawTextW(dc, L"\xE73E", 1, &mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, text_font);
    SetTextColor(dc, kWhite);
    RECT text{x + 28, y - 2, x + width, y + 22};
    DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    AddHit(dc, RECT{x, y - 4, x + width, y + 26}, id);
}

void PaintSettings(HDC dc, const RECT& client) {
    HFONT title = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Display");
    HFONT body = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Text");
    HFONT icon = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe MDL2 Assets");
    SelectObject(dc, title);
    SetTextColor(dc, kWhite);
    RECT heading{32, 28, client.right - 32, 68};
    DrawTextW(dc, L"Settings", -1, &heading, DT_LEFT | DT_SINGLELINE);
    CheckRow(dc, body, icon, 32, 88, client.right - 64, L"Show device photos and percentages on the taskbar", g_settings.taskbar_photos, 2);
    CheckRow(dc, body, icon, 32, 126, client.right - 64, L"Keep this window on top", g_settings.always_on_top, 3);
    CheckRow(dc, body, icon, 32, 164, client.right - 64, L"Keep running when I close the window", g_settings.close_to_tray, 4);
    CheckRow(dc, body, icon, 32, 202, client.right - 64, L"Compact view", g_settings.compact, 5);
    CheckRow(dc, body, icon, 32, 240, client.right - 64, L"Start with Windows", g_settings.startup, 6);
    SelectObject(dc, body);
    SetTextColor(dc, kMuted);
    RECT refresh{32, 286, 180, 314};
    DrawTextW(dc, L"Refresh every", -1, &refresh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int choices[] = {30, 60, 120, 300};
    for (int i = 0; i < 4; ++i) {
        RECT chip{180 + i * 78, 286, 248 + i * 78, 314};
        const bool on = g_settings.refresh_seconds == choices[i];
        HBRUSH brush = CreateSolidBrush(on ? kLow : RGB(39, 43, 50));
        FillRect(dc, &chip, brush);
        DeleteObject(brush);
        SetTextColor(dc, kWhite);
        const std::wstring label = std::to_wstring(choices[i]) + L"s";
        DrawTextW(dc, label.c_str(), -1, &chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHit(dc, chip, 7 + i);
    }
    RECT folder{32, 340, 280, 372};
    HBRUSH folder_brush = CreateSolidBrush(RGB(39, 43, 50));
    FillRect(dc, &folder, folder_brush);
    DeleteObject(folder_brush);
    SetTextColor(dc, kWhite);
    DrawTextW(dc, L"Open settings folder", -1, &folder, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(dc, folder, 11);
    RECT quit{300, 340, 420, 372};
    HBRUSH quit_brush = CreateSolidBrush(kLow);
    FillRect(dc, &quit, quit_brush);
    DeleteObject(quit_brush);
    DrawTextW(dc, L"Quit", -1, &quit, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(dc, quit, 12);
    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(icon);
}

void PaintWindow(HWND window) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(window, &ps);
    RECT client{};
    GetClientRect(window, &client);
    g_hits.clear();
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    if (g_show_settings) {
        PaintSettings(dc, client);
        HFONT nav = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Text");
        SelectObject(dc, nav);
        SetTextColor(dc, kWhite);
        RECT back{client.right - 140, 24, client.right - 24, 52};
        HBRUSH back_brush = CreateSolidBrush(RGB(39, 43, 50));
        FillRect(dc, &back, back_brush);
        DeleteObject(back_brush);
        DrawTextW(dc, L"Devices", -1, &back, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHit(dc, back, 1);
        DeleteObject(nav);
        EndPaint(window, &ps);
        return;
    }
    std::vector<Mouse> mice;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        mice = g_mice;
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT title = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Display");
    HFONT body = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Text");
    HFONT big = CreateFontW(-40, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Display");
    HFONT icon_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe MDL2 Assets");
    if (mice.empty()) {
        SelectObject(dc, body);
        SetTextColor(dc, kMuted);
        RECT rect = client;
        rect.left += 32;
        DrawTextW(dc, L"Looking for the ATK receivers…", -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        const int slot = std::max(280, (int)(client.right - 48) / (int)mice.size());
        for (size_t i = 0; i < mice.size(); ++i) {
            const Mouse& mouse = mice[i];
            const int x = 24 + (int)i * slot;
            const Rgba* source = mouse.pid == 0x11d8 ? &g_ghost : &g_a9;
            const Rgba photo = Fit(*source, 180, 200);
            if (photo.w > 0) {
                BITMAPINFO info = {};
                info.bmiHeader.biSize = sizeof(info.bmiHeader);
                info.bmiHeader.biWidth = photo.w;
                info.bmiHeader.biHeight = -photo.h;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                void* bits = nullptr;
                HDC mem = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                auto* dest = static_cast<uint8_t*>(bits);
                for (size_t p = 0; p < photo.px.size(); p += 4) {
                    const uint8_t a = photo.px[p + 3];
                    dest[p] = (uint8_t)(photo.px[p + 2] * a / 255);
                    dest[p + 1] = (uint8_t)(photo.px[p + 1] * a / 255);
                    dest[p + 2] = (uint8_t)(photo.px[p] * a / 255);
                    dest[p + 3] = a;
                }
                HGDIOBJ old = SelectObject(mem, bitmap);
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                AlphaBlend(dc, x + (160 - photo.w) / 2, 36, photo.w, photo.h, mem, 0, 0, photo.w, photo.h, blend);
                SelectObject(mem, old);
                DeleteObject(bitmap);
                DeleteDC(mem);
            }
            SelectObject(dc, title);
            SetTextColor(dc, kWhite);
            const int center = x + 80;
            RECT name{center - 90, 250, center + 90, 290};
            DrawTextW(dc, mouse.name.c_str(), -1, &name, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, big);
            const bool full = mouse.charging && mouse.percent >= 100;
            SetTextColor(dc, mouse.percent >= 0 && mouse.percent <= 15 ? kLow : kWhite);
            const std::wstring percent = mouse.percent < 0 ? L"—" : std::to_wstring(mouse.percent) + L"%";
            SIZE extent{0, 40};
            GetTextExtentPoint32W(dc, percent.c_str(), (int)percent.size(), &extent);
            RECT level{center - extent.cx / 2, 296, center + (extent.cx + 1) / 2, 296 + extent.cy};
            DrawTextW(dc, percent.c_str(), -1, &level, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            if (mouse.charging) {
                SelectObject(dc, icon_font);
                SetTextColor(dc, full ? RGB(108, 203, 95) : RGB(96, 205, 255));
                const wchar_t mark[] = {full ? L'\xE73E' : L'\xE945', 0};
                RECT bolt{level.right + 6, level.top, level.right + 28, level.bottom};
                DrawTextW(dc, mark, -1, &bolt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, big);
            }
            if (mouse.percent >= 0) {
                RECT track{level.left, level.bottom + 8, level.right, level.bottom + 12};
                HBRUSH track_brush = CreateSolidBrush(RGB(55, 58, 66));
                FillRect(dc, &track, track_brush);
                DeleteObject(track_brush);
                RECT filled = track;
                filled.right = track.left + (track.right - track.left) * std::max(mouse.percent, 0) / 100;
                HBRUSH fill_brush = CreateSolidBrush(full ? RGB(108, 203, 95) : mouse.charging ? RGB(96, 205, 255) : RGB(241, 241, 237));
                FillRect(dc, &filled, fill_brush);
                DeleteObject(fill_brush);
            }
            SelectObject(dc, body);
            SetTextColor(dc, mouse.charging ? RGB(96, 205, 255) : kMuted);
            RECT detail{center - 90, level.bottom + 20, center + 90, level.bottom + 44};
            DrawTextW(dc, mouse.detail.c_str(), -1, &detail, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    DeleteObject(title);
    DeleteObject(body);
    DeleteObject(big);
    DeleteObject(icon_font);
    HFONT nav = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_NATURAL_QUALITY, 0, L"Segoe UI Variable Text");
    SelectObject(dc, nav);
    SetTextColor(dc, kWhite);
    RECT settings{client.right - 148, client.bottom - 44, client.right - 24, client.bottom - 16};
    HBRUSH settings_brush = CreateSolidBrush(RGB(39, 43, 50));
    FillRect(dc, &settings, settings_brush);
    DeleteObject(settings_brush);
    DrawTextW(dc, L"Settings", -1, &settings, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHit(dc, settings, 1);
    DeleteObject(nav);
    EndPaint(window, &ps);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CLOSE:
        if (g_settings.close_to_tray) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (const Hit& hit : g_hits) {
            if (!PtInRect(&hit.rect, point)) continue;
            if (hit.id == 1) g_show_settings = !g_show_settings;
            if (hit.id == 2) g_settings.taskbar_photos = !g_settings.taskbar_photos;
            if (hit.id == 3) g_settings.always_on_top = !g_settings.always_on_top;
            if (hit.id == 4) g_settings.close_to_tray = !g_settings.close_to_tray;
            if (hit.id == 5) g_settings.compact = !g_settings.compact;
            if (hit.id == 6) {
                g_settings.startup = !g_settings.startup;
                SetStartup(g_settings.startup);
            }
            if (hit.id >= 7 && hit.id <= 10) g_settings.refresh_seconds = hit.id == 7 ? 30 : hit.id == 8 ? 60 : hit.id == 9 ? 120 : 300;
            if (hit.id == 11) ShellExecuteW(window, L"open", SettingsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (hit.id == 12) {
                g_settings.close_to_tray = false;
                DestroyWindow(window);
                return 0;
            }
            if (hit.id != 1 && hit.id != 11 && hit.id != 12) {
                SaveSettings();
                ApplyWindow();
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_strip) DestroyWindow(g_strip);
        g_strip = nullptr;
        if (g_stop) SetEvent(g_stop);
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintWindow(window);
        return 0;
    case WM_APP_READINGS:
    case WM_TIMER:
        {
            std::vector<Mouse> mice;
            {
                std::lock_guard<std::mutex> guard(g_lock);
                mice = g_mice;
            }
            bool charging = false;
            for (const Mouse& mouse : mice) {
                if (mouse.charging && mouse.percent < 100) charging = true;
            }
            if (charging) g_strip_dirty = true;
            PlaceStrip(mice);
            if (message == WM_APP_READINGS || charging) InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    default:
        return DefWindowProcW(window, message, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HANDLE existing = CreateMutexW(nullptr, FALSE, L"Local\\BatteryHubPhotosCpp");
    if (existing && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND window = FindWindowW(L"BatteryHubPhotosWindow", L"BatteryHub Photos");
        if (window) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring dir = ExeDir();
    g_a9 = Trim(LoadPng(dir + L"\\atk-a9-pink.png"));
    g_ghost = Trim(LoadPng(dir + L"\\atk-ghost-purple.png"));

    WNDCLASSW cls = {};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"BatteryHubPhotosWindow";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    LoadSettings();
    g_window = CreateWindowExW(g_settings.always_on_top ? WS_EX_TOPMOST : 0, cls.lpszClassName, L"BatteryHub Photos", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, g_settings.compact ? 480 : 760, g_settings.compact ? 390 : 520, nullptr, nullptr, instance, nullptr);
    ShowWindow(g_window, show);
    UpdateWindow(g_window);
    LoadCachedReadings();
    SetTimer(g_window, kTick, 500, nullptr);
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_poll = std::thread(PollLoop);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_poll.joinable()) g_poll.join();
    if (g_stop) CloseHandle(g_stop);
    CoUninitialize();
    if (existing) CloseHandle(existing);
    return (int)msg.wParam;
}
