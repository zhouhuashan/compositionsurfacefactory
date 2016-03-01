﻿using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.UI.Composition;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Windows.Foundation;
using Windows.Foundation.Metadata;
using Windows.Graphics.DirectX;
using Windows.UI.Composition;

namespace Robmikh.Util.CompositionImageLoader
{
    public interface IImageLoader : IDisposable
    {
        event EventHandler<Object> DeviceReplacedEvent;
        CompositionDrawingSurface LoadImageFromUri(Uri uri);
        IAsyncOperation<CompositionDrawingSurface> LoadImageFromUriAsync(Uri uri);
        IManagedSurface CreateManagedSurfaceFromUri(Uri uri);
        IAsyncOperation<IManagedSurface> CreateManagedSurfaceFromUriAsync(Uri uri);
    }

    interface IImageLoaderInternal : IImageLoader
    {
        CompositionDrawingSurface CreateSurface();
        Task DrawSurface(CompositionDrawingSurface surface, Uri uri);
    }

    public static class ImageLoaderFactory
    {
        [DefaultOverloadAttribute]
        public static IImageLoader CreateImageLoader(Compositor compositor)
        {
            var imageLoader = new ImageLoader();
            imageLoader.Initialize(compositor);
            return imageLoader;
        }

        public static IImageLoader CreateImageLoader(CompositionGraphicsDevice graphicsDevice)
        {
            var imageLoader = new ImageLoader();
            imageLoader.Initialize(graphicsDevice);
            return imageLoader;
        }
    }

    class ImageLoader : IImageLoaderInternal
    {
        public event EventHandler<Object> DeviceReplacedEvent;

        private Compositor _compositor;
        private CanvasDevice _canvasDevice;
        private CompositionGraphicsDevice _graphicsDevice;
        private Object _deviceLock;

        private bool _isDeviceCreator;

        public ImageLoader() { }

        public void Initialize(Compositor compositor)
        {
            _compositor = compositor;
            _deviceLock = new object();
            _isDeviceCreator = true;
            CreateDevice();
        }

        public void Initialize(CompositionGraphicsDevice graphicsDevice)
        {
            _graphicsDevice = graphicsDevice;
            _deviceLock = new object();
            _isDeviceCreator = false;
            //
            // We don't call CreateDevice, as it wouldn't do anything
            // since we don't have a Compositor.
            //
        }

        private void CreateDevice()
        {
            if (_compositor != null)
            {
                if (_canvasDevice == null)
                {
                    _canvasDevice = CanvasDevice.GetSharedDevice();
                    _canvasDevice.DeviceLost += DeviceLost;
                }

                if (_graphicsDevice == null)
                {
                    _graphicsDevice = CanvasComposition.CreateCompositionGraphicsDevice(_compositor, _canvasDevice);
                    _graphicsDevice.RenderingDeviceReplaced += RenderingDeviceReplaced;
                }
            }
        }

        private void DeviceLost(CanvasDevice sender, object args)
        {
            if (_canvasDevice != null)
            {
                _canvasDevice.DeviceLost -= DeviceLost;
                _canvasDevice.Dispose();
                _canvasDevice = null;
            }

            if (_graphicsDevice != null)
            {
                _graphicsDevice.RenderingDeviceReplaced -= RenderingDeviceReplaced;
                _graphicsDevice.Dispose();
                _graphicsDevice = null;
            }

            CreateDevice();
        }

        private void RenderingDeviceReplaced(CompositionGraphicsDevice sender, RenderingDeviceReplacedEventArgs args)
        {
            Task.Run(() =>
            {
                if (DeviceReplacedEvent != null)
                {
                    RaiseDeviceReplacedEvent();
                }
            });
        }

        private void RaiseDeviceReplacedEvent()
        {
            var deviceEvent = DeviceReplacedEvent;
            if (deviceEvent != null)
            {
                deviceEvent(this, new EventArgs());
            }

        }

        public CompositionDrawingSurface LoadImageFromUri(Uri uri)
        {
            var surface = CreateSurface();

            //
            // We don't await this call, as we want it to happen
            // seperately on a thread pool thread.
            //
            DrawSurface(surface, uri);

            return surface;
        }

        private async Task<CompositionDrawingSurface> LoadImageFromUriAsyncWorker(Uri uri)
        {
            var surface = CreateSurface();

            await DrawSurface(surface, uri);

            return surface;
        }

        public IAsyncOperation<CompositionDrawingSurface> LoadImageFromUriAsync(Uri uri)
        {
            return LoadImageFromUriAsyncWorker(uri).AsAsyncOperation<CompositionDrawingSurface>();
        }

        public IManagedSurface CreateManagedSurfaceFromUri(Uri uri)
        {
            var managedSurface = new ManagedSurface(this, uri);
            managedSurface.RedrawSurface();

            return managedSurface;
        }

        public async Task<IManagedSurface> CreateManagedSurfaceFromUriAsyncWorker(Uri uri)
        {
            var managedSurface = new ManagedSurface(this, uri);
            await managedSurface.RedrawSurface();

            return managedSurface;
        }

        public IAsyncOperation<IManagedSurface> CreateManagedSurfaceFromUriAsync(Uri uri)
        {
            return CreateManagedSurfaceFromUriAsyncWorker(uri).AsAsyncOperation<IManagedSurface>();
        }

        public async Task DrawSurface(CompositionDrawingSurface surface, Uri uri)
        {
            using (var canvasBitmap = await CanvasBitmap.LoadAsync(_canvasDevice, uri))
            {
                var size = canvasBitmap.Size;

                //
                // Because the drawing is done asynchronously and multiple threads could
                // be trying to get access to the device/surface at the same time, we need
                // to do any device/surface work under a lock.
                //
                lock (_deviceLock)
                {
                    // Resize the surface to the size of the image
                    CanvasComposition.Resize(surface, size);
                    // Draw the image to the surface
                    using (var session = CanvasComposition.CreateDrawingSession(surface))
                    {
                        session.Clear(Windows.UI.Color.FromArgb(0, 0, 0, 0));
                        session.DrawImage(canvasBitmap, new Rect(0, 0, size.Width, size.Height), new Rect(0, 0, size.Width, size.Height));
                    }
                }
            }
        }

        public CompositionDrawingSurface CreateSurface()
        {
            //
            // We start out with a size of 0,0 for the surface, because we don't know
            // the size of the image at this time. We resize the surface later.
            //
            var surface = _graphicsDevice.CreateDrawingSurface(new Size(0, 0), DirectXPixelFormat.B8G8R8A8UIntNormalized, DirectXAlphaMode.Premultiplied);

            return surface;
        }

        public void Dispose()
        {
            lock(_deviceLock)
            {
                _compositor = null;

                if (_canvasDevice != null)
                {
                    _canvasDevice.DeviceLost -= DeviceLost;
                    //
                    // Only dispose the canvas device if we own the device.
                    //
                    if (_isDeviceCreator)
                    {
                        _canvasDevice.Dispose();
                    }
                    _canvasDevice = null;
                }

                if (_graphicsDevice != null)
                {
                    _graphicsDevice.RenderingDeviceReplaced -= RenderingDeviceReplaced;
                    //
                    // Only dispose the composition graphics device if we own the device.
                    //
                    if (_isDeviceCreator)
                    {
                        _graphicsDevice.Dispose();
                    }
                    _graphicsDevice = null;
                }
            }
        }
    }
}