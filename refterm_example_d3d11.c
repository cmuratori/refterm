// TODO(casey): These are lightly adapted from Martins' original sketch, which he
// did in like an hour.  I don't have much D3D11 knowledge so someone who does
// have some should go through this and give advice about if there are any things
// that should be done differently in production code.

#pragma comment (lib, "d3d11.lib")
#pragma comment (lib, "dxguid.lib")

static int D3D11RendererIsValid(d3d11_renderer *Renderer)
{
    int Result = (Renderer->Device &&
                  Renderer->SwapChain &&
                  Renderer->ComputeShader &&
                  Renderer->ConstantBuffer &&
                  Renderer->CellView &&
                  Renderer->GlyphTextureView);

    return Result;
}

static void ActivateD3D11DebugInfo(ID3D11Device *Device)
{
    ID3D11InfoQueue *Info;
    if(SUCCEEDED(IProvideClassInfo_QueryInterface(Device, &IID_ID3D11InfoQueue, (void**)&Info)))
    {
        ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        ID3D11InfoQueue_SetBreakOnSeverity(Info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);

        ID3D11InfoQueue_Release(Info);
    }
}

static IDXGIFactory2 *AcquireDXGIFactory(ID3D11Device *Device)
{
    IDXGIFactory2 *Result = 0;

    if(Device)
    {
        IDXGIDevice *DxgiDevice = 0;
        if(SUCCEEDED(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (void **)&DxgiDevice)))
        {
            IDXGIAdapter *DxgiAdapter = 0;
            if(SUCCEEDED(IDXGIDevice_GetAdapter(DxgiDevice, &DxgiAdapter)))
            {
                IDXGIAdapter_GetParent(DxgiAdapter, &IID_IDXGIFactory2, (void**)&Result);

                IDXGIAdapter_Release(DxgiAdapter);
            }

            IDXGIDevice_Release(DxgiDevice);
        }
    }

    return Result;
}

static IDXGISwapChain2 *AcquireDXGISwapChain(ID3D11Device *Device, HWND Window, int UseComputeShader)
{
    IDXGISwapChain2 *Result = 0;

    if(Device)
    {
        IDXGIFactory2 *DxgiFactory = AcquireDXGIFactory(Device);
        if(DxgiFactory)
        {
            DXGI_SWAP_CHAIN_DESC1 SwapChainDesc =
            {
                .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                .SampleDesc = {1, 0},
                .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                .BufferCount = 2,
                .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
                .Scaling = DXGI_SCALING_NONE,
                .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
                .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
            };

            if(UseComputeShader)
            {
                SwapChainDesc.BufferUsage |= DXGI_USAGE_UNORDERED_ACCESS;
            }

            IDXGISwapChain1 *SwapChain1 = 0;
            if(SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(DxgiFactory, (IUnknown*)Device, Window, &SwapChainDesc, NULL, NULL, &SwapChain1)))
            {
                if(SUCCEEDED(IDXGISwapChain1_QueryInterface(SwapChain1, &IID_IDXGISwapChain2, (void **)&Result)))
                {
                    IDXGIFactory2_MakeWindowAssociation(DxgiFactory, Window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                }

                IDXGISwapChain1_Release(SwapChain1);
            }

            IDXGIFactory2_Release(DxgiFactory);
        }
    }

    return Result;
}

static void ReleaseD3DCellBuffer(d3d11_renderer *Renderer)
{
    if(Renderer->CellBuffer)
    {
        ID3D11Buffer_Release(Renderer->CellBuffer);
        Renderer->CellBuffer = 0;
    }

    if(Renderer->CellView)
    {
        ID3D11ShaderResourceView_Release(Renderer->CellView);
        Renderer->CellView = 0;
    }
}

static void ClearD3D11GlyphTexture(d3d11_renderer *Renderer)
{
    if(Renderer->GlyphTextureView)
    {
        FLOAT Zero[4] = {0};
        ID3D11DeviceContext1_ClearView(Renderer->DeviceContext1, (ID3D11View *)Renderer->GlyphTextureView, Zero, 0, 0);
    }
}

static void SetD3D11MaxCellCount(d3d11_renderer *Renderer, uint32_t Count)
{
    ReleaseD3DCellBuffer(Renderer);

    if(Renderer->Device)
    {
        D3D11_BUFFER_DESC CellBufferDesc =
        {
            .ByteWidth = Count * sizeof(renderer_cell),
            .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
            .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
            .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
            .StructureByteStride = sizeof(renderer_cell),
        };

        if(SUCCEEDED(ID3D11Device_CreateBuffer(Renderer->Device, &CellBufferDesc, 0, &Renderer->CellBuffer)))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC CellViewDesc =
            {
                .ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
                .Buffer.FirstElement = 0,
                .Buffer.NumElements = Count,
            };

            ID3D11Device_CreateShaderResourceView(Renderer->Device, (ID3D11Resource *)Renderer->CellBuffer, &CellViewDesc, &Renderer->CellView);
        }
        
        Renderer->MaxCellCount = Count;
    }
}

