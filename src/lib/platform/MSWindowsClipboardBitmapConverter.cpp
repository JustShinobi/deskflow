/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardBitmapConverter.h"

#include "base/Log.h"

#include <QtEndian>

#include <cstdlib>
#include <limits>

namespace {
bool normaliseMalformedMacDib(const std::string &data, std::string &normalisedData)
{
  if (data.size() < sizeof(BITMAPINFOHEADER)) {
    return false;
  }

  const auto *header = reinterpret_cast<const BITMAPINFOHEADER *>(data.data());
  if (header->biWidth <= 0) {
    return false;
  }

  const auto width = static_cast<size_t>(header->biWidth);
  const auto height = static_cast<size_t>(std::abs(static_cast<int64_t>(header->biHeight)));
  if (height == 0 || width > (std::numeric_limits<size_t>::max() - sizeof(BITMAPINFOHEADER)) / 4 / height) {
    return false;
  }
  const auto expectedSize = sizeof(BITMAPINFOHEADER) + width * height * 4;

  // macOS can describe an INFOHEADER-sized 32-bit pixel payload as a V5 DIB.
  // Windows then interprets the first pixels as V5 colour masks. The pixel
  // bytes are ordinary BGRA, so publish a canonical BI_RGB DIB instead.
  if (header->biSize <= sizeof(BITMAPINFOHEADER) || header->biPlanes != 1 || header->biBitCount != 32 ||
      header->biCompression != BI_BITFIELDS || expectedSize != data.size()) {
    return false;
  }

  normalisedData = data;
  qToLittleEndian<quint32>(sizeof(BITMAPINFOHEADER), reinterpret_cast<quint8 *>(&normalisedData[0]));
  qToLittleEndian<quint32>(BI_RGB, reinterpret_cast<quint8 *>(&normalisedData[0]) + 16);
  LOG_INFO("normalised malformed macOS clipboard image to BI_RGB");
  return true;
}
} // namespace

//
// MSWindowsClipboardBitmapConverter
//

IClipboard::Format MSWindowsClipboardBitmapConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

UINT MSWindowsClipboardBitmapConverter::getWin32Format() const
{
  return CF_DIB;
}

HANDLE
MSWindowsClipboardBitmapConverter::fromIClipboard(const std::string &data) const
{
  if (data.size() < sizeof(BITMAPINFOHEADER)) {
    return nullptr;
  }

  std::string normalisedData;
  const auto *clipboardData = &data;
  if (normaliseMalformedMacDib(data, normalisedData)) {
    clipboardData = &normalisedData;
  }

  const auto *hdr = reinterpret_cast<const BITMAPINFOHEADER *>(clipboardData->data());
  if (hdr->biSize < sizeof(BITMAPINFOHEADER) || hdr->biSize > clipboardData->size() || hdr->biWidth <= 0 ||
      hdr->biHeight == 0) {
    return nullptr;
  }

  // copy to memory handle
  HGLOBAL gData = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, clipboardData->size());
  if (gData != nullptr) {
    // get a pointer to the allocated memory
    char *dst = (char *)GlobalLock(gData);
    if (dst != nullptr) {
      memcpy(dst, clipboardData->data(), clipboardData->size());
      GlobalUnlock(gData);
    } else {
      GlobalFree(gData);
      gData = nullptr;
    }
  }

  return gData;
}

