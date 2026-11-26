#include "clipboard.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <string>
#include <cstring>
#include <vector>
#include <combaseapi.h>

#include "clipboard_image_wire.h"
#include "platform/clipboard_backend.h"
#include "rd_log.h"
#include "filesystem_utf8.h"

namespace crossdesk {
namespace {

HWND g_clipboard_window = nullptr;
constexpr const char* kClipboardWindowClass = "CrossDeskClipboardMonitor";

LRESULT CALLBACK ClipboardWindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
  if (message == WM_CLIPBOARDUPDATE) {
    platform::HandleClipboardChange();
    return 0;
  }
  return DefWindowProc(window, message, wparam, lparam);
}

}  // namespace

std::string Clipboard::GetText() {
  if (!OpenClipboard(nullptr)) {
    LOG_ERROR("Clipboard::GetText: failed to open clipboard");
    return {};
  }

  std::string result;
  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data) {
    const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
    if (text) {
      const int size =
          WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
      if (size > 1) {
        result.resize(static_cast<size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size,
                            nullptr, nullptr);
        result.resize(static_cast<size_t>(size - 1));
      }
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return result;
}

bool Clipboard::SetText(const std::string& text) {
  if (!OpenClipboard(nullptr)) {
    LOG_ERROR("Clipboard::SetText: failed to open clipboard");
    return false;
  }
  if (!EmptyClipboard()) {
    LOG_ERROR("Clipboard::SetText: failed to empty clipboard");
    CloseClipboard();
    return false;
  }

  const int size =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  HGLOBAL memory =
      size > 0 ? GlobalAlloc(GMEM_MOVEABLE, size * sizeof(wchar_t)) : nullptr;
  if (!memory) {
    LOG_ERROR("Clipboard::SetText: failed to allocate memory");
    CloseClipboard();
    return false;
  }

  wchar_t* output = static_cast<wchar_t*>(GlobalLock(memory));
  if (!output) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, output, size);
  GlobalUnlock(memory);
  if (!SetClipboardData(CF_UNICODETEXT, memory)) {
    GlobalFree(memory);
    CloseClipboard();
    return false;
  }
  CloseClipboard();
  return true;
}

bool Clipboard::HasText() {
  if (!OpenClipboard(nullptr)) return false;
  const bool result = IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                      IsClipboardFormatAvailable(CF_TEXT);
  CloseClipboard();
  return result;
}

std::optional<std::vector<std::string>> Clipboard::GetFiles() {
  if (!IsClipboardFormatAvailable(CF_HDROP)) return std::vector<std::string>{};
  // Preserve file precedence when another process briefly holds the clipboard.
  if (!OpenClipboard(nullptr)) return std::nullopt;
  const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  std::vector<std::string> paths;
  const UINT count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
  if (count <= 128) for (UINT i = 0; i < count; ++i) {
    const UINT size = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(size + 1, L'\0');
    if (!size || !DragQueryFileW(drop, i, path.data(), size + 1)) break;
    path.resize(size);
    const auto utf8 = std::filesystem::path(path).u8string();
    paths.emplace_back(utf8.begin(), utf8.end());
  }
  CloseClipboard();
  if (!count || paths.size() != count) return std::nullopt;
  return paths;
}

uint64_t Clipboard::ChangeCount() { return GetClipboardSequenceNumber(); }

bool Clipboard::SetFiles(const std::vector<std::string>& paths) {
  if (paths.empty()) return false;
  std::wstring list;
  for (const auto& path : paths) {
    const auto native = PathFromUtf8(path);
    if (!native.is_absolute()) return false;
    list.append(native.native()); list.push_back(L'\0');
  }
  list.push_back(L'\0');
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                               sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
  if (!memory) return false;
  auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
  if (!drop) { GlobalFree(memory); return false; }
  drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
  memcpy(reinterpret_cast<char*>(drop) + sizeof(DROPFILES), list.data(),
         list.size() * sizeof(wchar_t));
  GlobalUnlock(memory);
  // OpenClipboard(NULL) cannot own a clipboard after EmptyClipboard().
  static HWND owner = CreateWindowExW(0, L"STATIC", L"CrossDesk file clipboard",
      0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!owner || !OpenClipboard(owner)) { GlobalFree(memory); return false; }
  const bool ok = EmptyClipboard() && SetClipboardData(CF_HDROP, memory);
  if (!ok) GlobalFree(memory);
  if (ok) {
    // Explorer must copy these temporary files, even if the source used Cut.
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effect) {
      auto* value = static_cast<DWORD*>(GlobalLock(effect));
      if (value) {
        *value = DROPEFFECT_COPY; GlobalUnlock(effect);
        if (!SetClipboardData(RegisterClipboardFormatW(L"Preferred DropEffect"), effect))
          GlobalFree(effect);
      } else GlobalFree(effect);
    }
  }
  CloseClipboard();
  return ok;
}

