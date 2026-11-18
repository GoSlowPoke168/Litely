#include <propkey.h>
#include <mfreadwrite.h>
#include <regex>
#include <set>
#include <sstream>
#include <uxtheme.h>
#pragma comment(lib,"mfreadwrite.lib")

// Native library resources exist only while this window is open.
HWND libraryWindow = nullptr, libraryList = nullptr;
bool settingsPage = false;
std::vector<fs::path> libraryFiles;
HFONT uiFont = nullptr;
HBRUSH uiBrush = nullptr;
ULONG_PTR graphicsToken = 0;
std::unique_ptr<Gdiplus::Bitmap> previewImage;
std::wstring statsText = L"Measuring...";
double lastCPU = 0;
ULONGLONG lastSample = 0;
float previewOpacity = 1;
// True while the Library panel is moving: images are scaled quickly and tiles stretched instead of redrawn.
#pragma comment(lib,"winmm.lib")
#pragma comment(lib,"msimg32.lib")
bool fastDraw = false;
float barFade = 1;
// While the panel moves, paints reuse one image each instead of allocating a full-window image per frame.
struct Canvas { HBITMAP bitmap = nullptr; int wide = 0, tall = 0; };
Canvas canvases[2];
HBITMAP canvasFor(int slot, HDC reference, int wide, int tall, bool &owned) {
  owned = !fastDraw;
  if (owned)
    return CreateCompatibleBitmap(reference, wide, tall);
  auto &c = canvases[slot];
  if (!c.bitmap || c.wide < wide || c.tall < tall) {
    if (c.bitmap)
      DeleteObject(c.bitmap);
    c = {CreateCompatibleBitmap(reference, std::max(wide, c.wide), std::max(tall, c.tall)), std::max(wide, c.wide), std::max(tall, c.tall)};
  }
  return c.bitmap;
}
void releaseCanvases() {
  for (auto &c : canvases) {
    if (c.bitmap)
      DeleteObject(c.bitmap);
    c = {};
  }
}
COLORREF mixColor(COLORREF from, COLORREF to, double t) {
  t = std::clamp(t, 0., 1.);
  return RGB(GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t, GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t,
             GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t);
}
int libraryDpi = 96;
int px(int v){return MulDiv(v,libraryDpi,96);}
int dip(int v){return MulDiv(v,96,libraryDpi);}
RECT units(RECT r){return {dip(r.left),dip(r.top),dip(r.right),dip(r.bottom)};}
struct UiDC {
  HDC dc; int saved;
  UiDC(HDC h):dc(h),saved(SaveDC(h)){
    SetMapMode(h,MM_ANISOTROPIC);
    SetWindowExtEx(h,96,96,nullptr);SetViewportExtEx(h,libraryDpi,libraryDpi,nullptr);
  }
  ~UiDC(){RestoreDC(dc,saved);}
};
constexpr COLORREF cWindow=RGB(25,27,37),cSidebar=RGB(23,25,35),cRaised=RGB(38,40,54),
  cHover=RGB(42,43,57),cLine=RGB(52,53,65),cInk=RGB(36,27,53),
  cText=RGB(245,243,250),cSoft=RGB(217,215,225),cMuted=RGB(172,174,191),cDisabled=RGB(121,115,134);
// The accent and everything tinted by it; setAccent derives the family from one light colour.
constexpr COLORREF defaultAccent=RGB(215,196,255);
COLORREF cAccent,cAccentHot,cAccentDown,cAccentOff,cAccentPress,cSelected,cSelectedLine;
void setAccent(COLORREF a){
  cAccent=a;
  cAccentHot=mixColor(a,RGB(255,255,255),.3);
  cAccentDown=mixColor(a,RGB(0,0,0),.1);
  cAccentOff=mixColor(cWindow,a,.25);
  cAccentPress=mixColor(cWindow,a,.17);
  cSelected=mixColor(cWindow,a,.12);
  cSelectedLine=mixColor(cWindow,a,.3);
}
const bool accentReady=(setAccent(defaultAccent),true);
constexpr COLORREF accentPresets[8]={RGB(215,196,255),RGB(169,199,255),RGB(143,224,214),RGB(174,227,162),
                                     RGB(246,211,138),RGB(255,181,156),RGB(255,177,193),RGB(245,179,236)};
int accentMode=0;
RECT swatchRect(int k);
COLORREF accentPick=defaultAccent;
bool fontExists(const wchar_t *family){
  HDC dc=GetDC(nullptr);LOGFONT query{};query.lfCharSet=DEFAULT_CHARSET;wcscpy_s(query.lfFaceName,family);
  bool found=false;
  EnumFontFamiliesEx(dc,&query,[](const LOGFONT*,const TEXTMETRIC*,DWORD,LPARAM out){*(bool*)out=true;return 0;},(LPARAM)&found,0);
  ReleaseDC(nullptr,dc);return found;
}
const wchar_t *textFace(){static auto face=fontExists(L"Segoe UI Variable Text")?L"Segoe UI Variable Text":L"Segoe UI";return face;}
const wchar_t *displayFace(){static auto face=fontExists(L"Segoe UI Variable Display")?L"Segoe UI Variable Display":L"Segoe UI";return face;}
const wchar_t *iconFace(){static auto face=fontExists(L"Segoe Fluent Icons")?L"Segoe Fluent Icons":L"Segoe MDL2 Assets";return face;}
struct DpiScope{int old;DpiScope(int dpi):old(libraryDpi){libraryDpi=dpi;}~DpiScope(){libraryDpi=old;}};
// GDI+ drawing in device pixels with anti-aliasing; callers pass 96-dpi layout units.
struct DeviceDC{HDC dc;int saved;DeviceDC(HDC h):dc(h),saved(SaveDC(h)){SetMapMode(h,MM_TEXT);}~DeviceDC(){RestoreDC(dc,saved);}};
struct Smooth:DeviceDC{
  Gdiplus::Graphics g;
  Smooth(HDC h):DeviceDC(h),g(h){g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);}
};
float fpx(float v){return v*libraryDpi/96.f;}
Gdiplus::RectF frect(RECT r){return {fpx((float)r.left),fpx((float)r.top),fpx((float)(r.right-r.left)),fpx((float)(r.bottom-r.top))};}
Gdiplus::Color gdip(COLORREF c,BYTE alpha=255){return Gdiplus::Color(alpha,GetRValue(c),GetGValue(c),GetBValue(c));}
void roundPath(Gdiplus::GraphicsPath &path,Gdiplus::RectF r,float radius){
  float d=std::min({radius*2,r.Width,r.Height});
  path.AddArc(r.X,r.Y,d,d,180,90);path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
  path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90);path.AddArc(r.X,r.GetBottom()-d,d,d,90,90);
  path.CloseFigure();
}
void fillRound(HDC dc,RECT r,COLORREF color,float radius,BYTE alpha=255){
  Smooth s(dc);Gdiplus::GraphicsPath path;roundPath(path,frect(r),fpx(radius));
  Gdiplus::SolidBrush brush(gdip(color,alpha));s.g.FillPath(&brush,&path);
}
void strokeRound(HDC dc,RECT r,COLORREF color,float radius,float thickness,BYTE alpha=255){
  Smooth s(dc);auto box=frect(r);float w=fpx(thickness);box.Inflate(-w/2,-w/2);
  Gdiplus::GraphicsPath path;roundPath(path,box,fpx(radius)-w/2);
  Gdiplus::Pen pen(gdip(color,alpha),w);s.g.DrawPath(&pen,&path);
}
// Paints the background colour outside a rounded rectangle so image corners are anti-aliased.
void roundCorners(HDC dc,RECT r,float radius,COLORREF background){
  Smooth s(dc);auto box=frect(r);Gdiplus::GraphicsPath path(Gdiplus::FillModeAlternate);
  path.AddRectangle(box);roundPath(path,box,fpx(radius));
  Gdiplus::SolidBrush brush(gdip(background));s.g.FillPath(&brush,&path);
}
void drawCover(HDC dc,Gdiplus::Image *image,RECT r,float opacity=1){
  Smooth s(dc);auto box=frect(r);
  float iw=(float)image->GetWidth(),ih=(float)image->GetHeight();
  float scale=std::max(box.Width/iw,box.Height/ih),sw=box.Width/scale,sh=box.Height/scale;
  if(fastDraw)s.g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
  Gdiplus::ColorMatrix matrix={1,0,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0,opacity,0,0,0,0,0,1};
  Gdiplus::ImageAttributes attributes;
  if(opacity<.999f)attributes.SetColorMatrix(&matrix);
  attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
  s.g.DrawImage(image,box,(iw-sw)/2,(ih-sh)/2,sw,sh,Gdiplus::UnitPixel,&attributes);
}
int paintText(HDC dc, const std::wstring &text, RECT rect, int size,
               COLORREF color, int weight=FW_NORMAL, UINT flags=DT_LEFT|DT_TOP|DT_SINGLELINE,
               const wchar_t *family=nullptr) {
  // Measure and rasterize in device pixels so fractional DPI doesn't round glyph advances in layout units.
  DeviceDC deviceDc(dc);
  rect={px(rect.left),px(rect.top),px(rect.right),px(rect.bottom)};
  if(!family)family=size>=24?displayFace():textFace();
  HFONT font =
      CreateFont(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, family);
  auto old = SelectObject(dc, font);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  SIZE extent{};GetTextExtentPoint32(dc,text.c_str(),(int)text.size(),&extent);
  DrawText(dc,text.c_str(),-1,&rect,flags|DT_END_ELLIPSIS|DT_NOPREFIX);
  SelectObject(dc, old);
  DeleteObject(font);
  return dip(extent.cx);
}
void paintIcon(HDC dc,const wchar_t *glyph,RECT box,int size,COLORREF color){
  paintText(dc,glyph,box,size,color,FW_NORMAL,DT_CENTER|DT_VCENTER|DT_SINGLELINE,iconFace());
}
void focusRing(HDC dc,RECT r,float radius){strokeRound(dc,r,cAccent,radius,2);}
std::vector<std::unique_ptr<Gdiplus::Bitmap>> rowImages;
RECT titleBox{};
bool titleHot=false;
std::wstring cpuValue=L"—",ramValue=L"—",gpuValue=L"—";
int hoveredRow = -1, selectedRow = -1;
// The Library panel's width in layout units; it can be dragged wider or fill the window.
int sideWidth = 236, sideLast = 236;
bool sideFull = false, mainOff = false;
bool gridMode() { return sideWidth >= 400; }
int heroBottom(int widthDip, int heightDip) {
  return std::min(heightDip - 212, 92 + MulDiv(widthDip - sideWidth - 56, 100, 197));
}
std::vector<std::wstring> videoDetails;
std::wstring cleanName(std::wstring name){
  for(auto &c:name)if(c==L'-'||c==L'_'||c==L'.')c=L' ';
  static const std::wregex tag(LR"(\d{3,4}p|\d{3,5}x\d{3,5}|\d{2,3}fps|original|[48]k|u?hd|fhd)",std::regex::icase);
  static const std::wregex glued(LR"((.*[a-z])([48]k|u?hd|fhd))",std::regex::icase);
  static const std::set<std::wstring> minor{L"a",L"an",L"and",L"at",L"by",L"for",L"in",L"of",L"on",L"the",L"to"};
  std::wistringstream words(name);std::wstring word,out;
  while(words>>word){
    if(std::regex_match(word,tag))continue;
    std::wsmatch parts;if(std::regex_match(word,parts,glued))word=parts[1];
    bool lower=std::none_of(word.begin(),word.end(),[](wchar_t c){return iswupper(c)!=0;});
    if(lower && (out.empty() || !minor.count(word)))word[0]=towupper(word[0]);
    out+=(out.empty()?L"":L" ")+word;
  }
  return out.empty()?name:out;
}
std::wstring readVideoDetails(const fs::path &path,DWORD *widthOut=nullptr,DWORD *heightOut=nullptr,DWORD *rateOut=nullptr,ULONGLONG *durationOut=nullptr) {
  ComPtr<IPropertyStore> properties;
  std::wstring details;
  DWORD frameWidth = 0, frameHeight = 0, rate = 0;
  ULONGLONG duration = 0;
  if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                                  IID_PPV_ARGS(&properties)))) {
    auto number = [&](REFPROPERTYKEY key) {
      PROPVARIANT value{};
      DWORD result = 0;
      if (SUCCEEDED(properties->GetValue(key, &value)) && value.vt == VT_UI4)
        result = value.ulVal;
      PropVariantClear(&value);
      return result;
    };
    frameWidth = number(PKEY_Video_FrameWidth);
    frameHeight = number(PKEY_Video_FrameHeight);
    rate = number(PKEY_Video_FrameRate);
    PROPVARIANT value{};
    if(SUCCEEDED(properties->GetValue(PKEY_Media_Duration,&value)) && value.vt==VT_UI8)duration=value.uhVal.QuadPart;
    PropVariantClear(&value);
  }
  // Windows' file properties miss some formats, such as GIF and WebM.
  if (!frameWidth || !frameHeight || !rate) {
    DWORD w = 0, h = 0, r = 0;
    ULONGLONG d = 0;
    if (videoFacts(path, w, h, r, d)) {
      if (!frameWidth || !frameHeight) frameWidth = w, frameHeight = h;
      if (!rate) rate = r;
      if (!duration) duration = d;
    }
  }
  if(widthOut)*widthOut=frameWidth;
  if(heightOut)*heightOut=frameHeight;
  if(rateOut)*rateOut=rate;
  if(durationOut)*durationOut=duration;
  if (frameHeight) details = std::to_wstring(frameHeight) + L"p";
  if (rate) {
    wchar_t fps[32];
    swprintf_s(fps, rate % 1000 ? L"%.2f" : L"%.0f", rate / 1000.);
    details += (details.empty()?L"":L" · ") + std::wstring(fps) + L" fps";
  }
  return details.empty()?L"Video":details;
}
bool isPlaybackCopy(const fs::path &video){
  auto copies=(folder/L"optimized").wstring()+L"\\";
  return !_wcsnicmp(video.c_str(),copies.c_str(),copies.size());
}
void menu(const POINT *anchor=nullptr);
std::wstring displayName(const fs::path &p){
  wchar_t custom[256]{};
  auto key=posterPath(p).stem().wstring();
  GetPrivateProfileString(L"names",key.c_str(),L"",custom,256,(folder/L"library-names.ini").c_str());
  if(*custom)return custom;
  return cleanName(p.stem().wstring());
}
fs::path thumbnailPath(const fs::path &video){
  auto path=posterPath(video);
  path.replace_extension(L".preview.bmp");
  return path;
}
fs::path artworkPath(const fs::path &video){
  auto thumbnail=thumbnailPath(video);
  return fs::exists(thumbnail)?thumbnail:posterPath(video);
}
void panel(HDC dc,RECT r,COLORREF color){
  HBRUSH b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);
}
void fillRect(HDC dc,RECT r,COLORREF color){UiDC mapping(dc);panel(dc,r,color);}

