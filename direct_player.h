// Drives the GPU's H.264 decoder directly for MP4s in a simple form: progressive 4:2:0,
// one reference frame, no reordering, one slice per picture. Anything else is left to the lean path.
#include <dxva.h>

bool directActive = false;
UINT directWidth = 0, directHeight = 0;
double directRate = 60, directDuration = 0;
LONGLONG directFrameTime = 166667, directPts = 0, directTime = 0;
DXGI_COLOR_SPACE_TYPE directColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
bool directPending = false;

struct DirectBits {
  std::vector<BYTE> data;
  size_t at = 0;
  // Emulation-prevention bytes are removed so header fields read as plain bits.
  DirectBits(const BYTE *nal, size_t size) {
    for (size_t i = 0; i < size; i++) {
      if (i >= 2 && nal[i] == 3 && !nal[i - 1] && !nal[i - 2])
        continue;
      data.push_back(nal[i]);
    }
  }
  unsigned bit() {
    if (at >= data.size() * 8)
      throw E_INVALIDARG;
    unsigned v = (data[at >> 3] >> (7 - (at & 7))) & 1;
    ++at;
    return v;
  }
  unsigned u(int n) {
    unsigned v = 0;
    while (n--)
      v = v << 1 | bit();
    return v;
  }
  unsigned ue() {
    int zeros = 0;
    while (!bit())
      if (++zeros > 31)
        throw E_INVALIDARG;
    return (1u << zeros) - 1 + u(zeros);
  }
  int se() {
    unsigned k = ue();
    return k & 1 ? int((k + 1) / 2) : -int(k / 2);
  }
  bool more() {
    size_t last = data.size() * 8;
    while (last && !((data[(last - 1) >> 3] >> (7 - ((last - 1) & 7))) & 1))
      --last;
    return at + 1 < last;
  }
};
struct DirectSps {
  unsigned level = 0, log2FrameNum = 4, pocType = 0, log2PocLsb = 4, refs = 0, widthMbs = 0, heightMbs = 0;
  unsigned direct8x8 = 0, cropRight = 0, cropBottom = 0, matrix = 2, fullRange = 0;
};
struct DirectPps {
  unsigned cabac = 0, bottomPoc = 0, refL0 = 0, refL1 = 0, weightedBi = 0, deblockControl = 0, constrainedIntra = 0;
  unsigned redundant = 0, transform8x8 = 0;
  int qp = 0, qs = 0, chromaOffset = 0, chromaOffset2 = 0;
};
struct DirectSample {
  ULONGLONG offset;
  UINT size;
  bool sync;
};
struct DirectState {
  DirectSps sps;
  DirectPps pps;
  std::vector<DirectSample> samples;
  std::vector<BYTE> packet, bitstream;
  HANDLE file = INVALID_HANDLE_VALUE;
  unsigned lengthBytes = 4;
  ComPtr<ID3D11VideoDevice> videoDevice;
  ComPtr<ID3D11VideoContext> videoContext;
  ComPtr<ID3D11VideoDecoder> decoder;
  ComPtr<ID3D11Texture2D> surfaces;
  ComPtr<ID3D11VideoDecoderOutputView> views[2];
  size_t next = 0;
  UINT decoded = 0, shown = 0, feedback = 0, delta = 1, timescale = 1;
  bool hasShown = false, hasReference = false, started = false;
  int referencePoc = 0, pocMsb = 0, previousPocLsb = 0;
  unsigned referenceFrameNum = 0, frameNumOffset = 0, previousFrameNum = 0;
};
std::unique_ptr<DirectState> direct;
// Test hook: LITELY_TEST_DIRECT_FAIL=N makes the Nth decode fail.
unsigned directFailAfter = [] {
  wchar_t value[16]{};
  return GetEnvironmentVariableW(L"LITELY_TEST_DIRECT_FAIL", value, 16) ? unsigned(_wtoi(value)) : 0u;
}();