namespace {

// Registers the clipboard format browsers use when an image is copied as a
// compressed PNG payload.
UINT PngClipboardFormat() {
  static const UINT format = RegisterClipboardFormatW(L"PNG");
  return format;
}

// WIC factory with process-wide COM initialization. The GUI thread already
// runs COM for shell integration; CoInitializeEx with COINIT_APARTMENTTHREADED
// is a no-op there and the correct model for WIC. Compile-checked, untested
// on macOS (Windows-only runtime path).
IWICImagingFactory* WicFactory() {
  static struct Factory {
    IWICImagingFactory* instance = nullptr;
    Factory() {
      if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) &&
          FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return;
      if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&instance))))
        instance = nullptr;
    }
    ~Factory() { if (instance) instance->Release(); }
  } factory;
  return factory.instance;
}

// Decodes PNG bytes into a top-down 32bpp BGRA pixel buffer via WIC.
bool DecodePngPixels(const std::string& png, std::vector<BYTE>* pixels,
                     UINT* width_out, UINT* height_out) {
  IWICImagingFactory* factory = WicFactory();
  if (!factory) return false;
  IStream* memory = nullptr;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memory))) return false;
  bool ok = false;
  ULONG written = 0;
  if (SUCCEEDED(memory->Write(png.data(), ULONG(png.size()), &written)) &&
      written == png.size()) {
    LARGE_INTEGER zero = {};
    memory->Seek(zero, STREAM_SEEK_SET, nullptr);
    IWICBitmapDecoder* decoder = nullptr;
    if (SUCCEEDED(factory->CreateDecoderFromStream(
            memory, nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
      IWICBitmapFrameDecode* frame = nullptr;
      if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
        IWICFormatConverter* converter = nullptr;
        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                            WICBitmapDitherTypeNone, nullptr,
                                            0.0, WICBitmapPaletteTypeCustom))) {
          UINT width = 0, height = 0;
          converter->GetSize(&width, &height);
          if (width && height && width <= 32768 && height <= 32768) {
            const UINT stride = width * 4;
            pixels->resize(size_t(stride) * height);
            if (SUCCEEDED(converter->CopyPixels(nullptr, stride,
                                                UINT(pixels->size()),
                                                pixels->data()))) {
              *width_out = width;
              *height_out = height;
              ok = true;
            }
          }
        }
        if (converter) converter->Release();
        frame->Release();
      }
      decoder->Release();
    }
  }
  memory->Release();
  return ok;
}

// Encodes a top-down 32bpp BGRA pixel buffer as PNG bytes via WIC.
bool EncodePngPixels(const BYTE* pixels, UINT width, UINT height,
                     std::string* png_out) {
  IWICImagingFactory* factory = WicFactory();
  if (!factory || !width || !height) return false;
  const UINT stride = width * 4;
  IWICBitmap* bitmap = nullptr;
  if (FAILED(factory->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppBGRA, stride,
          stride * height, const_cast<BYTE*>(pixels), &bitmap)))
    return false;
  bool ok = false;
  IStream* memory = nullptr;
  if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &memory))) {
    IWICBitmapEncoder* encoder = nullptr;
    if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                         &encoder)) &&
        SUCCEEDED(encoder->Initialize(memory, WICBitmapEncoderNoCache))) {
      IWICBitmapFrameEncode* frame = nullptr;
      if (SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height)) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) &&
            SUCCEEDED(frame->WriteSource(bitmap, nullptr)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
          STATSTG stats = {};
          if (SUCCEEDED(memory->Stat(&stats, STATFLAG_NONAME)) &&
              stats.cbSize.QuadPart > 0 &&
              stats.cbSize.QuadPart <= ULONGLONG(clipboard_image::kMaxImageBytes)) {
            png_out->resize(size_t(stats.cbSize.QuadPart));
            LARGE_INTEGER zero = {};
            memory->Seek(zero, STREAM_SEEK_SET, nullptr);
            ULONG read = 0;
            if (SUCCEEDED(memory->Read(png_out->data(), ULONG(png_out->size()),
                                       &read)) &&
                read == png_out->size())
              ok = true;
          }
        }
        frame->Release();
      }
    }
    if (encoder) encoder->Release();
    memory->Release();
  }
  bitmap->Release();
  return ok;
}

