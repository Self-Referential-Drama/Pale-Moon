/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ipc/AutoOpenSurface.h"
#include "mozilla/layers/PLayerTransaction.h"
#include "gfxSharedImageSurface.h"

#include "ImageLayerD3D9.h"
#include "ThebesLayerD3D9.h"
#include "gfxPlatform.h"
#include "gfxImageSurface.h"
#include "yuv_convert.h"
#include "nsIServiceManager.h"
#include "nsIConsoleService.h"
#include "D3D9SurfaceImage.h"

namespace mozilla {
namespace layers {

static inline _D3DFORMAT
D3dFormatForGfxFormat(gfxImageFormat aFormat)
{
  if (aFormat == gfxASurface::ImageFormatA8) {
    return D3DFMT_A8;
  }

  return D3DFMT_A8R8G8B8;
}

static already_AddRefed<IDirect3DTexture9>
DataToTexture(IDirect3DDevice9 *aDevice,
              unsigned char *aData,
              int aStride,
              const gfxIntSize &aSize,
              _D3DFORMAT aFormat)
{
  nsRefPtr<IDirect3DTexture9> texture;
  nsRefPtr<IDirect3DDevice9Ex> deviceEx;
  aDevice->QueryInterface(IID_IDirect3DDevice9Ex,
                          (void**)getter_AddRefs(deviceEx));

  nsRefPtr<IDirect3DSurface9> surface;
  D3DLOCKED_RECT lockedRect;
  if (deviceEx) {
    // D3D9Ex doesn't support managed textures. We could use dynamic textures
    // here but since Images are immutable that probably isn't such a great
    // idea.
    if (FAILED(aDevice->
               CreateTexture(aSize.width, aSize.height,
                             1, 0, aFormat, D3DPOOL_DEFAULT,
                             getter_AddRefs(texture), NULL)))
    {
      return NULL;
    }

    nsRefPtr<IDirect3DTexture9> tmpTexture;
    if (FAILED(aDevice->
               CreateTexture(aSize.width, aSize.height,
                             1, 0, aFormat, D3DPOOL_SYSTEMMEM,
                             getter_AddRefs(tmpTexture), NULL)))
    {
      return NULL;
    }

    tmpTexture->GetSurfaceLevel(0, getter_AddRefs(surface));
    HRESULT hr = surface->LockRect(&lockedRect, NULL, 0);
    if (FAILED(hr) || !lockedRect.pBits) {
      NS_WARNING("Imagelayer D3D9: Could not lock surface");
      return NULL;
    }
  } else {
    if (FAILED(aDevice->
               CreateTexture(aSize.width, aSize.height,
                             1, 0, aFormat, D3DPOOL_MANAGED,
                             getter_AddRefs(texture), NULL)))
    {
      return NULL;
    }

    /* lock the entire texture */
    texture->LockRect(0, &lockedRect, NULL, 0);
  }

  uint32_t width = aSize.width;
  if (aFormat == D3DFMT_A8R8G8B8) {
    width *= 4;
  }
  for (int y = 0; y < aSize.height; y++) {
    memcpy((char*)lockedRect.pBits + lockedRect.Pitch * y,
            aData + aStride * y,
            width);
  }

  if (deviceEx) {
    surface->UnlockRect();
    nsRefPtr<IDirect3DSurface9> dstSurface;
    texture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
    aDevice->UpdateSurface(surface, NULL, dstSurface, NULL);
  } else {
    texture->UnlockRect(0);
  }

  return texture.forget();
}

static already_AddRefed<IDirect3DTexture9>
OpenSharedTexture(const D3DSURFACE_DESC& aDesc,
                  HANDLE aShareHandle,
                  IDirect3DDevice9 *aDevice)
{
  MOZ_ASSERT(aDesc.Format == D3DFMT_X8R8G8B8);

  // Open the frame from DXVA's device in our device using the resource
  // sharing handle.
  nsRefPtr<IDirect3DTexture9> sharedTexture;
  HRESULT hr = aDevice->CreateTexture(aDesc.Width,
                                      aDesc.Height,
                                      1,
                                      D3DUSAGE_RENDERTARGET,
                                      D3DFMT_X8R8G8B8,
                                      D3DPOOL_DEFAULT,
                                      getter_AddRefs(sharedTexture),
                                      &aShareHandle);
  if (FAILED(hr)) {
    NS_WARNING("Failed to open shared texture on our device");
  }
  return sharedTexture.forget();
}

static already_AddRefed<IDirect3DTexture9>
SurfaceToTexture(IDirect3DDevice9 *aDevice,
                 gfxASurface *aSurface,
                 const gfxIntSize &aSize)
{

  nsRefPtr<gfxImageSurface> imageSurface = aSurface->GetAsImageSurface();

  if (!imageSurface) {
    imageSurface = new gfxImageSurface(aSize,
                                       gfxASurface::ImageFormatARGB32);

    nsRefPtr<gfxContext> context = new gfxContext(imageSurface);
    context->SetSource(aSurface);
    context->SetOperator(gfxContext::OPERATOR_SOURCE);
    context->Paint();
  }

  return DataToTexture(aDevice, imageSurface->Data(), imageSurface->Stride(),
                       aSize, D3dFormatForGfxFormat(imageSurface->Format()));
}

static void AllocateTexturesYCbCr(PlanarYCbCrImage *aImage,
                                  IDirect3DDevice9 *aDevice,
                                  LayerManagerD3D9 *aManager)
{
  nsAutoPtr<PlanarYCbCrD3D9BackendData> backendData(
    new PlanarYCbCrD3D9BackendData);

  const PlanarYCbCrImage::Data *data = aImage->GetData();

  D3DLOCKED_RECT lockrectY;
  D3DLOCKED_RECT lockrectCb;
  D3DLOCKED_RECT lockrectCr;
  uint8_t* src;
  uint8_t* dest;

  nsRefPtr<IDirect3DSurface9> tmpSurfaceY;
  nsRefPtr<IDirect3DSurface9> tmpSurfaceCb;
  nsRefPtr<IDirect3DSurface9> tmpSurfaceCr;

  nsRefPtr<IDirect3DDevice9Ex> deviceEx;
  aDevice->QueryInterface(IID_IDirect3DDevice9Ex,
                           getter_AddRefs(deviceEx));

  bool isD3D9Ex = deviceEx;

  if (isD3D9Ex) {
    nsRefPtr<IDirect3DTexture9> tmpYTexture;
    nsRefPtr<IDirect3DTexture9> tmpCbTexture;
    nsRefPtr<IDirect3DTexture9> tmpCrTexture;
    // D3D9Ex does not support the managed pool, could use dynamic textures
    // here. But since an Image is immutable static textures are probably a
    // better idea.

    HRESULT hr;
    hr = aDevice->CreateTexture(data->mYSize.width, data->mYSize.height,
                                1, 0, D3DFMT_L8, D3DPOOL_DEFAULT,
                                getter_AddRefs(backendData->mYTexture), NULL);
    if (!FAILED(hr)) {
      hr = aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                                  1, 0, D3DFMT_L8, D3DPOOL_DEFAULT,
                                  getter_AddRefs(backendData->mCbTexture), NULL);
    }
    if (!FAILED(hr)) {
      hr = aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                                  1, 0, D3DFMT_L8, D3DPOOL_DEFAULT,
                                  getter_AddRefs(backendData->mCrTexture), NULL);
    }
    if (!FAILED(hr)) {
      hr = aDevice->CreateTexture(data->mYSize.width, data->mYSize.height,
                                  1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM,
                                  getter_AddRefs(tmpYTexture), NULL);
    }
    if (!FAILED(hr)) {
      hr = aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                                  1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM,
                                  getter_AddRefs(tmpCbTexture), NULL);
    }
    if (!FAILED(hr)) {
      hr = aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                                  1, 0, D3DFMT_L8, D3DPOOL_SYSTEMMEM,
                                  getter_AddRefs(tmpCrTexture), NULL);
    }

    if (FAILED(hr)) {
      aManager->ReportFailure(NS_LITERAL_CSTRING("PlanarYCbCrImageD3D9::AllocateTextures(): Failed to create texture (isD3D9Ex)"),
                              hr);
      return;
    }

    tmpYTexture->GetSurfaceLevel(0, getter_AddRefs(tmpSurfaceY));
    tmpCbTexture->GetSurfaceLevel(0, getter_AddRefs(tmpSurfaceCb));
    tmpCrTexture->GetSurfaceLevel(0, getter_AddRefs(tmpSurfaceCr));
    tmpSurfaceY->LockRect(&lockrectY, NULL, 0);
    tmpSurfaceCb->LockRect(&lockrectCb, NULL, 0);
    tmpSurfaceCr->LockRect(&lockrectCr, NULL, 0);
  } else {
    HRESULT hr;
    hr = aDevice->CreateTexture(data->mYSize.width, data->mYSize.height,
                                1, 0, D3DFMT_L8, D3DPOOL_MANAGED,
                                getter_AddRefs(backendData->mYTexture), NULL);
    if (!FAILED(hr)) {
      aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                             1, 0, D3DFMT_L8, D3DPOOL_MANAGED,
                             getter_AddRefs(backendData->mCbTexture), NULL);
    }
    if (!FAILED(hr)) {
      aDevice->CreateTexture(data->mCbCrSize.width, data->mCbCrSize.height,
                             1, 0, D3DFMT_L8, D3DPOOL_MANAGED,
                             getter_AddRefs(backendData->mCrTexture), NULL);
    }

    if (FAILED(hr)) {
      aManager->ReportFailure(NS_LITERAL_CSTRING("PlanarYCbCrImageD3D9::AllocateTextures(): Failed to create texture (!isD3D9Ex)"),
                              hr);
      return;
    }

    /* lock the entire texture */
    backendData->mYTexture->LockRect(0, &lockrectY, NULL, 0);
    backendData->mCbTexture->LockRect(0, &lockrectCb, NULL, 0);
    backendData->mCrTexture->LockRect(0, &lockrectCr, NULL, 0);
  }

  src  = data->mYChannel;
  //FIX cast
  dest = (uint8_t*)lockrectY.pBits;

  // copy over data
  for (int h=0; h<data->mYSize.height; h++) {
    memcpy(dest, src, data->mYSize.width);
    dest += lockrectY.Pitch;
    src += data->mYStride;
  }

  src  = data->mCbChannel;
  //FIX cast
  dest = (uint8_t*)lockrectCb.pBits;

  // copy over data
  for (int h=0; h<data->mCbCrSize.height; h++) {
    memcpy(dest, src, data->mCbCrSize.width);
    dest += lockrectCb.Pitch;
    src += data->mCbCrStride;
  }

  src  = data->mCrChannel;
  //FIX cast
  dest = (uint8_t*)lockrectCr.pBits;

  // copy over data
  for (int h=0; h<data->mCbCrSize.height; h++) {
    memcpy(dest, src, data->mCbCrSize.width);
    dest += lockrectCr.Pitch;
    src += data->mCbCrStride;
  }

  if (isD3D9Ex) {
    tmpSurfaceY->UnlockRect();
    tmpSurfaceCb->UnlockRect();
    tmpSurfaceCr->UnlockRect();
    nsRefPtr<IDirect3DSurface9> dstSurface;
    backendData->mYTexture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
    aDevice->UpdateSurface(tmpSurfaceY, NULL, dstSurface, NULL);
    backendData->mCbTexture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
    aDevice->UpdateSurface(tmpSurfaceCb, NULL, dstSurface, NULL);
    backendData->mCrTexture->GetSurfaceLevel(0, getter_AddRefs(dstSurface));
    aDevice->UpdateSurface(tmpSurfaceCr, NULL, dstSurface, NULL);
  } else {
    backendData->mYTexture->UnlockRect(0);
    backendData->mCbTexture->UnlockRect(0);
    backendData->mCrTexture->UnlockRect(0);
  }

  aImage->SetBackendData(mozilla::layers::LAYERS_D3D9, backendData.forget());
}