std::string MSWindowsClipboardBitmapConverter::toIClipboard(HANDLE data) const
{
  // get datator
  LPVOID src = GlobalLock(data);
  if (src == nullptr) {
    return std::string();
  }
  const size_t srcSize = static_cast<size_t>(GlobalSize(data));
  if (srcSize < sizeof(BITMAPINFOHEADER)) {
    GlobalUnlock(data);
    return std::string();
  }

  // check image type and bounds against untrusted clipboard handles
  const BITMAPINFO *bitmap = static_cast<const BITMAPINFO *>(src);
  const LONG rawW = bitmap->bmiHeader.biWidth;
  const LONG rawH = bitmap->bmiHeader.biHeight;
  if (bitmap->bmiHeader.biSize < sizeof(BITMAPINFOHEADER) || bitmap->bmiHeader.biSize > srcSize || rawW <= 0 ||
      rawH == 0 || bitmap->bmiHeader.biPlanes != 1) {
    GlobalUnlock(data);
    return std::string();
  }

  const size_t w64 = static_cast<size_t>(rawW);
  const int64_t hSigned = static_cast<int64_t>(rawH);
  const size_t absH64 = static_cast<size_t>((hSigned < 0) ? -hSigned : hSigned);
  static constexpr size_t kMaxDimension = 32768;
  if (w64 > kMaxDimension || absH64 > kMaxDimension) {
    GlobalUnlock(data);
    return std::string();
  }

  LOG(
      (CLOG_INFO "bitmap: %dx%d %d", bitmap->bmiHeader.biWidth, bitmap->bmiHeader.biHeight,
       (int)bitmap->bmiHeader.biBitCount)
  );
  if ((bitmap->bmiHeader.biBitCount == 24 || bitmap->bmiHeader.biBitCount == 32) &&
      bitmap->bmiHeader.biCompression == BI_RGB) {
    const size_t rowStride = (((w64 * static_cast<size_t>(bitmap->bmiHeader.biBitCount)) + 31U) / 32U) * 4U;
    const size_t hdrOffset = static_cast<size_t>(bitmap->bmiHeader.biSize);
    if (rowStride <= (srcSize - hdrOffset) / absH64) {
      // already in canonical form
      std::string image(static_cast<char const *>(src), srcSize);
      GlobalUnlock(data);
      return image;
    }
    GlobalUnlock(data);
    return std::string();
  }

  // Fast-path modern Windows 32-bit BI_BITFIELDS (standard BGRA8888 masks) without GDI round-trip
  if (bitmap->bmiHeader.biBitCount == 32 && bitmap->bmiHeader.biCompression == BI_BITFIELDS) {
    const size_t maskOffset = sizeof(BITMAPINFOHEADER);
    const size_t biSize = static_cast<size_t>(bitmap->bmiHeader.biSize);
    if (biSize == sizeof(BITMAPINFOHEADER) || biSize >= sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD)) {
      const size_t pixelOffset =
          (biSize == sizeof(BITMAPINFOHEADER)) ? (sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD)) : biSize;
      if (pixelOffset <= srcSize && w64 <= (srcSize - pixelOffset) / 4U / absH64) {
        const size_t pixelBytes = w64 * absH64 * 4U;
        const auto *rawBytes = static_cast<const quint8 *>(src);
        const quint32 rMask = qFromLittleEndian<quint32>(rawBytes + maskOffset);
        const quint32 gMask = qFromLittleEndian<quint32>(rawBytes + maskOffset + 4);
        const quint32 bMask = qFromLittleEndian<quint32>(rawBytes + maskOffset + 8);
        if (rMask == 0x00ff0000u && gMask == 0x0000ff00u && bMask == 0x000000ffu) {
          BITMAPINFOHEADER canonical = bitmap->bmiHeader;
          canonical.biSize = sizeof(BITMAPINFOHEADER);
          canonical.biCompression = BI_RGB;
          canonical.biSizeImage = 0;
          canonical.biClrUsed = 0;
          canonical.biClrImportant = 0;
          std::string image;
          image.reserve(sizeof(BITMAPINFOHEADER) + pixelBytes);
          image.append(reinterpret_cast<const char *>(&canonical), sizeof(BITMAPINFOHEADER));
          image.append(reinterpret_cast<const char *>(rawBytes + pixelOffset), pixelBytes);
          GlobalUnlock(data);
          return image;
        }
      }
    }
  }

  // Compute and validate pixel data offset & size before invoking GDI
  size_t srcBitsOffset = static_cast<size_t>(bitmap->bmiHeader.biSize);
  const WORD bitCount = bitmap->bmiHeader.biBitCount;
  if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 && bitCount != 24 && bitCount != 32) {
    GlobalUnlock(data);
    return std::string();
  }
  if (bitCount >= 16) {
    if (bitmap->bmiHeader.biCompression == BI_BITFIELDS && bitmap->bmiHeader.biSize == sizeof(BITMAPINFOHEADER) &&
        (bitCount == 16 || bitCount == 32)) {
      srcBitsOffset += 3 * sizeof(DWORD);
    }
  } else if (bitmap->bmiHeader.biClrUsed != 0) {
    if (bitmap->bmiHeader.biClrUsed > (srcSize - srcBitsOffset) / sizeof(RGBQUAD)) {
      GlobalUnlock(data);
      return std::string();
    }
    srcBitsOffset += static_cast<size_t>(bitmap->bmiHeader.biClrUsed) * sizeof(RGBQUAD);
  } else {
    const size_t paletteBytes = (size_t{1} << bitCount) * sizeof(RGBQUAD);
    if (paletteBytes > srcSize - srcBitsOffset) {
      GlobalUnlock(data);
      return std::string();
    }
    srcBitsOffset += paletteBytes;
  }

  if (srcBitsOffset > srcSize) {
    GlobalUnlock(data);
    return std::string();
  }
  const size_t availPixelBytes = srcSize - srcBitsOffset;
  if (bitmap->bmiHeader.biCompression == BI_RGB || bitmap->bmiHeader.biCompression == BI_BITFIELDS) {
    const size_t rowStride = (((w64 * static_cast<size_t>(bitCount)) + 31U) / 32U) * 4U;
    if (rowStride > availPixelBytes / absH64) {
      GlobalUnlock(data);
      return std::string();
    }
  } else {
    if (bitmap->bmiHeader.biSizeImage == 0 || bitmap->bmiHeader.biSizeImage > availPixelBytes) {
      GlobalUnlock(data);
      return std::string();
    }
  }

  // create a destination DIB section
  LOG_INFO("convert image from: depth=%d comp=%d", bitmap->bmiHeader.biBitCount, bitmap->bmiHeader.biCompression);
  void *raw = nullptr;
  BITMAPINFOHEADER info;
  LONG w = rawW;
  LONG h = rawH;
  const LONG absHeight = static_cast<LONG>(absH64);
  info.biSize = sizeof(BITMAPINFOHEADER);
  info.biWidth = w;
  info.biHeight = h;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  info.biSizeImage = 0;
  info.biXPelsPerMeter = 1000;
  info.biYPelsPerMeter = 1000;
  info.biClrUsed = 0;
  info.biClrImportant = 0;
  HDC dc = GetDC(nullptr);
  HBITMAP dst = CreateDIBSection(dc, (BITMAPINFO *)&info, DIB_RGB_COLORS, &raw, nullptr, 0);
  if (dst == nullptr || raw == nullptr) {
    LOG_WARN("failed to allocate destination bitmap for clipboard image");
    ReleaseDC(nullptr, dc);
    GlobalUnlock(data);
    return std::string();
  }

  const char *srcBits = static_cast<const char *>(src) + srcBitsOffset;

  // copy source image to destination image
  HDC dstDC = CreateCompatibleDC(dc);
  HGDIOBJ oldBitmap = SelectObject(dstDC, dst);
  SetDIBitsToDevice(dstDC, 0, 0, w, absHeight, 0, 0, 0, absHeight, srcBits, bitmap, DIB_RGB_COLORS);
  SelectObject(dstDC, oldBitmap);
  DeleteDC(dstDC);
  GdiFlush();

  // extract data
  std::string image((const char *)&info, info.biSize);
  image.append((const char *)raw, 4U * w64 * absH64);

  // clean up GDI
  DeleteObject(dst);
  ReleaseDC(nullptr, dc);

  // release handle
  GlobalUnlock(data);

  return image;
}
