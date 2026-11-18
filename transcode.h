// Playback copies made in-process with Windows' H.264 decoder, video processor and encoder (hardware when present).
#include <atomic>
#include <codecapi.h>
#include <functional>
#include <wincodec.h>

constexpr DWORD transcodeVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
#define TRANSCODE_CHECK(x) do { HRESULT result_ = (x); if (FAILED(result_)) return result_; } while (0)

struct Transcode {
  fs::path source, destination;
  int limit = 0;
  bool cap = false, lowest = false;
  std::atomic<bool> cancel{false};
  std::atomic<int> percent{-1};
  HANDLE thread = nullptr;
  // Set by the worker: why it failed, the source's video format when Windows recognised one, and whether any frame was read.
  HRESULT error = S_OK;
  GUID codec = GUID_NULL;
  bool decoded = false;
};

bool isGif(const fs::path &file) { return !_wcsicmp(file.extension().c_str(), L".gif"); }
// GIFs can't play as they are, and MPEG streams don't loop or start reliably, so these always get a copy.
bool needsCopy(const fs::path &file) {
  for (auto extension : {L".gif", L".ts", L".mts", L".m2ts", L".mpg", L".mpeg"})
    if (!_wcsicmp(file.extension().c_str(), extension))
      return true;
  return false;
}
const wchar_t *const videoExtensions[] = {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi", L".wmv", L".asf",
                                          L".3gp", L".3g2", L".ts", L".mts", L".m2ts", L".mpg", L".mpeg", L".gif"};
bool isVideoFile(const fs::path &file) {
  for (auto extension : videoExtensions)
    if (!_wcsicmp(file.extension().c_str(), extension))
      return true;
  return false;
}
// The Open dialog filter for every format above.
std::wstring videoFilter() {
  std::wstring patterns;
  for (auto extension : videoExtensions)
    patterns += (patterns.empty() ? L"*" : L";*") + std::wstring(extension);
  return L"Videos and GIFs" + std::wstring(1, L'\0') + patterns + std::wstring(1, L'\0') + L"All files" +
         std::wstring(1, L'\0') + L"*.*" + std::wstring(1, L'\0');
}

// Packed NV12 frames at the output size; next() leaves frame empty at the end.
struct TranscodeSource {
  UINT32 width = 0, height = 0, num = 0, den = 0;
  UINT32 matrix = 0, range = 0, primaries = 0, transfer = 0;
  LONGLONG duration = 0;
  std::function<HRESULT(UINT32 ow, UINT32 oh)> prepare;
  std::function<HRESULT(UINT32 ow, UINT32 oh, ComPtr<IMFMediaBuffer> &frame, LONGLONG &time, LONGLONG &span)> next;
};