void directSps(const BYTE *nal, size_t size, DirectSps &s) {
  if (size < 4 || (nal[0] & 31) != 7)
    throw E_NOTIMPL;
  DirectBits b(nal + 1, size - 1);
  unsigned profile = b.u(8);
  b.u(8);
  s.level = b.u(8);
  b.ue();
  if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
      profile == 86 || profile == 118 || profile == 128) {
    if (b.ue() != 1 || b.ue() || b.ue())
      throw E_NOTIMPL;
    b.bit();
    if (b.bit())
      throw E_NOTIMPL;
  }
  s.log2FrameNum = b.ue() + 4;
  s.pocType = b.ue();
  if (s.pocType == 0)
    s.log2PocLsb = b.ue() + 4;
  else if (s.pocType == 1)
    throw E_NOTIMPL;
  s.refs = b.ue();
  b.bit();
  s.widthMbs = b.ue() + 1;
  s.heightMbs = b.ue() + 1;
  if (s.refs > 1 || s.log2FrameNum > 16 || s.log2PocLsb > 16 || !s.widthMbs || !s.heightMbs || s.widthMbs > 512 ||
      s.heightMbs > 512 || !b.bit())
    throw E_NOTIMPL;
  s.direct8x8 = b.bit();
  if (b.bit()) {
    unsigned left = b.ue();
    s.cropRight = b.ue() * 2;
    unsigned top = b.ue();
    s.cropBottom = b.ue() * 2;
    if (left || top || s.cropRight >= 16 || s.cropBottom >= 16)
      throw E_NOTIMPL;
  }
  if (b.bit()) {
    if (b.bit() && b.u(8) == 255)
      b.u(32);
    if (b.bit())
      b.bit();
    if (b.bit()) {
      b.u(3);
      s.fullRange = b.bit();
      if (b.bit()) {
        b.u(16);
        s.matrix = b.u(8);
      }
    }
  }
}
void directPps(const BYTE *nal, size_t size, DirectPps &p) {
  if (size < 2 || (nal[0] & 31) != 8)
    throw E_NOTIMPL;
  DirectBits b(nal + 1, size - 1);
  b.ue();
  b.ue();
  p.cabac = b.bit();
  p.bottomPoc = b.bit();
  if (b.ue())
    throw E_NOTIMPL;
  p.refL0 = b.ue();
  p.refL1 = b.ue();
  bool weighted = b.bit() != 0;
  p.weightedBi = b.u(2);
  if (weighted || p.refL0 > 31 || p.refL1 > 31)
    throw E_NOTIMPL;
  p.qp = b.se();
  p.qs = b.se();
  p.chromaOffset = p.chromaOffset2 = b.se();
  p.deblockControl = b.bit();
  p.constrainedIntra = b.bit();
  p.redundant = b.bit();
  if (b.more()) {
    p.transform8x8 = b.bit();
    if (b.bit())
      throw E_NOTIMPL;
    p.chromaOffset2 = b.se();
  }
}
// A bounds-checked view of MP4 box contents; numbers are big-endian.
struct DirectBox {
  const BYTE *bytes = nullptr;
  size_t length = 0;
  const BYTE *need(size_t at, size_t count) const {
    if (at > length || count > length - at)
      throw E_INVALIDARG;
    return bytes + at;
  }
  UINT u32(size_t at) const {
    auto p = need(at, 4);
    return UINT(p[0]) << 24 | UINT(p[1]) << 16 | UINT(p[2]) << 8 | p[3];
  }
  ULONGLONG u64(size_t at) const { return ULONGLONG(u32(at)) << 32 | u32(at + 4); }
  UINT u16(size_t at) const {
    auto p = need(at, 2);
    return UINT(p[0]) << 8 | p[1];
  }
  DirectBox from(size_t at) const { return {need(at, 0), length - at}; }
  // The contents of the first child box of this type, or an empty view.
  DirectBox child(const char *type) const {
    for (size_t at = 0; at + 8 <= length;) {
      ULONGLONG size = u32(at);
      size_t header = 8;
      if (size == 1) {
        size = u64(at + 8);
        header = 16;
      } else if (!size)
        size = length - at;
      if (size < header || size > length - at)
        throw E_INVALIDARG;
      if (!memcmp(need(at + 4, 4), type, 4))
        return {bytes + at + header, size_t(size) - header};
      at += size_t(size);
    }
    return {};
  }
};
template <class T> void directSend(DirectState &d, D3D11_VIDEO_DECODER_BUFFER_TYPE type, const T *data, UINT bytes,
                                   D3D11_VIDEO_DECODER_BUFFER_DESC &desc) {
  UINT capacity = 0;
  void *target = nullptr;
  check(d.videoContext->GetDecoderBuffer(d.decoder.Get(), type, &capacity, &target));
  if (bytes <= capacity)
    memcpy(target, data, bytes);
  check(d.videoContext->ReleaseDecoderBuffer(d.decoder.Get(), type));
  if (bytes > capacity)
    throw E_OUTOFMEMORY;
  desc = {};
  desc.BufferType = type;
  desc.DataSize = bytes;
}
// Decodes the next picture into the surface that is not the reference.
void directDecode() {
  DirectState &d = *direct;
  directPending = false;
  if (directFailAfter && !--directFailAfter)
    throw E_FAIL;
  const size_t index = d.next;
  const DirectSample &sample = d.samples[index];
  d.packet.resize(sample.size);
  OVERLAPPED where{};
  where.Offset = DWORD(sample.offset);
  where.OffsetHigh = DWORD(sample.offset >> 32);
  DWORD got = 0;
  if (!ReadFile(d.file, d.packet.data(), sample.size, &got, &where) || got != sample.size)
    throw HRESULT_FROM_WIN32(ERROR_READ_FAULT);
  const BYTE *nal = nullptr;
  size_t nalSize = 0;
  for (size_t at = 0; at + d.lengthBytes <= d.packet.size();) {
    size_t length = 0;
    for (unsigned i = 0; i < d.lengthBytes; i++)
      length = length << 8 | d.packet[at + i];
    at += d.lengthBytes;
    if (!length || length > d.packet.size() - at)
      throw E_INVALIDARG;
    int type = d.packet[at] & 31;
    if (type == 1 || type == 5) {
      if (nal)
        throw E_NOTIMPL;
      nal = &d.packet[at];
      nalSize = length;
    }
    at += length;
  }
  if (!nal || nalSize < 2)
    throw E_INVALIDARG;
  const bool idr = (nal[0] & 31) == 5;
  DirectBits b(nal + 1, std::min<size_t>(nalSize - 1, 64));
  if (b.ue())
    throw E_NOTIMPL;
  unsigned sliceType = b.ue() % 5;
  b.ue();
  unsigned frameNum = b.u(d.sps.log2FrameNum);
  if (sliceType != 0 && sliceType != 2)
    throw E_NOTIMPL;
  if (idr) {
    b.ue();
    d.pocMsb = d.previousPocLsb = 0;
    d.frameNumOffset = 0;
    d.hasReference = false;
  }
  int poc = 0;
  if (d.sps.pocType == 0) {
    int lsb = int(b.u(d.sps.log2PocLsb)), range = 1 << d.sps.log2PocLsb;
    if (lsb < d.previousPocLsb && d.previousPocLsb - lsb >= range / 2)
      d.pocMsb += range;
    else if (lsb > d.previousPocLsb && lsb - d.previousPocLsb > range / 2)
      d.pocMsb -= range;
    d.previousPocLsb = lsb;
    poc = d.pocMsb + lsb;
  } else {
    if (!idr && frameNum < d.previousFrameNum)
      d.frameNumOffset += 1u << d.sps.log2FrameNum;
    poc = idr ? 0 : int(2 * (d.frameNumOffset + frameNum));
  }
  d.previousFrameNum = frameNum;
  if (!d.hasReference && sliceType != 2)
    throw E_INVALIDARG;
  const UINT target = d.hasReference ? 1 - d.decoded : 0;
  DXVA_PicParams_H264 pp{};
  pp.wFrameWidthInMbsMinus1 = USHORT(d.sps.widthMbs - 1);
  pp.wFrameHeightInMbsMinus1 = USHORT(d.sps.heightMbs - 1);
  pp.CurrPic.Index7Bits = target;
  pp.num_ref_frames = UCHAR(d.sps.refs);
  pp.chroma_format_idc = 1;
  pp.RefPicFlag = 1;
  pp.constrained_intra_pred_flag = d.pps.constrainedIntra;
  pp.weighted_bipred_idc = d.pps.weightedBi;
  pp.MbsConsecutiveFlag = 1;
  pp.frame_mbs_only_flag = 1;
  pp.transform_8x8_mode_flag = d.pps.transform8x8;
  pp.MinLumaBipredSize8x8Flag = d.sps.level >= 31;
  pp.IntraPicFlag = sliceType == 2;
  pp.Reserved16Bits = 3;
  if (!++d.feedback)
    ++d.feedback;
  pp.StatusReportFeedbackNumber = d.feedback;
  for (auto &entry : pp.RefFrameList)
    entry.bPicEntry = 0xff;
  if (d.hasReference) {
    pp.RefFrameList[0].Index7Bits = d.decoded;
    pp.FieldOrderCntList[0][0] = pp.FieldOrderCntList[0][1] = d.referencePoc;
    pp.FrameNumList[0] = USHORT(d.referenceFrameNum);
    pp.UsedForReferenceFlags = 3;
  }
  pp.CurrFieldOrderCnt[0] = pp.CurrFieldOrderCnt[1] = poc;
  pp.pic_init_qs_minus26 = CHAR(d.pps.qs);
  pp.chroma_qp_index_offset = CHAR(d.pps.chromaOffset);
  pp.second_chroma_qp_index_offset = CHAR(d.pps.chromaOffset2);
  pp.ContinuationFlag = 1;
  pp.pic_init_qp_minus26 = CHAR(d.pps.qp);
  pp.num_ref_idx_l0_active_minus1 = UCHAR(d.pps.refL0);
  pp.num_ref_idx_l1_active_minus1 = UCHAR(d.pps.refL1);
  pp.frame_num = USHORT(frameNum);
  pp.log2_max_frame_num_minus4 = UCHAR(d.sps.log2FrameNum - 4);
  pp.pic_order_cnt_type = UCHAR(d.sps.pocType);
  pp.log2_max_pic_order_cnt_lsb_minus4 = UCHAR(d.sps.pocType == 0 ? d.sps.log2PocLsb - 4 : 0);
  pp.direct_8x8_inference_flag = UCHAR(d.sps.direct8x8);
  pp.entropy_coding_mode_flag = UCHAR(d.pps.cabac);
  pp.pic_order_present_flag = UCHAR(d.pps.bottomPoc);
  pp.deblocking_filter_control_present_flag = UCHAR(d.pps.deblockControl);
  pp.redundant_pic_cnt_present_flag = UCHAR(d.pps.redundant);
  DXVA_Qmatrix_H264 flat;
  memset(&flat, 16, sizeof flat);
  // The driver wants a start code before the slice and zero padding to a multiple of 128 bytes.
  d.bitstream.assign({0, 0, 1});
  d.bitstream.insert(d.bitstream.end(), nal, nal + nalSize);
  DXVA_Slice_H264_Short slice{};
  slice.SliceBytesInBuffer = UINT(d.bitstream.size());
  d.bitstream.resize((d.bitstream.size() + 127) / 128 * 128, 0);
  check(d.videoContext->DecoderBeginFrame(d.decoder.Get(), d.views[target].Get(), 0, nullptr));
  HRESULT sent = S_OK;
  try {
    D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4];
    directSend(d, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &pp, sizeof pp, buffers[0]);
    directSend(d, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &flat, sizeof flat, buffers[1]);
    directSend(d, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, d.bitstream.data(), UINT(d.bitstream.size()), buffers[2]);
    directSend(d, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &slice, sizeof slice, buffers[3]);
    buffers[2].NumMBsInBuffer = buffers[3].NumMBsInBuffer = d.sps.widthMbs * d.sps.heightMbs;
    sent = d.videoContext->SubmitDecoderBuffers(d.decoder.Get(), 4, buffers);
  } catch (HRESULT error) {
    sent = error;
  }
  HRESULT ended = d.videoContext->DecoderEndFrame(d.decoder.Get());
  check(sent);
  check(ended);
  d.decoded = target;
  d.hasReference = true;
  d.referencePoc = poc;
  d.referenceFrameNum = frameNum;
  d.next = (index + 1) % d.samples.size();
  const LONGLONG pts = LONGLONG(index * 1e7 * d.delta / d.timescale + 0.5);
  if (d.started)
    directTime += index ? pts - directPts : directFrameTime;
  d.started = true;
  directPts = pts;
  directPending = true;
}
ID3D11Texture2D *directTexture() { return direct->surfaces.Get(); }
UINT directPendingIndex() { return direct->decoded; }
// The last frame handed to the screen, or the first decoded one before any was shown.
UINT directStillIndex() { return direct->hasShown ? direct->shown : direct->decoded; }
void directShow() {
  direct->shown = direct->decoded;
  direct->hasShown = true;
  directPending = false;
}
void directClose() {
  if (direct && direct->file != INVALID_HANDLE_VALUE)
    CloseHandle(direct->file);
  direct.reset();
  directActive = directPending = false;
}
// False when the file is outside the simple form or this GPU cannot decode it this way.
bool directOpen(const fs::path &path, double start) {
  directClose();
  try {
    direct = std::make_unique<DirectState>();
    DirectState &d = *direct;
    d.file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (d.file == INVALID_HANDLE_VALUE)
      throw E_NOTIMPL;
    LARGE_INTEGER fileSize{};
    check(GetFileSizeEx(d.file, &fileSize) ? S_OK : E_FAIL);
    std::vector<BYTE> index;
    for (ULONGLONG at = 0; at + 8 <= ULONGLONG(fileSize.QuadPart);) {
      BYTE header[16]{};
      OVERLAPPED where{};
      where.Offset = DWORD(at);
      where.OffsetHigh = DWORD(at >> 32);
      DWORD got = 0;
      if (!ReadFile(d.file, header, 16, &got, &where) || got < 8)
        throw E_INVALIDARG;
      DirectBox top{header, got};
      ULONGLONG size = top.u32(0), skip = 8;
      if (size == 1) {
        size = top.u64(8);
        skip = 16;
      } else if (!size)
        size = ULONGLONG(fileSize.QuadPart) - at;
      if (size < skip || size > ULONGLONG(fileSize.QuadPart) - at)
        throw E_INVALIDARG;
      if (!memcmp(header + 4, "moov", 4)) {
        if (size - skip > 64u << 20)
          throw E_NOTIMPL;
        index.resize(size_t(size - skip));
        where.Offset = DWORD(at + skip);
        where.OffsetHigh = DWORD((at + skip) >> 32);
        if (!ReadFile(d.file, index.data(), DWORD(index.size()), &got, &where) || got != index.size())
          throw E_INVALIDARG;
        break;
      }
      at += size;
    }
    DirectBox movie{index.data(), index.size()}, table;
    UINT timescale = 0;
    for (size_t at = 0; at + 8 <= movie.length && !table.bytes;) {
      UINT size = movie.u32(at);
      if (size < 8 || size > movie.length - at)
        throw E_INVALIDARG;
      if (!memcmp(movie.need(at + 4, 4), "trak", 4)) {
        DirectBox media = DirectBox{movie.bytes + at + 8, size - 8}.child("mdia");
        DirectBox handler = media.child("hdlr"), header = media.child("mdhd");
        if (handler.bytes && header.bytes && !memcmp(handler.need(8, 4), "vide", 4)) {
          timescale = header.u32(header.need(0, 1)[0] == 1 ? 20 : 12);
          table = media.child("minf").child("stbl");
        }
      }
      at += size;
    }
    if (!table.bytes || !timescale || table.child("ctts").bytes)
      throw E_NOTIMPL;
    DirectBox description = table.child("stsd");
    if (description.u32(4) != 1 || memcmp(description.need(12, 4), "avc1", 4))
      throw E_NOTIMPL;
    DirectBox setup = description.from(16 + 78).child("avcC");
    d.lengthBytes = (setup.need(4, 1)[0] & 3) + 1;
    if ((setup.need(5, 1)[0] & 31) != 1)
      throw E_NOTIMPL;
    UINT spsSize = setup.u16(6);
    directSps(setup.need(8, spsSize), spsSize, d.sps);
    if (setup.need(8 + spsSize, 1)[0] != 1)
      throw E_NOTIMPL;
    UINT ppsSize = setup.u16(9 + spsSize);
    directPps(setup.need(11 + spsSize, ppsSize), ppsSize, d.pps);
    DirectBox times = table.child("stts"), sizes = table.child("stsz"), chunks = table.child("stsc");
    DirectBox offsets32 = table.child("stco"), offsets64 = table.child("co64"), syncs = table.child("stss");
    UINT delta = times.u32(12);
    for (UINT i = 1; i < times.u32(4); i++)
      if (times.u32(8 + 8 * i + 4) != delta)
        throw E_NOTIMPL;
    UINT fixedSize = sizes.u32(4), count = sizes.u32(8);
    UINT chunkCount = (offsets32.bytes ? offsets32 : offsets64).u32(4), runs = chunks.u32(4);
    if (!delta || !count || count > 1u << 22 || !chunkCount || !runs)
      throw E_NOTIMPL;
    d.delta = delta;
    d.timescale = timescale;
    d.samples.reserve(count);
    UINT run = 0;
    for (UINT chunk = 1; chunk <= chunkCount && d.samples.size() < count; chunk++) {
      while (run + 1 < runs && chunks.u32(8 + 12 * (run + 1)) <= chunk)
        ++run;
      ULONGLONG at = offsets32.bytes ? offsets32.u32(8 + 4 * (chunk - 1)) : offsets64.u64(8 + 8 * (chunk - 1));
      for (UINT i = chunks.u32(8 + 12 * run + 4); i && d.samples.size() < count; i--) {
        UINT size = fixedSize ? fixedSize : sizes.u32(12 + 4 * UINT(d.samples.size()));
        if (size > 64u << 20 || at + size > ULONGLONG(fileSize.QuadPart))
          throw E_INVALIDARG;
        d.samples.push_back({at, size, !syncs.bytes});
        at += size;
      }
    }
    if (d.samples.size() != count)
      throw E_INVALIDARG;
    for (UINT i = 0; syncs.bytes && i < syncs.u32(4); i++) {
      UINT number = syncs.u32(8 + 4 * i);
      if (number && number <= count)
        d.samples[number - 1].sync = true;
    }
    if (!d.samples[0].sync)
      throw E_NOTIMPL;
    directWidth = d.sps.widthMbs * 16 - d.sps.cropRight;
    directHeight = d.sps.heightMbs * 16 - d.sps.cropBottom;
    directRate = double(timescale) / delta;
    directFrameTime = LONGLONG(1e7 * delta / timescale + 0.5);
    directDuration = double(count) * delta / timescale;
    bool bt601 = d.sps.matrix == 5 || d.sps.matrix == 6 || (d.sps.matrix != 1 && directHeight < 720);
    directColorSpace = bt601 ? (d.sps.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                                                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601)
                             : (d.sps.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    check(device.As(&d.videoDevice));
    check(context.As(&d.videoContext));
    D3D11_VIDEO_DECODER_DESC desc{D3D11_DECODER_PROFILE_H264_VLD_NOFGT, d.sps.widthMbs * 16, d.sps.heightMbs * 16,
                                  DXGI_FORMAT_NV12};
    UINT configs = 0;
    check(d.videoDevice->GetVideoDecoderConfigCount(&desc, &configs));
    D3D11_VIDEO_DECODER_CONFIG config{};
    bool found = false;
    for (UINT i = 0; i < configs && !found; i++) {
      check(d.videoDevice->GetVideoDecoderConfig(&desc, i, &config));
      found = config.ConfigBitstreamRaw == 2;
    }
    if (!found)
      throw E_NOTIMPL;
    check(d.videoDevice->CreateVideoDecoder(&desc, &config, &d.decoder));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.SampleWidth;
    td.Height = desc.SampleHeight;
    td.MipLevels = 1;
    td.ArraySize = 2;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DECODER;
    check(device->CreateTexture2D(&td, nullptr, &d.surfaces));
    for (UINT i = 0; i < 2; i++) {
      D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view{};
      view.DecodeProfile = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
      view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
      view.Texture2D.ArraySlice = i;
      check(d.videoDevice->CreateVideoDecoderOutputView(d.surfaces.Get(), &view, &d.views[i]));
    }
    // Start at the first frame at or after the saved position, decoding forward from its key frame.
    size_t wanted = size_t(std::max(0.0, std::ceil(start * timescale / delta - 0.001)));
    if (wanted >= count)
      wanted = 0;
    size_t key = wanted;
    while (!d.samples[key].sync)
      --key;
    for (d.next = key; d.next <= wanted && d.next >= key;) {
      directDecode();
      if (d.next == 0)
        break;
    }
    directTime = directPts;
    directActive = true;
    return true;
  } catch (...) {
    directClose();
    return false;
  }
}