static void ReleaseD3DGlyphCache(d3d11_renderer *Renderer)
{
    if(Renderer->GlyphTexture)
    {
        ID3D11ShaderResourceView_Release(Renderer->GlyphTexture);
        Renderer->GlyphTexture = 0;
    }

    if(Renderer->GlyphTextureView)
    {
        ID3D11ShaderResourceView_Release(Renderer->GlyphTextureView);
        Renderer->GlyphTextureView = 0;
    }
}

static void ReleaseD3DGlyphTransfer(d3d11_renderer *Renderer)
{
    D2DRelease(&Renderer->DWriteRenderTarget, &Renderer->DWriteFillBrush);

    if(Renderer->GlyphTransfer)
    {
        ID3D11ShaderResourceView_Release(Renderer->GlyphTransfer);
        Renderer->GlyphTransfer = 0;
    }

    if(Renderer->GlyphTransferView)
    {
        ID3D11ShaderResourceView_Release(Renderer->GlyphTransferView);
        Renderer->GlyphTransferView = 0;
    }

    if(Renderer->GlyphTransferSurface)
    {
        IDXGISurface_Release(Renderer->GlyphTransferSurface);
        Renderer->GlyphTransferSurface = 0;
    }
}

static void SetD3D11GlyphCacheDim(d3d11_renderer *Renderer, uint32_t Width, uint32_t Height)
{
    ReleaseD3DGlyphCache(Renderer);

    if(Renderer->Device)
    {
        D3D11_TEXTURE2D_DESC TextureDesc =
        {
            .Width = Width,
            .Height = Height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE,
        };

        if(SUCCEEDED(ID3D11Device_CreateTexture2D(Renderer->Device, &TextureDesc, NULL, &Renderer->GlyphTexture)))
        {
            ID3D11Device_CreateShaderResourceView(Renderer->Device, (ID3D11Resource*)Renderer->GlyphTexture, NULL, &Renderer->GlyphTextureView);
        }
    }
}

static void SetD3D11GlyphTransferDim(d3d11_renderer *Renderer, uint32_t Width, uint32_t Height)
{
    ReleaseD3DGlyphTransfer(Renderer);

    if(Renderer->Device)
    {
        D3D11_TEXTURE2D_DESC TextureDesc =
        {
            .Width = Width,
            .Height = Height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = { 1, 0 },
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,
        };

        if(SUCCEEDED(ID3D11Device_CreateTexture2D(Renderer->Device, &TextureDesc, 0, &Renderer->GlyphTransfer)))
        {
            ID3D11Device_CreateShaderResourceView(Renderer->Device, (ID3D11Resource *)Renderer->GlyphTransfer, 0, &Renderer->GlyphTransferView);
            ID3D11Texture2D_QueryInterface(Renderer->GlyphTransfer, &IID_IDXGISurface, (void **)&Renderer->GlyphTransferSurface);

            D2DAcquire(Renderer->GlyphTransferSurface,
                       &Renderer->DWriteRenderTarget,
                       &Renderer->DWriteFillBrush);
        }
    }
}

static void ReleaseD3D11RenderTargets(d3d11_renderer *Renderer)
{
    if (Renderer->RenderView)
    {
        ID3D11UnorderedAccessView_Release(Renderer->RenderView);
        Renderer->RenderView = 0;
    }

    if (Renderer->RenderTarget)
    {
        ID3D11RenderTargetView_Release(Renderer->RenderTarget);
        Renderer->RenderTarget = 0;
    }
}

static void ReleaseD3D11Renderer(d3d11_renderer *Renderer)
{
    // TODO(casey): When you want to release a D3D11 device, do you have to release all the sub-components?
    // Can you just release the main device and have all the sub-components release themselves?

    ReleaseD3DCellBuffer(Renderer);
    ReleaseD3DGlyphCache(Renderer);
    ReleaseD3DGlyphTransfer(Renderer);
    ReleaseD3D11RenderTargets(Renderer);

    if(Renderer->ComputeShader) ID3D11ComputeShader_Release(Renderer->ComputeShader);
    if(Renderer->PixelShader) ID3D11ComputeShader_Release(Renderer->PixelShader);
    if(Renderer->VertexShader) ID3D11ComputeShader_Release(Renderer->VertexShader);

    if(Renderer->ConstantBuffer) ID3D11Buffer_Release(Renderer->ConstantBuffer);

    if(Renderer->RenderView) ID3D11UnorderedAccessView_Release(Renderer->RenderView);
    if(Renderer->SwapChain) IDXGISwapChain2_Release(Renderer->SwapChain);

    if(Renderer->DeviceContext) ID3D11DeviceContext_Release(Renderer->DeviceContext);
    if(Renderer->DeviceContext1) ID3D11DeviceContext1_Release(Renderer->DeviceContext1);
    if(Renderer->Device) ID3D11Device_Release(Renderer->Device);

    d3d11_renderer ZeroRenderer = {0};
    *Renderer = ZeroRenderer;
}

