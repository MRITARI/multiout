// MultiOut - mirror Windows system audio to several output devices, each with its own volume.
// Native Win32 + WASAPI, no dependencies.
//
// Build (MSVC, Developer Command Prompt):
//   cl /O2 /EHsc /DUNICODE /D_UNICODE multiout.cpp /link /SUBSYSTEM:WINDOWS user32.lib ole32.lib comctl32.lib winmm.lib
// Build (MinGW-w64 / MSYS2 / w64devkit):
//   g++ -O2 -municode -mwindows -static multiout.cpp -o multiout.exe -lole32 -luuid -lcomctl32 -lwinmm

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static const PROPERTYKEY kFriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const GUID kSubtypeFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

#define WM_OUT_FAILED (WM_APP + 1)

// ---------- shared audio format (default device mix format, float32) ----------
static std::vector<BYTE> g_fmtBuf;
static WAVEFORMATEX* g_fmt = nullptr;
static UINT32 g_ch = 2, g_rate = 48000;

// ---------- ring buffer of interleaved float samples ----------
struct Ring {
    std::vector<float> buf;
    size_t r = 0, w = 0, count = 0, ch = 2;
    std::mutex m;
    void init(size_t frames, size_t channels) {
        ch = channels; buf.assign(frames * channels, 0.f); r = w = count = 0;
    }
    unsigned long long total = 0;                    // samples ever pushed (activity detector)
    size_t frames() { std::lock_guard<std::mutex> l(m); return count / ch; }
    unsigned long long pushed() { std::lock_guard<std::mutex> l(m); return total; }
    void drop(size_t nframes) {                      // discard oldest frames
        std::lock_guard<std::mutex> l(m);
        size_t n = std::min(nframes * ch, count);
        r = (r + n) % buf.size(); count -= n;
    }
    void push(const float* d, size_t n) {            // n = samples (multiple of ch)
        std::lock_guard<std::mutex> l(m);
        size_t cap = buf.size();
        if (n > cap) { d += n - cap; n = cap; }
        if (count + n > cap) {                       // overflow: drop oldest, keep latency bounded
            size_t over = count + n - cap;
            over = (over + ch - 1) / ch * ch;
            r = (r + over) % cap; count -= over;
        }
        for (size_t i = 0; i < n; i++) { buf[w] = d ? d[i] : 0.f; w = (w + 1) % cap; }
        count += n; total += n;
    }
    size_t pop(float* out, size_t n) {
        std::lock_guard<std::mutex> l(m);
        n = std::min(n, count);
        for (size_t i = 0; i < n; i++) { out[i] = buf[r]; r = (r + 1) % buf.size(); }
        count -= n;
        return n;
    }
};

struct Output {
    std::wstring id, name;
    std::atomic<float> vol{1.0f};
    std::atomic<bool> run{false};
    std::thread th;
    Ring ring;
    std::atomic<int> delayMs{0};
    std::atomic<unsigned long long> written{0};
    HWND label = nullptr, slider = nullptr, btn = nullptr, dSlider = nullptr, dText = nullptr, vText = nullptr;
};

static HWND g_hwnd, g_plus, g_title;
static std::mutex g_mtx;                              // guards g_outs
static std::vector<std::unique_ptr<Output>> g_outs;
static std::atomic<bool> g_run{true};
static std::atomic<unsigned long long> g_capFrames{0}, g_capSilent{0};
static std::thread g_capThread;
static IAudioClient* g_capClient = nullptr;
static IAudioCaptureClient* g_capSvc = nullptr;
static std::wstring g_defId, g_defName;

// ---------- helpers ----------
static std::wstring DevId(IMMDevice* d) {
    LPWSTR p = nullptr; std::wstring s;
    if (SUCCEEDED(d->GetId(&p)) && p) { s = p; CoTaskMemFree(p); }
    return s;
}
static std::wstring DevName(IMMDevice* d) {
    std::wstring r = L"(unknown device)";
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return r;
}
static IMMDeviceEnumerator* Enumerator() {
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), (void**)&en);
    return en;
}
static bool IsFloatFmt(const WAVEFORMATEX* f) {
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto e = (const WAVEFORMATEXTENSIBLE*)f;
        return memcmp(&e->SubFormat, &kSubtypeFloat, sizeof(GUID)) == 0;
    }
    return false;
}

