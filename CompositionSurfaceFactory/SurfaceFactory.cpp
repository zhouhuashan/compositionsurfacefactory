﻿#include "pch.h"
#include "SurfaceFactory.h"
#include "Lock.h"
#include "UriSurface.h"
#include "TextSurface.h"
#include "SurfaceFactoryOptions.h"

using namespace Robmikh::CompositionSurfaceFactory;
using namespace Platform;
using namespace Windows::Graphics::Display;

namespace CSF = Robmikh::CompositionSurfaceFactory;

SurfaceFactory^ SurfaceFactory::CreateFromCompositor(WUC::Compositor^ compositor)
{
    auto options = SurfaceFactoryOptions();
    options.UseSoftwareRenderer = false;

    auto surfaceFactory = ref new SurfaceFactory(compositor, options);
    return surfaceFactory;
}

SurfaceFactory^ SurfaceFactory::CreateFromCompositor(WUC::Compositor^ compositor, SurfaceFactoryOptions options)
{
    auto surfaceFactory = ref new SurfaceFactory(compositor, options);
    return surfaceFactory;
}

SurfaceFactory^ SurfaceFactory::CreateFromGraphicsDevice(CompositionGraphicsDevice^ graphicsDevice)
{
    auto surfaceFactory = ref new SurfaceFactory(graphicsDevice, nullptr);
    return surfaceFactory;
}

SurfaceFactory^ SurfaceFactory::CreateFromGraphicsDevice(CompositionGraphicsDevice^ graphicsDevice, Lock^ lock)
{
    auto surfaceFactory = ref new SurfaceFactory(graphicsDevice, lock);
    return surfaceFactory;
}

SurfaceFactory::SurfaceFactory(WUC::Compositor^ compositor, SurfaceFactoryOptions options)
{
    m_compositor = compositor;
    m_drawingLock = ref new Lock();
    m_isGraphicsDeviceCreator = true;
	m_deviceLostHelper = ref new DeviceLostHelper();
	OnDeviceLostHandler = m_deviceLostHelper->DeviceLost
		+= ref new EventHandler<DeviceLostEventArgs^>(this, &SurfaceFactory::OnDeviceLost);

    CreateDevice(options);
}

SurfaceFactory::SurfaceFactory(CompositionGraphicsDevice^ graphicsDevice, Lock^ lock)
{
    m_compositor = graphicsDevice->Compositor;
    m_graphicsDevice = graphicsDevice;
    m_isGraphicsDeviceCreator = false;
    if (lock == nullptr)
    {
        m_drawingLock = ref new Lock();
    }
    else
    {
        m_drawingLock = lock;
    }

    OnRenderingDeviceReplacedHandler = m_graphicsDevice->RenderingDeviceReplaced 
        += ref new TypedEventHandler<CompositionGraphicsDevice ^, RenderingDeviceReplacedEventArgs ^>(this, &SurfaceFactory::OnRenderingDeviceReplaced);
}

SurfaceFactory::~SurfaceFactory()
{
    Uninitialize();
}

void SurfaceFactory::OnRenderingDeviceReplaced(CompositionGraphicsDevice ^sender, RenderingDeviceReplacedEventArgs ^args)
{
    OutputDebugString(L"CompositionSurfaceFactory - Rendering Device Replaced");
    concurrency::create_async([this, args]
    {
        this->RaiseDeviceReplacedEvent(args);
    });
}

void SurfaceFactory::RaiseDeviceReplacedEvent(RenderingDeviceReplacedEventArgs ^args)
{
    DeviceReplaced(this, args);
}

void SurfaceFactory::CreateDevice(SurfaceFactoryOptions options)
{
    if (m_compositor != nullptr && m_isGraphicsDeviceCreator)
    {
        if (m_canvasDevice == nullptr)
        {
			m_canvasDevice = ref new CanvasDevice(options.UseSoftwareRenderer);

			m_deviceLostHelper->WatchDevice(m_canvasDevice);
        }

        if (m_graphicsDevice == nullptr)
        {
            m_graphicsDevice = CanvasComposition::CreateCompositionGraphicsDevice(m_compositor, m_canvasDevice);
            OnRenderingDeviceReplacedHandler = m_graphicsDevice->RenderingDeviceReplaced
                += ref new TypedEventHandler<CompositionGraphicsDevice ^, RenderingDeviceReplacedEventArgs ^>(this, &SurfaceFactory::OnRenderingDeviceReplaced);
        }
    }
}