Layer*
ImageLayerD3D9::GetLayer()
{
  return this;
}

/*
  * Returns a texture which backs aImage
  * Will only work if aImage is a cairo or remote image.
  * Returns nullptr if unsuccessful.
  * If successful, aHasAlpha will be set to true if the texture has an
  * alpha component, false otherwise.
  */
IDirect3DTexture9*
ImageLayerD3D9::GetTexture(Image *aImage, bool& aHasAlpha)
{
  NS_ASSERTION(aImage, "Null image.");

  if (aImage->GetFormat() == REMOTE_IMAGE_BITMAP) {
    RemoteBitmapImage *remoteImage =
      static_cast<RemoteBitmapImage*>(aImage);

    if (!aImage->GetBackendData(mozilla::layers::LAYERS_D3D9)) {
      nsAutoPtr<TextureD3D9BackendData> dat(new TextureD3D9BackendData());
      dat->mTexture = DataToTexture(device(), remoteImage->mData, remoteImage->mStride, remoteImage->mSize, D3DFMT_A8R8G8B8);
      if (dat->mTexture) {
        aImage->SetBackendData(mozilla::layers::LAYERS_D3D9, dat.forget());
      }
    }

    aHasAlpha = remoteImage->mFormat == RemoteImageData::BGRA32;
  } else if (aImage->GetFormat() == CAIRO_SURFACE) {
    CairoImage *cairoImage =
      static_cast<CairoImage*>(aImage);

    if (!cairoImage->mSurface) {
      return nullptr;
    }

    if (!aImage->GetBackendData(mozilla::layers::LAYERS_D3D9)) {
      nsAutoPtr<TextureD3D9BackendData> dat(new TextureD3D9BackendData());
      dat->mTexture = SurfaceToTexture(device(), cairoImage->mSurface, cairoImage->mSize);
      if (dat->mTexture) {
        aImage->SetBackendData(mozilla::layers::LAYERS_D3D9, dat.forget());
      }
    }

    aHasAlpha = cairoImage->mSurface->GetContentType() == gfxASurface::CONTENT_COLOR_ALPHA;
  } else if (aImage->GetFormat() == D3D9_RGB32_TEXTURE) {
    if (!aImage->GetBackendData(mozilla::layers::LAYERS_D3D9)) {
      // The texture in which the frame is stored belongs to DXVA's D3D9 device.
      // We need to open it on our device before we can use it.
      nsAutoPtr<TextureD3D9BackendData> backendData(new TextureD3D9BackendData());
      D3D9SurfaceImage* image = static_cast<D3D9SurfaceImage*>(aImage);
      backendData->mTexture = OpenSharedTexture(image->GetDesc(), image->GetShareHandle(), device());
      if (backendData->mTexture) {
        aImage->SetBackendData(mozilla::layers::LAYERS_D3D9, backendData.forget());
      }
    }
    aHasAlpha = false;
  } else {
    NS_WARNING("Inappropriate image type.");
    return nullptr;
  }

  TextureD3D9BackendData *data =
    static_cast<TextureD3D9BackendData*>(aImage->GetBackendData(mozilla::layers::LAYERS_D3D9));

  if (!data) {
    return nullptr;
  }

  nsRefPtr<IDirect3DDevice9> dev;
  data->mTexture->GetDevice(getter_AddRefs(dev));
  if (dev != device()) {
    return nullptr;
  }

  return data->mTexture;
}