HRESULT openVideo(Transcode &t, TranscodeSource &s) {
  ComPtr<IMFAttributes> readerSettings;
  TRANSCODE_CHECK(MFCreateAttributes(&readerSettings, 1));
  readerSettings->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> reader;
  TRANSCODE_CHECK(MFCreateSourceReaderFromURL(t.source.c_str(), readerSettings.Get(), &reader));
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  TRANSCODE_CHECK(reader->SetStreamSelection(transcodeVideo, TRUE));
  ComPtr<IMFMediaType> native;
  TRANSCODE_CHECK(reader->GetNativeMediaType(transcodeVideo, 0, &native));
  native->GetGUID(MF_MT_SUBTYPE, &t.codec);
  TRANSCODE_CHECK(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &s.width, &s.height));
  readRate(reader.Get(), native.Get(), t.source, s.num, s.den);
  s.matrix = MFGetAttributeUINT32(native.Get(), MF_MT_YUV_MATRIX, 0);
  s.range = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0);
  s.primaries = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_PRIMARIES, 0);
  s.transfer = MFGetAttributeUINT32(native.Get(), MF_MT_TRANSFER_FUNCTION, 0);
  PROPVARIANT length;
  PropVariantInit(&length);
  if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length)))
    s.duration = LONGLONG(length.uhVal.QuadPart);
  PropVariantClear(&length);
  s.prepare = [reader](UINT32 ow, UINT32 oh) -> HRESULT {
    ComPtr<IMFMediaType> decoded;
    TRANSCODE_CHECK(MFCreateMediaType(&decoded));
    decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(decoded.Get(), MF_MT_FRAME_SIZE, ow, oh);
    return reader->SetCurrentMediaType(transcodeVideo, nullptr, decoded.Get());
  };
  s.next = [reader, &t, duration = s.duration, first = LONGLONG(-1)](UINT32 ow, UINT32 oh, ComPtr<IMFMediaBuffer> &frame,
                                                                   LONGLONG &time, LONGLONG &span) mutable -> HRESULT {
    frame.Reset();
    ComPtr<IMFSample> sample;
    while (!sample) {
      DWORD flags = 0;
      TRANSCODE_CHECK(reader->ReadSample(transcodeVideo, 0, nullptr, &flags, &time, &sample));
      if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
        return S_OK;
    }
    if (first < 0)
      first = time;
    time -= first;
    if (duration > 0)
      t.percent = int(std::min(99LL, time * 100 / duration));
    if (FAILED(sample->GetSampleDuration(&span)))
      span = 0;
    // A decoded frame can be padded (1080 rows stored as 1088); the encoder wants tightly packed planes.
    ComPtr<IMFMediaBuffer> buffer;
    TRANSCODE_CHECK(sample->ConvertToContiguousBuffer(&buffer));
    ComPtr<IMF2DBuffer> planes;
    BYTE *from = nullptr;
    LONG pitch = LONG(ow);
    DWORD size = 0;
    buffer->GetCurrentLength(&size);
    if (SUCCEEDED(buffer.As(&planes)))
      TRANSCODE_CHECK(planes->Lock2D(&from, &pitch));
    else
      TRANSCODE_CHECK(buffer->Lock(&from, nullptr, nullptr));
    UINT32 rows = std::max(oh, UINT32(size / std::max(1L, pitch) * 2 / 3));
    ComPtr<IMFMediaBuffer> packed;
    HRESULT made = MFCreateMemoryBuffer(ow * oh * 3 / 2, &packed);
    BYTE *to = nullptr;
    if (SUCCEEDED(made))
      made = packed->Lock(&to, nullptr, nullptr);
    if (SUCCEEDED(made)) {
      for (UINT32 y = 0; y < oh; y++)
        memcpy(to + y * ow, from + y * pitch, ow);
      for (UINT32 y = 0; y < oh / 2; y++)
        memcpy(to + ow * oh + y * ow, from + (rows + y) * pitch, ow);
      packed->Unlock();
      packed->SetCurrentLength(ow * oh * 3 / 2);
    }
    if (planes)
      planes->Unlock2D();
    else
      buffer->Unlock();
    TRANSCODE_CHECK(made);
    frame = packed;
    return S_OK;
  };
  return S_OK;
}

