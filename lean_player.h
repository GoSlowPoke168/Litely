// Hardware decode on a worker thread; the UI thread presents the decoder's NV12 frames.
constexpr LPARAM LeanReady = 0x10001, LeanFailed = 0x10002, DirectFailed = 0x10003;
struct LeanFrame {
  ComPtr<IMFSample> sample;
  LONGLONG pts = 0, time = 0;
};
bool leanActive = false;
UINT leanWidth = 0, leanHeight = 0;
double leanDuration = 0;
DXGI_COLOR_SPACE_TYPE leanColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
HRESULT leanError = S_OK;
unsigned leanLate = 0, leanRebased = 0;
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
void leanOutputType(IMFTransform *decoder, ComPtr<IMFMediaType> &type) {
  for (DWORD i = 0;; i++) {
    type.Reset();
    check(decoder->GetOutputAvailableType(0, i, &type));
    GUID subtype{};
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (subtype == MFVideoFormat_NV12)
      break;
  }
  check(decoder->SetOutputType(0, type.Get(), 0));
}
// Windows states 30.00003 fps for every MKV and WebM, so there the span of the first frames' timestamps decides.
// Other containers can split frames across samples, so their stated rate stands.
void readRate(IMFSourceReader *reader, IMFMediaType *native, const fs::path &file, UINT32 &num, UINT32 &den) {
  bool stated = SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den)) && num && den;
  bool matroska = !_wcsicmp(file.extension().c_str(), L".mkv") || !_wcsicmp(file.extension().c_str(), L".webm");
  if (stated && !matroska)
    return;
  if (!stated)
    num = 30, den = 1;
  std::vector<LONGLONG> times;
  for (int i = 0; i < 120; i++) {
    DWORD flags = 0;
    LONGLONG time = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &time, &sample)) ||
        (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
      break;
    if (sample)
      times.push_back(time);
  }
  PROPVARIANT start;
  PropVariantInit(&start);
  start.vt = VT_I8;
  reader->SetCurrentPosition(GUID_NULL, start);
  PropVariantClear(&start);
  std::sort(times.begin(), times.end());
  // Frames still to come in decode order leave gaps among the latest times.
  if (times.size() > 16)
    times.resize(times.size() - 8);
  if (times.size() < 2 || times.back() <= times.front())
    return;
  double fps = 1e7 * (times.size() - 1) / double(times.back() - times.front());
  for (double standard : {24000 / 1001., 30000 / 1001., 60000 / 1001.})
    if (std::abs(fps - standard) < standard * .0003) {
      num = UINT32(standard * 1001 + .5), den = 1001;
      return;
    }
  if (std::abs(fps - std::round(fps)) < fps * .005)
    num = UINT32(std::round(fps)), den = 1;
  else
    num = UINT32(fps * 1000 + .5), den = 1000;
}

