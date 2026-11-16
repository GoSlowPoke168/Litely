#define NOMINMAX
#include <windows.h>
#include "resource.h"

#include <algorithm>
#include <avrt.h>
#pragma comment(lib,"avrt.lib")
#include <cmath>
#include <gdiplus.h>
#include <memory>
#include <chrono>
#include <commdlg.h>
#include <commctrl.h>
#pragma comment(lib,"comctl32.lib")
#include <d3d10_1.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <deque>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mutex>
#include <mfmediaengine.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <string>
#include <thread>
#include <vector>

#include <windowsx.h>
#include <wrl.h>
#include <wrl/implements.h>
#include <wtsapi32.h>
using Microsoft::WRL::ComPtr;
using namespace Microsoft::WRL;
namespace fs = std::filesystem;
constexpr UINT Tray = WM_APP + 1, Media = WM_APP + 2, Visibility = WM_APP + 3,
               PosterApplied = WM_APP + 4;
constexpr UINT_PTR Draw = 1, Inspect = 2, Unload = 3;
HWND controller;
HINSTANCE instance;
NOTIFYICONDATAW icon{sizeof(icon)};
UINT taskbarRestart;
std::vector<HWINEVENTHOOK> hooks;
HPOWERNOTIFY powerHook;
bool locked = false, screenOff = false, paused = false, covered = false,
     ready = false, freezePending = false;