// Converts PNG bytes to a CF_DIB payload (BITMAPINFOHEADER + bottom-up BGRA).
bool PngToDibBytes(const std::string& png, std::string* dib_out) {
  std::vector<BYTE> pixels;
  UINT width = 0, height = 0;
  if (!DecodePngPixels(png, &pixels, &width, &height)) return false;
  const size_t stride = size_t(width) * 4;
  BITMAPINFOHEADER header = {};
  header.biSize = sizeof(header);
  header.biWidth = LONG(width);
  header.biHeight = LONG(height);  // bottom-up
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  dib_out->resize(sizeof(header) + stride * height);
  std::memcpy(dib_out->data(), &header, sizeof(header));
  char* out = dib_out->data() + sizeof(header);
  for (UINT row = 0; row < height; ++row)
    std::memcpy(out + size_t(row) * stride,
                pixels.data() + size_t(height - 1 - row) * stride, stride);
  return true;
}

// Converts a CF_DIB payload (BITMAPINFOHEADER + 24/32bpp pixels) to PNG.
bool DibToPngBytes(const void* dib, size_t size, std::string* png_out) {
  if (!dib || size < sizeof(BITMAPINFOHEADER)) return false;
  const auto* header = static_cast<const BITMAPINFOHEADER*>(dib);
  if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biPlanes != 1 ||
      header->biCompression != BI_RGB ||
      (header->biBitCount != 32 && header->biBitCount != 24))
    return false;
  const LONG width = header->biWidth;
  const LONG height = header->biHeight < 0 ? -header->biHeight : header->biHeight;
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return false;
  const UINT bits = UINT(header->biBitCount);
  const size_t stride_in = (size_t(width) * bits + 31) / 32 * 4;
  if (sizeof(BITMAPINFOHEADER) + stride_in * size_t(height) > size) return false;

  // Normalize to top-down 32bpp BGRA for the encoder.
  std::vector<BYTE> pixels(size_t(width) * 4 * size_t(height));
  const auto* rows = static_cast<const BYTE*>(dib) + sizeof(BITMAPINFOHEADER);
  const bool bottom_up = header->biHeight > 0;
  for (LONG row = 0; row < height; ++row) {
    const LONG source_row = bottom_up ? height - 1 - row : row;
    const BYTE* source = rows + size_t(source_row) * stride_in;
    BYTE* target = pixels.data() + size_t(row) * size_t(width) * 4;
    if (bits == 32) {
      std::memcpy(target, source, size_t(width) * 4);
    } else {
      for (LONG x = 0; x < width; ++x) {
        target[x * 4 + 0] = source[x * 3 + 0];  // B
        target[x * 4 + 1] = source[x * 3 + 1];  // G
        target[x * 4 + 2] = source[x * 3 + 2];  // R
        target[x * 4 + 3] = 0xff;               // opaque
      }
    }
  }
  return EncodePngPixels(pixels.data(), UINT(width), UINT(height), png_out);
}

// Precedence rule for Windows image sync: an image is the primary payload
// when the clipboard carries CF_DIB or the registered PNG format and no
// text format is present. Windows clipboard owners do not declare a type
// ordering, so text wins whenever both exist: browsers copying an image
// always include a URL string, and pasting that as pixels would corrupt the
// far more common text experience. Dedicated screenshot tools publish no
// text at all, which is exactly the case this feature serves.
// Compile-checked, untested on macOS (Windows-only runtime path).
bool ImageIsPrimaryPayload() {
  const bool has_image = IsClipboardFormatAvailable(CF_DIB) ||
                         IsClipboardFormatAvailable(PngClipboardFormat());
  if (!has_image) return false;
  return !IsClipboardFormatAvailable(CF_UNICODETEXT) &&
         !IsClipboardFormatAvailable(CF_TEXT);
}

}  // namespace