// ---------- capture: loopback of the default output device ----------
static bool InitCapture() {
    IMMDeviceEnumerator* en = Enumerator();
    if (!en) return false;
    IMMDevice* dev = nullptr;
    HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr)) return false;
    g_defId = DevId(dev); g_defName = DevName(dev);
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_capClient);
    dev->Release();
    if (FAILED(hr)) return false;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(g_capClient->GetMixFormat(&mix))) return false;
    g_fmtBuf.assign((BYTE*)mix, (BYTE*)mix + sizeof(WAVEFORMATEX) + mix->cbSize);
    CoTaskMemFree(mix);
    g_fmt = (WAVEFORMATEX*)g_fmtBuf.data();
    if (!IsFloatFmt(g_fmt) || g_fmt->wBitsPerSample != 32) return false;
    g_ch = g_fmt->nChannels; g_rate = g_fmt->nSamplesPerSec;
    hr = g_capClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                 2000000, 0, g_fmt, nullptr);
    if (FAILED(hr)) return false;
    if (FAILED(g_capClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_capSvc))) return false;
    return SUCCEEDED(g_capClient->Start());
}

static void CaptureLoop() {
    std::vector<float> zeros;
    while (g_run) {
        Sleep(10);
        for (;;) {
            UINT32 n = 0;
            if (FAILED(g_capSvc->GetNextPacketSize(&n)) || n == 0) break;
            BYTE* data; UINT32 frames; DWORD flags;
            if (FAILED(g_capSvc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return;
            size_t samples = (size_t)frames * g_ch;
            g_capFrames += frames;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) g_capSilent += frames;
            const float* src = (const float*)data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                if (zeros.size() < samples) zeros.assign(samples, 0.f);
                src = zeros.data();
            }
            {
                std::lock_guard<std::mutex> l(g_mtx);
                for (auto& o : g_outs) o->ring.push(src, samples);
            }
            g_capSvc->ReleaseBuffer(frames);
        }
    }
}

// ---------- one render thread per added device ----------
static void OutputLoop(Output* o) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = Enumerator();
    IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr;
    HRESULT hr = en ? en->GetDevice(o->id.c_str(), &dev) : E_FAIL;
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
    if (SUCCEEDED(hr))
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            2000000, 0, g_fmt, nullptr);
    UINT32 bufFrames = 0;
    if (SUCCEEDED(hr)) hr = ac->GetBufferSize(&bufFrames);
    if (SUCCEEDED(hr)) hr = ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc);
    if (SUCCEEDED(hr)) hr = ac->Start();
    if (FAILED(hr)) {
        PostMessage(g_hwnd, WM_OUT_FAILED, (WPARAM)o, 0);
    } else {
        bool primed = false;
        unsigned long long lastTotal = 0; int idle = 0;
        const size_t prime = g_rate / 50;             // 20 ms safety prebuffer (lower = less latency, more risk)
        while (o->run) {
            Sleep(5);
            unsigned long long tot = o->ring.pushed();
            idle = (tot == lastTotal) ? idle + 1 : 0; lastTotal = tot;
            bool flushing = idle >= 8;                // ~40 ms without new audio: stream paused, play out the rest
            size_t avail = o->ring.frames();
            size_t hold = prime + (size_t)o->delayMs.load() * g_rate / 1000;   // frames kept back = prebuffer + user delay
            if (!primed) { if (avail >= hold || (flushing && avail > 0)) primed = true; else continue; }
            if (avail > hold + g_rate / 10) {         // delay was lowered live: discard backlog
                o->ring.drop(avail - hold); avail = hold;
            }
            size_t keep = flushing ? 0 : hold;
            if (avail <= keep) { if (avail == 0 && flushing) primed = false; continue; }
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) break;
            UINT32 n = (UINT32)std::min<size_t>(bufFrames - pad, avail - keep);
            if (!n) continue;
            BYTE* p = nullptr;
            if (FAILED(rc->GetBuffer(n, &p))) break;
            float* f = (float*)p;
            size_t got = o->ring.pop(f, (size_t)n * g_ch);
            float v = o->vol;
            for (size_t i = 0; i < got; i++) f[i] *= v;
            rc->ReleaseBuffer(n, 0);
            o->written += n;
        }
        ac->Stop();
    }
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    CoUninitialize();
}