constexpr UINT SmoothResume = 4, MaximumSavings = 5;
bool maximumSavings = false, batteryRule = false, batteryOverride = false;
HPOWERNOTIFY acHook, saverHook;
void updateBattery();
void updateBatteryCopy();
void finishBatteryCopy();
void stopBatteryCopy(bool cancelled);
extern bool batteryFps;
extern HANDLE batteryCopyProcess;
bool posterPending = false, presented = false, parked = false;
bool mediaStarted = false, applyingPause = false, resumeAfterPause = false;
std::thread pauseWorker;
bool inspectQueued = false, probe = false, layoutDirty = true;
unsigned generation = 0, frames = 0;
double position = 0;
fs::path source, folder, snapshot;
std::wofstream report;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IMFDXGIDeviceManager> manager;
ComPtr<IMFMediaEngine> engine;
ComPtr<IDXGISwapChain1> chain;
ComPtr<IDCompositionDevice> compositor;
UINT width = 0, height = 0, cadence = 16;
double frameRate = 60;
struct Display {
  RECT rect, work;
  HWND window{};
  bool visible = true;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual> visual;
};
std::vector<Display> displays;
void check(HRESULT h) {
  if (FAILED(h))
    throw h;
}
void status(const wchar_t *msg) {
  if (!wcscmp(icon.szTip, msg))
    return;
  SetWindowText(controller, msg);
  wcscpy_s(icon.szTip, msg);
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}
void saveState() {
  if (probe)
    return;
  auto file = (folder / L"settings.ini").wstring();
  WritePrivateProfileStringW(L"wallpaper", L"file", source.c_str(),
                             file.c_str());
  WritePrivateProfileStringW(L"wallpaper", L"paused", paused ? L"1" : L"0",
                             file.c_str());
  WritePrivateProfileStringW(L"wallpaper", L"time",
                             std::to_wstring(position).c_str(), file.c_str());
}
HANDLE ticks;
ComPtr<IDXGIAdapter3> probeAdapter;
double cpuSeconds() {
  FILETIME a, b, k, u;
  GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
  ULARGE_INTEGER x{}, y{};
  x.LowPart = k.dwLowDateTime;
  x.HighPart = k.dwHighDateTime;
  y.LowPart = u.dwLowDateTime;
  y.HighPart = u.dwHighDateTime;
  return (x.QuadPart + y.QuadPart) / 1e7;
}
void memory(const wchar_t *label) {
  if (probeAdapter) {
    for (auto segment : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                         DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (SUCCEEDED(probeAdapter->QueryVideoMemoryInfo(0, segment, &info)))
        report << label
               << (segment == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? L" local"
                                                              : L" nonlocal")
               << L" GPU allocation MiB " << info.CurrentUsage / 1048576.0
               << std::endl;
    }
  }
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m,
                           sizeof(m)))
    report << label << L" working set MiB " << m.WorkingSetSize / 1048576.0
           << L" private commit MiB " << m.PrivateUsage / 1048576.0
           << std::endl;
}
using Clock = std::chrono::steady_clock;
HANDLE surfaceHandle;
fs::path leanFailedFor;
std::wstring playbackPath;
#include "lean_player.h"
void closePlayer() {
  if (ticks)
    CancelWaitableTimer(ticks);
  KillTimer(controller, Draw);
  KillTimer(controller, Unload);
  ++generation;
  if (engine) {
    // position tracks the frame submitted to the compositor.
    engine->Shutdown();
  }
  leanClose();
  ready = presented = parked = false;
  engine.Reset();
  for (auto &d : displays) {
    d.visual.Reset();
    d.target.Reset();
    if (d.window)
      DestroyWindow(d.window);
    d.window = nullptr;
  }
  chain.Reset();
  if (surfaceHandle)
    CloseHandle(surfaceHandle);
  surfaceHandle = nullptr;
  compositor.Reset();
  manager.Reset();
  context.Reset();
  device.Reset();
}
class Events final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaEngineNotify> {
public:
  unsigned id = generation;
  HRESULT STDMETHODCALLTYPE EventNotify(DWORD e, DWORD_PTR a, DWORD) override {
    if (e == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE)
      SetEvent(reinterpret_cast<HANDLE>(a));
    if (e == MF_MEDIA_ENGINE_EVENT_LOADEDDATA ||
        e == MF_MEDIA_ENGINE_EVENT_ERROR)
      PostMessage(controller, Media, id, e);
    return S_OK;
  }
};
fs::path playFile() { return source; }
bool playerOpen() { return engine || leanActive; }
HRESULT playerPlay() {
  if (!leanActive)
    return engine->Play();
  leanPlay();
  return S_OK;
}
HRESULT playerPause() {
  if (!leanActive)
    return engine->Pause();
  leanPause();
  return S_OK;
}
bool playerPaused() { return leanActive ? leanPaused() : engine->IsPaused() != FALSE; }
double playerTime() { return leanActive ? position : engine->GetCurrentTime(); }
double playerDuration() { return leanActive ? leanDuration : engine->GetDuration(); }
void playerSeek(double seconds) {
  if (leanActive)
    leanRestart(playFile(), seconds);
  else
    check(engine->SetCurrentTime(seconds));
}
void createDevice() {
  if (!mediaStarted) {
    check(MFStartup(MF_VERSION));
    mediaStarted = true;
  }
  UINT flags =
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
  check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                          nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  ComPtr<ID3D10Multithread> multi;
  check(context.As(&multi));
  multi->SetMultithreadProtected(TRUE);
  UINT token;
  check(MFCreateDXGIDeviceManager(&token, &manager));
  check(manager->ResetDevice(device.Get(), token));
}
void openPlayer() {
  if (playerOpen() || source.empty())
    return;
  createDevice();
  wchar_t forced[16]{};
  GetEnvironmentVariableW(L"LITELY_PIPELINE", forced, 16);
  if (wcscmp(forced, L"legacy") && leanFailedFor != source) {
    leanOpen(playFile(), position);
    return;
  }
  ComPtr<IMFAttributes> attr;
  check(MFCreateAttributes(&attr, 4));
  auto events = Make<Events>();
  check(attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, events.Get()));
  check(attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager.Get()));
  check(attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                        DXGI_FORMAT_B8G8R8A8_UNORM));
  ComPtr<IMFMediaEngineClassFactory> factory;
  check(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  check(factory->CreateInstance(0, attr.Get(), &engine));
  check(engine->SetMuted(TRUE));
  check(engine->SetLoop(TRUE));
  DWORD chars = 32768;
  wchar_t url[32768];
  check(UrlCreateFromPathW(playFile().c_str(), url, &chars, 0));
  BSTR b = SysAllocString(url);
  HRESULT result = engine->SetSource(b);
  SysFreeString(b);
  check(result);
  check(engine->Load());
}
BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM) {
  MONITORINFO info{sizeof(info)};
  if (GetMonitorInfo(monitor, &info)) {
    Display d{};
    d.rect = info.rcMonitor;
    d.work = info.rcWork;
    displays.push_back(d);
  }
  return TRUE;
}
HWND iconLayer;
BOOL CALLBACK findWorker(HWND w, LPARAM out) {
  if (FindWindowEx(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
    HWND next = FindWindowEx(nullptr, w, L"WorkerW", nullptr);
    if (next) {
      *reinterpret_cast<HWND *>(out) = next;
      return FALSE;
    }
  }
  return TRUE;
}
HWND desktop() {
  HWND prog = FindWindow(L"Progman", nullptr);
  if (!prog)
    throw E_HANDLE;
  HWND worker = nullptr;
  for (int attempt = 0; attempt < 2; ++attempt) {
    iconLayer = FindWindowEx(prog, nullptr, L"SHELLDLL_DefView", nullptr);
    if (iconLayer &&
        (GetWindowLongPtr(prog, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) &&
        FindWindowEx(prog, nullptr, L"WorkerW", nullptr))
      return prog;
    iconLayer = nullptr;
    EnumWindows(findWorker, reinterpret_cast<LPARAM>(&worker));
    if (worker || attempt)
      break;
    DWORD_PTR result;
    SendMessageTimeout(prog, 0x052C, 0xD, 1, SMTO_ABORTIFHUNG, 1000, &result);
  }
  if (!worker)
    throw E_HANDLE;
  return worker;
}
void createOutput(UINT videoWidth, UINT videoHeight, bool nv12) {
  width = videoWidth;
  height = videoHeight;
  if (!width || !height)
    throw E_FAIL;
  UINT maxW = 0, maxH = 0;
  for (auto &d : displays) {
    maxW = std::max(maxW, UINT(d.rect.right - d.rect.left));
    maxH = std::max(maxH, UINT(d.rect.bottom - d.rect.top));
  }
  double scale = nv12 ? 1 :
      std::min(1.0, std::max(double(maxW) / width, double(maxH) / height));
  width = std::max(1u, UINT(std::ceil(width * scale)));
  height = std::max(1u, UINT(std::ceil(height * scale)));
  ComPtr<IDXGIDevice> dx;
  check(device.As(&dx));
  ComPtr<IDXGIAdapter> adapter;
  check(dx->GetAdapter(&adapter));
  if (probe)
    adapter.As(&probeAdapter);
  ComPtr<IDXGIFactory2> f;
  check(adapter->GetParent(IID_PPV_ARGS(&f)));
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  desc.Scaling = DXGI_SCALING_STRETCH;
  check(DCompositionCreateDevice(dx.Get(), IID_PPV_ARGS(&compositor)));
  ComPtr<IUnknown> content;
  if (nv12) {
    desc.Format = DXGI_FORMAT_NV12;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO | DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO;
    ComPtr<IDXGIFactoryMedia> media;
    check(f.As(&media));
    check(DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surfaceHandle));
    check(media->CreateSwapChainForCompositionSurfaceHandle(device.Get(), surfaceHandle, &desc, nullptr, &chain));
    ComPtr<IDXGISwapChain3> colour;
    check(chain.As(&colour));
    check(colour->SetColorSpace1(leanColorSpace));
    check(compositor->CreateSurfaceFromHandle(surfaceHandle, &content));
  } else {
    check(f->CreateSwapChainForComposition(device.Get(), &desc, nullptr, &chain));
    content = chain;
  }
  // Probe only: a 1:1 topmost window whose screen pixels can be compared with the still.
  bool topmost = probe && GetEnvironmentVariableW(L"LITELY_PROBE_TOPMOST", nullptr, 0);
  HWND parent = topmost ? nullptr : desktop();
  for (auto &d : displays) {
    int w = d.rect.right - d.rect.left, h = d.rect.bottom - d.rect.top;
    POINT p{d.rect.left, d.rect.top};
    if (parent)
      ScreenToClient(parent, &p);
    DWORD ex = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    if (topmost) {
      w = width;
      h = height;
      p = {40, 40};
      ex |= WS_EX_TOPMOST;
    }
    if (parent && iconLayer)
      ex |= WS_EX_LAYERED;
    d.window = CreateWindowEx(ex, L"LitelySurface", L"Litely",
                              parent ? WS_CHILD : WS_POPUP, p.x, p.y, w, h,
                              parent, nullptr, instance, nullptr);
    if (!d.window) {
      DWORD error = GetLastError();
      if (probe)
        report << L"CreateWindow error " << error << L" parent " << parent
               << L" icon layer " << iconLayer << std::endl;
      throw HRESULT_FROM_WIN32(error);
    }
    if (ex & WS_EX_LAYERED)
      SetLayeredWindowAttributes(d.window, 0, 255, LWA_ALPHA);
    check(compositor->CreateTargetForHwnd(d.window, TRUE, &d.target));
    check(compositor->CreateVisual(&d.visual));
    D2D_MATRIX_3X2_F m{};
    float k = std::max(float(w) / width, float(h) / height);
    m._11 = m._22 = k;
    m._31 = (w - width * k) / 2;
    m._32 = (h - height * k) / 2;
    check(d.visual->SetTransform(m));
    check(d.visual->SetContent(content.Get()));
    check(d.target->SetRoot(d.visual.Get()));
    if (!topmost)
      SetWindowPos(d.window, iconLayer ? iconLayer : HWND_BOTTOM, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  check(compositor->Commit());
}
void setupOutput() {
  DWORD nativeW, nativeH;
  check(engine->GetNativeVideoSize(&nativeW, &nativeH));
  createOutput(nativeW, nativeH, false);
  // Use source cadence where available. Poll only while the wallpaper is
  // visible.
  cadence = 16;
  frameRate = 60;
  ComPtr<IMFMediaEngineEx> ex;
  if (SUCCEEDED(engine.As(&ex))) {
    DWORD count = 0;
    ex->GetNumberOfStreams(&count);
    for (DWORD i = 0; i < count; i++) {
      PROPVARIANT v{};
      if (SUCCEEDED(ex->GetStreamAttribute(i, MF_MT_FRAME_RATE, &v)) &&
          v.vt == VT_UI8 && v.uhVal.HighPart && v.uhVal.LowPart) {
        frameRate = double(v.uhVal.HighPart) / v.uhVal.LowPart;
        cadence = std::clamp(UINT(1000.0 * v.uhVal.LowPart / v.uhVal.HighPart),
                             1u, 1000u);
        PropVariantClear(&v);
        break;
      }
      PropVariantClear(&v);
    }
  }
}
fs::path posterPath(const fs::path &video) {
  auto name = fs::weakly_canonical(video).wstring();
  CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
  unsigned long long hash = 14695981039346656037ull;
  for (wchar_t c : name) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return folder / (L"wallpaper-" + std::to_wstring(hash) + L".bmp");
}
// Explorer records history after SetWallpaper returns, so the main loop
// watches this key and keeps Litely's stills out of Recent images.
HKEY historyKey;
HANDLE historyChanged;
void forgetPosterHistory() {
  HKEY key = historyKey;
  if (!key)
    return;
  // Re-arm first so Explorer writes that land during this pass trigger another.
  RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                          historyChanged, TRUE);
  auto prefix = folder.wstring() + L"\\";
  std::vector<std::wstring> kept;
  int slots = 0;
  bool dropped = false;
  for (int i = 0; i < 16; i++) {
    wchar_t value[MAX_PATH * 4]{};
    DWORD size = sizeof value - sizeof(wchar_t), type = 0;
    auto name = L"BackgroundHistoryPath" + std::to_wstring(i);
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, (BYTE *)value, &size) ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
      continue;
    slots = i + 1;
    if (!_wcsnicmp(value, prefix.c_str(), prefix.size()))
      dropped = true;
    else
      kept.push_back(value);
  }
  for (int i = 0; dropped && i < slots; i++) {
    auto name = L"BackgroundHistoryPath" + std::to_wstring(i);
    if (i < (int)kept.size())
      RegSetValueExW(key, name.c_str(), 0, REG_SZ, (const BYTE *)kept[i].c_str(),
                     DWORD((kept[i].size() + 1) * sizeof(wchar_t)));
    else
      RegDeleteValueW(key, name.c_str());
  }
}
void applyPoster(bool changed, const fs::path &path) {
  if (probe || GetEnvironmentVariableW(
                   L"LITELY_TEST_SKIP_PERSONALIZATION", nullptr, 0))
    return;
  // COM can't call Explorer while a sender waits on us (RPC_E_CANTCALLOUT_ININPUTSYNCCALL).
  if (InSendMessage())
    ReplyMessage(0);
  ComPtr<IDesktopWallpaper> wallpaper;
  check(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&wallpaper)));
  PWSTR current = nullptr;
  HRESULT hr = wallpaper->GetWallpaper(nullptr, &current);
  bool same = hr == S_OK && current && !_wcsicmp(current, path.c_str());
  CoTaskMemFree(current);
  if (changed || !same)
    check(wallpaper->SetWallpaper(nullptr, path.c_str()));
  DESKTOP_WALLPAPER_POSITION style;
  if (SUCCEEDED(wallpaper->GetPosition(&style)) && style != DWPOS_FILL)
    check(wallpaper->SetPosition(DWPOS_FILL));
}
void preparePoster(bool applyCached) {
  snapshot = posterPath(source);
  posterPending = !fs::exists(snapshot) ||
                  fs::last_write_time(snapshot) < fs::last_write_time(source);
  if (!posterPending && applyCached)
    applyPoster(false, snapshot);
}
void capture(ID3D11Texture2D *frame);
bool checkFrontBuffer = false;
Clock::time_point probeResume;
double millisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}
ULONGLONG seekStarted = 0;
bool drawEngine(LONGLONG &pts, ComPtr<ID3D11Texture2D> &texture) {
  if (engine->OnVideoStreamTick(&pts) != S_OK)
    return false;
  // A successful tick can still belong to the beginning of the clip while the
  // seek is pending.
  if (!presented && position > 0 && pts / 1e7 + 0.0001 < position) {
    const bool wrapped =
        position + 2 * cadence / 1000.0 >= engine->GetDuration() &&
        GetTickCount64() - seekStarted > 200 && !engine->IsSeeking() &&
        engine->GetCurrentTime() < 0.5;
    if (!wrapped)
      return false;
  }
  check(chain->GetBuffer(0, IID_PPV_ARGS(&texture)));
  RECT dest{0, 0, LONG(width), LONG(height)};
  MFARGB black{};
  check(engine->TransferVideoFrame(texture.Get(), nullptr, &dest, &black));
  return true;
}
void draw() {
  LONGLONG pts;
  ComPtr<ID3D11Texture2D> texture;
  if (leanActive) {
    bool acquired = leanAcquire(&pts);
    leanSchedule();
    if (!acquired)
      return;
    if (posterPending || checkFrontBuffer)
      texture = leanStill();
  } else if (!drawEngine(pts, texture))
    return;
  if (posterPending) {
    capture(texture.Get());
    if (probe) {
      auto first = snapshot;
      CopyFileW(snapshot.c_str(), first.replace_extension(L".first.bmp").c_str(), FALSE);
    }
    applyPoster(true, snapshot);
    posterPending = false;
  }
  if (checkFrontBuffer)
    capture(texture.Get());
  if (leanActive) {
    texture.Reset();
    leanDropStill();
  }
  check(chain->Present(0, 0));
  if (!presented && probe)
    report << L"resume offset (ms): " << (pts / 1e7 - position) * 1000
           << std::endl;
  position = pts / 1e7;
  if (!presented) {
    presented = true;
    for (auto &d : displays)
      if (d.visible)
        ShowWindow(d.window, SW_SHOWNOACTIVATE);
  }
  if (probe && probeResume.time_since_epoch().count()) {
    report << L"resume to first frame ms " << millisecondsSince(probeResume)
           << std::endl;
    probeResume = {};
  }
  ++frames;
}
void capture(ID3D11Texture2D *frame) {
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> staging;
  desc.BindFlags = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  check(device->CreateTexture2D(&desc, nullptr, &staging));
  context->CopyResource(staging.Get(), frame);
  D3D11_MAPPED_SUBRESOURCE map{};
  check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = width;
  ih.biHeight = -LONG(height);
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  BITMAPFILEHEADER header{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(ih);
  header.bfSize = header.bfOffBits + width * height * 4;
  auto tmp = snapshot;
  tmp += L".tmp";
  std::ofstream out(tmp, std::ios::binary);
  out.write((char *)&header, sizeof(header));
  out.write((char *)&ih, sizeof(ih));
  for (UINT i = 0; i < height; i++)
    out.write((char *)map.pData + i * map.RowPitch, width * 4);
  out.close();
  context->Unmap(staging.Get(), 0);
  if (!out) {
    DeleteFileW(tmp.c_str());
    throw HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
  }
  // Explorer and thumbnailers briefly hold the current still open.
  for (int attempt = 0;; attempt++) {
    if (MoveFileEx(tmp.c_str(), snapshot.c_str(), MOVEFILE_REPLACE_EXISTING))
      return;
    DWORD error = GetLastError();
    if (attempt == 20 || (error != ERROR_SHARING_VIOLATION &&
                          error != ERROR_ACCESS_DENIED &&
                          error != ERROR_LOCK_VIOLATION)) {
      DeleteFileW(tmp.c_str());
      throw HRESULT_FROM_WIN32(error);
    }
    Sleep(25);
  }
}
void parkPlayer() {
  if (maximumSavings || batteryOverride || locked || screenOff)
    closePlayer();
  else
    parked = ready;
}
void finishPause() {
  parkPlayer();
  saveState();
  status(L"Litely - paused");
}
void startClock();
void freeze(bool manual = true) {
  if (applyingPause)return;
  KillTimer(controller, Unload);
  if (manual && covered && !playerOpen() && !posterPending && fs::exists(snapshot)) {
    paused = true;
    finishPause();
    return;
  }
  if (!presented) {
    freezePending = true;
    if (!ready)
      openPlayer();
    else {
      check(playerPlay());
      startClock();
    }
    return;
  }
  check(playerPause());
  CancelWaitableTimer(ticks);
  ComPtr<ID3D11Texture2D> lastFrame;
  if (leanActive)
    lastFrame = leanStill();
  else
    check(chain->GetBuffer(1, IID_PPV_ARGS(&lastFrame)));
  capture(lastFrame.Get());
  lastFrame.Reset();
  leanDropStill();
  posterPending = freezePending = false;
  paused = manual;
  if (probe) {
    finishPause();
    return;
  }
  if (manual)
    status(L"Litely - paused");
  applyingPause = true;
  pauseWorker = std::thread([path = snapshot, window = controller] {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(result)) {
      try {
        applyPoster(true, path);
      } catch (HRESULT error) {
        result = error;
      } catch (...) {
        result = E_FAIL;
      }
      CoUninitialize();
    }
    PostMessage(window, PosterApplied, static_cast<WPARAM>(result), 0);
  });
}
struct Coverage {
  HRGN region;
};
BOOL CALLBACK subtractWindow(HWND w, LPARAM arg) {
  if (!IsWindowVisible(w) || IsIconic(w) || w == controller)
    return TRUE;
  DWORD pid;
  GetWindowThreadProcessId(w, &pid);
  if (pid == GetCurrentProcessId())
    return TRUE;
  wchar_t cls[128];
  GetClassName(w, cls, 128);
  if ((!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) &&
      FindWindowEx(w, nullptr, L"SHELLDLL_DefView", nullptr))
    return FALSE;
  if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
      !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
    return TRUE;
  DWORD cloak = 0;
  DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloak, sizeof cloak);
  if (cloak)
    return TRUE;
  auto style = GetWindowLongPtr(w, GWL_EXSTYLE);
  if (style & WS_EX_TRANSPARENT)
    return TRUE;
  if (style & WS_EX_LAYERED) {
    BYTE alpha{};
    DWORD flags{};
    if (!GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) ||
        flags != LWA_ALPHA || alpha != 255)
      return TRUE;
  }
  RECT r;
  if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r,
                                   sizeof r)) &&
      !GetWindowRect(w, &r))
    return TRUE;
  HRGN cut = CreateRectRgn(0, 0, 0, 0);
  if (!cut)
    return TRUE;
  if (GetWindowRgn(w, cut) == ERROR)
    SetRectRgn(cut, r.left, r.top, r.right, r.bottom);
  else {
    RECT wr;
    GetWindowRect(w, &wr);
    OffsetRgn(cut, wr.left, wr.top);
  }
  HRGN region = reinterpret_cast<Coverage *>(arg)->region;
  int result = CombineRgn(region, region, cut, RGN_DIFF);
  DeleteObject(cut);
  return result != NULLREGION;
}