struct Gif {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  UINT width = 0, height = 0;
  struct Frame { UINT left, top, width, height, disposal, delay; };  // delay in 10 ms units
  std::vector<Frame> frames;
};
UINT gifNumber(IWICMetadataQueryReader *query, const wchar_t *name, UINT fallback) {
  PROPVARIANT value;
  PropVariantInit(&value);
  UINT result = fallback;
  if (query && SUCCEEDED(query->GetMetadataByName(name, &value)))
    result = value.vt == VT_UI1 ? value.bVal : value.vt == VT_UI2 ? value.uiVal : value.vt == VT_UI4 ? value.ulVal : fallback;
  PropVariantClear(&value);
  return result;
}
HRESULT gifOpen(const fs::path &file, Gif &gif) {
  TRANSCODE_CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&gif.factory)));
  TRANSCODE_CHECK(gif.factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &gif.decoder));
  GUID format{};
  TRANSCODE_CHECK(gif.decoder->GetContainerFormat(&format));
  if (format != GUID_ContainerFormatGif)
    return MF_E_UNSUPPORTED_FORMAT;
  UINT count = 0;
  TRANSCODE_CHECK(gif.decoder->GetFrameCount(&count));
  for (UINT i = 0; i < count; i++) {
    ComPtr<IWICBitmapFrameDecode> frame;
    TRANSCODE_CHECK(gif.decoder->GetFrame(i, &frame));
    ComPtr<IWICMetadataQueryReader> query;
    frame->GetMetadataQueryReader(&query);
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    Gif::Frame f{gifNumber(query.Get(), L"/imgdesc/Left", 0), gifNumber(query.Get(), L"/imgdesc/Top", 0),
                 fw, fh, gifNumber(query.Get(), L"/grctlext/Disposal", 0), gifNumber(query.Get(), L"/grctlext/Delay", 0)};
    // Browsers show frames with almost no delay for 100 ms.
    if (f.delay < 2)
      f.delay = 10;
    gif.frames.push_back(f);
  }
  ComPtr<IWICMetadataQueryReader> query;
  gif.decoder->GetMetadataQueryReader(&query);
  gif.width = gifNumber(query.Get(), L"/logscrdesc/Width", 0);
  gif.height = gifNumber(query.Get(), L"/logscrdesc/Height", 0);
  if (!gif.width || !gif.height) {
    gif.width = gif.frames.empty() ? 0 : gif.frames[0].width;
    gif.height = gif.frames.empty() ? 0 : gif.frames[0].height;
  }
  return gif.frames.empty() || !gif.width || !gif.height ? MF_E_INVALIDMEDIATYPE : S_OK;
}
// Output rate: one frame per shortest delay, so every frame shows for its own time.
UINT gifShortestDelay(const Gif &gif) {
  UINT shortest = 1000;
  for (auto &f : gif.frames)
    shortest = std::min(shortest, f.delay);
  return shortest;
}
void nv12FromBgra(const UINT32 *pixels, UINT stride, UINT w, UINT h, bool bt709, BYTE *out) {
  double kr = bt709 ? .2126 : .299, kb = bt709 ? .0722 : .114, kg = 1 - kr - kb;
  auto channel = [](UINT32 p, int shift) { return double((p >> shift) & 255); };
  for (UINT y = 0; y < h; y++)
    for (UINT x = 0; x < w; x++) {
      UINT32 p = pixels[y * stride + x];
      out[y * w + x] = BYTE(16 + (kr * channel(p, 16) + kg * channel(p, 8) + kb * channel(p, 0)) * 219 / 255 + .5);
    }
  BYTE *chroma = out + w * h;
  for (UINT y = 0; y < h / 2; y++)
    for (UINT x = 0; x < w / 2; x++) {
      double r = 0, g = 0, b = 0;
      for (UINT dy = 0; dy < 2; dy++)
        for (UINT dx = 0; dx < 2; dx++) {
          UINT32 p = pixels[(2 * y + dy) * stride + 2 * x + dx];
          r += channel(p, 16) / 4, g += channel(p, 8) / 4, b += channel(p, 0) / 4;
        }
      double luma = kr * r + kg * g + kb * b;
      chroma[y * w + 2 * x] = BYTE(std::clamp(128 + (b - luma) / (2 * (1 - kb)) * 224 / 255 + .5, 16., 240.));
      chroma[y * w + 2 * x + 1] = BYTE(std::clamp(128 + (r - luma) / (2 * (1 - kr)) * 224 / 255 + .5, 16., 240.));
    }
}
// GIF frames are patches over a shared canvas; each one is composed onto it, then converted to NV12.
HRESULT openGif(Transcode &t, TranscodeSource &s) {
  auto gif = std::make_shared<Gif>();
  TRANSCODE_CHECK(gifOpen(t.source, *gif));
  UINT total = 0;
  for (auto &f : gif->frames)
    total += f.delay;
  s.width = gif->width, s.height = gif->height, s.num = 100, s.den = gifShortestDelay(*gif);
  s.duration = LONGLONG(total) * 100000;
  s.prepare = [](UINT32, UINT32) { return S_OK; };
  auto canvas = std::make_shared<std::vector<UINT32>>(size_t(gif->width) * gif->height);
  auto saved = std::make_shared<std::vector<UINT32>>();
  s.next = [gif, canvas, saved, &t, &s, index = size_t(0), clock = LONGLONG(0)](
               UINT32 ow, UINT32 oh, ComPtr<IMFMediaBuffer> &frame, LONGLONG &time, LONGLONG &span) mutable -> HRESULT {
    frame.Reset();
    if (index == gif->frames.size())
      return S_OK;
    UINT W = gif->width, H = gif->height;
    auto &f = gif->frames[index];
    if (index) {
      auto &previous = gif->frames[index - 1];
      if (previous.disposal == 2)
        for (UINT y = previous.top; y < std::min(H, previous.top + previous.height); y++)
          for (UINT x = previous.left; x < std::min(W, previous.left + previous.width); x++)
            (*canvas)[size_t(y) * W + x] = 0;
      else if (previous.disposal == 3 && !saved->empty())
        *canvas = *saved;
    }
    if (f.disposal == 3)
      *saved = *canvas;
    ComPtr<IWICBitmapFrameDecode> picture;
    TRANSCODE_CHECK(gif->decoder->GetFrame(UINT(index), &picture));
    ComPtr<IWICFormatConverter> bgra;
    TRANSCODE_CHECK(gif->factory->CreateFormatConverter(&bgra));
    TRANSCODE_CHECK(bgra->Initialize(picture.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom));
    UINT fw = 0, fh = 0;
    TRANSCODE_CHECK(bgra->GetSize(&fw, &fh));
    std::vector<UINT32> patch(size_t(fw) * fh);
    TRANSCODE_CHECK(bgra->CopyPixels(nullptr, fw * 4, UINT(patch.size() * 4), reinterpret_cast<BYTE *>(patch.data())));
    for (UINT y = 0; y < fh && f.top + y < H; y++)
      for (UINT x = 0; x < fw && f.left + x < W; x++)
        if (UINT32 p = patch[size_t(y) * fw + x]; p >> 24)
          (*canvas)[size_t(f.top + y) * W + f.left + x] = p;
    // Transparent pixels are zero, so they show as black.
    const UINT32 *pixels = canvas->data();
    UINT stride = W;
    std::vector<UINT32> scaled;
    if (ow != W || oh != H) {
      ComPtr<IWICBitmap> whole;
      TRANSCODE_CHECK(gif->factory->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGRA, W * 4, W * H * 4,
                                                           reinterpret_cast<BYTE *>(canvas->data()), &whole));
      ComPtr<IWICBitmapScaler> scaler;
      TRANSCODE_CHECK(gif->factory->CreateBitmapScaler(&scaler));
      TRANSCODE_CHECK(scaler->Initialize(whole.Get(), ow, oh, WICBitmapInterpolationModeFant));
      scaled.resize(size_t(ow) * oh);
      TRANSCODE_CHECK(scaler->CopyPixels(nullptr, ow * 4, UINT(scaled.size() * 4), reinterpret_cast<BYTE *>(scaled.data())));
      pixels = scaled.data(), stride = ow;
    }
    ComPtr<IMFMediaBuffer> packed;
    TRANSCODE_CHECK(MFCreateMemoryBuffer(ow * oh * 3 / 2, &packed));
    BYTE *to = nullptr;
    TRANSCODE_CHECK(packed->Lock(&to, nullptr, nullptr));
    nv12FromBgra(pixels, stride, ow, oh, s.matrix == MFVideoTransferMatrix_BT709, to);
    packed->Unlock();
    packed->SetCurrentLength(ow * oh * 3 / 2);
    frame = packed;
    time = clock * 100000, span = LONGLONG(f.delay) * 100000;
    clock += f.delay;
    t.percent = int(std::min<size_t>(99, index * 100 / gif->frames.size()));
    index++;
    return S_OK;
  };
  return S_OK;
}