void SurfaceFactory::OnDeviceLost(Platform::Object^ sender, DeviceLostEventArgs^ args)
{
    OutputDebugString(L"CompositionSurfaceFactory - Canvas Device Lost");

	bool softwareRenderer = m_canvasDevice->ForceSoftwareRenderer;

	m_canvasDevice = ref new CanvasDevice(softwareRenderer);
	m_deviceLostHelper->WatchDevice(m_canvasDevice);

    CanvasComposition::SetCanvasDevice(m_graphicsDevice, m_canvasDevice);
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromUri(Uri^ uri)
{
    return CreateSurfaceFromUri(uri, Size::Empty);
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromUri(Uri^ uri, Size size)
{
    return CreateSurfaceFromUri(uri, size, InterpolationMode::Linear);
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromUri(Uri^ uri, Size size, InterpolationMode interpolation)
{
    auto surface = CreateSurface(size);

    // We don't await this call, as we don't want to block the caller
    auto ignored = DrawSurface(surface, uri, size, interpolation);

    return surface;
}

IAsyncOperation<CompositionDrawingSurface^>^ SurfaceFactory::CreateSurfaceFromUriAsync(Uri^ uri)
{
    return CreateSurfaceFromUriAsync(uri, Size::Empty);
}

IAsyncOperation<CompositionDrawingSurface^>^ SurfaceFactory::CreateSurfaceFromUriAsync(Uri^ uri, Size size)
{
    return CreateSurfaceFromUriAsync(uri, size, InterpolationMode::Linear);
}

IAsyncOperation<CompositionDrawingSurface^>^ SurfaceFactory::CreateSurfaceFromUriAsync(Uri^ uri, Size size, InterpolationMode interpolation)
{
    return concurrency::create_async([=]() -> CompositionDrawingSurface^
    {
        auto surface = CreateSurface(size);

        auto drawTask = DrawSurface(surface, uri, size, interpolation).then([surface]()
        {
            return surface;
        });

        return drawTask.get();
    });
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurface(Size size)
{
    Size surfaceSize = size;
    if (surfaceSize.IsEmpty)
    {
        // We start out with a size of 0,0 for the surface, because we don't know
        // the size of the image at this time. We resize the surface later.
        surfaceSize = { 0, 0 };
    }

    CompositionGraphicsDevice^ graphicsDevice = m_graphicsDevice;
    CompositionDrawingSurface^ surface = nullptr;

	{
		auto lockSession = m_drawingLock->GetLockSession();
		surface = graphicsDevice->CreateDrawingSurface(
			surfaceSize,
			Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
			Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
	}

    return surface;
}

concurrency::task<void> SurfaceFactory::DrawSurface(CompositionDrawingSurface^ surface, Uri^ uri, Size size, InterpolationMode interpolation) __resumable
{
    auto canvasDevice = CanvasComposition::GetCanvasDevice(m_graphicsDevice);
	if (uri != nullptr)
	{
		auto canvasBitmap = co_await CanvasBitmap::LoadAsync(canvasDevice, uri);
		DrawBitmap(surface, canvasBitmap, size, interpolation);
	}
	else
	{
		{
			auto lockSession = m_drawingLock->GetLockSession();

			Size size;
			size.Width = 1;
			size.Height = 1;
			CanvasComposition::Resize(surface, size);

			auto session = CanvasComposition::CreateDrawingSession(surface);
			session->Clear(Windows::UI::Colors::Transparent);
		}
	}
}

void SurfaceFactory::DrawBitmap(CompositionDrawingSurface^ surface, CanvasBitmap^ canvasBitmap, Size size, InterpolationMode interpolation)
{
    auto bitmapSize = canvasBitmap->Size;

    // Because the drawing is done asynchronously and multiple threads could
    // be trying to get access to the device/surface at the same time, we need
    // to do any device/surface work under a lock.
	{
		auto lockSession = m_drawingLock->GetLockSession();
		Size surfaceSize = size;
		if (surfaceSize.IsEmpty)
		{
			// Resize the surface to the size of the image
			CanvasComposition::Resize(surface, bitmapSize);
			surfaceSize = bitmapSize;
		}

		{
			auto session = CanvasComposition::CreateDrawingSession(surface);
			Rect surfaceRect = { 0, 0, surfaceSize.Width, surfaceSize.Height };
			Rect bitmapRect = { 0, 0, bitmapSize.Width, bitmapSize.Height };
			CanvasImageInterpolation canvasInterpolation = InterpolationModeHelper::GetCanvasImageInterpolation(interpolation);
			session->Clear(Windows::UI::Colors::Transparent);
			session->DrawImage(canvasBitmap, surfaceRect, bitmapRect, 1.0f, canvasInterpolation);
		}
	}
}

void SurfaceFactory::ResizeSurface(CompositionDrawingSurface^ surface, Size size)
{
	{
		auto lockSession = m_drawingLock->GetLockSession();
		CanvasComposition::Resize(surface, size);
	}
}

UriSurface^ SurfaceFactory::CreateUriSurface()
{
	return CreateUriSurface(nullptr);
}

UriSurface^ SurfaceFactory::CreateUriSurface(Uri^ uri)
{
    return CreateUriSurface(uri, Size::Empty);
}

UriSurface^ SurfaceFactory::CreateUriSurface(Uri^ uri, Size size)
{
    return CreateUriSurface(uri, size, InterpolationMode::Linear);
}

UriSurface^ SurfaceFactory::CreateUriSurface(Uri^ uri, Size size, InterpolationMode interpolation)
{
    auto uriSurface = UriSurface::Create(this, uri, size, interpolation);
    auto ignored = uriSurface->RedrawSurfaceAsync();

    return uriSurface;
}

IAsyncOperation<UriSurface^>^ SurfaceFactory::CreateUriSurfaceAsync(Uri^ uri)
{
    return CreateUriSurfaceAsync(uri, Size::Empty);
}

IAsyncOperation<UriSurface^>^ SurfaceFactory::CreateUriSurfaceAsync(Uri^ uri, Size size)
{
    return CreateUriSurfaceAsync(uri, size, InterpolationMode::Linear);
}

IAsyncOperation<UriSurface^>^ SurfaceFactory::CreateUriSurfaceAsync(Uri^ uri, Size size, InterpolationMode interpolation)
{
    return concurrency::create_async([=]() -> UriSurface^
    {
        auto uriSurface = UriSurface::Create(this, uri, size, interpolation);

        auto drawTask = concurrency::create_task(uriSurface->RedrawSurfaceAsync()).then([uriSurface]() -> UriSurface^
        {
            return uriSurface;
        });

        return drawTask.get();
    });
}

TextSurface^ SurfaceFactory::CreateTextSurface()
{
	return CreateTextSurface("");
}

TextSurface^ SurfaceFactory::CreateTextSurface(Platform::String^ text)
{
    auto textSurface = TextSurface::Create(this, text);
    textSurface->RedrawSurface();

    return textSurface;
}

TextSurface^ SurfaceFactory::CreateTextSurface(Platform::String^ text,
                                               float width,
                                               float height,
                                               Platform::String^ fontFamily,
                                               float fontSize,
                                               WUT::FontStyle fontStyle,
                                               TextHorizontalAlignment horizontalAlignment,
                                               TextVerticalAlignment verticalAlignment,
                                               CSF::WordWrapping wordWrapping,
                                               CSF::Padding padding,
                                               Windows::UI::Color foregroundColor,
                                               Windows::UI::Color backgroundColor)
{
    auto textSurface = TextSurface::Create(this, 
                                           text,
                                           width,
                                           height,
                                           fontFamily,
                                           fontSize,
                                           fontStyle,
                                           horizontalAlignment,
                                           verticalAlignment,
                                           wordWrapping,
                                           padding,
                                           foregroundColor,
                                           backgroundColor);
    textSurface->RedrawSurface();

    return textSurface;
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromBytes(const Array<byte>^ bytes, int widthInPixels, int heightInPixels)
{
    return CreateSurfaceFromBytes(bytes, widthInPixels, heightInPixels, Size::Empty);
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromBytes(const Array<byte>^ bytes, int widthInPixels, int heightInPixels, Size size)
{
    return CreateSurfaceFromBytes(bytes, widthInPixels, heightInPixels, size, InterpolationMode::Linear);
}

CompositionDrawingSurface^ SurfaceFactory::CreateSurfaceFromBytes(const Array<byte>^ bytes, int widthInPixels, int heightInPixels, Size size, InterpolationMode interpolation)
{
    auto surface = CreateSurface(size);

    auto canvasDevice = CanvasComposition::GetCanvasDevice(m_graphicsDevice);
    auto canvasBitmap = CanvasBitmap::CreateFromBytes(canvasDevice, bytes, widthInPixels, heightInPixels, Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized);
    DrawBitmap(surface, canvasBitmap, size, interpolation);

    return surface;
}

void SurfaceFactory::Uninitialize()
{
	{
		auto lockSession = m_drawingLock->GetLockSession();
		m_compositor = nullptr;
		if (m_canvasDevice != nullptr)
		{
			m_deviceLostHelper->StopWatchingCurrentDevice();
			m_deviceLostHelper = nullptr;

			m_canvasDevice->~CanvasDevice();
			m_canvasDevice = nullptr;
		}

		if (m_graphicsDevice != nullptr)
		{
			m_graphicsDevice->RenderingDeviceReplaced -= OnRenderingDeviceReplacedHandler;
			// Only dispose the composition graphics device if we own the device.
			if (m_isGraphicsDeviceCreator)
			{
				m_graphicsDevice->~CompositionGraphicsDevice();
			}
			m_graphicsDevice = nullptr;
		}
	}
	m_drawingLock = nullptr;
}