// ---------- UI ----------
static const int ROW_H = 78, TOP = 52, WIDTH = 380;
static HFONT g_font;

static void Layout() {
    int y = TOP;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto& o : g_outs) {
            SetWindowPos(o->label, nullptr, 10, y, 360, 16, SWP_NOZORDER);
            SetWindowPos(o->vText, nullptr, 10, y + 24, 60, 16, SWP_NOZORDER);
            SetWindowPos(o->slider, nullptr, 70, y + 18, 240, 24, SWP_NOZORDER);
            SetWindowPos(o->dText, nullptr, 10, y + 50, 60, 16, SWP_NOZORDER);
            SetWindowPos(o->dSlider, nullptr, 70, y + 44, 240, 24, SWP_NOZORDER);
            SetWindowPos(o->btn, nullptr, 320, y + 30, 50, 26, SWP_NOZORDER);
            y += ROW_H;
        }
    }
    RECT r = {0, 0, WIDTH, y + 8};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    SetWindowPos(g_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static HWND MakeChild(const wchar_t* cls, const wchar_t* text, DWORD style, HWND parent) {
    HWND h = CreateWindowEx(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, nullptr,
                            GetModuleHandle(nullptr), nullptr);
    SendMessage(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void AddOutput(const std::wstring& id, const std::wstring& name) {
    auto o = std::make_unique<Output>();
    o->id = id; o->name = name;
    o->ring.init(g_rate, g_ch);                       // 1 s cap (max delay 500 ms + headroom)
    o->label = MakeChild(L"STATIC", name.c_str(), SS_ENDELLIPSIS, g_hwnd);
    o->vText = MakeChild(L"STATIC", L"Volume", 0, g_hwnd);
    o->slider = MakeChild(TRACKBAR_CLASS, L"", TBS_HORZ | TBS_NOTICKS, g_hwnd);
    SendMessage(o->slider, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessage(o->slider, TBM_SETPOS, TRUE, 100);
    o->dText = MakeChild(L"STATIC", L"Delay 0 ms", 0, g_hwnd);
    o->dSlider = MakeChild(TRACKBAR_CLASS, L"", TBS_HORZ | TBS_NOTICKS, g_hwnd);
    SendMessage(o->dSlider, TBM_SETRANGE, TRUE, MAKELONG(0, 500));
    SendMessage(o->dSlider, TBM_SETPAGESIZE, 0, 10);
    SendMessage(o->dSlider, TBM_SETPOS, TRUE, 0);
    o->btn = MakeChild(L"BUTTON", L"Remove", BS_PUSHBUTTON, g_hwnd);
    o->run = true;
    Output* p = o.get();
    p->th = std::thread(OutputLoop, p);
    { std::lock_guard<std::mutex> l(g_mtx); g_outs.push_back(std::move(o)); }
    Layout();
}

static void RemoveOutput(Output* p) {
    std::unique_ptr<Output> o;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto it = g_outs.begin(); it != g_outs.end(); ++it)
            if (it->get() == p) { o = std::move(*it); g_outs.erase(it); break; }
    }
    if (!o) return;
    o->run = false;
    if (o->th.joinable()) o->th.join();
    DestroyWindow(o->label); DestroyWindow(o->slider); DestroyWindow(o->btn);
    DestroyWindow(o->vText); DestroyWindow(o->dSlider); DestroyWindow(o->dText);
    Layout();
}

static void ShowAddMenu() {
    IMMDeviceEnumerator* en = Enumerator();
    if (!en) return;
    IMMDeviceCollection* coll = nullptr;
    std::vector<std::pair<std::wstring, std::wstring>> items;
    HMENU m = CreatePopupMenu();
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
        UINT cnt = 0; coll->GetCount(&cnt);
        for (UINT i = 0; i < cnt; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(coll->Item(i, &d))) continue;
            std::wstring id = DevId(d), name = DevName(d);
            d->Release();
            if (id == g_defId) continue;              // would feed back into the captured device
            bool used = false;
            { std::lock_guard<std::mutex> l(g_mtx); for (auto& o : g_outs) if (o->id == id) used = true; }
            if (used) continue;
            items.push_back({id, name});
            AppendMenu(m, MF_STRING, 1000 + items.size() - 1, name.c_str());
        }
        coll->Release();
    }
    en->Release();
    if (items.empty()) AppendMenu(m, MF_STRING | MF_GRAYED, 0, L"No other output devices");
    RECT rc; GetWindowRect(g_plus, &rc);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rc.left, rc.bottom, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && (size_t)(cmd - 1000) < items.size())
        AddOutput(items[cmd - 1000].first, items[cmd - 1000].second);
}