// Size, frame rate in thousandths and duration in 100 ns units, read the way the converter reads the file.
bool videoFacts(const fs::path &file, DWORD &frameWidth, DWORD &frameHeight, DWORD &rate, ULONGLONG &duration) {
  if (isGif(file)) {
    Gif gif;
    if (FAILED(gifOpen(file, gif)))
      return false;
    UINT total = 0;
    for (auto &f : gif.frames)
      total += f.delay;
    frameWidth = gif.width, frameHeight = gif.height, rate = 100000 / gifShortestDelay(gif), duration = ULONGLONG(total) * 100000;
    return true;
  }
  if (FAILED(MFStartup(MF_VERSION)))
    return false;
  bool read = false;
  {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> native;
    UINT32 w = 0, h = 0, num = 0, den = 0;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader)) &&
        SUCCEEDED(reader->GetNativeMediaType(transcodeVideo, 0, &native)) &&
        SUCCEEDED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
      frameWidth = w, frameHeight = h, read = true;
      readRate(reader.Get(), native.Get(), file, num, den);
      rate = DWORD(num * 1000ULL / den);
      PROPVARIANT length;
      PropVariantInit(&length);
      if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length)))
        duration = length.uhVal.QuadPart;
      PropVariantClear(&length);
    }
  }
  MFShutdown();
  return read;
}

