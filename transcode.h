// Playback copies made in-process with Windows' H.264 decoder, video processor and encoder (hardware when present).
#include <atomic>
#include <codecapi.h>

constexpr DWORD transcodeVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
#define TRANSCODE_CHECK(x) do { HRESULT result_ = (x); if (FAILED(result_)) return result_; } while (0)

struct Transcode {
  fs::path source, destination;
  int limit = 0;
  bool cap = false, lowest = false;
  std::atomic<bool> cancel{false};
  std::atomic<int> percent{-1};
  HANDLE thread = nullptr;
};

HRESULT transcodeRun(Transcode &t, const fs::path &temp) {
  ComPtr<IMFAttributes> readerSettings;
  TRANSCODE_CHECK(MFCreateAttributes(&readerSettings, 1));
  readerSettings->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> reader;
  TRANSCODE_CHECK(MFCreateSourceReaderFromURL(t.source.c_str(), readerSettings.Get(), &reader));
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  TRANSCODE_CHECK(reader->SetStreamSelection(transcodeVideo, TRUE));
  ComPtr<IMFMediaType> native;
  TRANSCODE_CHECK(reader->GetNativeMediaType(transcodeVideo, 0, &native));
  UINT32 w = 0, h = 0, num = 0, den = 0;
  TRANSCODE_CHECK(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h));
  if (FAILED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) || !num || !den)
    num = 30, den = 1;
  double scale = t.limit ? std::min({1., t.limit / double(h), t.limit * 16 / 9. / w}) : 1;
  UINT32 ow = UINT32(w * scale) & ~1u, oh = UINT32(h * scale) & ~1u;
  if (t.cap && num > 30.5 * den)
    num = 30, den = 1;
  PROPVARIANT length;
  PropVariantInit(&length);
  LONGLONG duration = SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &length))
                          ? LONGLONG(length.uhVal.QuadPart) : 0;
  PropVariantClear(&length);

  ComPtr<IMFMediaType> decoded;
  TRANSCODE_CHECK(MFCreateMediaType(&decoded));
  decoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  decoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(decoded.Get(), MF_MT_FRAME_SIZE, ow, oh);
  TRANSCODE_CHECK(reader->SetCurrentMediaType(transcodeVideo, nullptr, decoded.Get()));

  ComPtr<IMFAttributes> writerSettings;
  TRANSCODE_CHECK(MFCreateAttributes(&writerSettings, 3));
  wchar_t software[8]{};
  writerSettings->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                            !GetEnvironmentVariableW(L"LITELY_TEST_SOFTWARE_ENCODER", software, 8));
  writerSettings->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  writerSettings->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
  ComPtr<IMFSinkWriter> writer;
  TRANSCODE_CHECK(MFCreateSinkWriterFromURL(temp.c_str(), nullptr, writerSettings.Get(), &writer));
  // Keep the source's colour tags; untagged video follows the player's height rule.
  UINT32 matrix = MFGetAttributeUINT32(native.Get(), MF_MT_YUV_MATRIX,
                                       h >= 720 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601);
  UINT32 range = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
  UINT32 primaries = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_PRIMARIES,
                                          h >= 720 ? MFVideoPrimaries_BT709 : MFVideoPrimaries_SMPTE170M);
  UINT32 transfer = MFGetAttributeUINT32(native.Get(), MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  ComPtr<IMFMediaType> encoded;
  TRANSCODE_CHECK(MFCreateMediaType(&encoded));
  encoded->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  encoded->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  encoded->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min(400e6, ow * double(oh) * num / den / 3)));
  encoded->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  encoded->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
  encoded->SetUINT32(MF_MT_YUV_MATRIX, matrix);
  encoded->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, range);
  encoded->SetUINT32(MF_MT_VIDEO_PRIMARIES, primaries);
  encoded->SetUINT32(MF_MT_TRANSFER_FUNCTION, transfer);
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
  LONGLONG heldEnd = 0, first = -1, k = 0;
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
    DWORD flags = 0;
    LONGLONG time = 0;
    ComPtr<IMFSample> sample;
    TRANSCODE_CHECK(reader->ReadSample(transcodeVideo, 0, nullptr, &flags, &time, &sample));
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))
      break;
    if (!sample)
      continue;
    if (first < 0)
      first = time;
    time -= first;
    if (duration > 0)
      t.percent = int(std::min(99LL, time * 100 / duration));
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
    // 1 ms of slack absorbs rounding in source timestamps.
    TRANSCODE_CHECK(emit(time - 10000));
    LONGLONG span = 0;
    held = packed;
    heldEnd = time + (SUCCEEDED(sample->GetSampleDuration(&span)) && span > 0 ? span : at(1));
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