static d3d11_renderer AcquireD3D11Renderer(HWND Window, int EnableDebugging)
{
    d3d11_renderer Result = {0};

    UINT Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
    if(EnableDebugging)
    {
        Flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, Flags, Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION,
                                   &Result.Device, 0, &Result.DeviceContext);
    if(FAILED(hr))
    {
        hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_WARP, 0, Flags, Levels, ARRAYSIZE(Levels), D3D11_SDK_VERSION,
                               &Result.Device, 0, &Result.DeviceContext);
    }

    if(SUCCEEDED(hr))
    {
        if(SUCCEEDED(ID3D11DeviceContext1_QueryInterface(Result.DeviceContext, &IID_ID3D11DeviceContext1, (void **)&Result.DeviceContext1)))
        {
            if(EnableDebugging)
            {
                ActivateD3D11DebugInfo(Result.Device);
            }

            Result.SwapChain = AcquireDXGISwapChain(Result.Device, Window, 0);
            if(Result.SwapChain)
            {
                Result.FrameLatencyWaitableObject = IDXGISwapChain2_GetFrameLatencyWaitableObject(Result.SwapChain);

                D3D11_BUFFER_DESC ConstantBufferDesc =
                {
                    .ByteWidth = sizeof(renderer_const_buffer),
                    .Usage = D3D11_USAGE_DYNAMIC,
                    .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
                    .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
                };
                ID3D11Device_CreateBuffer(Result.Device, &ConstantBufferDesc, 0, &Result.ConstantBuffer);

                ID3D11Device_CreateComputeShader(Result.Device, ReftermCSShaderBytes, sizeof(ReftermCSShaderBytes), 0, &Result.ComputeShader);
                ID3D11Device_CreatePixelShader(Result.Device, ReftermPSShaderBytes, sizeof(ReftermPSShaderBytes), 0, &Result.PixelShader);
                ID3D11Device_CreateVertexShader(Result.Device, ReftermVSShaderBytes, sizeof(ReftermVSShaderBytes), 0, &Result.VertexShader);
            }
        }
    }

    if(!Result.SwapChain)
    {
        ReleaseD3D11Renderer(&Result);
    }

    return Result;
}