HRESULT transcodeRun(Transcode &t, const fs::path &temp) {
  TranscodeSource s;
  TRANSCODE_CHECK(isGif(t.source) ? openGif(t, s) : openVideo(t, s));
  UINT32 w = s.width, h = s.height, num = s.num, den = s.den;
  double scale = t.limit ? std::min({1., t.limit / double(h), t.limit * 16 / 9. / w}) : 1;
  UINT32 ow = UINT32(w * scale) & ~1u, oh = UINT32(h * scale) & ~1u;
  if (t.cap && num > 30.5 * den)
    num = 30, den = 1;
  // The MP4 writer counts time in units of rate x 1000; a rate that isn't whole there gets uneven frame lengths.
  if (UINT64(num) * 1000 % den)
    num = UINT32((UINT64(num) * 1000 + den / 2) / den), den = 1000;
  // Keep the source's colour tags; untagged video follows the player's height rule.
  if (!s.matrix)
    s.matrix = h >= 720 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601;
  if (!s.range)
    s.range = MFNominalRange_16_235;
  if (!s.primaries)
    s.primaries = h >= 720 ? MFVideoPrimaries_BT709 : MFVideoPrimaries_SMPTE170M;
  if (!s.transfer)
    s.transfer = MFVideoTransFunc_709;
  TRANSCODE_CHECK(s.prepare(ow, oh));

  ComPtr<IMFAttributes> writerSettings;
  TRANSCODE_CHECK(MFCreateAttributes(&writerSettings, 3));
  wchar_t software[8]{};
  writerSettings->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                            !GetEnvironmentVariableW(L"LITELY_TEST_SOFTWARE_ENCODER", software, 8));
  writerSettings->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  writerSettings->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
  ComPtr<IMFSinkWriter> writer;
  TRANSCODE_CHECK(MFCreateSinkWriterFromURL(temp.c_str(), nullptr, writerSettings.Get(), &writer));
  ComPtr<IMFMediaType> encoded;
  TRANSCODE_CHECK(MFCreateMediaType(&encoded));
  encoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  encoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  encoded->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min(400e6, ow * double(oh) * num / den / 3)));
  encoded->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  encoded->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
  encoded->SetUINT32(MF_MT_YUV_MATRIX, s.matrix);
  encoded->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, s.range);
  encoded->SetUINT32(MF_MT_VIDEO_PRIMARIES, s.primaries);
  encoded->SetUINT32(MF_MT_TRANSFER_FUNCTION, s.transfer);
  MFSetAttributeSize(encoded.Get(), MF_MT_FRAME_SIZE, ow, oh);
  MFSetAttributeRatio(encoded.Get(), MF_MT_FRAME_RATE, num, den);
  MFSetAttributeRatio(encoded.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  DWORD stream = 0;
  TRANSCODE_CHECK(writer->AddStream(encoded.Get(), &stream));
  ComPtr<IMFMediaType> input;
  TRANSCODE_CHECK(MFCreateMediaType(&input));
  TRANSCODE_CHECK(encoded->CopyAllItems(input.Get()));
  input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  input->SetUINT32(MF_MT_DEFAULT_STRIDE, ow);
  input->DeleteItem(MF_MT_AVG_BITRATE);
  input->DeleteItem(MF_MT_MPEG2_PROFILE);
  ComPtr<IMFAttributes> encoder;
  TRANSCODE_CHECK(MFCreateAttributes(&encoder, 6));
  // Setting CABAC explicitly with best quality corrupts the Microsoft software encoder's output, so it keeps its default.
  encoder->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
  encoder->SetUINT32(CODECAPI_AVEncCommonQuality, t.lowest ? 100 : 90);
  encoder->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 100);
  if (t.lowest) {
    // The form the direct decoder accepts: one reference frame, no B-frames.
    encoder->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    encoder->SetUINT32(CODECAPI_AVEncVideoMaxNumRefFrame, 1);
    encoder->SetUINT32(CODECAPI_AVEncMPVGOPSize, 60);
  }
  TRANSCODE_CHECK(writer->SetInputMediaType(stream, input.Get(), encoder.Get()));
  TRANSCODE_CHECK(writer->BeginWriting());

  // Constant frame rate: output frame k shows the latest source frame at or before k / rate.
  auto at = [&](LONGLONG k) { return k * 10000000LL * den / num; };
  ComPtr<IMFMediaBuffer> held;
  LONGLONG heldEnd = 0, k = 0;
  auto emit = [&](LONGLONG until) -> HRESULT {
    for (; held && at(k) < until; k++) {
      ComPtr<IMFSample> out;
      TRANSCODE_CHECK(MFCreateSample(&out));
      out->AddBuffer(held.Get());
      out->SetSampleTime(at(k));
      out->SetSampleDuration(at(k + 1) - at(k));
      TRANSCODE_CHECK(writer->WriteSample(stream, out.Get()));
    }
    return S_OK;
  };
  for (;;) {
    if (t.cancel)
      return E_ABORT;
    ComPtr<IMFMediaBuffer> frame;
    LONGLONG time = 0, span = 0;
    TRANSCODE_CHECK(s.next(ow, oh, frame, time, span));
    if (!frame)
      break;
    t.decoded = true;
    // 1 ms of slack absorbs rounding in source timestamps.
    TRANSCODE_CHECK(emit(time - 10000));
    held = frame;
    heldEnd = time + (span > 0 ? span : at(1));
  }
  if (!held)
    return E_FAIL;
  TRANSCODE_CHECK(emit(heldEnd - 10000));
  return writer->Finalize();
}

