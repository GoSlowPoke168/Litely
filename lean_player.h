// Hardware decode on a worker thread; the UI thread presents the decoder's NV12 frames.
constexpr LPARAM LeanReady = 0x10001, LeanFailed = 0x10002;
struct LeanFrame {
  ComPtr<IMFSample> sample;
  LONGLONG pts = 0, time = 0;
};
bool leanActive = false;
UINT leanWidth = 0, leanHeight = 0;
double leanDuration = 0;
DXGI_COLOR_SPACE_TYPE leanColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
HRESULT leanError = S_OK;
unsigned leanLate = 0;
double leanWorstLate = 0;

std::thread leanWorker;
std::mutex leanLock;
std::condition_variable leanSpace;
std::deque<LeanFrame> leanQueue;
bool leanStop = false, leanHeld = false, leanRebase = true;
LeanFrame leanShown;
LONGLONG leanBase = 0, leanFrameTime = 166667;
ComPtr<ID3D11VideoProcessorEnumerator> leanEnumerator;
ComPtr<ID3D11VideoProcessor> leanProcessor;
ComPtr<ID3D11Texture2D> leanStillTexture;

LONGLONG leanNow() {
  return std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>>(
             Clock::now().time_since_epoch())
      .count();
}
void leanSurface(const LeanFrame &frame, ComPtr<ID3D11Texture2D> &texture, UINT &index) {
  ComPtr<IMFMediaBuffer> buffer;
  check(frame.sample->GetBufferByIndex(0, &buffer));
  ComPtr<IMFDXGIBuffer> surface;
  check(buffer.As(&surface));
  check(surface->GetResource(IID_PPV_ARGS(&texture)));
  check(surface->GetSubresourceIndex(&index));
}
void leanDecode(fs::path file, double start, unsigned id, ComPtr<IMFDXGIDeviceManager> devices) {
  HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // Background processes get descheduled long enough to underrun the queue.
  DWORD task = 0;
  HANDLE mm = AvSetMmThreadCharacteristicsW(L"Playback", &task);
  try {
    check(com);
    ComPtr<IMFAttributes> attr;
    check(MFCreateAttributes(&attr, 3));
    check(attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, devices.Get()));
    check(attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    check(attr->SetUINT32(MF_LOW_LATENCY, TRUE));
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(file.c_str(), attr.Get(), &reader));
    const DWORD video = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    check(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE));
    check(reader->SetStreamSelection(video, TRUE));
    ComPtr<IMFMediaType> wanted, type;
    check(MFCreateMediaType(&wanted));
    check(wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    check(wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    check(reader->SetCurrentMediaType(video, nullptr, wanted.Get()));
    check(reader->GetCurrentMediaType(video, &type));
    UINT w = 0, h = 0, num = 0, den = 0;
    check(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h));
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, nullptr)) &&
        area.Area.cx && area.Area.cy) {
      w = area.Area.cx;
      h = area.Area.cy;
    }
    if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) || !num || !den) {
      num = 60;
      den = 1;
    }
    UINT matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, 0);
    bool bt601 = matrix == MFVideoTransferMatrix_BT601 || (matrix != MFVideoTransferMatrix_BT709 && h < 720);
    bool full = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0) == MFNominalRange_0_255;
    PROPVARIANT length{};
    double seconds = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &length)) &&
        length.vt == VT_UI8)
      seconds = length.uhVal.QuadPart / 1e7;
    PropVariantClear(&length);
    const LONGLONG frameTime = 10000000ll * den / num;
    LONGLONG offset = 0, last = 0, from = LONGLONG(start * 1e7), loopEnd = 0;
    bool looped = false;
    if (from > 0) {
      PROPVARIANT to{};
      to.vt = VT_I8;
      to.hVal.QuadPart = from;
      check(reader->SetCurrentPosition(GUID_NULL, to));
    }
    unsigned decoded = 0;
    bool first = true;
    for (;;) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      check(reader->ReadSample(video, 0, nullptr, &flags, &ts, &sample));
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (!decoded)
          throw MF_E_INVALID_STREAM_DATA;
        decoded = 0;
        PROPVARIANT to{};
        to.vt = VT_I8;
        check(reader->SetCurrentPosition(GUID_NULL, to));
        loopEnd = offset + last + frameTime;
        looped = true;
        from = 0;
        continue;
      }
      if (!sample)
        continue;
      ++decoded;
      last = ts;
      // The first timestamp of a clip is not always zero.
      if (looped) {
        offset = loopEnd - ts;
        looped = false;
      }
      if (ts < from)
        continue;
      LeanFrame frame;
      frame.sample = sample;
      frame.pts = ts;
      frame.time = offset + ts;
      if (first) {
        ComPtr<ID3D11Texture2D> texture;
        UINT index;
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFDXGIBuffer> surface;
        D3D11_TEXTURE2D_DESC desc{};
        if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&surface)) ||
            FAILED(surface->GetResource(IID_PPV_ARGS(&texture))) ||
            FAILED(surface->GetSubresourceIndex(&index)))
          throw MF_E_UNSUPPORTED_D3D_TYPE;
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12 || desc.Width < w || desc.Height < h)
          throw MF_E_UNSUPPORTED_D3D_TYPE;
      }
      std::unique_lock<std::mutex> hold(leanLock);
      leanSpace.wait(hold, [] { return leanStop || leanQueue.size() < 3; });
      if (leanStop)
        break;
      if (first) {
        leanWidth = w;
        leanHeight = h;
        frameRate = double(num) / den;
        leanFrameTime = frameTime;
        leanDuration = seconds;
        leanColorSpace = bt601 ? (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                                       : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601)
                               : (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                       : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
      }
      leanQueue.push_back(std::move(frame));
      if (first)
        PostMessage(controller, Media, id, LeanReady);
      first = false;
    }
  } catch (HRESULT error) {
    std::lock_guard<std::mutex> hold(leanLock);
    leanError = error;
    if (!leanStop)
      PostMessage(controller, Media, id, LeanFailed);
  }
  if (mm)
    AvRevertMmThreadCharacteristics(mm);
  if (SUCCEEDED(com))
    CoUninitialize();
}
void leanJoin() {
  {
    std::lock_guard<std::mutex> hold(leanLock);
    leanStop = true;
  }
  leanSpace.notify_all();
  if (leanWorker.joinable())
    leanWorker.join();
  leanQueue.clear();
  leanStop = false;
}
void leanStart(const fs::path &file, double start) {
  leanError = S_OK;
  leanRebase = true;
  leanWorker = std::thread(leanDecode, file, start, generation, manager);
}
void leanOpen(const fs::path &file, double start) {
  leanActive = true;
  leanHeld = false;
  leanLate = 0;
  leanWorstLate = 0;
  leanStart(file, start);
}
// Stills are rare, so the converter and its BGRA frame are not kept.
void leanDropStill() {
  leanProcessor.Reset();
  leanEnumerator.Reset();
  leanStillTexture.Reset();
}
void leanClose() {
  leanJoin();
  leanShown = {};
  leanDropStill();
  leanActive = false;
}
void leanRestart(const fs::path &file, double start) {
  leanJoin();
  ++generation;
  leanStart(file, start);
}
// False while the next frame is not due yet or has not been decoded.
bool leanAcquire(LONGLONG *pts) {
  LeanFrame frame;
  LONGLONG now = leanNow();
  {
    std::lock_guard<std::mutex> hold(leanLock);
    if (leanQueue.empty() || (!leanRebase && now + 5000 < leanBase + leanQueue.front().time))
      return false;
    frame = std::move(leanQueue.front());
    leanQueue.pop_front();
  }
  leanSpace.notify_one();
  if (leanRebase) {
    leanBase = now - frame.time;
    leanRebase = false;
  } else if (LONGLONG lateness = now - leanBase - frame.time; lateness > leanFrameTime) {
    leanBase += lateness;
    ++leanLate;
    leanWorstLate = std::max(leanWorstLate, lateness / 1e4);
  }
  ComPtr<ID3D11Texture2D> texture, back;
  UINT index;
  leanSurface(frame, texture, index);
  check(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
  D3D11_BOX box{0, 0, 0, width, height, 1};
  context->CopySubresourceRegion(back.Get(), 0, 0, 0, 0, texture.Get(), index, &box);
  *pts = frame.pts;
  leanShown = std::move(frame);
  return true;
}
void leanSchedule() {
  LONGLONG wait = 20000;
  {
    std::lock_guard<std::mutex> hold(leanLock);
    if (!leanQueue.empty())
      wait = leanRebase ? 0 : leanBase + leanQueue.front().time - leanNow();
  }
  LARGE_INTEGER due;
  due.QuadPart = wait > 0 ? -wait : -1;
  if (!SetWaitableTimer(ticks, &due, 0, nullptr, nullptr, FALSE))
    throw E_FAIL;
}
void leanPlay() {
  leanHeld = false;
  leanRebase = true;
}
void leanPause() { leanHeld = true; }
bool leanPaused() { return leanHeld; }
ID3D11Texture2D *leanStill() {
  ComPtr<ID3D11VideoDevice> videoDevice;
  check(device.As(&videoDevice));
  ComPtr<ID3D11VideoContext1> video;
  check(context.As(&video));
  if (!leanProcessor) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = content.OutputWidth = width;
    content.InputHeight = content.OutputHeight = height;
    content.InputFrameRate.Numerator = content.OutputFrameRate.Numerator = 60;
    content.InputFrameRate.Denominator = content.OutputFrameRate.Denominator = 1;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check(videoDevice->CreateVideoProcessorEnumerator(&content, &leanEnumerator));
    check(videoDevice->CreateVideoProcessor(leanEnumerator.Get(), 0, &leanProcessor));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check(device->CreateTexture2D(&desc, nullptr, &leanStillTexture));
  }
  ComPtr<ID3D11Texture2D> texture;
  UINT index;
  leanSurface(leanShown, texture, index);
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
  inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  inputDesc.Texture2D.ArraySlice = index;
  ComPtr<ID3D11VideoProcessorInputView> input;
  check(videoDevice->CreateVideoProcessorInputView(texture.Get(), leanEnumerator.Get(), &inputDesc, &input));
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc{};
  outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorOutputView> output;
  check(videoDevice->CreateVideoProcessorOutputView(leanStillTexture.Get(), leanEnumerator.Get(), &outputDesc, &output));
  RECT area{0, 0, LONG(width), LONG(height)};
  auto processor = leanProcessor.Get();
  video->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &area);
  video->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &area);
  video->VideoProcessorSetOutputTargetRect(processor, TRUE, &area);
  video->VideoProcessorSetStreamAutoProcessingMode(processor, 0, FALSE);
  video->VideoProcessorSetStreamColorSpace1(processor, 0, leanColorSpace);
  video->VideoProcessorSetOutputColorSpace1(processor, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.pInputSurface = input.Get();
  check(video->VideoProcessorBlt(processor, output.Get(), 0, 1, &stream));
  return leanStillTexture.Get();
}