void
ImageLayerD3D9::RenderLayer()
{
  ImageContainer *container = GetContainer();
  if (!container || mD3DManager->CompositingDisabled()) {
    return;
  }

  AutoLockImage autoLock(container);

  Image *image = autoLock.GetImage();
  if (!image) {
    return;
  }

  SetShaderTransformAndOpacity();

  gfxIntSize size = image->GetSize();

  if (image->GetFormat() == CAIRO_SURFACE ||
      image->GetFormat() == REMOTE_IMAGE_BITMAP ||
      image->GetFormat() == D3D9_RGB32_TEXTURE)
  {
    NS_ASSERTION(image->GetFormat() != CAIRO_SURFACE ||
                 !static_cast<CairoImage*>(image)->mSurface ||
                 static_cast<CairoImage*>(image)->mSurface->GetContentType() != gfxASurface::CONTENT_ALPHA,
                 "Image layer has alpha image");

    bool hasAlpha = false;
    nsRefPtr<IDirect3DTexture9> texture = GetTexture(image, hasAlpha);

    device()->SetVertexShaderConstantF(CBvLayerQuad,
                                       ShaderConstantRect(0,
                                                          0,
                                                          size.width,
                                                          size.height),
                                       1);

    if (hasAlpha) {
      mD3DManager->SetShaderMode(DeviceManagerD3D9::RGBALAYER, GetMaskLayer());
    } else {
      mD3DManager->SetShaderMode(DeviceManagerD3D9::RGBLAYER, GetMaskLayer());
    }

    if (mFilter == gfxPattern::FILTER_NEAREST) {
      device()->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      device()->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    }
    device()->SetTexture(0, texture);

    image = nullptr;
    autoLock.Unlock();

    device()->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
    if (mFilter == gfxPattern::FILTER_NEAREST) {
      device()->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
      device()->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    }
  } else {
    PlanarYCbCrImage *yuvImage =
      static_cast<PlanarYCbCrImage*>(image);

    if (!yuvImage->IsValid()) {
      return;
    }

    if (!yuvImage->GetBackendData(mozilla::layers::LAYERS_D3D9)) {
      AllocateTexturesYCbCr(yuvImage, device(), mD3DManager);
    }

    PlanarYCbCrD3D9BackendData *data =
      static_cast<PlanarYCbCrD3D9BackendData*>(yuvImage->GetBackendData(mozilla::layers::LAYERS_D3D9));

    if (!data) {
      return;
    }

    nsRefPtr<IDirect3DDevice9> dev;
    data->mYTexture->GetDevice(getter_AddRefs(dev));
    if (dev != device()) {
      return;
    }

    device()->SetVertexShaderConstantF(CBvLayerQuad,
                                       ShaderConstantRect(0,
                                                          0,
                                                          size.width,
                                                          size.height),
                                       1);

    device()->SetVertexShaderConstantF(CBvTextureCoords,
      ShaderConstantRect(
        (float)yuvImage->GetData()->mPicX / yuvImage->GetData()->mYSize.width,
        (float)yuvImage->GetData()->mPicY / yuvImage->GetData()->mYSize.height,
        (float)yuvImage->GetData()->mPicSize.width / yuvImage->GetData()->mYSize.width,
        (float)yuvImage->GetData()->mPicSize.height / yuvImage->GetData()->mYSize.height
      ),
      1);

    mD3DManager->SetShaderMode(DeviceManagerD3D9::YCBCRLAYER, GetMaskLayer());

    // Linear scaling is default here, adhering to mFilter is difficult since
    // presumably even with point filtering we'll still want chroma upsampling
    // to be linear. In the current approach we can't.
    device()->SetTexture(0, data->mYTexture);
    device()->SetTexture(1, data->mCbTexture);
    device()->SetTexture(2, data->mCrTexture);

    image = nullptr;
    data = nullptr;
    autoLock.Unlock();

    device()->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

    device()->SetVertexShaderConstantF(CBvTextureCoords,
      ShaderConstantRect(0, 0, 1.0f, 1.0f), 1);
  }

  GetContainer()->NotifyPaintedImage(image);
}

already_AddRefed<IDirect3DTexture9>
ImageLayerD3D9::GetAsTexture(gfxIntSize* aSize)
{
  if (!GetContainer()) {
    return nullptr;
  }

  AutoLockImage autoLock(GetContainer());

  Image *image = autoLock.GetImage();

  if (!image) {
    return nullptr;
  }

  if (image->GetFormat() != CAIRO_SURFACE &&
      image->GetFormat() != REMOTE_IMAGE_BITMAP) {
    return nullptr;
  }

  bool dontCare;
  *aSize = image->GetSize();
  nsRefPtr<IDirect3DTexture9> result = GetTexture(image, dontCare);
  return result.forget();
}

} /* layers */
} /* mozilla */