// The demuxer loops by seeking; the decoder is never flushed, so the loop has no gap.
void leanDecode(fs::path file, double start, unsigned id, ComPtr<IMFDXGIDeviceManager> devices) {
  HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // Background processes get descheduled long enough to underrun the queue.
  DWORD task = 0;
  HANDLE mm = AvSetMmThreadCharacteristicsW(L"Playback", &task);
  try {
    check(com);
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader));
    const DWORD video = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    check(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE));
    check(reader->SetStreamSelection(video, TRUE));
    ComPtr<IMFMediaType> native, type;
    check(reader->GetNativeMediaType(video, 0, &native));
    UINT w = 0, h = 0, num = 0, den = 0;
    check(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h));
    readRate(reader.Get(), native.Get(), file, num, den);
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, {}};
    check(native->GetGUID(MF_MT_SUBTYPE, &input.guidSubtype));
    IMFActivate **found = nullptr;
    UINT32 count = 0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                    nullptr, &found, &count));
    ComPtr<IMFTransform> decoder;
    HRESULT made = count ? found[0]->ActivateObject(IID_PPV_ARGS(&decoder)) : MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
      found[i]->Release();
    CoTaskMemFree(found);
    check(made);
    check(decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)devices.Get()));
    check(decoder->SetInputType(0, native.Get(), 0));
    leanOutputType(decoder.Get(), type);
    check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    PROPVARIANT length{};
    double seconds = 0;
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &length)) &&
        length.vt == VT_UI8)
      seconds = length.uhVal.QuadPart / 1e7;
    PropVariantClear(&length);
    const LONGLONG frameTime = 10000000ll * den / num;
    LONGLONG offset = 0, last = -1, from = LONGLONG(start * 1e7);
    if (from > 0) {
      PROPVARIANT to{};
      to.vt = VT_I8;
      to.hVal.QuadPart = from;
      check(reader->SetCurrentPosition(GUID_NULL, to));
    }
    unsigned fed = 0, unanswered = 0;
    unsigned wraps = 0;
    bool first = true;
    for (;;) {
      {
        std::lock_guard<std::mutex> hold(leanLock);
        if (leanStop)
          break;
      }
      MFT_OUTPUT_DATA_BUFFER out{};
      DWORD status = 0;
      HRESULT hr = decoder->ProcessOutput(0, 1, &out, &status);
      if (out.pEvents)
        out.pEvents->Release();
      ComPtr<IMFSample> sample;
      sample.Attach(out.pSample);
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        leanOutputType(decoder.Get(), type);
        continue;
      }
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        DWORD flags = 0;
        LONGLONG stamp = 0;
        ComPtr<IMFSample> packet;
        check(reader->ReadSample(video, 0, nullptr, &flags, &stamp, &packet));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          if (!fed)
            throw MF_E_INVALID_STREAM_DATA;
          fed = 0;
          ++wraps;
          PROPVARIANT to{};
          to.vt = VT_I8;
          check(reader->SetCurrentPosition(GUID_NULL, to));
          from = 0;
        } else if (packet) {
          ++fed;
          // A stream the decoder swallows without ever producing a frame.
          if (++unanswered > 600)
            throw MF_E_INVALID_STREAM_DATA;
          check(decoder->ProcessInput(0, packet.Get(), 0));
        }
        continue;
      }
      check(hr);
      if (!sample)
        continue;
      LONGLONG ts = 0;
      check(sample->GetSampleTime(&ts));
      unanswered = 0;
      // Timestamps running backwards mark the loop point; a clip's first one is not always zero.
      if (last >= 0 && (ts < last || (wraps && ts == last))) {
        offset += last + frameTime - ts;
        from = 0;
        if (wraps)
          --wraps;
      } else if (ts < from)
        continue;
      last = ts;
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
        if (desc.Format != DXGI_FORMAT_NV12 || desc.Width < w || desc.Height < h || (w | h) & 1)
          throw MF_E_UNSUPPORTED_D3D_TYPE;
      }
      std::unique_lock<std::mutex> hold(leanLock);
      leanSpace.wait(hold, [] { return leanStop || leanQueue.size() < 3; });
      if (leanStop)
        break;
      if (first) {
        UINT matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, 0);
        bool bt601 = matrix == MFVideoTransferMatrix_BT601 || (matrix != MFVideoTransferMatrix_BT709 && h < 720);
        bool full = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0) == MFNominalRange_0_255;
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
void ensureManager();
void leanStart(const fs::path &file, double start) {
  leanError = S_OK;
  leanRebase = true;
  wchar_t forced[16]{};
  GetEnvironmentVariableW(L"LITELY_PIPELINE", forced, 16);
  bool rejected = std::find(directRejected.begin(), directRejected.end(), file) != directRejected.end();
  if (wcscmp(forced, L"lean") && !rejected && directFailedFor != file && directOpen(file, start)) {
    leanShown = {};
    leanWidth = directWidth;
    leanHeight = directHeight;
    frameRate = directRate;
    leanFrameTime = directFrameTime;
    leanDuration = directDuration;
    leanColorSpace = directColorSpace;
    PostMessage(controller, Media, generation, LeanReady);
    return;
  }
  if (!rejected && directFailedFor != file && wcscmp(forced, L"lean"))
    directRejected.push_back(file);
  ensureManager();
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
  if (context)
    context->Flush();
}
void leanClose() {
  leanJoin();
  directClose();
  leanShown = {};
  leanDropStill();
  leanActive = false;
}
void leanRestart(const fs::path &file, double start) {
  leanJoin();
  directClose();
  ++generation;
  leanStart(file, start);
}
void leanClock(LONGLONG now, LONGLONG time) {
  if (leanRebase) {
    leanBase = now - time;
    leanRebase = false;
  } else if (LONGLONG lateness = now - leanBase - time; lateness > leanFrameTime) {
    leanBase += lateness;
    ++leanRebased;
    // One refresh of delay is Present waiting on the display, not a stutter.
    if (lateness > leanFrameTime * 3 / 2) {
      ++leanLate;
      leanWorstLate = std::max(leanWorstLate, lateness / 1e4);
    }
  }
}
// False while the next frame is not due yet or has not been decoded.
bool leanAcquire(LONGLONG *pts) {
  LeanFrame frame;
  LONGLONG now = leanNow();
  if (directActive) {
    if (!directPending || (!leanRebase && now + 5000 < leanBase + directTime))
      return false;
    leanClock(now, directTime);
    ComPtr<ID3D11Texture2D> back;
    check(chain->GetBuffer(0, IID_PPV_ARGS(&back)));
    D3D11_BOX box{0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(back.Get(), 0, 0, 0, 0, directTexture(), directPendingIndex(), &box);
    *pts = directPts;
    directShow();
    return true;
  }
  {
    std::lock_guard<std::mutex> hold(leanLock);
    if (leanQueue.empty() || (!leanRebase && now + 5000 < leanBase + leanQueue.front().time))
      return false;
    frame = std::move(leanQueue.front());
    leanQueue.pop_front();
  }
  leanSpace.notify_one();
  leanClock(now, frame.time);
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
// Decodes the frame after the one just presented, so a slow read never delays a frame that is ready.
void leanAdvance() {
  if (!directActive || directPending)
    return;
  try {
    directDecode();
  } catch (...) {
    PostMessage(controller, Media, generation, DirectFailed);
  }
}
// False until a frame exists to make a still from.
bool leanHasFrame() { return directActive || leanShown.sample; }
void leanSchedule() {
  LONGLONG wait = 20000;
  if (directActive) {
    if (directPending)
      wait = leanRebase ? 0 : leanBase + directTime - leanNow();
  } else {
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
  if (directActive) {
    texture = directTexture();
    index = directStillIndex();
  } else
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