void startClock() {
  if (leanActive) {
    leanSchedule();
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -10000;
  if (!SetWaitableTimer(ticks, &due, cadence, nullptr, nullptr, FALSE))
    throw E_FAIL;
}
void resumePlayer() {
  check(playerPlay());
  startClock();
  parked = false;
}
bool testVisible = GetEnvironmentVariableW(L"LITELY_TEST_VISIBLE", nullptr, 0) != 0;
void visibility() {
  if (probe)
    return;
  if (applyingPause)
    return;
  if (layoutDirty) {
    closePlayer();
    displays.clear();
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, 0);
    layoutDirty = false;
  }
  if (parked && (locked || screenOff))
    closePlayer();
  if (batteryOverride && !posterPending) {
    if (parked)
      closePlayer();
    return;
  }
  if (paused || source.empty())
    return;
  bool any = false;
  for (auto &d : displays) {
    Coverage c{CreateRectRgnIndirect(&d.work)};
    if (!locked && !screenOff && c.region && !testVisible)
      EnumWindows(subtractWindow, reinterpret_cast<LPARAM>(&c));
    RECT box{};
    bool wasVisible = d.visible;
    d.visible = !locked && !screenOff &&
                (!c.region || GetRgnBox(c.region, &box) != NULLREGION);
    if (c.region)
      DeleteObject(c.region);
    any |= d.visible;
    if (d.window && presented && wasVisible != d.visible &&
        (displays.size() > 1 || d.visible))
      ShowWindow(d.window, d.visible ? SW_SHOWNOACTIVATE : SW_HIDE);
  }
  if (!any) {
    if (posterPending && !playerOpen())
      openPlayer();
    if (!covered) {
      covered = true;
      if (playerOpen() && !posterPending && !parked) {
        playerPause();
        CancelWaitableTimer(ticks);
        SetTimer(controller, Unload, 2000, nullptr);
      }
    }
    status(L"Litely - desktop covered");
    return;
  }
  KillTimer(controller, Unload);
  bool resume = covered || parked;
  covered = false;
  if (!playerOpen())
    openPlayer();
  else if (resume && ready)
    resumePlayer();
  status(L"Litely - playing");
}
void CALLBACK windowEvent(HWINEVENTHOOK, DWORD event, HWND w, LONG object,
                          LONG child, DWORD, DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || !w ||
      (event != EVENT_OBJECT_DESTROY && GetAncestor(w, GA_ROOT) != w) ||
      paused || inspectQueued)
    return;
  inspectQueued = true;
  PostMessage(controller, Visibility, 0, 0);
}
void updateHooks() {
  for (auto h : hooks)
    UnhookWinEvent(h);
  hooks.clear();
  if (paused)
    return;
  for (auto e :
       {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE, EVENT_OBJECT_DESTROY, EVENT_OBJECT_LOCATIONCHANGE,
        EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}) {
    auto hook =
        SetWinEventHook(e, e, nullptr, windowEvent, 0, 0,
                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hook)
      hooks.push_back(hook);
  }
}
void toggle() {
  if (batteryOverride)
    return;
  if (applyingPause) {
    if (!paused) {
      paused = true;
      resumeAfterPause = false;
    } else
      resumeAfterPause = !resumeAfterPause;
    return;
  }
  if (source.empty())
    return;
  if (freezePending) {
    freezePending = false;
    visibility();
    return;
  }
  if (paused) {
    paused = covered = false;
    if (!ready)
      preparePoster(true);
    if (hooks.empty())
      updateHooks();
    saveState();
    visibility();
  } else
    freeze();
}
void activateSource(const fs::path &path) {
  closePlayer();
  source = path;
  preparePoster(true);
  position = 0;
  paused = covered = freezePending = false;
  updateHooks();
  saveState();
  updateBatteryCopy();
  visibility();
}
void selectFile() {
  wchar_t path[32768]{};
  OPENFILENAME of{sizeof(of)};
  of.hwndOwner = controller;
  of.lpstrFilter = L"MP4 video\0*.mp4\0";
  of.lpstrFile = path;
  of.nMaxFile = 32768;
  of.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  wchar_t exe[32768];
  GetModuleFileName(nullptr, exe, 32768);
  auto samples = fs::path(exe).parent_path() / L"wallpapers";
  if (fs::is_directory(samples))
    of.lpstrInitialDir = samples.c_str();
  if (!GetOpenFileName(&of))
    return;
  if (!paused && !source.empty() && posterPath(path) == posterPath(source)) {
    preparePoster(true);
    visibility();
    return;
  }
  activateSource(path);
}
#include "library_ui.h"
bool menuOpen = false, menuDismissed = false;
DWORD menuDismissTime = 0;
void menu(const POINT *anchor) {
  if (menuOpen)
    return;
  menuOpen = true;
  HMENU m = CreatePopupMenu();
  AppendMenu(m, MF_STRING, 10, L"Open Litely");
  SetMenuDefaultItem(m, 10, FALSE);
  AppendMenu(m, MF_STRING | (applyingPause ? MF_GRAYED : 0), 1,
             L"Choose MP4...");
  AppendMenu(m, MF_STRING | (source.empty() ? MF_GRAYED : 0), 2,
             paused ? L"Resume" : L"Pause on this frame");
  AppendMenu(m, MF_SEPARATOR, 0, nullptr);
  HMENU modes = CreatePopupMenu();
  AppendMenu(modes, MF_STRING, SmoothResume, L"Smooth resume");
  AppendMenu(modes, MF_STRING, MaximumSavings, L"Maximum savings");
  CheckMenuRadioItem(modes, SmoothResume, MaximumSavings,
                     maximumSavings ? MaximumSavings : SmoothResume,
                     MF_BYCOMMAND);
  AppendMenu(m, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"Performance");
  AppendMenu(m, MF_STRING, 11, L"Settings...");
  AppendMenu(m, MF_STRING, 3, L"Exit Litely");
  POINT p;
  if(anchor)p=*anchor;else GetCursorPos(&p);
  SetForegroundWindow(controller);
  // The native popup runs its own message loop until dismissal.
  SetTimer(controller, Draw, USER_TIMER_MINIMUM, nullptr);
  int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTBUTTON | (anchor ? TPM_BOTTOMALIGN : 0), p.x, p.y, 0,
                          controller, nullptr);
  KillTimer(controller, Draw);
  menuDismissTime = GetTickCount();
  menuDismissed = true;
  menuOpen = false;
  DestroyMenu(m);
  PostMessage(controller, WM_NULL, 0, 0);
  if (id == 10 || id == 11)
    openLibrary(id == 11);
  else if (id == 1)
    selectFile();
  else if (id == 2)
    toggle();
  else if (id == 3)
    DestroyWindow(controller);
  else if (id == SmoothResume || id == MaximumSavings)
    SendMessage(controller, WM_COMMAND, id, 0);
}
void failed(HRESULT h) {
  freezePending = false;
  paused = true;
  CancelWaitableTimer(ticks);
  closePlayer();
  wchar_t msg[256];
  swprintf_s(msg,
             L"Playback stopped (0x%08X).",
             unsigned(h));
  if (probe) {
    report << msg << std::endl;
    PostQuitMessage(1);
  } else {
    status(L"Litely - playback stopped");
    MessageBox(controller, msg, L"Litely", MB_OK | MB_ICONERROR);
  }
}
void playbackTick() {
  if (!ready || paused || (covered && !freezePending && !posterPending))
    return;
  bool preparing = posterPending;
  auto prev = frames;
  draw();
  if (batteryOverride && ready && presented && !applyingPause) freeze(false);
  else if (freezePending && frames != prev)
    freeze();
  else if (preparing && !posterPending && covered) {
    check(playerPause());
    CancelWaitableTimer(ticks);
    SetTimer(controller, Unload, 2000, nullptr);
  }
}
LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM a, LPARAM b) {
  try {
    if (taskbarRestart && msg == taskbarRestart) {
      Shell_NotifyIcon(NIM_ADD, &icon);
      layoutDirty = true;
      visibility();
      return 0;
    }
    switch (msg) {
    case WM_COMMAND:
      if (a == 10 || a == 11) {
        openLibrary(a == 11);
        return 0;
      }
      if (a == 2) {
        toggle();
        return 0;
      }
      if (a == SmoothResume || a == MaximumSavings) {
        bool selected = a == MaximumSavings;
        if (maximumSavings != selected) {
          if (!WritePrivateProfileStringW(L"wallpaper", L"maximumSavings",
                                          selected ? L"1" : L"0",
                                          (folder / L"settings.ini").c_str()))
            throw HRESULT_FROM_WIN32(GetLastError());
          maximumSavings = selected;
          if (parked && maximumSavings && !applyingPause)
            closePlayer();
        }
        return 0;
      }
      break;
    case PosterApplied:
      pauseWorker.join();
      applyingPause = false;
      if (paused)
        finishPause();
      else {
        parkPlayer();
        saveState();
        visibility();
      }
      check(static_cast<HRESULT>(a));
      if (resumeAfterPause) {
        resumeAfterPause = false;
        toggle();
      }
      return 0;
    case Tray: {
      DWORD time = static_cast<DWORD>(GetMessageTime());
      if (menuOpen ||
          (menuDismissed && static_cast<LONG>(time - menuDismissTime) <= 0))
        return 0;
      menuDismissed = false;
      if (b == WM_RBUTTONUP || b == WM_LBUTTONUP)
        menu();
      return 0;
    }
    case Visibility:
      if (!SetTimer(w, Inspect, 80, nullptr))
        inspectQueued = false;
      return 0;
    case WM_DISPLAYCHANGE:
      layoutDirty = true;
      SetTimer(w, Inspect, 100, nullptr);
      return 0;
    case WM_SETTINGCHANGE:
      if (a == SPI_SETWORKAREA)
        layoutDirty = true;
      SetTimer(w, Inspect, 100, nullptr);
      return 0;
    case WM_WTSSESSION_CHANGE:
      if (a == WTS_SESSION_LOCK || a == WTS_SESSION_UNLOCK) {
        locked = a == WTS_SESSION_LOCK;
        visibility();
      }
      return 0;
    case WM_POWERBROADCAST:
      updateBattery();
      if (a == PBT_POWERSETTINGCHANGE) {
        auto p = reinterpret_cast<POWERBROADCAST_SETTING *>(b);
        if (p->PowerSetting == GUID_CONSOLE_DISPLAY_STATE &&
            p->DataLength == 4) {
          screenOff = *(DWORD *)p->Data == 0;
          visibility();
        }
      }
      return TRUE;
    case Media:
      if (a != generation || !playerOpen())
        return 0;
      if (b == LeanFailed) {
        leanFailedFor = source;
        closePlayer();
        openPlayer();
        return 0;
      }
      if (b == LeanReady && ready) {
        if (!leanPaused())
          startClock();
        return 0;
      }
      if (b == MF_MEDIA_ENGINE_EVENT_ERROR) {
        ComPtr<IMFMediaError> e;
        engine->GetError(&e);
        throw(e ? e->GetExtendedErrorCode() : E_FAIL);
      }
      if (!ready) {
        if (leanActive)
          createOutput(leanWidth, leanHeight, true);
        else
          setupOutput();
        ready = true;
        {
          ComPtr<IDXGIDevice> dx;
          ComPtr<IDXGIAdapter> adapter;
          DXGI_ADAPTER_DESC about{};
          if (SUCCEEDED(device.As(&dx)) && SUCCEEDED(dx->GetAdapter(&adapter)))
            adapter->GetDesc(&about);
          playbackPath = (leanActive ? L"Hardware decode, direct · " : L"Compatibility mode · ") + std::wstring(about.Description);
        }
        if (position > 0 && !leanActive) {
          seekStarted = GetTickCount64();
          check(engine->SetCurrentTime(position));
        }
        check(playerPlay());
        startClock();
        if (covered && !freezePending && !posterPending) {
          playerPause();
          CancelWaitableTimer(ticks);
          SetTimer(w, Unload, 2000, nullptr);
        }
      }
      return 0;
    case WM_TIMER:
      if (a == Draw && menuOpen &&
          WaitForSingleObject(ticks, 0) == WAIT_OBJECT_0)
        playbackTick();
      if (a == 4 && probe && menuOpen) {
        KillTimer(w, 4);
        EndMenu();
      }
      if (a == Inspect) {
        KillTimer(w, Inspect);
        inspectQueued = false;
        visibility();
      }
      if (a == Unload) {
        KillTimer(w, Unload);
        if (covered && !freezePending && !posterPending) {
          CancelWaitableTimer(ticks);
          if (presented)
            freeze(false);
          else {
            closePlayer();
            saveState();
          }
        }
      }
      return 0;
    case WM_DESTROY:
      CancelWaitableTimer(ticks);
      closePlayer();
      saveState();
      Shell_NotifyIcon(NIM_DELETE, &icon);
      PostQuitMessage(0);
      return 0;
    }
  } catch (HRESULT h) {
    failed(h);
  } catch (...) {
    failed(E_FAIL);
  }
  return DefWindowProc(w, msg, a, b);
}
int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR, int) {
  instance = app;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int argc;
  auto argv = CommandLineToArgvW(GetCommandLine(), &argc);
  probe = argc == 4 && !wcscmp(argv[1], L"--probe");
  HANDLE singleton = nullptr;
  if (!probe) {
    singleton = CreateMutex(nullptr, FALSE, L"Local\\Litely.Tray");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS)
      return 0;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return 1;

  int result = 0;
  try {
    if (probe) {
      source = argv[2];
      report.open(argv[3]);
      snapshot = fs::path(argv[3]);
      snapshot += L".bmp";
    } else {
      wchar_t base[MAX_PATH];
      if (!GetEnvironmentVariable(L"LOCALAPPDATA", base, MAX_PATH))
        throw E_FAIL;
      folder = fs::path(base) / L"Litely";
      fs::create_directories(folder);
      historyChanged = CreateEvent(nullptr, FALSE, FALSE, nullptr);
      if (historyChanged &&
          RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers",
                        0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_NOTIFY,
                        &historyKey))
        historyKey = nullptr;
      forgetPosterHistory();

      auto file = (folder / L"settings.ini").wstring();
      if (!fs::exists(file)) {
        std::ofstream ini(fs::path(file), std::ios::binary);
        ini.write("\xff\xfe", 2);
      }
      wchar_t path[32768], time[64];
      GetPrivateProfileString(L"wallpaper", L"file", L"", path, 32768,
                              file.c_str());
      GetPrivateProfileString(L"wallpaper", L"time", L"0", time, 64,
                              file.c_str());
      batteryFps = GetPrivateProfileIntW(L"wallpaper", L"batteryFps", 0,
                                         file.c_str()) != 0;
      batteryRule = GetPrivateProfileIntW(L"wallpaper", L"batteryRule", 0,
                                          file.c_str()) != 0;
      maximumSavings = GetPrivateProfileIntW(L"wallpaper", L"maximumSavings", 0,
                                             file.c_str()) != 0;
      position = _wtof(time);
      source = argc == 2 ? fs::path(argv[1]) : fs::path(path);
      paused = argc != 2 &&
               GetPrivateProfileInt(L"wallpaper", L"paused", 0, file.c_str());
      if (argc == 2)
        position = 0;
      if (!source.empty()) {
        snapshot = posterPath(source);
        if (!paused && fs::exists(source))
          preparePoster(true);
      }
    }
    LocalFree(argv);
    WNDCLASS wc{};
    wc.hInstance = app;
    wc.hIcon = LoadIcon(app, MAKEINTRESOURCE(IDI_LITELY));
    wc.lpfnWndProc = DefWindowProc;
    wc.lpszClassName = L"LitelySurface";
    RegisterClass(&wc);
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"LitelyController";
    RegisterClass(&wc);
    controller = CreateWindow(wc.lpszClassName, L"Litely", WS_POPUP, 0, 0, 0, 0,
                              nullptr, nullptr, app, nullptr);
    if (!controller)
      throw E_HANDLE;
    ticks = CreateWaitableTimerEx(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    // Keeps frame presentation on time while Litely is a background process.
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Playback", &task);
    if (!ticks)
      ticks = CreateWaitableTimer(nullptr, FALSE, nullptr);
    if (!ticks)
      throw E_FAIL;
    if (probe) {
      posterPending = true;
      memory(L"idle");
      EnumDisplayMonitors(nullptr, nullptr, collectMonitor, 0);
      openPlayer();
    } else {
      taskbarRestart = RegisterWindowMessage(L"TaskbarCreated");
      icon.hWnd = controller;
      icon.uID = 1;
      icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
      icon.uCallbackMessage = Tray;
      icon.hIcon = static_cast<HICON>(
          LoadImage(app, MAKEINTRESOURCE(IDI_LITELY), IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON),
                    GetSystemMetrics(SM_CYSMICON), LR_SHARED));
      if (!icon.hIcon)
        throw HRESULT_FROM_WIN32(GetLastError());
      wcscpy_s(icon.szTip, L"Litely");
      if (!Shell_NotifyIcon(NIM_ADD, &icon))
        throw E_FAIL;
      updateHooks();
      powerHook = RegisterPowerSettingNotification(
          controller, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
      acHook = RegisterPowerSettingNotification(
          controller, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
      saverHook = RegisterPowerSettingNotification(
          controller, &GUID_POWER_SAVING_STATUS, DEVICE_NOTIFY_WINDOW_HANDLE);
      updateBattery();
      updateBatteryCopy();
      WTSRegisterSessionNotification(controller, NOTIFY_FOR_THIS_SESSION);
      visibility();
      if (paused)
        status(L"Litely - paused");
      if (source.empty())
        selectFile();
    }
    MSG m;
    bool running = true;
    ULONGLONG start = GetTickCount64();
    unsigned phase = 0;
    ULONGLONG sampleStart = 0;
    double initialCPU = 0;
    unsigned firstFrames = 0;
    wchar_t secondsText[16]{};
    GetEnvironmentVariableW(L"LITELY_PROBE_SECONDS", secondsText, 16);
    const ULONGLONG sampleMs = 1000ull * std::max(4, _wtoi(secondsText));
    Clock::time_point pacingStart;
    while (running) {
      HANDLE waits[3] = {ticks};
      DWORD count = 1, historyAt = 0, copyAt = 0;
      if (historyKey)
        waits[historyAt = count++] = historyChanged;
      if (batteryCopyProcess)
        waits[copyAt = count++] = batteryCopyProcess;
      DWORD wait = MsgWaitForMultipleObjectsEx(
          count, waits, probe ? 100 : INFINITE, QS_ALLINPUT,
          MWMO_INPUTAVAILABLE);
      if (wait == WAIT_FAILED)
        throw HRESULT_FROM_WIN32(GetLastError());
      if (historyAt && wait == WAIT_OBJECT_0 + historyAt)
        forgetPosterHistory();
      if (copyAt && wait == WAIT_OBJECT_0 + copyAt)
        finishBatteryCopy();
      if (wait == WAIT_OBJECT_0) {
        try {
          playbackTick();
        } catch (HRESULT h) {
          failed(h);
        }
      }
      while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
          result = (int)m.wParam;
          running = false;
          break;
        }
        if (libraryWindow && IsDialogMessage(libraryWindow, &m))
          continue;
        TranslateMessage(&m);
        DispatchMessage(&m);
      }
      if (probe && running) {
        if (phase == 0 && frames >= 5 && !sampleStart) {
          if (!phase) {
            auto before = frames;
            SetTimer(controller, 4, 650, nullptr);
            PostMessage(controller, Tray, 0, WM_RBUTTONUP);
            PostMessage(controller, Tray, 0, WM_LBUTTONUP);
            menu();
            if (paused || frames - before < 5)
              throw E_FAIL;
            report << L"frames during open tray menu " << frames - before
                   << std::endl;
          }
          sampleStart = GetTickCount64();
          pacingStart = Clock::now();
          initialCPU = cpuSeconds();
          firstFrames = frames;
          report << (leanActive ? L"pipeline lean" : L"pipeline legacy") << std::endl;
          leanLate = 0;
          leanWorstLate = 0;
        }
        if (phase == 0 && sampleStart &&
            GetTickCount64() - sampleStart >= sampleMs) {
          report << L"pacing frames " << frames - firstFrames << L" expected "
                 << std::llround(millisecondsSince(pacingStart) / 1000 * frameRate)
                 << L" late " << leanLate << L" worst " << leanWorstLate << L" ms" << std::endl;
          report << L"sample wall seconds "
                 << (GetTickCount64() - sampleStart) / 1000.0
                 << L" process CPU seconds " << cpuSeconds() - initialCPU
                 << L" frames " << frames - firstFrames << std::endl;
          report << L"frames " << frames << L" position "
                 << playerTime() << L" output " << width << L"x"
                 << height << std::endl;
          if (posterPending || !fs::exists(snapshot))
            throw E_FAIL;
          report << L"automatic backdrop ready before manual pause"
                 << std::endl;
          memory(L"playing");
          checkFrontBuffer = true;
          auto displayed = frames;
          for (int i = 0; i < 100 && frames == displayed; i++) {
            Sleep(2);
            draw();
          }
          checkFrontBuffer = false;
          if (frames == displayed)
            throw E_FAIL;
          auto readPixels = [&]() {
            std::ifstream in(snapshot, std::ios::binary);
            return std::vector<char>(std::istreambuf_iterator<char>(in), {});
          };
          auto reference = readPixels();
          freeze();
          if (reference.empty() || reference != readPixels())
            throw E_FAIL;
          report << L"paused pixels exactly match the last submitted frame"
                 << std::endl;
          if (GetEnvironmentVariableW(L"LITELY_PROBE_TOPMOST", nullptr, 0)) {
            Sleep(400);
            HWND shown = displays[0].window;
            POINT centre{40 + LONG(width) / 2, 40 + LONG(height) / 2};
            if (WindowFromPoint(centre) != shown)
              throw E_ACCESSDENIED;
            HDC screen = GetDC(nullptr), memoryDC = CreateCompatibleDC(screen);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof info.bmiHeader;
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -LONG(height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            void *bits = nullptr;
            HBITMAP grab = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ previous = SelectObject(memoryDC, grab);
            BitBlt(memoryDC, 0, 0, width, height, screen, 40, 40, SRCCOPY);
            GdiFlush();
            BITMAPFILEHEADER header{};
            header.bfType = 0x4d42;
            header.bfOffBits = sizeof header + sizeof info.bmiHeader;
            header.bfSize = header.bfOffBits + width * height * 4;
            auto path = snapshot;
            std::ofstream out(path.replace_extension(L".screen.bmp"), std::ios::binary);
            out.write((char *)&header, sizeof header);
            out.write((char *)&info.bmiHeader, sizeof info.bmiHeader);
            out.write((char *)bits, width * height * 4);
            SelectObject(memoryDC, previous);
            DeleteObject(grab);
            DeleteDC(memoryDC);
            ReleaseDC(nullptr, screen);
          }
          memory(L"paused");
          if (!playerOpen() || !playerPaused() || !parked ||
              !fs::exists(snapshot))
            throw E_FAIL;
          report << L"paused: renderer ready, playback clock stopped"
                 << std::endl;
          firstFrames = frames;
          phase = 1;
          sampleStart = GetTickCount64();
          initialCPU = cpuSeconds();
        }
        if (phase == 1 && GetTickCount64() - sampleStart >= 1500) {
          memory(L"settled pause");
          report << L"pause CPU seconds " << cpuSeconds() - initialCPU
                 << std::endl;
          paused = false;
          if (frames != firstFrames)
            throw E_FAIL;
          probeResume = Clock::now();
          resumePlayer();
          report << L"synchronous resume setup ms "
                 << millisecondsSince(probeResume) << std::endl;
          phase = 2;
        }
        if (phase == 2 && frames >= firstFrames + 30) {
          report << L"resumed at " << playerTime() << std::endl;
          playerSeek(std::max(0.0, playerDuration() - 0.6));
          phase = 3;
        }
        if (phase == 3 &&
            playerTime() > playerDuration() - 0.8)
          phase = 4;
        if (phase == 4 && playerTime() < 0.5) {
          report << L"loop restarted" << std::endl;
          running = false;
        }
        if (GetTickCount64() - start > 26000 + sampleMs)
          throw HRESULT_FROM_WIN32(WAIT_TIMEOUT);
      }
    }
  } catch (HRESULT h) {
    if (probe)
      report << L"failure " << std::hex << (unsigned)h << std::endl;
    else
      failed(h);
    result = 1;
  } catch (...) {
    if (probe)
      report << L"unexpected failure" << std::endl;
    else
      failed(E_FAIL);
    result = 1;
  }
  if (libraryWindow)
    DestroyWindow(libraryWindow);
  if (batteryCopyProcess)
    stopBatteryCopy(true);
  closePlayer();
  for (auto h : hooks)
    UnhookWinEvent(h);
  if (acHook)
    UnregisterPowerSettingNotification(acHook);
  if (saverHook)
    UnregisterPowerSettingNotification(saverHook);
  if (powerHook)
    UnregisterPowerSettingNotification(powerHook);
  if (controller && IsWindow(controller))
    DestroyWindow(controller);
  if (pauseWorker.joinable())
    pauseWorker.join();
  if (ticks)
    CloseHandle(ticks);
  if (historyKey)
    RegCloseKey(historyKey);
  if (historyChanged)
    CloseHandle(historyChanged);
  if (mediaStarted)
    MFShutdown();
  CoUninitialize();
  if (singleton)
    CloseHandle(singleton);
  return result;
}