static void RendererDraw(example_terminal *Terminal, uint32_t Width, uint32_t Height, terminal_buffer *Term, uint32_t BlinkModulate)
{
    // TODO(casey): This should be split into two routines now, since we don't actually
    // need to resubmit anything if the terminal hasn't updated.

    glyph_table *Table = Terminal->GlyphTable;
    d3d11_renderer *Renderer = &Terminal->Renderer;
    glyph_generator *GlyphGen = &Terminal->GlyphGen;
    source_buffer *Source = &Terminal->ScrollBackBuffer;

    HRESULT hr;

    // resize RenderView to match window size
    if(Width != Renderer->CurrentWidth || Height != Renderer->CurrentHeight)
    {
        ID3D11DeviceContext_ClearState(Renderer->DeviceContext);
        ReleaseD3D11RenderTargets(Renderer);
        ID3D11DeviceContext_Flush(Renderer->DeviceContext);

        if (Width != 0 && Height != 0)
        {
            hr = IDXGISwapChain_ResizeBuffers(Renderer->SwapChain, 0, Width, Height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
            AssertHR(hr);

            ID3D11Texture2D *Buffer;
            hr = IDXGISwapChain_GetBuffer(Renderer->SwapChain, 0, &IID_ID3D11Texture2D, (void**)&Buffer);
            AssertHR(hr);

            if(Renderer->UseComputeShader)
            {
                hr = ID3D11Device_CreateUnorderedAccessView(Renderer->Device, (ID3D11Resource*)Buffer, 0, &Renderer->RenderView);
                AssertHR(hr);
            }
            else
            {
                hr = ID3D11Device_CreateRenderTargetView(Renderer->Device, (ID3D11Resource*)Buffer, 0, &Renderer->RenderTarget);
                AssertHR(hr);

                D3D11_VIEWPORT Viewport =
                {
                    .TopLeftX = 0.0f,
                    .TopLeftY = 0.0f,
                    .Width = (float)Width,
                    .Height = (float)Height,
                };
                ID3D11DeviceContext_RSSetViewports(Renderer->DeviceContext, 1, &Viewport);
                ID3D11DeviceContext_IASetPrimitiveTopology(Renderer->DeviceContext, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            }

            ID3D11Texture2D_Release(Buffer);
        }

        Renderer->CurrentWidth = Width;
        Renderer->CurrentHeight = Height;
    }

    uint32_t CellCount = Renderer->CurrentWidth*Renderer->CurrentHeight;
    if(Renderer->MaxCellCount < CellCount)
    {
        SetD3D11MaxCellCount(Renderer, CellCount);
    }
        
    if(Renderer->RenderView || Renderer->RenderTarget)
    {
        D3D11_MAPPED_SUBRESOURCE Mapped;
        hr = ID3D11DeviceContext_Map(Renderer->DeviceContext, (ID3D11Resource*)Renderer->ConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        AssertHR(hr);
        {
            renderer_const_buffer ConstData =
            {
                .CellSize = { GlyphGen->CellWidth, GlyphGen->CellHeight },
                .GlyphSize = { GlyphGen->GlyphWidth, GlyphGen->GlyphHeight },
                .TermSize = { Term->DimX, Term->DimY },
                .TopLeftMargin = {8, 8},
                .BlinkModulate = BlinkModulate,
                .MarginColor = 0x000c0c0c,
            };
            memcpy(Mapped.pData, &ConstData, sizeof(ConstData));
        }
        ID3D11DeviceContext_Unmap(Renderer->DeviceContext, (ID3D11Resource*)Renderer->ConstantBuffer, 0);

        hr = ID3D11DeviceContext_Map(Renderer->DeviceContext, (ID3D11Resource*)Renderer->CellBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped);
        AssertHR(hr);
        {
            renderer_cell *Cells = Mapped.pData;

            uint32_t TopCellCount = Term->DimX * (Term->DimY - Term->FirstLineY);
            uint32_t BotCellCount = Term->DimX * (Term->FirstLineY);
            Assert((TopCellCount + BotCellCount) == (Term->DimX * Term->DimY));
            memcpy(Cells, Term->Cells + Term->FirstLineY*Term->DimX, TopCellCount*sizeof(renderer_cell));
            memcpy(Cells + TopCellCount, Term->Cells, BotCellCount*sizeof(renderer_cell));
        }
        ID3D11DeviceContext_Unmap(Renderer->DeviceContext, (ID3D11Resource*)Renderer->CellBuffer, 0);

        // this should match t0/t1 order in hlsl shader
        ID3D11ShaderResourceView* Resources[] = { Renderer->CellView, Renderer->GlyphTextureView };

        if(Renderer->UseComputeShader)
        {
            // this issues compute shader for window size, which in real terminal should match its size
            ID3D11DeviceContext_CSSetConstantBuffers(Renderer->DeviceContext, 0, 1, &Renderer->ConstantBuffer);
            ID3D11DeviceContext_CSSetShaderResources(Renderer->DeviceContext, 0, ARRAYSIZE(Resources), Resources);
            ID3D11DeviceContext_CSSetUnorderedAccessViews(Renderer->DeviceContext, 0, 1, &Renderer->RenderView, NULL);
            ID3D11DeviceContext_CSSetShader(Renderer->DeviceContext, Renderer->ComputeShader, 0, 0);
            ID3D11DeviceContext_Dispatch(Renderer->DeviceContext, (Renderer->CurrentWidth + 7) / 8, (Renderer->CurrentHeight + 7) / 8, 1);
        }
        else
        {
            // NOTE(casey): This MUST be set every frame, because PAGE FLIPPING, I guess :/
            ID3D11DeviceContext_OMSetRenderTargets(Renderer->DeviceContext, 1, &Renderer->RenderTarget, 0);

            ID3D11DeviceContext_PSSetConstantBuffers(Renderer->DeviceContext, 0, 1, &Renderer->ConstantBuffer);
            ID3D11DeviceContext_PSSetShaderResources(Renderer->DeviceContext, 0, ARRAYSIZE(Resources), Resources);
            ID3D11DeviceContext_VSSetShader(Renderer->DeviceContext, Renderer->VertexShader, 0, 0);
            ID3D11DeviceContext_PSSetShader(Renderer->DeviceContext, Renderer->PixelShader, 0, 0);
            ID3D11DeviceContext_Draw(Renderer->DeviceContext, 4, 0);
        }
    }

    BOOL Vsync = FALSE;
    hr = IDXGISwapChain1_Present(Renderer->SwapChain, Vsync ? 1 : 0, 0);
    if((hr == DXGI_ERROR_DEVICE_RESET) || (hr == DXGI_ERROR_DEVICE_REMOVED))
    {
        Assert(!"Device lost!");
        ReleaseD3D11Renderer(Renderer);
    }
    else
    {
        AssertHR(hr);
    }

    if(Renderer->RenderView)
    {
        ID3D11DeviceContext1_DiscardView(Renderer->DeviceContext1, (ID3D11View*)Renderer->RenderView);
    }
}