bool Clipboard::HasImage() { return ImageIsPrimaryPayload(); }

std::optional<std::string> Clipboard::GetImagePng() {
  if (!ImageIsPrimaryPayload()) return std::nullopt;
  if (!OpenClipboard(nullptr)) return std::nullopt;
  std::optional<std::string> result;

  // Prefer the PNG format when present: zero re-encode.
  if (HANDLE data = GetClipboardData(PngClipboardFormat())) {
    if (const void* bytes = GlobalLock(data)) {
      const size_t size = GlobalSize(data);
      if (size > 0 && size <= clipboard_image::kMaxImageBytes &&
          clipboard_image::ValidPngSignature(bytes, size))
        result = std::string(static_cast<const char*>(bytes), size);
      GlobalUnlock(data);
    }
  }
  if (!result) {
    if (HANDLE data = GetClipboardData(CF_DIB)) {
      if (const void* bytes = GlobalLock(data)) {
        const size_t size = GlobalSize(data);
        std::string png;
        if (DibToPngBytes(bytes, size, &png)) result = std::move(png);
        GlobalUnlock(data);
      }
    }
  }
  CloseClipboard();
  return result;
}

bool Clipboard::SetImagePng(const std::string& png) {
  if (!clipboard_image::ValidPngSignature(png.data(), png.size()) ||
      png.size() > clipboard_image::kMaxImageBytes)
    return false;
  std::string dib;
  if (!PngToDibBytes(png, &dib)) {
    LOG_ERROR("Clipboard::SetImagePng: PNG to DIB conversion failed");
    return false;
  }
  static HWND owner = CreateWindowExW(0, L"STATIC", L"CrossDesk image clipboard",
      0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!owner || !OpenClipboard(owner)) return false;
  bool ok = false;
  if (EmptyClipboard()) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, dib.size());
    if (memory) {
      if (void* target = GlobalLock(memory)) {
        std::memcpy(target, dib.data(), dib.size());
        GlobalUnlock(memory);
        if (SetClipboardData(CF_DIB, memory)) {
          ok = true;
          // Also publish the raw PNG bytes for browsers and modern apps that
          // prefer the compressed format. Best-effort; failure is harmless.
          if (HGLOBAL png_memory = GlobalAlloc(GMEM_MOVEABLE, png.size())) {
            if (void* target_png = GlobalLock(png_memory)) {
              std::memcpy(target_png, png.data(), png.size());
              GlobalUnlock(png_memory);
              if (!SetClipboardData(PngClipboardFormat(), png_memory))
                GlobalFree(png_memory);
            } else {
              GlobalFree(png_memory);
            }
          }
        } else {
          GlobalFree(memory);
        }
      } else {
        GlobalFree(memory);
      }
    }
  }
  CloseClipboard();
  return ok;
}

namespace platform {

void RunClipboardMonitor(int) {
  WNDCLASSA window_class = {};
  window_class.lpfnWndProc = ClipboardWindowProc;
  window_class.hInstance = GetModuleHandle(nullptr);
  window_class.lpszClassName = kClipboardWindowClass;
  RegisterClassA(&window_class);

  g_clipboard_window = CreateWindowA(
      kClipboardWindowClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
      nullptr, nullptr);
  if (!g_clipboard_window ||
      !AddClipboardFormatListener(g_clipboard_window)) {
    LOG_ERROR("Failed to initialize clipboard monitor window");
    if (g_clipboard_window) DestroyWindow(g_clipboard_window);
    g_clipboard_window = nullptr;
    return;
  }

  LOG_INFO("Clipboard event monitoring started (Windows)");
  MSG message = {};
  while (ClipboardMonitoring()) {
    const BOOL result = GetMessage(&message, nullptr, 0, 0);
    if (result == 0 || result == -1) break;
    TranslateMessage(&message);
    DispatchMessage(&message);
  }

  RemoveClipboardFormatListener(g_clipboard_window);
  DestroyWindow(g_clipboard_window);
  g_clipboard_window = nullptr;
  UnregisterClassA(kClipboardWindowClass, GetModuleHandle(nullptr));
}

void WakeClipboardMonitor() {
  if (g_clipboard_window) PostMessage(g_clipboard_window, WM_QUIT, 0, 0);
}

}  // namespace platform
}  // namespace crossdesk