DWORD WINAPI transcodeThread(void *argument) {
  auto &t = *static_cast<Transcode *>(argument);
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool com = SUCCEEDED(result);
  if (com)
    result = MFStartup(MF_VERSION);
  auto temp = t.destination;
  temp += L".part";
  if (SUCCEEDED(result)) {
    std::error_code ignored;
    fs::create_directories(t.destination.parent_path(), ignored);
    result = transcodeRun(t, temp);
    MFShutdown();
  }
  if (SUCCEEDED(result) && !MoveFileExW(temp.c_str(), t.destination.c_str(), MOVEFILE_REPLACE_EXISTING))
    result = E_FAIL;
  if (FAILED(result))
    DeleteFileW(temp.c_str());
  if (com)
    CoUninitialize();
  t.error = result;
  return SUCCEEDED(result) ? 0 : 1;
}

bool transcodeStart(Transcode &t, int priority) {
  t.thread = CreateThread(nullptr, 0, transcodeThread, &t, CREATE_SUSPENDED, nullptr);
  if (!t.thread)
    return false;
  SetThreadPriority(t.thread, priority);
  ResumeThread(t.thread);
  return true;
}

// Waits for the worker; a cancelled one stops at its next frame. Returns 0 when the copy was made.
DWORD transcodeStop(Transcode &t, bool cancel) {
  DWORD code = 1;
  if (!t.thread)
    return code;
  if (cancel)
    t.cancel = true;
  WaitForSingleObject(t.thread, INFINITE);
  GetExitCodeThread(t.thread, &code);
  CloseHandle(t.thread);
  t.thread = nullptr;
  return code;
}