static Output* FindBy(HWND h, int kind) {            // 0 = remove button, 1 = volume slider, 2 = delay slider
    std::lock_guard<std::mutex> l(g_mtx);
    for (auto& o : g_outs) {
        HWND x = kind == 0 ? o->btn : kind == 1 ? o->slider : o->dSlider;
        if (x == h) return o.get();
    }
    return nullptr;
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if ((HWND)lp == g_plus && HIWORD(wp) == BN_CLICKED) ShowAddMenu();
        else if (Output* o = FindBy((HWND)lp, 0)) RemoveOutput(o);
        return 0;
    case WM_HSCROLL:
        if (Output* o = FindBy((HWND)lp, 1)) {
            float g = SendMessage(o->slider, TBM_GETPOS, 0, 0) / 100.0f;
            o->vol = g * g;                           // squared: closer to perceived loudness
        } else if (Output* d = FindBy((HWND)lp, 2)) {
            int ms = (int)SendMessage(d->dSlider, TBM_GETPOS, 0, 0);
            d->delayMs = ms;
            std::wstring t = L"Delay " + std::to_wstring(ms) + L" ms";
            SetWindowText(d->dText, t.c_str());
        }
        return 0;
    case WM_TIMER: {
        std::wstring c = L"MultiOut cap " + std::to_wstring(g_capFrames / 1000) + L"k sil " +
                         std::to_wstring(g_capSilent / 1000) + L"k " + std::to_wstring(g_ch) + L"ch " +
                         std::to_wstring(g_rate / 1000) + L"k";
        SetWindowText(h, c.c_str());
        std::lock_guard<std::mutex> l(g_mtx);
        for (auto& o : g_outs) {
            std::wstring t = o->name + L" [out " + std::to_wstring(o->written / 1000) + L"k]";
            SetWindowText(o->label, t.c_str());
        }
        return 0;
    }
    case WM_OUT_FAILED: {
        Output* o = (Output*)wp;
        bool exists = false;
        { std::lock_guard<std::mutex> l(g_mtx); for (auto& x : g_outs) if (x.get() == o) exists = true; }
        if (exists) {
            std::wstring m = L"Could not open: " + o->name;
            RemoveOutput(o);
            MessageBox(h, m.c_str(), L"MultiOut", MB_ICONWARNING);
        }
        return 0;
    }
    case WM_DESTROY: {
        g_run = false;
        std::vector<std::unique_ptr<Output>> outs;
        { std::lock_guard<std::mutex> l(g_mtx); outs.swap(g_outs); }
        for (auto& o : outs) { o->run = false; if (o->th.joinable()) o->th.join(); }
        if (g_capThread.joinable()) g_capThread.join();
        if (g_capClient) g_capClient->Stop();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);                               // accurate Sleep(5)
    INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_BAR_CLASSES};
    InitCommonControlsEx(&ic);
    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (!InitCapture()) {
        MessageBox(nullptr,
                   L"Could not start loopback capture of the default output device\n"
                   L"(a 32-bit float mix format is required).",
                   L"MultiOut", MB_ICONERROR);
        return 1;
    }

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"MultiOutWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClass(&wc);
    g_hwnd = CreateWindowEx(0, wc.lpszClassName, L"MultiOut",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, WIDTH, 120, nullptr, nullptr, hi, nullptr);
    g_plus = MakeChild(L"BUTTON", L"+", BS_PUSHBUTTON, g_hwnd);
    SetWindowPos(g_plus, nullptr, 10, 10, 40, 28, SWP_NOZORDER);
    std::wstring t = L"Mirroring: " + g_defName;
    g_title = MakeChild(L"STATIC", t.c_str(), SS_ENDELLIPSIS, g_hwnd);
    SetWindowPos(g_title, nullptr, 60, 17, 310, 16, SWP_NOZORDER);
    Layout();
    g_capThread = std::thread(CaptureLoop);
    SetTimer(g_hwnd, 1, 500, nullptr);
    ShowWindow(g_hwnd, show);

    MSG m;
    while (GetMessage(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