void updateBattery() {
  SYSTEM_POWER_STATUS power{};
  bool next = batteryRule && GetSystemPowerStatus(&power) &&
              power.ACLineStatus == 0 && power.SystemStatusFlag == 1;
  if (next == batteryOverride)
    return;
  batteryOverride = next;
  if (next) {
    if (!paused && !parked && ready && presented && !applyingPause)
      freeze(false);
    else if (!applyingPause)
      closePlayer();
    status(L"Litely - paused for Energy Saver");
  } else if (!paused && !applyingPause)
    visibility();
}
void saveLibrary() {
  if (folder.empty())
    return;
  auto temp = folder / L"library.tmp", dest = folder / L"library.txt";

  std::ofstream bytes(temp, std::ios::binary);
  const wchar_t bom = 0xfeff;
  bytes.write((const char *)&bom, sizeof bom);
  for (auto &path : libraryFiles) {
    auto line = path.wstring() + L"\n";
    bytes.write((const char *)line.data(), line.size() * sizeof(wchar_t));
  }
  bytes.close();
  if (!bytes ||
      !MoveFileEx(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
    throw E_FAIL;
}
void addLibrary(const fs::path &path) {
  if (path.empty())
    return;
  auto canonical = fs::weakly_canonical(path);
  for (auto &p : libraryFiles)
    if (!_wcsicmp(p.c_str(), canonical.c_str()))
      return;
  libraryFiles.push_back(canonical);
}
int selectedIndex() {
  return selectedRow < (int)libraryFiles.size() ? selectedRow : -1;
}
std::unique_ptr<Gdiplus::Bitmap> readArtwork(const fs::path &path,bool thumbnail=false){
  Gdiplus::Bitmap file(path.c_str());
  if(file.GetLastStatus()!=Gdiplus::Ok)return {};
  if(thumbnail){
    auto thumb=std::make_unique<Gdiplus::Bitmap>(106,88,PixelFormat32bppPARGB);
    Gdiplus::Graphics g(thumb.get());
    float sw=(float)file.GetWidth(),sh=(float)file.GetHeight(),scale=std::min(sw/106,sh/88);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    if(g.DrawImage(&file,Gdiplus::RectF(0,0,106,88),(sw-106*scale)/2,(sh-88*scale)/2,106*scale,88*scale,Gdiplus::UnitPixel)!=Gdiplus::Ok)return {};
    return thumb;
  }
  return std::unique_ptr<Gdiplus::Bitmap>(file.Clone(0,0,file.GetWidth(),file.GetHeight(),PixelFormat32bppPARGB));
}
constexpr UINT PreviewReady=WM_APP+30;
unsigned previewRequest=0;
void requestPreview(const fs::path &video){
  auto path=thumbnailPath(video);
  if(fs::exists(path))return;
  unsigned request=++previewRequest;
  HWND window=libraryWindow;
  std::thread([video,path,request,window]{
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return;
    try{
      ComPtr<IShellItemImageFactory> factory;
      HBITMAP image=nullptr;
      if(SUCCEEDED(SHCreateItemFromParsingName(video.c_str(),nullptr,IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->GetImage({768,432},SIIGBF_THUMBNAILONLY,&image))){
        Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;
        if(Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok){
          {
            Gdiplus::Bitmap bitmap(image,nullptr);
            CLSID encoder{0x557cf400,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};
            auto temp=path;temp+=L".tmp";
            if(bitmap.Save(temp.c_str(),&encoder,nullptr)==Gdiplus::Ok)
              MoveFileEx(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
          }
          Gdiplus::GdiplusShutdown(token);
        }
        DeleteObject(image);
      }
      if(!fs::exists(path)){
        check(MFStartup(MF_VERSION));
        struct MediaLifetime { ~MediaLifetime(){MFShutdown();} } mediaLifetime;
        ComPtr<IMFAttributes> attributes;
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFMediaType> type,actual;
        check(MFCreateAttributes(&attributes,1));
        check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,TRUE));
        check(MFCreateSourceReaderFromURL(video.c_str(),attributes.Get(),&reader));
        check(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS,FALSE));
        check(reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,TRUE));
        check(MFCreateMediaType(&type));
        check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));
        check(type->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32));
        check(reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,nullptr,type.Get()));
        check(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,&actual));
        UINT frameWidth=0,frameHeight=0;
        check(MFGetAttributeSize(actual.Get(),MF_MT_FRAME_SIZE,&frameWidth,&frameHeight));
        ComPtr<IMFSample> sample;
        for(int attempt=0;attempt<100 && !sample;attempt++){
          DWORD flags=0;
          check(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,0,nullptr,&flags,nullptr,&sample));
          if(flags&MF_SOURCE_READERF_ENDOFSTREAM)break;
        }
        if(sample){
          ComPtr<IMFMediaBuffer> buffer;check(sample->ConvertToContiguousBuffer(&buffer));
          BYTE *pixels=nullptr;DWORD length=0;
          check(buffer->Lock(&pixels,nullptr,&length));
          LONG stride=(LONG)MFGetAttributeUINT32(actual.Get(),MF_MT_DEFAULT_STRIDE,frameWidth*4);
          if(frameWidth && frameHeight && (unsigned long long)abs(stride)*frameHeight<=length){
            BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};
            info.biSize=sizeof info;info.biWidth=frameWidth;info.biHeight=stride<0?(LONG)frameHeight:-(LONG)frameHeight;
            info.biPlanes=1;info.biBitCount=32;
            header.bfType=0x4d42;header.bfOffBits=sizeof header+sizeof info;
            header.bfSize=header.bfOffBits+frameWidth*frameHeight*4;
            auto temp=path;temp+=L".tmp";
            std::ofstream out(temp,std::ios::binary);
            out.write((char*)&header,sizeof header);out.write((char*)&info,sizeof info);
            for(UINT row=0;row<frameHeight;row++)out.write((char*)pixels+row*abs(stride),frameWidth*4);
            out.close();
            if(out)MoveFileEx(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING);
          }
          buffer->Unlock();
        }
      }
    }catch(HRESULT error){
      std::ofstream log(path.wstring()+L".error");
      log<<std::hex<<(unsigned)error;
    }catch(...){}
    CoUninitialize();
    PostMessage(window,PreviewReady,request,0);
  }).detach();
}
// Shared look for radio rows (Settings) and radio chips (Import): anti-aliased indicator, label, optional description.
void drawChoice(HDC dc,RECT r,const std::wstring &label,const wchar_t *description,bool checked,bool checkbox,
                bool enabled,bool hot,bool focus,COLORREF background){
  fillRect(dc,r,background);
  if(hot && enabled)fillRound(dc,r,cHover,8);
  int top=description?r.top+10:(r.top+r.bottom-18)/2;
  RECT box{r.left+10,top,r.left+28,top+18};
  auto ring=!enabled?cDisabled:checked?cAccent:RGB(142,139,157);
  if(checkbox){
    if(checked)fillRound(dc,box,cAccent,4);else strokeRound(dc,box,ring,4,1.5f);
    if(checked)paintIcon(dc,L"",box,12,cInk);
  }else{
    strokeRound(dc,box,ring,9,1.5f);
    if(checked){RECT dot=box;InflateRect(&dot,-5,-5);fillRound(dc,dot,enabled?cAccent:cDisabled,4);}
  }
  auto ink=enabled?cText:cDisabled;
  if(description){
    paintText(dc,label,{r.left+40,r.top+8,r.right-12,r.top+30},14,ink,FW_SEMIBOLD);
    paintText(dc,description,{r.left+40,r.top+31,r.right-12,r.bottom-2},13,enabled?cMuted:cDisabled,FW_NORMAL,DT_LEFT|DT_WORDBREAK);
  }else paintText(dc,label,{r.left+36,r.top,r.right-6,r.bottom},13,ink,FW_NORMAL,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
  if(focus)focusRing(dc,r,8);
}
bool keyboardCues(HWND control){return !(SendMessage(control,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);}
bool hovered(HWND control){POINT p;GetCursorPos(&p);return WindowFromPoint(p)==control;}
struct ButtonStyle{bool primary;COLORREF background;const wchar_t *icon=nullptr;bool left=false;COLORREF tint=CLR_INVALID;bool outline=false;};
void drawButton(HDC dc,const DRAWITEMSTRUCT &d,RECT r,const ButtonStyle &style){
  bool down=(d.itemState&ODS_SELECTED)!=0,disabled=(d.itemState&ODS_DISABLED)!=0,hot=!disabled && hovered(d.hwndItem);
  fillRect(dc,r,style.background);
  if(style.primary)fillRound(dc,r,disabled?cAccentOff:down?cAccentDown:hot?cAccentHot:cAccent,7);
  else if(down || hot)fillRound(dc,r,down?cAccentPress:cHover,7);
  if(style.outline && !disabled)strokeRound(dc,r,cLine,7,1);
  wchar_t text[128];GetWindowText(d.hwndItem,text,128);
  auto ink=disabled?cDisabled:style.primary?cInk:style.tint!=CLR_INVALID?style.tint:cSoft;
  RECT label=r;
  UINT align=DT_CENTER;
  if(style.icon){
    int start=r.left+12;
    if(!style.left){
      HDC measure=CreateCompatibleDC(dc);
      int textWidth=paintText(measure,text,{0,0,0,0},14,ink,FW_NORMAL,DT_CALCRECT|DT_SINGLELINE);
      DeleteDC(measure);start=(r.left+r.right-textWidth-24)/2;
    }
    paintIcon(dc,style.icon,{start,r.top,start+16,r.bottom},14,ink);
    label.left=start+24;align=DT_LEFT;
  }
  paintText(dc,text,label,14,ink,style.primary?FW_SEMIBOLD:FW_NORMAL,align|DT_VCENTER|DT_SINGLELINE);
  if((d.itemState&ODS_FOCUS) && !(d.itemState&ODS_NOFOCUSRECT) && keyboardCues(d.hwndItem))focusRing(dc,r,7);
}
LRESULT CALLBACK uiButtonProc(HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR);
HBRUSH fieldBrush(){static HBRUSH brush=CreateSolidBrush(cRaised);return brush;}
HBRUSH windowBrush(){static HBRUSH brush=CreateSolidBrush(cWindow);return brush;}
void placeDip(HWND dialog,int id,int x,int y,int span,int tall){
  SetWindowPos(GetDlgItem(dialog,id),nullptr,px(x),px(y),px(span),px(tall),SWP_NOZORDER|SWP_NOACTIVATE);
}
void sizeDialog(HWND dialog,int clientWidth,int clientHeight){
  RECT frame{0,0,px(clientWidth),px(clientHeight)};
  AdjustWindowRectExForDpi(&frame,GetWindowLong(dialog,GWL_STYLE),FALSE,GetWindowLong(dialog,GWL_EXSTYLE),libraryDpi);
  RECT owner;GetWindowRect(GetParent(dialog),&owner);
  int w=frame.right-frame.left,h=frame.bottom-frame.top;
  SetWindowPos(dialog,nullptr,(owner.left+owner.right-w)/2,(owner.top+owner.bottom-h)/2,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
}
void themeWindow(HWND window){
  BOOL dark=TRUE;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof dark);
  COLORREF caption=cWindow;DwmSetWindowAttribute(window,35,&caption,sizeof caption);
}
using DialogPainter=void(*)(HWND,HDC);
// Common dialog behavior; painter draws the dialog's own text in 96-dpi units.
INT_PTR dialogTheme(HWND dialog,UINT message,WPARAM a,LPARAM b,DialogPainter painter=nullptr){
  DpiScope scale(GetDpiForWindow(dialog));
  if(message==WM_INITDIALOG){
    themeWindow(dialog);
    for(HWND child=GetWindow(dialog,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SetWindowSubclass(child,uiButtonProc,1,0);
  }
  if(message==WM_CTLCOLORDLG || message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN){
    SetTextColor((HDC)a,cText);SetBkColor((HDC)a,cWindow);return (INT_PTR)windowBrush();
  }
  if(message==WM_CTLCOLOREDIT){SetTextColor((HDC)a,cText);SetBkColor((HDC)a,cRaised);return (INT_PTR)fieldBrush();}
  if(message==WM_ERASEBKGND)return TRUE;
  if(message==WM_PAINT){
    PAINTSTRUCT ps;HDC screen=BeginPaint(dialog,&ps);
    RECT client;GetClientRect(dialog,&client);
    HDC dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,client.right,client.bottom);
    auto old=SelectObject(dc,bitmap);
    FillRect(dc,&client,windowBrush());
    for(HWND child=GetWindow(dialog,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
      wchar_t cls[16];GetClassName(child,cls,16);
      if(_wcsicmp(cls,L"Edit"))continue;
      RECT field;GetWindowRect(child,&field);MapWindowPoints(nullptr,dialog,(POINT*)&field,2);
      field=units(field);InflateRect(&field,10,8);
      fillRound(dc,field,cRaised,7);
      strokeRound(dc,field,GetFocus()==child?cAccent:cLine,7,1);
    }
    if(painter)painter(dialog,dc);
    BitBlt(screen,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    EndPaint(dialog,&ps);return TRUE;
  }
  if(message==WM_COMMAND && (HIWORD(a)==EN_SETFOCUS || HIWORD(a)==EN_KILLFOCUS))InvalidateRect(dialog,nullptr,FALSE);
  if(message==WM_DRAWITEM){
    auto &d=*(DRAWITEMSTRUCT*)b;
    if(d.CtlType!=ODT_BUTTON)return FALSE;
    HDC target=d.hDC;RECT bounds=d.rcItem;
    HDC dc=CreateCompatibleDC(target);HBITMAP bitmap=CreateCompatibleBitmap(target,bounds.right,bounds.bottom);
    auto old=SelectObject(dc,bitmap);
    drawButton(dc,d,units(bounds),{d.CtlID==IDOK,cWindow,nullptr,false,CLR_INVALID,d.CtlID!=IDOK});
    BitBlt(target,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
    return TRUE;
  }
  return FALSE;
}
const wchar_t *removeName=L"";
void paintRemove(HWND,HDC dc){
  paintText(dc,L"Remove “"+std::wstring(removeName)+L"” from your library?",{24,22,376,46},16,cText,FW_SEMIBOLD);
  paintText(dc,L"The video file stays where it is. You can add it again any time.",{24,52,376,96},13,cMuted,FW_NORMAL,DT_LEFT|DT_WORDBREAK);
}
INT_PTR CALLBACK removeProc(HWND w,UINT m,WPARAM a,LPARAM b){
  if(m==WM_INITDIALOG){
    removeName=reinterpret_cast<const wchar_t*>(b);
    DpiScope scale(GetDpiForWindow(w));
    sizeDialog(w,400,164);placeDip(w,IDCANCEL,280,108,96,36);placeDip(w,IDOK,176,108,96,36);
  }
  if(auto result=dialogTheme(w,m,a,b,paintRemove))return result;
  if(m==WM_INITDIALOG){SetFocus(GetDlgItem(w,IDCANCEL));return FALSE;}
  if(m==WM_COMMAND && (LOWORD(a)==IDOK || LOWORD(a)==IDCANCEL)){EndDialog(w,LOWORD(a));return TRUE;}
  return FALSE;
}
struct ImportJob {
  fs::path original, result;
  std::unique_ptr<Transcode> copy;
  DWORD width=0,height=0,rate=0;
  ULONGLONG duration=0,bytes=0;
  int percent=-1,tick=0;
};
ImportJob *activeImport=nullptr;
// GPU and RAM per decoded megapixel, fitted to 20 s Test-Playback.ps1 runs from 720p to 2160p.
double videoMemory(double megapixels){return 8+19.8*megapixels;}
double processMemory(double megapixels){return 62+9.5*megapixels;}
// The same for a lowest-memory copy on the direct decoder.
double directVideoMemory(double megapixels){return 8.6+6.8*megapixels;}
double directProcessMemory(double megapixels){return 56+7.7*megapixels;}
double importScale(const ImportJob &job,int limit){
  if(!limit || !job.width || !job.height)return 1;
  return std::min({1.,limit/(double)job.height,limit*16/9./job.width});
}
int chosenLimit(HWND w){return IsDlgButtonChecked(w,302)?1080:IsDlgButtonChecked(w,303)?720:0;}
bool convertChoice(HWND w){return chosenLimit(w) || IsDlgButtonChecked(w,306) || IsDlgButtonChecked(w,308);}
void paintImport(HWND w,HDC dc){
  auto &job=*activeImport;
  paintText(dc,job.original.filename().wstring(),{24,20,436,42},16,cText,FW_SEMIBOLD);
  std::wstring facts;
  auto add=[&](const std::wstring &fact){facts+=(facts.empty()?L"":L"  ·  ")+fact;};
  wchar_t text[160];
  if(job.width && job.height){swprintf_s(text,L"%lu × %lu",job.width,job.height);add(text);}
  if(job.rate){swprintf_s(text,job.rate%1000?L"%.2f fps":L"%.0f fps",job.rate/1000.);add(text);}
  if(job.duration){auto s=job.duration/10000000;swprintf_s(text,L"%llu:%02llu",s/60,s%60);add(text);}
  if(job.bytes){bool gb=job.bytes>=1000000000,mb=job.bytes>=1000000;swprintf_s(text,gb?L"%.1f GB":mb?L"%.0f MB":L"%.0f KB",job.bytes/(gb?1e9:mb?1e6:1e3));add(text);}
  paintText(dc,facts,{24,46,436,66},12,cMuted);
  paintText(dc,L"Resolution",{24,86,436,106},13,cSoft,FW_SEMIBOLD);
  paintText(dc,L"Frame rate",{24,156,436,176},13,cSoft,FW_SEMIBOLD);
  paintText(dc,L"Playback",{24,226,436,246},13,cSoft,FW_SEMIBOLD);
  bool lowest=IsDlgButtonChecked(w,308)!=0;
  RECT box{24,296,436,452};
  fillRound(dc,box,cRaised,10);
  paintText(dc,L"Estimated while playing, with the Library closed",{40,310,420,328},12,cMuted);
  if(!job.width || !job.height || !job.rate){
    paintText(dc,L"Windows couldn't read this video's size or frame rate, so Litely can't estimate savings.",{40,336,420,400},13,cSoft,FW_NORMAL,DT_LEFT|DT_WORDBREAK);
  }else{
    double scale=importScale(job,chosenLimit(w)),before=job.width*(double)job.height/1e6,after=before*scale*scale;
    double fps=job.rate/1000.,next=IsDlgButtonChecked(w,306)?std::min(30.,fps):fps;
    struct Row{const wchar_t *label;double from,to;const wchar_t *unit;};
    Row rows[]={{L"Video memory",videoMemory(before),lowest?directVideoMemory(after):videoMemory(after),L" MiB"},
                {L"RAM",processMemory(before),lowest?directProcessMemory(after):processMemory(after),L" MiB"},
                {L"Frames per second",fps,next,L""}};
    for(int k=0;k<3;k++){
      auto &row=rows[k];int y=336+k*38;
      paintText(dc,row.label,{40,y,240,y+18},13,cSoft);
      bool approx=*row.unit!=0;
      swprintf_s(text,approx?L"≈%.0f%s  →  ≈%.0f%s":L"%.0f%s  →  %.0f%s",row.from,row.unit,row.to,row.unit);
      if(row.to==row.from)swprintf_s(text,approx?L"≈%.0f%s":L"%.0f%s",row.from,row.unit);
      paintText(dc,text,{200,y,360,y+18},13,cText,FW_NORMAL,DT_RIGHT|DT_SINGLELINE);
      double ratio=row.from>0?row.to/row.from:1;
      if(ratio<.995)swprintf_s(text,L"−%.0f%%",(1-ratio)*100);else wcscpy_s(text,L"Same");
      paintText(dc,text,{364,y,420,y+18},13,ratio<.995?cAccent:cMuted,FW_SEMIBOLD,DT_RIGHT|DT_SINGLELINE);
      RECT track{40,y+23,420,y+28};
      fillRound(dc,track,cLine,2.5f);
      RECT fill=track;fill.right=fill.left+std::max(5,(int)((track.right-track.left)*ratio));
      fillRound(dc,fill,ratio<.995?cAccent:RGB(142,139,157),2.5f);
    }
  }
  RECT note{24,458,436,476};
  if(job.copy){
    if(job.percent>=0)swprintf_s(text,L"Converting…  %d%%",job.percent);else wcscpy_s(text,L"Converting…");
    paintText(dc,text,note,12,cSoft);
    RECT track{24,482,436,487};
    fillRound(dc,track,cLine,2.5f);
    RECT fill=track;
    if(job.percent>=0)fill.right=fill.left+std::max(5,(int)(track.right-track.left)*job.percent/100);
    else{int span=(track.right-track.left)/4,offset=(job.tick*12)%(track.right-track.left+span)-span;
         fill.left=std::max(track.left,track.left+offset);fill.right=std::min(track.right,track.left+offset+span);}
    if(fill.right>fill.left)fillRound(dc,fill,cAccent,2.5f);
  }else paintText(dc,lowest?L"Creates a larger copy that needs the least memory. Your original file is kept.":
                     convertChoice(w) || needsCopy(job.original)?L"Creates an H.264 playback copy in Litely's folder. Your original file is kept.":
                                     L"Plays your file directly. Nothing is copied.",note,12,cMuted);
}
void stopConversion(ImportJob &job,bool cancelled){
  if(job.copy){transcodeStop(*job.copy,cancelled);job.copy.reset();}
  std::error_code ignored;
  if(cancelled){
    auto dir=job.result.parent_path();
    std::vector<fs::path> scratch;
    for(auto &entry:fs::directory_iterator(dir,ignored))
      if(entry.is_directory(ignored))scratch.push_back(entry.path());
    for(auto &path:scratch)fs::remove_all(path,ignored);
    fs::remove(dir,ignored);fs::remove(dir.parent_path(),ignored);
  }
}
fs::path copyDirectory(const fs::path &original){
  return folder/L"optimized"/posterPath(original).stem()/std::to_wstring(fs::last_write_time(original).time_since_epoch().count());
}
bool batteryFps=false,batteryCopyFailed=false;
// The battery copy's worker thread; main.cpp waits on it.
HANDLE batteryCopyProcess=nullptr;
std::unique_ptr<Transcode> batteryCopy;
fs::path batteryCopyVideo;
bool lowestCopy(const fs::path &video){
  auto stem=video.stem().wstring();
  return stem.size()>7 && !stem.compare(stem.size()-7,7,L"-lowmem");
}
fs::path batteryCopyPath(const fs::path &video){
  std::error_code ignored;
  if(video.empty() || !fs::exists(video,ignored))return {};
  return copyDirectory(video)/(video.stem().wstring()+(lowestCopy(video)?L"-original-30fps-lowmem.mp4":L"-original-30fps.mp4"));
}
int testPower=[]{
  wchar_t value[16]{};
  return GetEnvironmentVariableW(L"LITELY_TEST_POWER",value,16)?int(!wcscmp(value,L"battery")):-1;
}();
bool onBattery(){
  if(testPower>=0)return testPower==1;
  SYSTEM_POWER_STATUS power{};
  return GetSystemPowerStatus(&power) && power.ACLineStatus==0;
}
fs::path playFile(){
  std::error_code ignored;
  if(batteryFps && onBattery()){
    auto copy=batteryCopyPath(source);
    if(!copy.empty() && fs::exists(copy,ignored))return copy;
  }
  return source;
}
void visibility();
// Swaps only the decoder's file when it can, so the last frame stays on screen.
void updatePlayFile(){
  auto next=playFile();
  if(!playerOpen() || next==playingFile)return;
  swapPending=true;swapAt=position;
  swapKept=leanActive && ready;
  if(swapKept){
    playingFile=next;
    leanRestart(next,position);
  }else closePlayer();
  visibility();
}
void stopBatteryCopy(bool cancelled){
  ImportJob job;
  job.copy=std::move(batteryCopy);job.result=batteryCopyPath(batteryCopyVideo);
  stopConversion(job,cancelled);
  batteryCopyProcess=nullptr;batteryCopyVideo.clear();
}
void refreshSettings();
void updateBatteryCopy(){
  std::error_code ignored;
  auto copy=batteryFps && !batteryCopyFailed?batteryCopyPath(source):fs::path();
  DWORD rate=0;
  bool wanted=!copy.empty() && !fs::exists(copy,ignored);
  if(wanted){readVideoDetails(source,nullptr,nullptr,&rate);wanted=rate>30500;}
  if(batteryCopyProcess && (!wanted || batteryCopyVideo!=source))stopBatteryCopy(true);
  if(!wanted || batteryCopyProcess)return;
  batteryCopyVideo=source;
  batteryCopy=std::make_unique<Transcode>();
  batteryCopy->source=source;batteryCopy->destination=copy;batteryCopy->cap=true;batteryCopy->lowest=lowestCopy(source);
  if(transcodeStart(*batteryCopy,THREAD_PRIORITY_BELOW_NORMAL)){batteryCopyProcess=batteryCopy->thread;return;}
  stopBatteryCopy(true);
  batteryCopyFailed=true;
  refreshSettings();
}
void finishBatteryCopy(){
  DWORD code=transcodeStop(*batteryCopy,false);
  std::error_code ignored;
  auto copy=batteryCopyPath(batteryCopyVideo);
  bool made=code==0 && !copy.empty() && fs::exists(copy,ignored);
  stopBatteryCopy(!made);
  if(!made)batteryCopyFailed=true;
  else updatePlayFile();
  refreshSettings();
}
// Names the missing Store extension when Windows recognised the video's format but has no decoder for it.
std::wstring conversionError(const Transcode &copy){
  std::wstring unchanged=L" Your original file is unchanged.";
  if(copy.error==MF_E_UNSUPPORTED_BYTESTREAM_TYPE || copy.error==MF_E_UNSUPPORTED_FORMAT)
    return L"Windows can't open this kind of file, so Litely can't use it."+unchanged;
  if(copy.error==MF_E_TOPO_CODEC_NOT_FOUND || copy.error==MF_E_INVALIDMEDIATYPE){
    struct Codec{GUID format;const wchar_t *name,*extension;};
    const Codec codecs[]={{MFVideoFormat_HEVC,L"HEVC",L"HEVC Video Extensions"},{MFVideoFormat_AV1,L"AV1",L"AV1 Video Extension"},
                          {MFVideoFormat_VP90,L"VP9",L"VP9 Video Extensions"},{MFVideoFormat_MPEG2,L"MPEG-2",L"MPEG-2 Video Extension"}};
    for(auto &codec:codecs)
      if(copy.codec==codec.format)
        return L"This video uses "+std::wstring(codec.name)+L", which Windows can't decode yet. Install "+codec.extension+
               L" from the Microsoft Store, then add the video again."+unchanged;
    return L"Windows doesn't have a decoder for this video's format, so Litely can't convert it."+unchanged;
  }
  return L"Litely couldn't convert this video."+unchanged;
}
INT_PTR CALLBACK importProc(HWND w,UINT msg,WPARAM a,LPARAM b){
  auto job=reinterpret_cast<ImportJob*>(GetWindowLongPtr(w,DWLP_USER));
  if(msg==WM_INITDIALOG){
    SetWindowLongPtr(w,DWLP_USER,b);
    job=activeImport=reinterpret_cast<ImportJob*>(b);
    readVideoDetails(job->original,&job->width,&job->height,&job->rate,&job->duration);
    std::error_code ignored;job->bytes=fs::file_size(job->original,ignored);
    DpiScope scale(GetDpiForWindow(w));
    sizeDialog(w,460,550);
    placeDip(w,301,14,110,174,32);placeDip(w,302,192,110,110,32);placeDip(w,303,306,110,110,32);
    placeDip(w,305,14,180,174,32);placeDip(w,306,192,180,110,32);
    placeDip(w,307,14,250,174,32);placeDip(w,308,192,250,150,32);
    placeDip(w,IDCANCEL,340,498,96,36);placeDip(w,IDOK,236,498,96,36);
    std::wstring original=L"Original";
    if(job->height)original+=L" ("+std::to_wstring(job->height)+L"p)";
    SetDlgItemText(w,301,original.c_str());
    wchar_t rate[48]=L"Original";
    if(job->rate)swprintf_s(rate,!(job->rate%1000)?L"Original (%.0f fps)":!(job->rate%100)?L"Original (%.1f fps)":L"Original (%.2f fps)",job->rate/1000.);
    SetDlgItemText(w,305,rate);
    bool known=job->width && job->height;
    EnableWindow(GetDlgItem(w,302),!known || importScale(*job,1080)<1);
    EnableWindow(GetDlgItem(w,303),!known || importScale(*job,720)<1);
    EnableWindow(GetDlgItem(w,306),!job->rate || job->rate>30500);
    CheckRadioButton(w,301,303,301);CheckRadioButton(w,305,306,305);CheckRadioButton(w,307,308,308);
  }
  if(auto result=dialogTheme(w,msg,a,b,paintImport))return result;
  if(!job)return FALSE;
  if(msg==WM_INITDIALOG){SetFocus(GetDlgItem(w,301));return FALSE;}
  if(msg==WM_DESTROY && job->copy){KillTimer(w,1);stopConversion(*job,true);return FALSE;}
  if(msg==WM_NOTIFY){
    auto header=reinterpret_cast<NMHDR*>(b);
    if(header->code==NM_CUSTOMDRAW && header->idFrom>=301 && header->idFrom<=308){
      auto &draw=*reinterpret_cast<NMCUSTOMDRAW*>(b);
      if(draw.dwDrawStage!=CDDS_PREPAINT){SetWindowLongPtr(w,DWLP_MSGRESULT,CDRF_DODEFAULT);return TRUE;}
      DpiScope scale(GetDpiForWindow(w));
      RECT bounds;GetClientRect(header->hwndFrom,&bounds);
      HDC dc=CreateCompatibleDC(draw.hdc);HBITMAP bitmap=CreateCompatibleBitmap(draw.hdc,bounds.right,bounds.bottom);
      auto old=SelectObject(dc,bitmap);
      wchar_t label[64];GetWindowText(header->hwndFrom,label,64);
      drawChoice(dc,units(bounds),label,nullptr,IsDlgButtonChecked(w,(int)header->idFrom)==BST_CHECKED,false,
                 IsWindowEnabled(header->hwndFrom)!=0,hovered(header->hwndFrom),
                 (draw.uItemState&CDIS_FOCUS) && keyboardCues(header->hwndFrom),cWindow);
      BitBlt(draw.hdc,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY);
      SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
      SetWindowLongPtr(w,DWLP_MSGRESULT,CDRF_SKIPDEFAULT);return TRUE;
    }
  }
  if(msg==WM_COMMAND && LOWORD(a)>=301 && LOWORD(a)<=308){
    for(int id=301;id<=308;id++)InvalidateRect(GetDlgItem(w,id),nullptr,FALSE);
    InvalidateRect(w,nullptr,FALSE);return TRUE;
  }
  if(msg==WM_TIMER && job->copy){
    job->tick++;
    if(WaitForSingleObject(job->copy->thread,0)==WAIT_OBJECT_0){
      DWORD code=transcodeStop(*job->copy,false);
      auto problem=conversionError(*job->copy);
      KillTimer(w,1);stopConversion(*job,false);
      std::error_code ignored;
      if(code || !fs::exists(job->result,ignored)){
        stopConversion(*job,true);
        MessageBox(w,problem.c_str(),L"Litely",MB_ICONERROR);
        EndDialog(w,0);
      }else EndDialog(w,1);
      return TRUE;
    }
    job->percent=job->copy->percent;
    DpiScope scale(GetDpiForWindow(w));
    RECT bar{px(24),px(454),px(436),px(490)};InvalidateRect(w,&bar,FALSE);
    return TRUE;
  }
  if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(a)==IDCANCEL)){
    if(job->copy){KillTimer(w,1);stopConversion(*job,true);}
    EndDialog(w,0);return TRUE;
  }
  if(msg!=WM_COMMAND || LOWORD(a)!=IDOK || job->copy)return FALSE;
  bool cap=IsDlgButtonChecked(w,306)!=0;
  int limit=chosenLimit(w);
  if(!convertChoice(w) && !needsCopy(job->original)){job->result=job->original;EndDialog(w,1);return TRUE;}
  std::wstring size=std::to_wstring(limit),suffix=limit?size+L"p":L"original";
  bool lowest=IsDlgButtonChecked(w,308)!=0;
  if(cap)suffix+=L"-30fps";
  if(lowest)suffix+=L"-lowmem";
  auto dir=copyDirectory(job->original);
  job->result=dir/(job->original.stem().wstring()+L"-"+suffix+L".mp4");
  if(fs::exists(job->result)){EndDialog(w,1);return TRUE;}
  job->copy=std::make_unique<Transcode>();
  job->copy->source=job->original;job->copy->destination=job->result;
  job->copy->limit=limit;job->copy->cap=cap;job->copy->lowest=lowest;
  if(!transcodeStart(*job->copy,THREAD_PRIORITY_NORMAL)){
    job->copy.reset();
    MessageBox(w,L"Could not start the conversion.",L"Litely",MB_ICONERROR);return TRUE;
  }
  for(int id:{301,302,303,305,306,307,308,IDOK})EnableWindow(GetDlgItem(w,id),FALSE);
  SetFocus(GetDlgItem(w,IDCANCEL));
  SetTimer(w,1,100,nullptr);InvalidateRect(w,nullptr,FALSE);
  return TRUE;
}
// Picks a video and shows the Import window. Returns the file to play, or nothing when cancelled.
fs::path importVideo(HWND owner,const wchar_t *initialDir){
  wchar_t path[32768]{};
  auto filter=videoFilter();
  OPENFILENAME of{sizeof of};
  of.hwndOwner=owner;
  of.lpstrFilter=filter.c_str();
  of.lpstrFile=path;
  of.nMaxFile=32768;
  of.lpstrInitialDir=initialDir;
  of.Flags=OFN_EXPLORER|OFN_FILEMUSTEXIST|OFN_NOCHANGEDIR;
  if(!GetOpenFileName(&of))return {};
  ImportJob job{fs::path(path),{}};
  // The Import window draws with GDI+, which the tray menu's path hasn't started.
  Gdiplus::GdiplusStartupInput input;ULONG_PTR token=0;
  bool drawing=Gdiplus::GdiplusStartup(&token,&input,nullptr)==Gdiplus::Ok;
  auto chosen=DialogBoxParam(instance,MAKEINTRESOURCE(300),owner,importProc,(LPARAM)&job);
  if(drawing)Gdiplus::GdiplusShutdown(token);
  if(chosen!=1)return {};
  if(job.result!=job.original){
    auto key=posterPath(job.result).stem().wstring();
    WritePrivateProfileString(L"names",key.c_str(),displayName(job.original).c_str(),(folder/L"library-names.ini").c_str());
  }
  return job.result;
}
INT_PTR CALLBACK renameProc(HWND dialog,UINT message,WPARAM a,LPARAM b){
  if(message==WM_INITDIALOG){
    DpiScope scale(GetDpiForWindow(dialog));
    sizeDialog(dialog,400,176);
    placeDip(dialog,201,34,58,332,22);placeDip(dialog,IDCANCEL,280,116,96,36);placeDip(dialog,IDOK,176,116,96,36);
    SendDlgItemMessage(dialog,201,WM_SETFONT,(WPARAM)CreateFont(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,textFace()),FALSE);
  }
  if(auto themed=dialogTheme(dialog,message,a,b,[](HWND,HDC dc){paintText(dc,L"Name shown in your library",{24,18,376,38},13,cSoft,FW_SEMIBOLD);}))return themed;
  if(message==WM_INITDIALOG){
    auto name=reinterpret_cast<const wchar_t*>(b);
    SetDlgItemText(dialog,201,name);
    SendDlgItemMessage(dialog,201,EM_SETLIMITTEXT,200,0);
    SendDlgItemMessage(dialog,201,EM_SETSEL,0,-1);
    SetFocus(GetDlgItem(dialog,201));
    return FALSE;
  }
  if(message==WM_DESTROY){DeleteObject((HFONT)SendDlgItemMessage(dialog,201,WM_GETFONT,0,0));return FALSE;}
  if(message==WM_COMMAND){
    if(LOWORD(a)==IDCANCEL){EndDialog(dialog,0);return TRUE;}
    if(LOWORD(a)==IDOK){
      wchar_t name[201];GetDlgItemText(dialog,201,name,201);
      std::wstring value=name;
      auto first=value.find_first_not_of(L" \t\r\n");
      if(first==std::wstring::npos)return TRUE;
      value=value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
      int i=selectedIndex();
      if(i<0)return TRUE;
      auto key=posterPath(libraryFiles[i]).stem().wstring();
      if(!WritePrivateProfileString(L"names",key.c_str(),value.c_str(),(folder/L"library-names.ini").c_str())){
        MessageBox(dialog,L"Could not save the name.",L"Litely",MB_ICONERROR);return TRUE;
      }
      EndDialog(dialog,1);return TRUE;
    }
  }
  return FALSE;
}
void updateActionLabel();

void loadPreview() {
  previewImage.reset();
  int i = selectedIndex();
  if (i >= 0 && i < (int)libraryFiles.size()) {
    auto image = artworkPath(libraryFiles[i]);
    requestPreview(libraryFiles[i]);
    if (fs::exists(image)) {
      auto bitmap = readArtwork(image);
      if (bitmap)
        previewImage = std::move(bitmap);
    }
  }
  BOOL motion = TRUE;
  SystemParametersInfo(SPI_GETCLIENTAREAANIMATION, 0, &motion, 0);
  motion=motion && !settingsPage && previewImage!=nullptr;
  previewOpacity = motion ? .35f : 1.f;
  if (motion)
    SetTimer(libraryWindow, 2, 16, nullptr);
  updateActionLabel();
  InvalidateRect(libraryWindow, nullptr, FALSE);
}
void placeList();
void scrollReveal(int index,bool animate);
// Finished row images, so scrolling copies pixels instead of redrawing rows.
struct RowImage{HBITMAP bitmap=nullptr;unsigned key=0;int wide=0,tall=0;};
std::vector<RowImage> rowCache;
std::vector<std::unique_ptr<Gdiplus::Bitmap>> tileImages;
void clearRowCache(){
  for(auto &row:rowCache)if(row.bitmap)DeleteObject(row.bitmap);
  rowCache.clear();
  tileImages.clear();
}
void dropRow(int i){
  if(i>=0 && i<(int)rowCache.size())rowCache[i].key=0;
  if(i>=0 && i<(int)tileImages.size())tileImages[i].reset();
}
void fillList(const fs::path &preferred = source) {
  clearRowCache();
  rowImages.clear();
  videoDetails.clear();
  int selected = 0;
  for (size_t i = 0; i < libraryFiles.size(); i++) {
    videoDetails.push_back(readVideoDetails(libraryFiles[i]));
    auto image=readArtwork(artworkPath(libraryFiles[i]),true);
    rowImages.push_back(std::move(image));
    if (!_wcsicmp(libraryFiles[i].c_str(), preferred.c_str()))
      selected = (int)i;
  }
  selectedRow=libraryFiles.empty()?-1:selected;
  placeList();
  scrollReveal(selectedRow,false);
  if(libraryList)InvalidateRect(libraryList,nullptr,FALSE);
  loadPreview();
}
std::wstring savingsText;
void sampleStats() {
  static fs::path measuredSource;
  static unsigned previousFrames=0, samples=0;
  static double playingCPU=0,playingRAM=0;
  static bool previousPlaying=false;

  ULONGLONG now = GetTickCount64();
  double cpu = cpuSeconds();
  PROCESS_MEMORY_COUNTERS_EX mem{};
  mem.cb = sizeof mem;
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&mem,
                       sizeof mem);
  SYSTEM_INFO sys;
  GetSystemInfo(&sys);
  wchar_t gpu[64] = L"0 MiB";
  if (device) {
    ComPtr<IDXGIDevice> dx;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(device.As(&dx)) && SUCCEEDED(dx->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter.As(&adapter3))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO info{};
      if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
              0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        swprintf_s(gpu, L"%.1f MiB", info.CurrentUsage / 1048576.);
      else
        wcscpy_s(gpu, L"Unavailable");
    }
  }
  double percent = lastSample && now > lastSample
                       ? 100000. * (cpu - lastCPU) / (now - lastSample) /
                             sys.dwNumberOfProcessors
                       : 0;
  wchar_t value[200];swprintf_s(value,L"%.1f%%",percent);cpuValue=lastSample?value:L"—";
  swprintf_s(value,L"%.1f MiB",mem.WorkingSetSize/1048576.);ramValue=value;gpuValue=gpu;
  if(measuredSource!=source){measuredSource=source;samples=0;previousPlaying=false;}
  bool playing=ready && !paused && !covered && !batteryOverride && !parked;
  double ram=mem.WorkingSetSize/1048576.;
  if(lastSample && now-lastSample<2500 && playing && previousPlaying && frames!=previousFrames){
    ++samples;
    playingCPU+=(percent-playingCPU)/std::min(samples,10u);
    playingRAM+=(ram-playingRAM)/std::min(samples,10u);
  }
  if(source.empty())savingsText.clear();
  else if(playing)savingsText=samples?L"Playing now. Pause it or cover the desktop to compare.":L"Measuring playback…";
  else if(!samples)savingsText=L"Play this wallpaper on a visible desktop to measure a baseline.";
  else if(!previousPlaying && lastSample){
    double ramSaved=playingRAM-ram,cpuSaved=playingCPU-percent;
    swprintf_s(value,L"%s: %.0f MiB %s RAM and %.1f%% %s CPU than while playing",
               batteryOverride?L"Energy Saver":paused?L"Paused":L"Desktop covered",
               std::fabs(ramSaved),ramSaved>=0?L"less":L"more",std::fabs(cpuSaved),cpuSaved>=0?L"less":L"more");
    savingsText=value;
  }
  previousPlaying=playing;previousFrames=frames;
  lastCPU = cpu;
  lastSample = now;
}
const wchar_t *actionCaption(){
  int i=selectedIndex();
  bool active=i>=0 && !_wcsicmp(libraryFiles[i].c_str(),source.c_str());
  return active?(paused||batteryOverride?L"Resume":L"Pause"):L"Set wallpaper";
}
void updateActionLabel(){
  int i=selectedIndex();
  HWND button=GetDlgItem(libraryWindow,102);
  wchar_t current[64]{};GetWindowText(button,current,64);
  const wchar_t *next=actionCaption();
  if(wcscmp(current,next)){
    SetWindowText(button,next);
    RECT client;GetClientRect(libraryWindow,&client);
    SendMessage(libraryWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));
  }
  bool enabled=!batteryOverride && !applyingPause && i>=0;
  if(!!IsWindowEnabled(button)!=enabled)EnableWindow(button,enabled);
  HWND remove=GetDlgItem(libraryWindow,109);
  if(!!IsWindowEnabled(remove)!=(i>=0))EnableWindow(remove,i>=0);
}
void applySide(int wanted,bool full);
// The Settings page scrolls below the back button; rows passing under it are clipped, not drawn over it.
// Test hook: LITELY_TEST_RUN_KEY points Start with Windows at another key, so tests leave the real one alone.
std::wstring runKey(){
  wchar_t key[260];
  return GetEnvironmentVariableW(L"LITELY_TEST_RUN_KEY",key,260)?key:L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}
// Task Manager's Startup apps can switch the entry off without removing it; an odd first byte means off.
const wchar_t *startupApproved=L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
bool startsWithWindows(){
  if(RegGetValueW(HKEY_CURRENT_USER,runKey().c_str(),L"Litely",RRF_RT_REG_SZ,nullptr,nullptr,nullptr))return false;
  BYTE state[12]{};DWORD size=sizeof state;
  return RegGetValueW(HKEY_CURRENT_USER,startupApproved,L"Litely",RRF_RT_REG_BINARY,nullptr,state,&size) || !(state[0]&1);
}
bool setStartWithWindows(bool on){
  HKEY key;
  if(RegCreateKeyExW(HKEY_CURRENT_USER,runKey().c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr))return false;
  LSTATUS result;
  if(on){
    wchar_t exe[32768];GetModuleFileName(nullptr,exe,32768);
    std::wstring command=L"\""+std::wstring(exe)+L"\" --startup";
    result=RegSetValueExW(key,L"Litely",0,REG_SZ,(const BYTE*)command.c_str(),DWORD((command.size()+1)*sizeof(wchar_t)));
    if(!GetEnvironmentVariableW(L"LITELY_TEST_RUN_KEY",nullptr,0))RegDeleteKeyValueW(HKEY_CURRENT_USER,startupApproved,L"Litely");
  }else{
    result=RegDeleteValueW(key,L"Litely");
    if(result==ERROR_FILE_NOT_FOUND)result=ERROR_SUCCESS;
  }
  RegCloseKey(key);
  return result==ERROR_SUCCESS;
}
double settingsOffset=0,settingsTarget=0;
Clock::time_point settingsTick;
bool settingsMoving=false;
constexpr int settingsTop=116,settingsBottom=914;
int settingsLimit(int heightDip){return std::max(0,settingsBottom-heightDip);}
void clipSettings(){
  for(int id:{103,104,105,111,112,113,114}){
    HWND row=GetDlgItem(libraryWindow,id);
    RECT box;GetWindowRect(row,&box);MapWindowPoints(nullptr,libraryWindow,(POINT*)&box,2);
    bool show=settingsPage && box.bottom>px(settingsTop);
    if(!!IsWindowVisible(row)!=show)ShowWindow(row,show?SW_SHOWNA:SW_HIDE);
    int cut=px(settingsTop)-box.top;
    SetWindowRgn(row,show && cut>0?CreateRectRgn(0,cut,box.right-box.left,box.bottom-box.top):nullptr,FALSE);
  }
}
void scrollSettings(double target){
  RECT client;GetClientRect(libraryWindow,&client);
  settingsTarget=std::clamp(target,0.,(double)settingsLimit(dip(client.bottom)));
  if(!settingsMoving){settingsMoving=true;settingsTick=Clock::now();}
  SetTimer(libraryWindow,8,USER_TIMER_MINIMUM,nullptr);
}
void setUiPage(bool settings) {
  bool restore=settings && sideFull;
  if(restore){sideFull=mainOff=false;sideWidth=sideLast;}
  settingsPage = settings;
  titleHot = false;
  ShowWindow(GetDlgItem(libraryWindow, 102), !settings && (!mainOff || sideFull) ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(libraryWindow, 109), !settings && !mainOff ? SW_SHOW : SW_HIDE);
  if(settings)settingsOffset=settingsTarget=0;
  for (int id : {103, 104, 105, 107, 111, 112, 113, 114})
    ShowWindow(GetDlgItem(libraryWindow, id), settings ? SW_SHOW : SW_HIDE);
  SendMessage(GetDlgItem(libraryWindow, 103), BM_SETCHECK,
              maximumSavings ? BST_UNCHECKED : BST_CHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 104), BM_SETCHECK,
              maximumSavings ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 105), BM_SETCHECK,
              batteryRule ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 111), BM_SETCHECK,
              batteryFps ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 112), BM_SETCHECK, accentMode ? BST_UNCHECKED : BST_CHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 113), BM_SETCHECK, accentMode ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessage(GetDlgItem(libraryWindow, 114), BM_SETCHECK, startsWithWindows() ? BST_CHECKED : BST_UNCHECKED, 0);
  for(int id:{103,104,105,106,111,112,113,114})InvalidateRect(GetDlgItem(libraryWindow,id),nullptr,FALSE);
  InvalidateRect(libraryWindow, nullptr, FALSE);
  if(restore){RECT client;GetClientRect(libraryWindow,&client);SendMessage(libraryWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));}
}
// The Library's own list: rows while narrow, tiles once wide, scrolled by pixels.
HWND libraryScroll=nullptr,tileMenu=nullptr;
double scrollOffset=0,scrollTarget=0;
Clock::time_point scrollTick;
bool scrollMoving=false,scrollHot=false,scrollDragging=false,hotMore=false;
int scrollGrab=0,menuHot=-1;
// How far each tile is lifted by hover, 0 to 1; animated by the list's timer 2.
std::vector<float> lift;
Clock::time_point liftTick;
bool lifting=false;
void startLift(){
  if(!libraryScroll)return;
  if(!lifting){lifting=true;liftTick=Clock::now();}
  SetTimer(libraryScroll,2,USER_TIMER_MINIMUM,nullptr);
}
bool playbackBusy(){
  static const bool forced=GetEnvironmentVariableW(L"LITELY_TEST_BUSY",nullptr,0)!=0;
  return forced || applyingPause || freezePending || (!paused && !batteryOverride && !covered && !source.empty() && !presented);
}
struct Cells{int cols,wide,tall,strip;};
Cells cellsFor(RECT r,bool grid){
  if(!grid){
    int strip=(int)libraryFiles.size()*px(72)>r.bottom?px(10):0;
    return {1,std::max(1,(int)r.right-strip),px(72),strip};
  }
  int usable=std::max(1,(int)r.right-px(10)),cols=std::max(1,usable/px(176)),wide=usable/cols;
  return {cols,wide,px((dip(wide)-16)*9/16+66),px(10)};
}
Cells cells(){RECT r;GetClientRect(libraryScroll,&r);return cellsFor(r,gridMode());}
int scrollView(){RECT r;GetClientRect(libraryScroll,&r);return r.bottom;}
int scrollContent(){auto c=cells();return ((int)libraryFiles.size()+c.cols-1)/c.cols*c.tall;}
int scrollLimit(){return std::max(0,scrollContent()-scrollView());}
RECT scrollThumb(){
  RECT r;GetClientRect(libraryScroll,&r);
  int limit=scrollLimit();
  if(!limit)return {};
  int tall=std::max(px(28),(int)((double)r.bottom*r.bottom/scrollContent()));
  int top=(int)((r.bottom-tall)*scrollOffset/limit);
  return {r.right-px(7),top+px(2),r.right-px(3),top+tall-px(2)};
}
void placeList(){
  if(!libraryScroll)return;
  int limit=scrollLimit();
  scrollTarget=std::clamp(scrollTarget,0.,(double)limit);
  scrollOffset=std::clamp(scrollOffset,0.,(double)limit);
  InvalidateRect(libraryScroll,nullptr,FALSE);
}
void scrollTo(double target,bool animate){
  scrollTarget=std::clamp(target,0.,(double)scrollLimit());
  if(animate){if(!scrollMoving){scrollMoving=true;scrollTick=Clock::now();}SetTimer(libraryScroll,1,USER_TIMER_MINIMUM,nullptr);}
  else{scrollOffset=scrollTarget;placeList();}
}
void scrollReveal(int index,bool animate){
  if(index<0 || !libraryScroll)return;
  auto c=cells();
  double top=index/c.cols*(double)c.tall,target=scrollTarget;
  if(top<target)target=top;
  else if(top+c.tall>target+scrollView())target=top+c.tall-scrollView();
  if(target!=scrollTarget)scrollTo(target,animate);
}
RECT cellRect(int i){
  auto c=cells();int x=i%c.cols*c.wide,y=i/c.cols*c.tall-(int)std::lround(scrollOffset);
  return {x,y,x+c.wide,y+c.tall};
}
int cellAt(POINT p){
  auto c=cells();
  if(p.x<0 || p.x>=c.cols*c.wide || p.y<0)return -1;
  int i=(int)((p.y+std::lround(scrollOffset))/c.tall)*c.cols+p.x/c.wide;
  return i<(int)libraryFiles.size()?i:-1;
}
float liftAmount(int i){
  float t=i>=0 && i<(int)lift.size()?lift[i]:0;
  return t*t*(3-2*t);
}
// A lifted tile grows about 4% around its centre and rises 2 units.
RECT liftedRect(int i){
  RECT cell=cellRect(i);
  float e=liftAmount(i);
  int growX=(int)std::lround((cell.right-cell.left)*.02f*e),growY=(int)std::lround((cell.bottom-cell.top)*.02f*e),rise=(int)std::lround(px(2)*e);
  RECT box{cell.left-growX,cell.top-growY-rise,cell.right+growX,cell.bottom+growY-rise};
  // The list can't draw outside itself, so a tile at its edge grows inward instead of being clipped.
  RECT view;GetClientRect(libraryScroll,&view);view.right-=cells().strip;
  if(cell.top>=0 && box.top<0)OffsetRect(&box,0,-box.top);
  if(cell.left>=0 && box.left<0)OffsetRect(&box,-box.left,0);
  if(cell.right<=view.right && box.right>view.right)OffsetRect(&box,view.right-box.right,0);
  return box;
}
RECT moreRect(int i){
  RECT cell=liftedRect(i);
  return {cell.right-px(42),cell.top+px(14),cell.right-px(14),cell.top+px(42)};
}
void selectRow(int i,bool notify){
  if(i<0 || i>=(int)libraryFiles.size())return;
  selectedRow=i;
  scrollReveal(i,true);
  InvalidateRect(libraryScroll,nullptr,FALSE);
  if(notify)SendMessage(GetParent(libraryScroll),WM_COMMAND,MAKEWPARAM(100,LBN_SELCHANGE),(LPARAM)libraryScroll);
}
std::unique_ptr<Gdiplus::Bitmap> readTile(const fs::path &path){
  Gdiplus::Bitmap file(path.c_str());
  if(file.GetLastStatus()!=Gdiplus::Ok)return {};
  auto tile=std::make_unique<Gdiplus::Bitmap>(480,270,PixelFormat32bppPARGB);
  Gdiplus::Graphics g(tile.get());
  float sw=(float)file.GetWidth(),sh=(float)file.GetHeight(),scale=std::min(sw/480,sh/270);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  if(g.DrawImage(&file,Gdiplus::RectF(0,0,480,270),(sw-480*scale)/2,(sh-270*scale)/2,480*scale,270*scale,Gdiplus::UnitPixel)!=Gdiplus::Ok)return {};
  return tile;
}
void drawCell(HDC buffer,RECT r,int i,bool selected,bool hot,bool more,bool focus,bool grid){
  if(!grid){
  fillRect(buffer,r,cSidebar);
  RECT card{r.left+2,r.top+2,r.right-2,r.bottom-2};
  COLORREF fill=selected?cSelected:hot?cHover:cSidebar;
  if(selected){fillRound(buffer,card,cSelectedLine,8);InflateRect(&card,-1,-1);}
  if(selected || hot)fillRound(buffer,card,fill,7);
  RECT thumb{r.left+12,r.top+14,r.left+65,r.top+58};
  if(i<(int)rowImages.size() && rowImages[i] && rowImages[i]->GetLastStatus()==Gdiplus::Ok)
    drawCover(buffer,rowImages[i].get(),thumb);
  else{fillRound(buffer,thumb,cRaised,5);paintIcon(buffer,L"",thumb,16,cMuted);}
  roundCorners(buffer,thumb,5,fill);
  paintText(buffer,displayName(libraryFiles[i]),{r.left+76,r.top+15,r.right-8,r.top+34},13,cText,FW_SEMIBOLD);
  bool active=!_wcsicmp(libraryFiles[i].c_str(),source.c_str());
  auto detail=active?(paused||batteryOverride?L"On desktop · paused":L"On desktop"):videoDetails[i];
  paintText(buffer,detail,{r.left+76,r.top+37,r.right-8,r.bottom-8},12,active?cAccent:cMuted);
  if(focus)focusRing(buffer,{r.left+2,r.top+2,r.right-2,r.bottom-2},8);
    return;
  }
  fillRect(buffer,r,cSidebar);
  RECT card{r.left+2,r.top+2,r.right-2,r.bottom-2};
  COLORREF fill=selected?cSelected:hot?cHover:cSidebar;
  if(selected){fillRound(buffer,card,cSelectedLine,10);InflateRect(&card,-1,-1);}
  if(selected || hot)fillRound(buffer,card,fill,9);
  RECT thumb{r.left+8,r.top+8,r.right-8,r.top+8+(r.right-r.left-16)*9/16};
  if(tileImages[i] && tileImages[i]->GetLastStatus()==Gdiplus::Ok)drawCover(buffer,tileImages[i].get(),thumb);
  else{fillRound(buffer,thumb,cRaised,7);paintIcon(buffer,L"",thumb,22,cMuted);}
  roundCorners(buffer,thumb,7,fill);
  if(hot){
    RECT dots{thumb.right-34,thumb.top+6,thumb.right-6,thumb.top+34};
    fillRound(buffer,dots,more?cSelected:cWindow,7,more?255:215);
    paintIcon(buffer,L"",dots,14,cText);
  }
  paintText(buffer,displayName(libraryFiles[i]),{r.left+10,thumb.bottom+9,r.right-10,thumb.bottom+28},13,cText,FW_SEMIBOLD);
  bool active=!_wcsicmp(libraryFiles[i].c_str(),source.c_str());
  auto detail=active?(paused||batteryOverride?L"On desktop · paused":L"On desktop"):videoDetails[i];
  paintText(buffer,detail,{r.left+10,thumb.bottom+30,r.right-10,thumb.bottom+48},12,active?cAccent:cMuted);
  if(focus)focusRing(buffer,{r.left+2,r.top+2,r.right-2,r.bottom-2},10);
}
const wchar_t *actionCaption();
RECT menuItem(int k){int top=5+k*34+(k==2?9:0);return {5,top,171,top+34};}
void closeTileMenu(){
  if(!tileMenu)return;
  HWND menuWindow=tileMenu;tileMenu=nullptr;menuHot=-1;
  ReleaseCapture();DestroyWindow(menuWindow);
  if(libraryScroll)InvalidateRect(libraryScroll,nullptr,FALSE);
}
LRESULT CALLBACK tileMenuProc(HWND w,UINT m,WPARAM a,LPARAM b){
  auto itemAt=[&](LPARAM where){
    POINT p{dip(GET_X_LPARAM(where)),dip(GET_Y_LPARAM(where))};
    for(int k=0;k<3;k++){RECT item=menuItem(k);if(PtInRect(&item,p))return k;}
    return -1;
  };
  switch(m){
  case WM_ERASEBKGND:return 1;
  case WM_PAINT:{
    PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);
    RECT r;GetClientRect(w,&r);
    HDC buffer=CreateCompatibleDC(dc);HBITMAP bitmap=CreateCompatibleBitmap(dc,r.right,r.bottom);
    auto old=SelectObject(buffer,bitmap);
    panel(buffer,r,cLine);
    RECT inner{1,1,r.right-1,r.bottom-1};panel(buffer,inner,cRaised);
    for(int k=0;k<3;k++){
      RECT item=menuItem(k);
      if(k==menuHot)fillRound(buffer,item,cHover,6);
      paintText(buffer,k==0?actionCaption():k==1?L"Rename":L"Remove",{item.left+11,item.top,item.right-8,item.bottom},13,
                k==2?RGB(255,154,168):k==menuHot?cText:cSoft,FW_NORMAL,DT_VCENTER|DT_SINGLELINE);
    }
    {UiDC mapping(buffer);panel(buffer,{11,77,165,78},cLine);}
    BitBlt(dc,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY);
    SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);
    EndPaint(w,&ps);return 0;
  }
  case WM_MOUSEMOVE:{int k=itemAt(b);if(k!=menuHot){menuHot=k;InvalidateRect(w,nullptr,FALSE);}return 0;}
  case WM_LBUTTONDOWN:case WM_RBUTTONDOWN:{
    RECT r;GetClientRect(w,&r);POINT p{GET_X_LPARAM(b),GET_Y_LPARAM(b)};
    if(!PtInRect(&r,p))closeTileMenu();
    return 0;
  }
  case WM_LBUTTONUP:{
    int k=itemAt(b);
    if(k<0)return 0;
    closeTileMenu();
    PostMessage(libraryWindow,WM_COMMAND,k==0?102:k==1?110:109,0);
    return 0;
  }
  case WM_CAPTURECHANGED:if(tileMenu==w){tileMenu=nullptr;menuHot=-1;DestroyWindow(w);}return 0;
  }
  return DefWindowProc(w,m,a,b);
}
void openTileMenu(int i){
  closeTileMenu();
  selectRow(i,true);
  RECT more=moreRect(i);POINT at{more.right,more.bottom+px(6)};
  ClientToScreen(libraryScroll,&at);
  tileMenu=CreateWindowEx(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE,L"LitelyMenu",L"",WS_POPUP,at.x-px(176),at.y,px(176),px(121),
                          libraryWindow,nullptr,instance,nullptr);
  if(!tileMenu)return;
  int round=2;DwmSetWindowAttribute(tileMenu,33,&round,sizeof round);
  ShowWindow(tileMenu,SW_SHOWNOACTIVATE);
  SetCapture(tileMenu);
}
unsigned cellKey(int i,bool grid,bool focus){
  bool chosen=i==selectedRow,hot=i==hoveredRow;
  return 128|(chosen?1:0)|(hot?2:0)|(chosen && focus?4:0)|(!_wcsicmp(libraryFiles[i].c_str(),source.c_str())?8:0)|
         (paused||batteryOverride?16:0)|(grid?32:0)|(hot && hotMore?64:0);
}
void renderCell(HDC reference,HDC cell,int i,const Cells &c,unsigned key,bool grid,bool focus){
  if((int)rowCache.size()<=i)rowCache.resize(libraryFiles.size());
  if((int)tileImages.size()<=i)tileImages.resize(libraryFiles.size());
  if(grid && !tileImages[i])tileImages[i]=readTile(artworkPath(libraryFiles[i]));
  auto &kept=rowCache[i];
  if(kept.bitmap)DeleteObject(kept.bitmap);
  kept={CreateCompatibleBitmap(reference,c.wide,c.tall),key,c.wide,c.tall};
  bool chosen=i==selectedRow,hot=i==hoveredRow;
  auto before=SelectObject(cell,kept.bitmap);
  drawCell(cell,{0,0,dip(c.wide),dip(c.tall)},i,chosen,hot,hot && hotMore,chosen && focus,grid);
  SelectObject(cell,before);
}
// Renders the tiles that will be visible at the panel's final width before the motion starts.
void warmCells(int panelWidth){
  bool grid=panelWidth>=400;
  if(!grid || !libraryScroll)return;
  RECT r;GetClientRect(libraryScroll,&r);r.right=px(panelWidth-24);
  auto c=cellsFor(r,grid);
  int count=(int)libraryFiles.size(),first=(int)scrollOffset/c.tall*c.cols,last=std::min(count,first+((int)r.bottom/c.tall+2)*c.cols);
  HDC dc=GetDC(libraryScroll),cell=CreateCompatibleDC(dc);
  bool focus=GetFocus()==libraryScroll && keyboardCues(libraryScroll);
  for(int i=first;i<last;i++){
    unsigned key=cellKey(i,grid,focus);
    if((int)rowCache.size()>i && rowCache[i].bitmap && rowCache[i].key==key && rowCache[i].wide==c.wide && rowCache[i].tall==c.tall)continue;
    renderCell(dc,cell,i,c,key,grid,focus);
  }
  DeleteDC(cell);ReleaseDC(libraryScroll,dc);
}
LRESULT CALLBACK scrollProc(HWND w,UINT m,WPARAM a,LPARAM b){
  switch(m){
  case WM_GETDLGCODE:return DLGC_WANTARROWS;
  case WM_SETFOCUS:case WM_KILLFOCUS:InvalidateRect(w,nullptr,FALSE);return 0;
  case WM_SIZE:placeList();return 0;
  case WM_ERASEBKGND:return 1;
  case WM_PAINT:{
    // Each cell is drawn once into its own image; a frame is those images copied into one off-screen page.
    PAINTSTRUCT ps;HDC dc=BeginPaint(w,&ps);
    RECT r;GetClientRect(w,&r);
    HDC buffer=CreateCompatibleDC(dc),cell=CreateCompatibleDC(dc);
    bool owned;HBITMAP bitmap=canvasFor(0,dc,r.right,r.bottom,owned);
    auto old=SelectObject(buffer,bitmap);
    panel(buffer,r,cSidebar);
    auto c=cells();bool grid=gridMode(),focus=GetFocus()==w && keyboardCues(w);
    int count=(int)libraryFiles.size(),offset=(int)std::lround(scrollOffset);
    if((int)rowCache.size()<count)rowCache.resize(count);
    if((int)tileImages.size()<count)tileImages.resize(count);
    if((int)lift.size()<count)lift.resize(count);
    std::vector<int> raised;
    for(int i=0;i<count;i++){
      int x=i%c.cols*c.wide,y=i/c.cols*c.tall-offset;
      if(y+c.tall<=0 || y>=r.bottom)continue;
      if(grid && !fastDraw && lift[i]>.001f){
        if(i==hoveredRow)raised.push_back(i);else raised.insert(raised.begin(),i);
        continue;
      }
      unsigned key=cellKey(i,grid,focus);
      auto &kept=rowCache[i];
      if(!kept.bitmap || kept.key!=key || kept.wide!=c.wide || kept.tall!=c.tall){
        if(fastDraw && grid && kept.bitmap && kept.key==key){
          // Mid-animation: scale the finished tile instead of redrawing it; the last frame redraws it sharp.
          auto before=SelectObject(cell,kept.bitmap);
          BLENDFUNCTION opaque{AC_SRC_OVER,0,255,0};
          GdiAlphaBlend(buffer,x,y,c.wide,c.tall,cell,0,0,kept.wide,kept.tall,opaque);
          SelectObject(cell,before);
          continue;
        }
        renderCell(dc,cell,i,c,key,grid,focus);
      }
      auto before=SelectObject(cell,kept.bitmap);
      BitBlt(buffer,x,y,c.wide,c.tall,cell,0,0,SRCCOPY);
      SelectObject(cell,before);
    }
    for(int i:raised){
      // Drawn last and at its lifted size, over a soft shadow; only the card is copied, so neighbours stay visible.
      RECT box=liftedRect(i);
      int wide=box.right-box.left,tall=box.bottom-box.top;
      float e=liftAmount(i);
      for(int k=4;k>=1;k--){
        RECT shade{dip(box.left)+4-k*2,dip(box.top)+4+k,dip(box.right)-4+k*2,dip(box.bottom)+2+k*2};
        fillRound(buffer,shade,RGB(0,0,0),12,(BYTE)(16*e));
      }
      HBITMAP image=CreateCompatibleBitmap(dc,wide,tall);
      auto before=SelectObject(cell,image);
      drawCell(cell,{0,0,dip(wide),dip(tall)},i,i==selectedRow,i==hoveredRow,i==hoveredRow && hotMore,i==selectedRow && focus,true);
      TransparentBlt(buffer,box.left,box.top,wide,tall,cell,0,0,wide,tall,cSidebar);
      SelectObject(cell,before);DeleteObject(image);
    }
    RECT thumb=scrollThumb();
    if(thumb.bottom>thumb.top)
      fillRound(buffer,{dip(thumb.left),dip(thumb.top),dip(thumb.right),dip(thumb.bottom)},scrollHot||scrollDragging?cMuted:cLine,2);
    BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,buffer,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
    SelectObject(buffer,old);if(owned)DeleteObject(bitmap);DeleteDC(buffer);DeleteDC(cell);
    EndPaint(w,&ps);return 0;
  }
  case WM_MOUSEWHEEL:
    closeTileMenu();
    scrollTo(scrollTarget-GET_WHEEL_DELTA_WPARAM(a)/120.*px(96),true);return 0;
  case WM_TIMER:{
    if(a==2){
      auto now=Clock::now();
      float step=(float)(std::chrono::duration<double,std::milli>(now-liftTick).count()/200);
      liftTick=now;
      bool moving=false;
      if(lift.size()<libraryFiles.size())lift.resize(libraryFiles.size());
      for(int i=0;i<(int)lift.size();i++){
        float target=i==hoveredRow && gridMode() && !fastDraw?1.f:0.f;
        if(lift[i]!=target){lift[i]=target>lift[i]?std::min(target,lift[i]+step):std::max(target,lift[i]-step);moving=true;}
      }
      if(!moving){KillTimer(w,2);lifting=false;}
      InvalidateRect(w,nullptr,FALSE);UpdateWindow(w);
      return 0;
    }
    // Eased by elapsed time, so uneven timer ticks still move evenly.
    auto now=Clock::now();
    double gap=scrollTarget-scrollOffset,step=1-std::exp(-std::chrono::duration<double,std::milli>(now-scrollTick).count()/55);
    scrollTick=now;
    if(std::abs(gap)<.75){scrollOffset=scrollTarget;KillTimer(w,1);scrollMoving=false;}
    else scrollOffset+=gap*step;
    InvalidateRect(w,nullptr,FALSE);UpdateWindow(w);return 0;
  }
  case WM_LBUTTONDOWN:{
    SendMessage(w,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    SetFocus(w);
    POINT p{GET_X_LPARAM(b),GET_Y_LPARAM(b)};RECT r;GetClientRect(w,&r);
    RECT thumb=scrollThumb();
    if(thumb.bottom>thumb.top && p.x>=r.right-px(10)){
      if(p.y<thumb.top)scrollTo(scrollTarget-scrollView(),true);
      else if(p.y>=thumb.bottom)scrollTo(scrollTarget+scrollView(),true);
      else{scrollDragging=true;scrollGrab=p.y-thumb.top;SetCapture(w);}
      return 0;
    }
    int hit=cellAt(p);
    if(hit<0)return 0;
    if(gridMode()){
      RECT more=moreRect(hit);
      if(PtInRect(&more,p)){openTileMenu(hit);return 0;}
    }
    selectRow(hit,true);
    if(gridMode())SendMessage(GetParent(w),WM_COMMAND,MAKEWPARAM(100,9),(LPARAM)w);
    return 0;
  }
  case WM_LBUTTONDBLCLK:{
    POINT p{GET_X_LPARAM(b),GET_Y_LPARAM(b)};
    if(!gridMode() && cellAt(p)>=0)SendMessage(GetParent(w),WM_COMMAND,MAKEWPARAM(100,LBN_DBLCLK),(LPARAM)w);
    return 0;
  }
  case WM_KEYDOWN:{
    int cols=cells().cols,count=(int)libraryFiles.size(),next=selectedRow;
    if(a==VK_UP)next-=cols;else if(a==VK_DOWN)next+=cols;
    else if(a==VK_LEFT && cols>1)next--;else if(a==VK_RIGHT && cols>1)next++;
    else if(a==VK_HOME)next=0;else if(a==VK_END)next=count-1;
    else if(a==VK_F2){SendMessage(GetParent(w),WM_COMMAND,110,0);return 0;}
    else break;
    SendMessage(w,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
    if(next>=0 && next<count && next!=selectedRow)selectRow(next,true);
    return 0;
  }
  case WM_MOUSEMOVE:{
    POINT p{GET_X_LPARAM(b),GET_Y_LPARAM(b)};RECT r;GetClientRect(w,&r);
    if(scrollDragging){
      RECT thumb=scrollThumb();int span=scrollView()-(thumb.bottom-thumb.top+px(4));
      KillTimer(w,1);scrollMoving=false;
      if(span>0)scrollTo((p.y-scrollGrab)*(double)scrollLimit()/span,false);
      return 0;
    }
    int hit=cellAt(p);bool more=false,bar=scrollLimit() && p.x>=r.right-px(10);
    if(hit>=0 && gridMode()){RECT dots=moreRect(hit);more=PtInRect(&dots,p)!=0;}
    if(hit!=hoveredRow){hoveredRow=hit;startLift();}
    if(more!=hotMore || bar!=scrollHot){hotMore=more;scrollHot=bar;InvalidateRect(w,nullptr,FALSE);}
    TRACKMOUSEEVENT leave{sizeof leave,TME_LEAVE,w,0};TrackMouseEvent(&leave);
    return 0;
  }
  case WM_LBUTTONUP:if(scrollDragging){scrollDragging=false;ReleaseCapture();InvalidateRect(w,nullptr,FALSE);}return 0;
  case WM_MOUSELEAVE:
    // Test hook: LITELY_TEST_HOVER keeps a simulated hover, which the real cursor would otherwise cancel.
    static const bool holdHover=GetEnvironmentVariableW(L"LITELY_TEST_HOVER",nullptr,0)!=0;
    if(!tileMenu && !holdHover){hoveredRow=-1;hotMore=false;startLift();}
    scrollHot=false;InvalidateRect(w,nullptr,FALSE);return 0;
  }
  return DefWindowProc(w,m,a,b);
}
// The divider between the Library and the preview: drag to resize, click to expand or restore.
std::vector<Clock::time_point> sideFrames;
// Test hook: with LITELY_TEST_LOG set, each expand or collapse appends its frame timing.
void logSideFrames(){
  wchar_t log[MAX_PATH];
  if(sideFrames.size()>1 && GetEnvironmentVariableW(L"LITELY_TEST_LOG",log,MAX_PATH)){
    std::vector<double> gaps;
    for(size_t i=1;i<sideFrames.size();i++)gaps.push_back(std::chrono::duration<double,std::milli>(sideFrames[i]-sideFrames[i-1]).count());
    std::sort(gaps.begin(),gaps.end());
    double total=std::chrono::duration<double,std::milli>(sideFrames.back()-sideFrames.front()).count();
    std::wofstream out(log,std::ios::app);
    out<<std::fixed<<std::setprecision(1)<<L"side frames "<<sideFrames.size()<<L" over "<<total<<L" ms, median "<<gaps[gaps.size()/2]
       <<L" ms, worst "<<gaps.back()<<L" ms"<<std::endl;
  }
  sideFrames.clear();
}
bool splitHot=false,splitDrag=false,splitMoved=false,sideAnimating=false,sideTargetFull=false;
int splitStartX=0,splitStartWidth=0;
double sideFrom=0,sideTo=0;
Clock::time_point sideStart,barStart;
bool overSplit(POINT p){return p.y>65 && p.x>=sideWidth-9 && p.x<=sideWidth+10;}
void applySide(int wanted,bool full){
  RECT client;GetClientRect(libraryWindow,&client);int cw=dip(client.right);
  sideFull=full;
  sideWidth=full?cw-14:std::clamp(wanted,236,std::max(236,sideAnimating || splitDrag?cw-14:cw-500));
  mainOff=sideFull || cw-sideWidth<(sideAnimating || splitDrag?330:486);
  ShowWindow(GetDlgItem(libraryWindow,102),!settingsPage && (!mainOff || sideFull)?SW_SHOW:SW_HIDE);
  ShowWindow(GetDlgItem(libraryWindow,109),!settingsPage && !mainOff?SW_SHOW:SW_HIDE);
  SendMessage(libraryWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));
  // Paint the window and every control in one pass, so moved controls never show a stale frame.
  RedrawWindow(libraryWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
}
void saveSide(){
  auto file=(folder/L"settings.ini").wstring();
  WritePrivateProfileStringW(L"wallpaper",L"libraryWidth",std::to_wstring(sideLast).c_str(),file.c_str());
  WritePrivateProfileStringW(L"wallpaper",L"libraryFull",sideFull?L"1":L"0",file.c_str());
}
// While the panel moves, the preview may shrink from its 500-unit minimum to 330, fading out over that band.
float previewRoom(int panelWidth){
  RECT client;GetClientRect(libraryWindow,&client);
  return (float)std::clamp((dip(client.right)-panelWidth-330)/170.,0.,1.);
}
void animateSide(int to,bool full){
  sideTargetFull=full;
  sideFrom=sideWidth;sideTo=to;
  warmCells(to);
  fastDraw=true;
  timeBeginPeriod(1);
  sideStart=Clock::now();sideAnimating=true;
  SetTimer(libraryWindow,5,1,nullptr);
}
void toggleSide(){
  if(settingsPage)setUiPage(false);
  RECT client;GetClientRect(libraryWindow,&client);
  if(!sideFull)sideLast=sideWidth;
  animateSide(sideFull?sideLast:dip(client.right)-14,!sideFull);
}
// Keep the native button's input, radio grouping and accessibility behavior.
LRESULT CALLBACK uiButtonProc(HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR){
  int id=GetDlgCtrlID(w);
  // These controls paint every pixel themselves, so an erase would only flash the old background.
  if(m==WM_ERASEBKGND)return 1;
  if(id==113 && m==WM_LBUTTONUP){
    POINT at{dip(GET_X_LPARAM(b)),dip(GET_Y_LPARAM(b))};
    for(int k=0;k<8;k++){RECT box=swatchRect(k);InflateRect(&box,3,3);if(PtInRect(&box,at))accentPick=accentPresets[k];}
  }
  if(m==WM_LBUTTONDOWN)SendMessage(GetParent(w),WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
  if(m==WM_MOUSEMOVE){
    TRACKMOUSEEVENT query{sizeof query,TME_QUERY,w,0};
    TrackMouseEvent(&query);
    if(query.hwndTrack!=w || !(query.dwFlags&TME_LEAVE)){
      TRACKMOUSEEVENT leave{sizeof leave,TME_LEAVE,w,0};
      TrackMouseEvent(&leave);
      if(id!=100)InvalidateRect(w,nullptr,FALSE);
    }
  }else if(m==WM_MOUSELEAVE && id!=100)InvalidateRect(w,nullptr,FALSE);
  return DefSubclassProc(w,m,a,b);
}
void refreshSettings(){
  if(libraryWindow)InvalidateRect(GetDlgItem(libraryWindow,111),nullptr,FALSE);
}
COLORREF hlsColor(double hue,double light,double saturation){
  auto channel=[&](double t){
    double q=light<.5?light*(1+saturation):light+saturation-light*saturation,p=2*light-q;
    t=t-std::floor(t);
    double v=t<1/6.?p+(q-p)*6*t:t<.5?q:t<2/3.?p+(q-p)*(2/3.-t)*6:p;
    return (BYTE)std::lround(v*255);
  };
  return RGB(channel(hue+1/3.),channel(hue),channel(hue-1/3.));
}
// The strongest vivid hue in the image, as a light tone so dark button text stays readable.
COLORREF wallpaperAccent(const fs::path &image){
  Gdiplus::Bitmap file(image.c_str());
  if(file.GetLastStatus()!=Gdiplus::Ok)return CLR_INVALID;
  Gdiplus::Bitmap tiny(64,36,PixelFormat32bppARGB);
  {Gdiplus::Graphics g(&tiny);g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);g.DrawImage(&file,0,0,64,36);}
  Gdiplus::BitmapData data{};Gdiplus::Rect all(0,0,64,36);
  if(tiny.LockBits(&all,Gdiplus::ImageLockModeRead,PixelFormat32bppARGB,&data)!=Gdiplus::Ok)return CLR_INVALID;
  double bins[36]{},xs[36]{},ys[36]{};
  for(int y=0;y<36;y++){
    auto row=reinterpret_cast<const UINT*>(static_cast<const BYTE*>(data.Scan0)+y*data.Stride);
    for(int x=0;x<64;x++){
      double r=(row[x]>>16&255)/255.,g=(row[x]>>8&255)/255.,b=(row[x]&255)/255.;
      double high=std::max({r,g,b}),low=std::min({r,g,b}),chroma=high-low,sat=high>0?chroma/high:0;
      if(high<.18 || sat<.22)continue;
      double hue=high==r?std::fmod((g-b)/chroma+6,6.):high==g?(b-r)/chroma+2:(r-g)/chroma+4;
      hue/=6;
      double weight=sat*sat*std::min(1.,high*1.4);
      int k=(int)(hue*36)%36;
      bins[k]+=weight;xs[k]+=weight*std::cos(hue*6.283185);ys[k]+=weight*std::sin(hue*6.283185);
    }
  }
  tiny.UnlockBits(&data);
  int best=0;double bestScore=-1,peak=0;
  for(int k=0;k<36;k++){
    peak=std::max(peak,bins[k]);
    double score=bins[k]+.6*(bins[(k+35)%36]+bins[(k+1)%36]);
    if(score>bestScore){bestScore=score;best=k;}
  }
  if(peak<3)return CLR_INVALID;
  double x=xs[best]+.6*(xs[(best+35)%36]+xs[(best+1)%36]),y=ys[best]+.6*(ys[(best+35)%36]+ys[(best+1)%36]);
  return hlsColor(std::fmod(std::atan2(y,x)/6.283185+1,1.),.83,.85);
}
COLORREF autoAccent(){
  static fs::path from;static bool hadArt=false;static COLORREF value=defaultAccent;
  if(source.empty())return defaultAccent;
  std::error_code ignored;
  auto art=artworkPath(source);
  bool has=fs::exists(art,ignored);
  if(source!=from || has!=hadArt){
    from=source;hadArt=has;
    COLORREF found=has?wallpaperAccent(art):CLR_INVALID;
    value=found==CLR_INVALID?defaultAccent:found;
  }
  return value;
}
void clearRowCache();
void refreshAccent(){
  COLORREF want=accentMode?accentPick:autoAccent();
  if(want==cAccent)return;
  setAccent(want);
  if(!libraryWindow)return;
  for(auto &row:rowCache)row.key=0;
  RedrawWindow(libraryWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
void saveAccent(){
  wchar_t text[16];
  if(accentMode)swprintf_s(text,L"%02x%02x%02x",GetRValue(accentPick),GetGValue(accentPick),GetBValue(accentPick));
  else wcscpy_s(text,L"auto");
  WritePrivateProfileStringW(L"wallpaper",L"accent",text,(folder/L"settings.ini").c_str());
}
void loadAccent(){
  wchar_t text[16]{};
  GetPrivateProfileStringW(L"wallpaper",L"accent",L"auto",text,16,(folder/L"settings.ini").c_str());
  unsigned value=0;
  accentMode=wcscmp(text,L"auto") && swscanf_s(text,L"%x",&value)==1;
  if(accentMode)accentPick=RGB(value>>16&255,value>>8&255,value&255);
}
RECT swatchRect(int k){return {40+k*34,36,66+k*34,62};}
void drawSwatches(HDC dc,RECT r){
  for(int k=0;k<8;k++){
    RECT box=swatchRect(k);OffsetRect(&box,r.left,r.top);
    if(accentMode && accentPick==accentPresets[k]){RECT ring=box;InflateRect(&ring,3,3);strokeRound(dc,ring,accentPresets[k],10,2);}
    fillRound(dc,box,accentPresets[k],7);
    strokeRound(dc,box,RGB(255,255,255),7,1,30);
  }
}
const wchar_t *choiceDescription(int id){
  if(id==112){
    static std::wstring text;
    text=source.empty()?L"Taken from the wallpaper on your desktop.":L"Taken from the wallpaper on your desktop: "+displayName(source)+L".";
    return text.c_str();
  }
  if(id==113)return L"";
  if(id==114)return L"Opens Litely in the tray when you sign in, so your wallpaper plays right away.";
  if(id==111)return batteryCopyFailed?L"Couldn't make the 30 fps copy of this video.":
                                      L"On battery, plays a 30 fps copy of wallpapers above 30 fps. The copy is made once, in the background.";
  return id==103?L"Resumes instantly when the desktop is uncovered. Keeps video memory reserved.":
         id==104?L"Releases video memory while paused or covered. Resuming takes a moment longer.":
                 L"On battery with Energy Saver on, shows a still and releases video memory.";
}
LRESULT CALLBACK libraryProc(HWND w, UINT message, WPARAM a, LPARAM b) {
  try {
    switch (message) {
    case WM_CREATE: {
      libraryDpi=GetDpiForWindow(w);
      uiBrush = CreateSolidBrush(cWindow);
      uiFont =
          CreateFont(-16, 0, 0, 0, 400, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                     0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
      auto control = [&](const wchar_t *cls, const wchar_t *text, DWORD style,
                         int id) {
        HWND c =
            CreateWindowEx(0,cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                         0, 0, 0, 0, w, (HMENU)(INT_PTR)id, instance, nullptr);
        SetWindowSubclass(c,uiButtonProc,1,0);
        SendMessage(c, WM_SETFONT, (WPARAM)uiFont, TRUE);
        return c;
      };
      libraryList = libraryScroll = CreateWindowEx(0, L"LitelyScroll", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                   0, 0, 0, 0, w, (HMENU)(INT_PTR)100, instance, nullptr);
      {
        auto file = (folder / L"settings.ini").wstring();
        sideLast = std::clamp((int)GetPrivateProfileIntW(L"wallpaper", L"libraryWidth", 236, file.c_str()), 236, 4000);
        sideFull = GetPrivateProfileIntW(L"wallpaper", L"libraryFull", 0, file.c_str()) != 0;
        sideWidth = sideLast;
      }
      loadAccent();
      refreshAccent();
      control(L"BUTTON", L"Add video", BS_OWNERDRAW, 101);
      control(L"BUTTON", L"Set wallpaper", BS_OWNERDRAW, 102);
      control(L"BUTTON", L"Smooth resume", BS_AUTORADIOBUTTON | WS_GROUP, 103);
      control(L"BUTTON", L"Maximum savings", BS_AUTORADIOBUTTON, 104);
      control(L"BUTTON", L"Pause when Energy Saver is on",
              BS_AUTOCHECKBOX | WS_GROUP, 105);
      control(L"BUTTON", L"30 fps on battery", BS_AUTOCHECKBOX, 111);
      control(L"BUTTON", L"Match wallpaper", BS_AUTORADIOBUTTON | WS_GROUP, 112);
      control(L"BUTTON", L"Choose a colour", BS_AUTORADIOBUTTON, 113);
      control(L"BUTTON", L"Start with Windows", BS_AUTOCHECKBOX | WS_GROUP, 114);
      control(L"BUTTON", L"Remove", BS_OWNERDRAW, 109);
      control(L"BUTTON", L"Settings", BS_OWNERDRAW, 106);
      control(L"BUTTON", L"Tray menu", BS_OWNERDRAW, 108);
      control(L"BUTTON", L"Library", BS_OWNERDRAW, 107);
      placeList();
      SetTimer(w, 1, 1000, nullptr);
      return 0;
    }
    case WM_SIZE: {
      if(a==SIZE_MINIMIZED)return 0;
      int cw=dip(LOWORD(b)),ch=dip(HIWORD(b));
      // One batched move without copying old pixels, so neighbours can't carry each other's stale bits.
      HDWP batch=BeginDeferWindowPos(10);
      auto move=[&](HWND child,int x,int y,int span,int tall){
        if(batch)batch=DeferWindowPos(batch,child,nullptr,px(x),px(y),px(span),px(std::max(0,tall)),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
      };
      if(sideFull)sideWidth=cw-14;
      else if(!sideAnimating && !splitDrag)sideWidth=std::clamp(sideWidth,236,std::max(236,cw-500));
      mainOff=sideFull || cw-sideWidth<(sideAnimating || splitDrag?330:486);
      bool grid=gridMode();
      int hero=heroBottom(cw,ch);
      move(libraryScroll,12,122,sideWidth-24,grid?ch-186:ch-226);
      move(GetDlgItem(w,101),sideWidth-112,80,100,30);
      if(grid){move(GetDlgItem(w,106),12,ch-48,110,36);move(GetDlgItem(w,108),126,ch-48,120,36);}
      else{move(GetDlgItem(w,106),12,ch-92,sideWidth-24,36);move(GetDlgItem(w,108),12,ch-50,sideWidth-24,36);}
      const wchar_t *caption=actionCaption();
      int actionWidth=!wcscmp(caption,L"Set wallpaper")?128:!wcscmp(caption,L"Resume")?96:88;
      move(GetDlgItem(w,102),cw-28-actionWidth,sideFull?ch-48:hero+16,actionWidth,36);
      move(GetDlgItem(w,109),cw-28-actionWidth-108,hero+16,100,36);
      move(GetDlgItem(w,107),sideWidth+18,80,104,32);
      settingsTarget=std::clamp(settingsTarget,0.,(double)settingsLimit(ch));
      settingsOffset=std::clamp(settingsOffset,0.,(double)settingsLimit(ch));
      int scrolled=(int)std::lround(settingsOffset);
      const int rows[][2]={{103,206},{104,278},{105,406},{111,478},{112,614},{113,686},{114,822}};
      for(auto &row:rows)move(GetDlgItem(w,row[0]),sideWidth+20,row[1]-scrolled,cw-sideWidth-48,68);
      if(batch)EndDeferWindowPos(batch);
      clipSettings();
      RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
      return 0;
    }
    case WM_DPICHANGED:{
      libraryDpi=HIWORD(a);
      placeList();
      RECT suggested=*(RECT*)b;
      SetWindowPos(w,nullptr,suggested.left,suggested.top,suggested.right-suggested.left,suggested.bottom-suggested.top,SWP_NOZORDER|SWP_NOACTIVATE);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(w);
      return 0;
    case WM_GETMINMAXINFO:
      ((MINMAXINFO *)b)->ptMinTrackSize = {px(720), px(627)};
      return 0;
    case WM_SHOWWINDOW:
      if (a) {
        lastSample = 0;
        SetTimer(w, 1, 1000, nullptr);
      } else
        KillTimer(w, 1);
      break;
    case WM_MOUSEWHEEL:
      if (settingsPage) {
        scrollSettings(settingsTarget - GET_WHEEL_DELTA_WPARAM(a) / 120. * 96);
        return 0;
      }
      break;
    case WM_TIMER:
      if (a == 8) {
        auto now = Clock::now();
        double gap = settingsTarget - settingsOffset, step = 1 - std::exp(-std::chrono::duration<double, std::milli>(now - settingsTick).count() / 55);
        settingsTick = now;
        if (std::abs(gap) < .5) {
          settingsOffset = settingsTarget;
          KillTimer(w, 8);
          settingsMoving = false;
        } else
          settingsOffset += gap * step;
        RECT client;GetClientRect(w, &client);
        SendMessage(w, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
        UpdateWindow(w);
        return 0;
      }
      if (a == 5) {
        sideFrames.push_back(Clock::now());
        double t = std::min(1., std::chrono::duration<double, std::milli>(Clock::now() - sideStart).count() / 300);
        // Ease in and out, so the panel starts and settles gently instead of jumping on the first frame.
        double eased = t < .5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
        if (t >= 1) {
          KillTimer(w, 5);
          timeEndPeriod(1);
          sideAnimating = fastDraw = false;
          releaseCanvases();
          previewOpacity = 1;
          if (sideTargetFull) {
            barFade = 0;
            barStart = Clock::now();
            SetTimer(w, 7, 1, nullptr);
          }
          applySide((int)sideTo, sideTargetFull);
          saveSide();
          logSideFrames();
        } else
        {
          int next = (int)std::lround(sideFrom + (sideTo - sideFrom) * eased);
          previewOpacity = previewRoom(next);
          applySide(next, false);
        }
        UpdateWindow(w);
        // Wait for the compositor, so each frame lands on a screen refresh.
        DwmFlush();
        return 0;
      }
      if (a == 7) {
        barFade = (float)std::min(1., std::chrono::duration<double, std::milli>(Clock::now() - barStart).count() / 220);
        if (barFade >= 1)
          KillTimer(w, 7);
        RECT bar;GetClientRect(w, &bar);bar.top = bar.bottom - px(60);
        InvalidateRect(w, &bar, FALSE);
        InvalidateRect(GetDlgItem(w, 102), nullptr, FALSE);
        return 0;
      }
      if (a == 6) {
        if (!playbackBusy()) {
          KillTimer(w, 6);
          updateActionLabel();
        }
        InvalidateRect(GetDlgItem(w, 102), nullptr, FALSE);
        return 0;
      }
      if (a == 3 && !posterPending) {
        KillTimer(w, 3);
        loadPreview();
      }
      if (a == 1 && !IsIconic(w)) {
        static fs::path shownSource;static bool shownPaused=false,shownSaver=false;
        refreshAccent();
        if(shownSource!=source || shownPaused!=paused || shownSaver!=batteryOverride){
          shownSource=source;shownPaused=paused;shownSaver=batteryOverride;InvalidateRect(libraryList,nullptr,FALSE);
        }
        if(!settingsPage){
          updateActionLabel();
          sampleStats();
          RECT dirty;GetClientRect(w,&dirty);
          if(sideFull){dirty.left=px(250);dirty.top=dirty.bottom-px(56);}
          else{dirty.left=px(sideWidth+28);dirty.top=px(heroBottom(dip(dirty.right),dip(dirty.bottom))+16);}
          if(playbackBusy())SetTimer(w,6,16,nullptr);
          InvalidateRect(w,&dirty,FALSE);
        }
      }
      if (a == 2) {
        previewOpacity = std::min(1.f, previewOpacity + .1f);
        if (previewOpacity == 1)
          KillTimer(w, 2);
        RECT art;GetClientRect(w,&art);art.left=px(sideWidth+28);art.top=px(92);art.bottom=px(heroBottom(dip(art.right),dip(art.bottom)));art.right-=px(28);
        InvalidateRect(w,&art,FALSE);
      }
      return 0;
    case PreviewReady:
      if(a==previewRequest){
        int i=selectedIndex();
        if(i>=0 && fs::exists(thumbnailPath(libraryFiles[i]))){
          rowImages[i]=readArtwork(artworkPath(libraryFiles[i]),true);dropRow(i);
          loadPreview();InvalidateRect(libraryList,nullptr,FALSE);
        }
      }
      return 0;
    case WM_SETCURSOR:
      if((HWND)a==w && (splitHot || splitDrag)){SetCursor(LoadCursor(nullptr,IDC_SIZEWE));return TRUE;}
      if((HWND)a==w && titleHot){SetCursor(LoadCursor(nullptr,IDC_HAND));return TRUE;}
      break;
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:{
      POINT at{dip(GET_X_LPARAM(b)),dip(GET_Y_LPARAM(b))};
      if(splitDrag && message==WM_MOUSEMOVE){
        int moved=at.x-splitStartX;
        if(std::abs(moved)>3)splitMoved=true;
        if(splitMoved){
          // The panel follows the cursor; the preview fades as it runs out of room.
          int wanted=splitStartWidth+moved;
          previewOpacity=previewRoom(wanted);
          applySide(wanted,false);
          UpdateWindow(w);
        }
        return 0;
      }
      bool edge=message==WM_MOUSEMOVE && overSplit(at);
      if(edge!=splitHot){splitHot=edge;RECT grip{px(sideWidth-12),0,px(sideWidth+12),px(4000)};InvalidateRect(w,&grip,FALSE);}
      bool hot=message==WM_MOUSEMOVE && !settingsPage && !mainOff && selectedIndex()>=0 && PtInRect(&titleBox,at);
      if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT leave{sizeof leave,TME_LEAVE,w,0};TrackMouseEvent(&leave);}
      if(hot!=titleHot){
        titleHot=hot;
        RECT dirty{px(titleBox.left)-2,px(titleBox.top)-2,px(titleBox.right)+2,px(titleBox.bottom)+2};
        InvalidateRect(w,&dirty,FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:{
      POINT press{dip(GET_X_LPARAM(b)),dip(GET_Y_LPARAM(b))};
      if(overSplit(press) && !sideAnimating){
        splitDrag=true;splitMoved=false;splitStartX=press.x;splitStartWidth=sideWidth;fastDraw=true;
        SetCapture(w);
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if(splitDrag || fastDraw){splitDrag=fastDraw=false;releaseCanvases();RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}
      return 0;
    case WM_LBUTTONUP:{
      POINT click{dip(GET_X_LPARAM(b)),dip(GET_Y_LPARAM(b))};
      if(splitDrag){
        bool moved=splitMoved;
        ReleaseCapture();
        RECT client;GetClientRect(w,&client);int cw=dip(client.right);
        if(!moved)toggleSide();
        else if(cw-sideWidth<500){
          // Released where the preview has no room: finish the way the drag was going.
          if(sideWidth>splitStartWidth){
            if(splitStartWidth<=cw-500)sideLast=splitStartWidth;
            animateSide(cw-14,true);
          }else{
            sideLast=cw-500;
            animateSide(cw-500,false);
          }
        }else{
          sideLast=sideWidth;previewOpacity=1;
          applySide(sideWidth,false);
          saveSide();
        }
        return 0;
      }
      if(!settingsPage && !mainOff && PtInRect(&titleBox,click))SendMessage(w,WM_COMMAND,110,0);
      return 0;
    }
    case WM_COMMAND: {
      int id = LOWORD(a);
      if (id == 100 && HIWORD(a) == 9) {
        // A click on a grid tile puts that wallpaper on the desktop; one already there is left alone.
        int i = selectedIndex();
        if (i < 0 || !_wcsicmp(libraryFiles[i].c_str(), source.c_str()))
          return 0;
        id = 102;
      }
      if (id == 100 && HIWORD(a) == LBN_SELCHANGE){
        setUiPage(false);loadPreview();
      }
      if (id == 106 || id == 107)
        setUiPage(id == 106);
      if(id==108){
        RECT button;GetWindowRect(GetDlgItem(w,108),&button);
        POINT anchor{button.left,button.top};
        menu(&anchor);return 0;
      }
      if(id==109){
        int i=selectedIndex();
        if(i<0 || applyingPause)return 0;
        auto name=displayName(libraryFiles[i]);
        if(DialogBoxParam(instance,MAKEINTRESOURCE(310),w,removeProc,(LPARAM)name.c_str())!=IDOK)return 0;
        auto removed=libraryFiles[i];libraryFiles.erase(libraryFiles.begin()+i);
        try{saveLibrary();}catch(...){libraryFiles.insert(libraryFiles.begin()+i,removed);throw;}
        if(!_wcsicmp(removed.c_str(),source.c_str())){closePlayer();paused=true;freezePending=posterPending=false;position=0;source.clear();saveState();status(L"Litely - no wallpaper selected");}
        ++previewRequest;fillList();return 0;
      }
      if(id==110 || (id==100 && HIWORD(a)==LBN_DBLCLK)){
        int i=selectedIndex();
        if(i>=0){
          auto name=displayName(libraryFiles[i]);
          if(DialogBoxParam(instance,MAKEINTRESOURCE(200),w,renameProc,(LPARAM)name.c_str())==1){
            fillList();selectedRow=i;scrollReveal(i,false);InvalidateRect(libraryList,nullptr,FALSE);loadPreview();
          }
        }
        return 0;
      }
      if (id == 101) {
        auto added = importVideo(w, nullptr);
        if (!added.empty()) {
          setUiPage(false);
          addLibrary(added);
          saveLibrary();
          fillList(fs::weakly_canonical(added));
        }
      }

      if (id == 102) {
        SetTimer(w, 6, 16, nullptr);
        int i = selectedIndex();
        if (i >= 0) {
          if (!fs::exists(libraryFiles[i])) {
            MessageBox(
                w, L"This video has moved. Add its new location with Add video.",
                L"Litely", MB_OK);
            return 0;
          }
          if (!_wcsicmp(libraryFiles[i].c_str(), source.c_str()))
            toggle();
          else
            activateSource(libraryFiles[i]);
          updateActionLabel();InvalidateRect(libraryList,nullptr,FALSE);
          if(posterPending)SetTimer(w, 3, 1000, nullptr);
        }
      }
      if (id == 112 || id == 113) {
        accentMode = id == 113;
        saveAccent();
        refreshAccent();
        for (int choice : {112, 113})
          InvalidateRect(GetDlgItem(w, choice), nullptr, FALSE);
        return 0;
      }
      if (id == 103 || id == 104) {
        SendMessage(controller, WM_COMMAND,
                    id == 104 ? MaximumSavings : SmoothResume, 0);
        SendMessage(GetDlgItem(w,103),BM_SETCHECK,!maximumSavings,0);
        SendMessage(GetDlgItem(w,104),BM_SETCHECK,maximumSavings,0);
        for(int choice:{103,104})InvalidateRect(GetDlgItem(w,choice),nullptr,FALSE);
        return 0;
      }
      if (id == 111) {
        batteryFps = SendMessage(GetDlgItem(w, 111), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!WritePrivateProfileString(L"wallpaper", L"batteryFps", batteryFps ? L"1" : L"0",
                                       (folder / L"settings.ini").c_str()))
          throw E_FAIL;
        batteryCopyFailed = false;
        updateBatteryCopy();
        updatePlayFile();
        refreshSettings();
        return 0;
      }
      if (id == 114) {
        bool on = SendMessage(GetDlgItem(w, 114), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!setStartWithWindows(on)) {
          SendMessage(GetDlgItem(w, 114), BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
          MessageBox(w, L"Windows didn't allow Litely to change its startup setting.", L"Litely", MB_ICONERROR);
        }
        InvalidateRect(GetDlgItem(w, 114), nullptr, FALSE);
        return 0;
      }
      if (id == 105) {
        batteryRule =
            SendMessage(GetDlgItem(w, 105), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!WritePrivateProfileString(L"wallpaper", L"batteryRule",
                                       batteryRule ? L"1" : L"0",
                                       (folder / L"settings.ini").c_str()))
          throw E_FAIL;
        updateBattery();return 0;
      }
      InvalidateRect(w, nullptr, FALSE);
      return 0;
    }
    case WM_NOTIFY: {
      auto header=reinterpret_cast<NMHDR*>(b);
      if(header->code!=NM_CUSTOMDRAW || (header->idFrom<111 || header->idFrom>114) && (header->idFrom<103 || header->idFrom>105))break;
      auto &draw=*reinterpret_cast<NMCUSTOMDRAW*>(b);
      if(draw.dwDrawStage!=CDDS_PREPAINT)return CDRF_DODEFAULT;
      RECT bounds;GetClientRect(header->hwndFrom,&bounds);
      HDC dc=CreateCompatibleDC(draw.hdc);HBITMAP bitmap=CreateCompatibleBitmap(draw.hdc,bounds.right,bounds.bottom);
      auto old=SelectObject(dc,bitmap);
      wchar_t label[128];GetWindowText(header->hwndFrom,label,128);
      int id=(int)header->idFrom;
      drawChoice(dc,units(bounds),label,choiceDescription(id),SendMessage(header->hwndFrom,BM_GETCHECK,0,0)==BST_CHECKED,
                 id==105 || id==111 || id==114,true,hovered(header->hwndFrom),(draw.uItemState&CDIS_FOCUS) && keyboardCues(header->hwndFrom),cWindow);
      if(id==113)drawSwatches(dc,units(bounds));
      BitBlt(draw.hdc,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY);
      SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
      return CDRF_SKIPDEFAULT;
    }
    case WM_DRAWITEM:{
      auto d=(DRAWITEMSTRUCT*)b;
      if(d->CtlID==100 && d->itemID==(UINT)-1)return TRUE;
      HDC target=d->hDC;
      RECT bounds=d->rcItem;
      int widthPx=bounds.right-bounds.left,heightPx=bounds.bottom-bounds.top;
      HDC buffer=CreateCompatibleDC(target);
      HBITMAP bitmap=CreateCompatibleBitmap(target,widthPx,heightPx);
      if(!buffer || !bitmap){if(buffer)DeleteDC(buffer);if(bitmap)DeleteObject(bitmap);return FALSE;}
      auto old=SelectObject(buffer,bitmap);
      RECT r{0,0,dip(widthPx),dip(heightPx)};
      {
        int id=d->CtlID;
        const wchar_t *glyph=id==101?L"":id==106?L"":id==108?L"":id==107?L"":id==109?L"":nullptr;
        bool sidebar=id==101 || id==106 || id==108;
        COLORREF background=sidebar?cSidebar:cWindow;
        if(id==106 && settingsPage){fillRect(buffer,r,background);fillRound(buffer,r,cSelected,7);background=cSelected;}
        ButtonStyle style{id==102,background,glyph,id==106 || id==108,id==101?cAccent:CLR_INVALID,id==109};
        drawButton(buffer,*d,r,style);
        float fade=id==102 || id==109?(sideFull?barFade:previewOpacity):1;
        if(id==102 && playbackBusy()){
          // While the player switches between playing and paused, a turning arc stands in for the label.
          bool off=(d->itemState&ODS_DISABLED)!=0;
          fillRound(buffer,r,off?cAccentOff:cAccent,7);
          Smooth smooth(buffer);
          Gdiplus::Pen pen(gdip(off?cSoft:cInk),fpx(2));
          pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);
          int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
          smooth.g.DrawArc(&pen,frect({cx-8,cy-8,cx+8,cy+8}),(float)(GetTickCount64()%800)*.45f,270);
        }
        if(fade<.999f){
          // Fade toward the background by laying a translucent sheet of it over the finished button.
          HDC sheet=CreateCompatibleDC(target);HBITMAP pixel=CreateCompatibleBitmap(target,1,1);
          auto previous=SelectObject(sheet,pixel);SetPixel(sheet,0,0,background);
          BLENDFUNCTION cover{AC_SRC_OVER,0,(BYTE)(255*(1-fade)),0};
          GdiAlphaBlend(buffer,0,0,widthPx,heightPx,sheet,0,0,1,1,cover);
          SelectObject(sheet,previous);DeleteObject(pixel);DeleteDC(sheet);
        }
      }
      BitBlt(target,bounds.left,bounds.top,widthPx,heightPx,buffer,0,0,SRCCOPY);
      SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);
      return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
      SetDCBrushColor((HDC)a,cSidebar);
      return (LRESULT)GetStockObject(DC_BRUSH);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetTextColor((HDC)a, cText);
      SetBkColor((HDC)a, cWindow);
      return (LRESULT)uiBrush;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC screen = BeginPaint(w, &ps);
      RECT r;
      GetClientRect(w, &r);
      HDC dc = CreateCompatibleDC(screen);
      bool owned;HBITMAP buffer = canvasFor(1, screen, r.right, r.bottom, owned);
      auto old = SelectObject(dc, buffer);
      RECT pixels=r;r=units(r);
      {UiDC mapping(dc);
      FillRect(dc, &r, uiBrush);
      panel(dc,{0,65,sideWidth,r.bottom},cSidebar);
      panel(dc,{0,64,r.right,65},cLine);
      panel(dc,{sideWidth-1,65,sideWidth,r.bottom},cLine);
      panel(dc,{24,r.bottom-(gridMode()?60:102),sideFull?r.right-28:sideWidth-24,r.bottom-(gridMode()?59:101)},cLine);
      }
      DrawIconEx(dc,px(23),px(16),LoadIcon(instance,MAKEINTRESOURCE(IDI_LITELY)),px(32),px(32),0,nullptr,DI_NORMAL);
      paintText(dc,L"Litely",{66,16,236,50},22,cText,FW_SEMIBOLD,DT_LEFT|DT_VCENTER|DT_SINGLELINE,displayFace());
      paintText(dc,L"Library",{24,84,120,108},15,cText,FW_SEMIBOLD,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
      {
        int middle=(65+r.bottom)/2;bool lit=splitHot || splitDrag;
        RECT grip{sideWidth-8,middle-24,sideWidth+8,middle+24};
        fillRound(dc,grip,lit?cSelected:cRaised,8);
        strokeRound(dc,grip,lit?cSelectedLine:cLine,8,1);
        paintIcon(dc,sideFull?L"":L"",grip,10,lit?cAccent:cSoft);
      }
      int left=sideWidth+28,right=r.right-28;
      if(sideFull){
        // With the preview hidden, playback state and usage fade in and sit in the bottom bar beside the action button.
        int y=r.bottom-48,edge=r.right-28;
        const wchar_t *caption=actionCaption();
        edge-=(!wcscmp(caption,L"Set wallpaper")?128:!wcscmp(caption,L"Resume")?96:88)+22;
        for(int k=2;k>=0;k--){
          const auto &value=k==0?cpuValue:k==1?ramValue:gpuValue;
          int used=paintText(dc,value,{edge-120,y,edge,y+36},13,mixColor(cWindow,cText,barFade),FW_SEMIBOLD,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
          int label=paintText(dc,k==0?L"CPU":k==1?L"RAM":L"Video memory",{edge-used-116,y,edge-used-6,y+36},12,mixColor(cWindow,cMuted,barFade),FW_NORMAL,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
          edge-=used+6+label+22;
        }
        if(!source.empty() && edge>330){
          bool playing=!paused && !batteryOverride && !covered;
          const wchar_t *state=batteryOverride?L"Paused for Energy Saver":paused?L"Paused":covered?L"Desktop covered":L"Playing";
          int used=paintText(dc,state,{262,y,edge,y+36},13,mixColor(cWindow,cSoft,barFade),FW_NORMAL,DT_VCENTER|DT_SINGLELINE);
          paintText(dc,displayName(source),{262+used+8,y,edge,y+36},13,playing?mixColor(cWindow,cAccent,barFade):cMuted,FW_SEMIBOLD,DT_VCENTER|DT_SINGLELINE);
        }
      }else if(mainOff){
      }else if(settingsPage){
        int o=(int)std::lround(settingsOffset),saved=SaveDC(dc);
        IntersectClipRect(dc,0,px(settingsTop),pixels.right,pixels.bottom);
        paintText(dc,L"Settings",{left,118-o,right,158-o},28,cText,FW_SEMIBOLD);
        paintText(dc,L"Performance",{left,176-o,right,198-o},15,cText,FW_SEMIBOLD);
        paintText(dc,playbackPath.empty()?L"Playback starts with the next wallpaper":playbackPath,{left+120,178-o,right,198-o},13,cMuted,FW_NORMAL,DT_RIGHT|DT_SINGLELINE|DT_END_ELLIPSIS);
        {UiDC mapping(dc);panel(dc,{left,358-o,right,359-o},cLine);panel(dc,{left,566-o,right,567-o},cLine);panel(dc,{left,774-o,right,775-o},cLine);}
        paintText(dc,L"Battery",{left,376-o,right,398-o},15,cText,FW_SEMIBOLD);
        paintText(dc,L"Appearance",{left,584-o,right,606-o},15,cText,FW_SEMIBOLD);
        paintText(dc,L"Accent colour for buttons, highlights and selection",{left+120,586-o,right,606-o},13,cMuted,FW_NORMAL,DT_RIGHT|DT_SINGLELINE|DT_END_ELLIPSIS);
        paintText(dc,L"Startup",{left,792-o,right,814-o},15,cText,FW_SEMIBOLD);
        RestoreDC(dc,saved);
        if(int limit=settingsLimit(r.bottom)){
          int view=r.bottom-settingsTop,tall=std::max(28,view*view/(view+limit)),top=settingsTop+(int)((view-tall)*settingsOffset/limit);
          fillRound(dc,{r.right-10,top+2,r.right-6,top+tall-2},cLine,2);
        }
      }else{
        int i=selectedIndex();
        int hero=heroBottom(r.right,r.bottom);
        RECT art{left,92,right,hero};
        {UiDC mapping(dc);panel(dc,art,cRaised);}
        RECT damage=units(ps.rcPaint),hit{};
        if(previewImage && IntersectRect(&hit,&art,&damage)){
          static std::unique_ptr<Gdiplus::Bitmap> quick;static Gdiplus::Bitmap *quickOf=nullptr;static HBITMAP quickBitmap=nullptr;
          if(fastDraw && quickOf!=previewImage.get()){
            UINT iw=previewImage->GetWidth(),ih=previewImage->GetHeight(),sw=std::min(iw,960u),sh=std::max(1u,ih*sw/std::max(1u,iw));
            quick=std::make_unique<Gdiplus::Bitmap>(sw,sh,PixelFormat32bppPARGB);
            Gdiplus::Graphics g(quick.get());g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(previewImage.get(),0,0,sw,sh);
            quickOf=previewImage.get();
            if(quickBitmap)DeleteObject(quickBitmap);
            quickBitmap=nullptr;quick->GetHBITMAP(Gdiplus::Color(255,38,40,54),&quickBitmap);
          }
          if(fastDraw && quickBitmap){
            DeviceDC devicePixels(dc);
            RECT box{px(art.left),px(art.top),px(art.right),px(art.bottom)};
            double bw=box.right-box.left,bh=box.bottom-box.top,iw=quick->GetWidth(),ih=quick->GetHeight(),scale=std::max(bw/iw,bh/ih);
            int sw=(int)(bw/scale),sh=(int)(bh/scale);
            HDC image=CreateCompatibleDC(dc);auto before=SelectObject(image,quickBitmap);
            BLENDFUNCTION blend{AC_SRC_OVER,0,(BYTE)(255*previewOpacity),0};
            GdiAlphaBlend(dc,box.left,box.top,box.right-box.left,box.bottom-box.top,image,(int)(iw-sw)/2,(int)(ih-sh)/2,sw,sh,blend);
            SelectObject(image,before);DeleteDC(image);
          }else drawCover(dc,previewImage.get(),art,previewOpacity);
          Smooth s(dc);auto box=frect({art.left,art.bottom-120,art.right,art.bottom});
          Gdiplus::LinearGradientBrush shade(Gdiplus::PointF(0,box.Y-1),Gdiplus::PointF(0,box.GetBottom()+1),gdip(RGB(8,9,16),0),gdip(RGB(8,9,16),225));
          s.g.FillRectangle(&shade,box);
        }
        if(i<0)paintIcon(dc,L"",{art.left,art.top,art.right,art.bottom-70},40,mixColor(cWindow,cMuted,previewOpacity));
        roundCorners(dc,art,12,cWindow);
        std::wstring details=i>=0?videoDetails[i]+(isPlaybackCopy(libraryFiles[i])?L" · Playback copy":L" · Original file"):L"Use Add video to import a video";
        std::wstring name=i>=0?displayName(libraryFiles[i]):L"Add your first wallpaper";
        paintText(dc,details,{left+22,hero-82,right-18,hero-62},13,mixColor(cRaised,RGB(227,222,233),previewOpacity));
        RECT nameBox{left+22,hero-60,right-58,hero-12};
        if(i>=0){
          HDC measure=CreateCompatibleDC(dc);
          int nameWidth=paintText(measure,name,{0,0,0,0},34,mixColor(cWindow,cText,previewOpacity),FW_SEMIBOLD,DT_CALCRECT|DT_SINGLELINE);
          DeleteDC(measure);
          titleBox={left+10,hero-62,std::min(left+22+nameWidth,(int)nameBox.right)+48,hero-8};
          if(titleHot){
            fillRound(dc,titleBox,RGB(255,255,255),10,22);
            strokeRound(dc,titleBox,RGB(255,255,255),10,1.5f,120);
            paintIcon(dc,L"",{titleBox.right-40,titleBox.top,titleBox.right-12,titleBox.bottom},18,mixColor(cWindow,cText,previewOpacity));
          }
        }else titleBox={};
        paintText(dc,name,nameBox,34,mixColor(cWindow,cText,previewOpacity),FW_SEMIBOLD);
        bool active=i>=0 && !_wcsicmp(libraryFiles[i].c_str(),source.c_str());
        bool playing=active && !paused && !batteryOverride && !covered;
        const wchar_t *state=i<0?L"":!active?L"Not on desktop":batteryOverride?L"Paused for Energy Saver":paused?L"Paused":
                            covered?L"Desktop covered":playingFile!=source?L"Playing · 30 fps on battery":L"Playing";
        const wchar_t *glyph=i<0?L"":!active?L"":playing?L"":L"";
        paintIcon(dc,glyph,{left,hero+24,left+18,hero+44},14,playing?mixColor(cWindow,cAccent,previewOpacity):cSoft);
        paintText(dc,state,{left+26,hero+16,right-(!wcscmp(actionCaption(),L"Set wallpaper")?128:!wcscmp(actionCaption(),L"Resume")?96:88)-124,hero+52},13,mixColor(cWindow,cSoft,previewOpacity),FW_NORMAL,DT_VCENTER|DT_SINGLELINE);
        int divider=hero+68;
        {UiDC mapping(dc);panel(dc,{left,divider,right,divider+1},cLine);}
        int headingEnd=left+paintText(dc,L"Litely usage",{left,divider+18,right,divider+40},15,mixColor(cWindow,cText,previewOpacity),FW_SEMIBOLD);
        if(!source.empty())
          paintText(dc,L"·  "+displayName(source),{headingEnd+10,divider+21,right,divider+40},12,mixColor(cWindow,cMuted,previewOpacity));
        int col=(right-left)/3;
        for(int k=0;k<3;k++){
          int x=left+col*k;
          if(k){UiDC mapping(dc);panel(dc,{x-1,divider+52,x,divider+98},cLine);x+=18;}
          paintText(dc,k==0?L"CPU":k==1?L"RAM":L"Video memory",{x,divider+50,right,divider+68},12,mixColor(cWindow,cMuted,previewOpacity));
          const auto &value=k==0?cpuValue:k==1?ramValue:gpuValue;
          size_t space=value.find(L' ');
          int end=x+paintText(dc,value.substr(0,space),{x,divider+68,right,divider+98},23,mixColor(cWindow,cText,previewOpacity),FW_SEMIBOLD,DT_LEFT|DT_TOP|DT_SINGLELINE,textFace());
          if(space!=std::wstring::npos)
            paintText(dc,value.substr(space+1),{end+4,divider+78,right,divider+98},12,mixColor(cWindow,cMuted,previewOpacity));
        }
        paintText(dc,savingsText,{left,divider+108,right,divider+142},12,mixColor(cWindow,cMuted,previewOpacity),FW_NORMAL,DT_LEFT|DT_WORDBREAK);
      }
      BitBlt(screen, 0, 0, pixels.right, pixels.bottom, dc, 0, 0, SRCCOPY);
      SelectObject(dc, old);
      if (owned)
        DeleteObject(buffer);
      DeleteDC(dc);
      EndPaint(w, &ps);
      return 0;
    }
    case WM_DESTROY:
      ++previewRequest;
      hoveredRow=-1;
      previewImage.reset();
      rowImages.clear();
      videoDetails.clear();
      DeleteObject(uiFont);
      DeleteObject(uiBrush);
      uiFont = nullptr;
      uiBrush = nullptr;
      libraryWindow = nullptr;
      closeTileMenu();
      releaseCanvases();
      libraryList = libraryScroll = nullptr;
      lift.clear();
      lifting = false;
      selectedRow = -1;
      sideAnimating = splitDrag = false;
      clearRowCache();
      scrollOffset = scrollTarget = 0;
      Gdiplus::GdiplusShutdown(graphicsToken);
      trimMemory();
      return 0;
    }
  } catch (HRESULT error) {
    wchar_t text[160];
    swprintf_s(text, L"Litely could not complete that action (0x%08X).", unsigned(error));
    MessageBox(w, text, L"Litely", MB_OK | MB_ICONERROR);
  } catch (const std::exception &error) {
    wchar_t text[400];
    swprintf_s(text, L"Litely could not complete that action.\n%hs", error.what());
    MessageBox(w, text, L"Litely", MB_OK | MB_ICONERROR);
  } catch (...) {
    MessageBox(w, L"Litely could not complete that action.", L"Litely", MB_OK | MB_ICONERROR);
  }
  return DefWindowProc(w, message, a, b);
}
void openLibrary(bool settings = false) {
  if (libraryWindow) {
    ShowWindow(libraryWindow, SW_RESTORE);
    SetForegroundWindow(libraryWindow);
    setUiPage(settings);
    return;
  }
  libraryFiles.clear();
  auto path = folder / L"library.txt";
  if (fs::exists(path)) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.size() >= 2 && bytes.size() % 2 == 0) {
      std::wstring text(bytes.size() / 2, L'\0');
      memcpy(text.data(), bytes.data(), bytes.size());
      size_t start = 1;
      for (size_t i = 1; i <= text.size(); i++)
        if (i == text.size() || text[i] == L'\n') {
          if (i > start)
            addLibrary(text.substr(start, i - start));
          start = i + 1;
        }
    }
  }
  addLibrary(source);
  wchar_t exe[32768];
  GetModuleFileName(nullptr, exe, 32768);
  auto samples = fs::path(exe).parent_path() / L"wallpapers";
  if (!fs::exists(path) && fs::exists(samples))
    for (auto &file : fs::directory_iterator(samples))
      if (isVideoFile(file.path()) && !needsCopy(file.path()))
        addLibrary(file.path());
  saveLibrary();
  Gdiplus::GdiplusStartupInput input;
  check(Gdiplus::GdiplusStartup(&graphicsToken, &input, nullptr) == Gdiplus::Ok
            ? S_OK
            : E_FAIL);
  WNDCLASS wc{};
  wc.hInstance = instance;
  wc.lpfnWndProc = libraryProc;
  wc.lpszClassName = L"LitelyLibrary";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_LITELY));
  RegisterClass(&wc);
  {
    WNDCLASS scroller{};
    scroller.hInstance = instance;
    scroller.lpfnWndProc = scrollProc;
    scroller.style = CS_DBLCLKS;
    scroller.hCursor = LoadCursor(nullptr, IDC_ARROW);
    scroller.lpszClassName = L"LitelyScroll";
    RegisterClass(&scroller);
    scroller.lpfnWndProc = tileMenuProc;
    scroller.style = 0;
    scroller.lpszClassName = L"LitelyMenu";
    RegisterClass(&scroller);
  }
  libraryDpi=GetDpiForSystem();
  RECT work{};SystemParametersInfo(SPI_GETWORKAREA,0,&work,0);
  libraryWindow =
      CreateWindowEx(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Litely",
                     WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, std::min<int>(px(1060),work.right-work.left),
                     std::min<int>(px(720),work.bottom-work.top), nullptr, nullptr, instance, nullptr);
  RECT client{};GetClientRect(libraryWindow,&client);
  SendMessage(libraryWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(client.right,client.bottom));
  themeWindow(libraryWindow);
  fillList();
  sampleStats();
  setUiPage(settings);
  SetWindowPos(libraryWindow,nullptr,0,0,0,0,SWP_NOSIZE|SWP_NOMOVE|SWP_NOZORDER|SWP_SHOWWINDOW);
  RedrawWindow(libraryWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
  SetForegroundWindow(libraryWindow);
}
