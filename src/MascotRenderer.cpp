#define NOMINMAX
#include "MascotRenderer.hpp"
#include "Text.hpp"
#include "Log.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <wincodec.h>

#include "ModelVS.h"
#include "ModelPS.h"
#include "EdgeVS.h"
#include "EdgePS.h"
#include "ParticleVS.h"
#include "ParticlePS.h"
#include "ShadowVS.h"
#include "ShadowPS.h"
#include "PostVS.h"
#include "PostPS.h"
#include "UpscalePS.h"

namespace MikuMikuDesktopPerch
{
    using Microsoft::WRL::ComPtr;
    using namespace DirectX;

    namespace
    {
        constexpr DXGI_FORMAT ColorFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
        constexpr DXGI_FORMAT DepthFormat=DXGI_FORMAT_D32_FLOAT;
        constexpr float ClearColor[4]={0,0,0,0};

        struct SceneConstants
        {
            XMFLOAT4X4 viewProj;
            XMFLOAT4X4 view;
            XMFLOAT3 lightDir; float edgePixels;
            XMFLOAT3 lightColor; float pad0;
            XMFLOAT2 viewportSize; XMFLOAT2 pad1;
            XMFLOAT4X4 particleViewProj;
            XMFLOAT4 cameraRight,cameraUp;
            XMFLOAT4X4 lightViewProj;
            XMFLOAT4 shadowInfo;
            XMFLOAT4X4 prevViewProj;
            XMFLOAT4 motionInfo;
            XMFLOAT4 blurInfo;
            std::uint32_t blurFlags[4];
        };
        // Motion blur: the share of a frame's motion smeared (a film camera's half-open
        // shutter), and the longest smear as a share of the drawn height at 30 fps (longer at
        // lower rates, where a frame's motion is longer, up to BlurLongestMax).
        constexpr float BlurShutter=0.5f,BlurLongest=0.06f,BlurLongestMax=0.15f;
        struct GpuParticle
        {
            XMFLOAT3 position; float size;
            XMFLOAT4 color;
            float angle; float pad[3];
        };
        struct MaterialConstants
        {
            std::array<float,4> diffuse;
            std::array<float,3> specular; float shininess;
            std::array<float,3> ambient; float edgeSize;
            std::array<float,4> edgeColor;
            std::array<float,4> textureMul,textureAdd,sphereMul,sphereAdd,toonMul,toonAdd;
            std::uint32_t useTexture,sphereMode,useToon,receiveShadow;
        };
        static_assert(sizeof(MaterialConstants)<=256);

        constexpr size_t Align256(size_t v){return (v+255)&~size_t(255);}

        D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
        {
            D3D12_HEAP_PROPERTIES p{};
            p.Type=type;
            p.CreationNodeMask=p.VisibleNodeMask=1;
            return p;
        }
        D3D12_RESOURCE_DESC BufferDesc(UINT64 size)
        {
            D3D12_RESOURCE_DESC d{};
            d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width=std::max<UINT64>(size,1);
            d.Height=d.DepthOrArraySize=d.MipLevels=1;
            d.SampleDesc.Count=1;
            d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return d;
        }
        D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource=resource;
            b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore=before;
            b.Transition.StateAfter=after;
            return b;
        }
        std::wstring HrText(const wchar_t* what,HRESULT hr)
        {
            wchar_t text[128]{};
            swprintf_s(text,L"%s (0x%08X)",what,static_cast<unsigned>(hr));
            return text;
        }
        std::wstring GpuId(const DXGI_ADAPTER_DESC1& desc)
        {
            wchar_t id[16]{};
            swprintf_s(id,L"%04X:%04X",desc.VendorId,desc.DeviceId);
            return id;
        }
    }

    std::vector<std::pair<std::wstring,std::wstring>> MascotRenderer::Gpus()
    {
        std::vector<std::pair<std::wstring,std::wstring>> list;
        ComPtr<IDXGIFactory6> factory;
        if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return list;
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_UNSPECIFIED,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))list.push_back({GpuId(desc),desc.Description});
            adapter.Reset();
        }
        return list;
    }

    MascotRenderer::~MascotRenderer()
    {
        if(device)WaitIdle();
        for(auto& f:frames)if(f.upload&&f.mapped)f.upload->Unmap(0,nullptr);
        if(fenceEvent)CloseHandle(fenceEvent);
    }

    std::shared_ptr<MascotRenderer::Gpu> MascotRenderer::SharedGpu(std::wstring& error)
    {
        static std::weak_ptr<Gpu> shared;
        if(auto existing=shared.lock())return existing;
        auto gpu=std::make_shared<Gpu>();
        HRESULT hr;
#if defined(_DEBUG)
        {
            ComPtr<ID3D12Debug> debug;
            if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        }
        const UINT factoryFlags=DXGI_CREATE_FACTORY_DEBUG;
#else
        const UINT factoryFlags=0;
#endif
        if(FAILED(hr=CreateDXGIFactory2(factoryFlags,IID_PPV_ARGS(&gpu->factory)))){error=HrText(L"DXGI factory",hr);return nullptr;}

        // In the order Windows prefers for this app (its graphics settings), the one chosen in
        // the settings first; one that cannot make a device is passed over.
        std::vector<ComPtr<IDXGIAdapter1>> adapters;
        {
            ComPtr<IDXGIAdapter1> adapter;
            for(UINT i=0;gpu->factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_UNSPECIFIED,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
                adapters.push_back(std::move(adapter));
        }
        if(!preferredGpu.empty())
        {
            const auto chosen=std::find_if(adapters.begin(),adapters.end(),[](const auto& a){
                DXGI_ADAPTER_DESC1 desc{};
                a->GetDesc1(&desc);
                return GpuId(desc)==preferredGpu;});
            Log(L"GPU chosen in the settings: %s%s",preferredGpu.c_str(),chosen==adapters.end()?L" (not found: Windows picks)":L"");
            if(chosen!=adapters.end())std::rotate(adapters.begin(),chosen,chosen+1);
        }
        for(UINT i=0;i<adapters.size();++i)
        {
            const auto& adapter=adapters[i];
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            LARGE_INTEGER driver{};
            const bool known=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));
            Log(L"GPU %u: %s (vendor %04X device %04X, %llu MB, driver %u.%u.%u.%u)%s",i,desc.Description,desc.VendorId,desc.DeviceId,
                static_cast<unsigned long long>(desc.DedicatedVideoMemory>>20),
                known?HIWORD(driver.HighPart):0u,known?LOWORD(driver.HighPart):0u,known?HIWORD(driver.LowPart):0u,known?LOWORD(driver.LowPart):0u,
                desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE?L", software":L"");
            if(gpu->device||desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            if(const HRESULT created=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&gpu->device));SUCCEEDED(created))
            {gpu->adapterName=desc.Description;Log(L"  using it");}
            else Log(L"  no Direct3D 12 device (0x%08X)",static_cast<unsigned>(created));
        }
        if(!gpu->device){error=T(L"D3D12 に対応した GPU が見つかりません。",L"No GPU supporting Direct3D 12 was found.");return nullptr;}
        shared=gpu;
        return gpu;
    }

    bool MascotRenderer::Initialize(HWND window,unsigned w,unsigned h,int msaa,bool blur,std::wstring& error)
    {
        hwnd=window;
        windowWidth=std::max(1u,w);windowHeight=std::max(1u,h);
        width=std::max(1u,static_cast<unsigned>(std::lround(windowWidth*resolutionScale)));
        height=std::max(1u,static_cast<unsigned>(std::lround(windowHeight*resolutionScale)));
        motionBlur=blur;
        bufferWidth=width+width/4;bufferHeight=height+height/4;
        swapWidth=windowWidth+windowWidth/4;swapHeight=windowHeight+windowHeight/4;
        HRESULT hr;
        gpu=SharedGpu(error);
        if(!gpu)return false;
        factory=gpu->factory;
        device=gpu->device;
        adapterName=gpu->adapterName;

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(hr=device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)))){error=HrText(L"Command queue",hr);return false;}

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width=swapWidth;sc.Height=swapHeight;
        sc.Format=ColorFormat;
        sc.SampleDesc.Count=1;
        sc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount=FrameCount;
        sc.Scaling=DXGI_SCALING_STRETCH;
        sc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        ComPtr<IDXGISwapChain1> sc1;
        if(FAILED(hr=factory->CreateSwapChainForComposition(queue.Get(),&sc,nullptr,&sc1))){error=HrText(L"Composition swap chain",hr);return false;}
        sc1.As(&swapChain);

        if(FAILED(hr=DCompositionCreateDevice(nullptr,IID_PPV_ARGS(&composition)))||
           FAILED(hr=composition->CreateTargetForHwnd(hwnd,TRUE,&compositionTarget))||
           FAILED(hr=composition->CreateVisual(&compositionVisual))||
           FAILED(hr=compositionVisual->SetContent(swapChain.Get()))||
           FAILED(hr=compositionTarget->SetRoot(compositionVisual.Get()))||
           FAILED(hr=composition->Commit()))
        {error=HrText(L"DirectComposition",hr);return false;}

        // Pick the highest supported MSAA count not above the request.
        sampleCount=1;
        for(UINT count=static_cast<UINT>(std::max(1,msaa));count>1;count/=2)
        {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
            q.Format=ColorFormat;q.SampleCount=count;
            if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))&&q.NumQualityLevels>0)
            {sampleCount=count;break;}
        }
        if(motionBlur&&sampleCount>1)
        {
            // The motion target is multisampled with the colour: without support, no blur.
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
            q.Format=MotionFormat;q.SampleCount=sampleCount;
            if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))||q.NumQualityLevels==0)motionBlur=false;
        }
        Log(L"  drawing: MSAA x%u (asked x%d), motion blur %s, resolution %.0f%%",sampleCount,msaa,motionBlur?L"on":L"off",resolutionScale*100);

        D3D12_DESCRIPTOR_HEAP_DESC rtv{};
        rtv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtv.NumDescriptors=FrameCount+5;   // back buffers, MSAA colour, scene colour, motion, MSAA motion, blurred colour
        D3D12_DESCRIPTOR_HEAP_DESC dsv{};
        dsv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dsv.NumDescriptors=2;   // the frame's depth, the shadow map
        if(FAILED(hr=device->CreateDescriptorHeap(&rtv,IID_PPV_ARGS(&rtvHeap)))||FAILED(hr=device->CreateDescriptorHeap(&dsv,IID_PPV_ARGS(&dsvHeap))))
        {error=HrText(L"Descriptor heap",hr);return false;}
        rtvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        for(auto& f:frames)
        {
            if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator)))){error=HrText(L"Allocator",hr);return false;}
            auto readbackHeap=HeapProperties(D3D12_HEAP_TYPE_READBACK);
            auto desc=BufferDesc(256);
            if(FAILED(hr=device->CreateCommittedResource(&readbackHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&f.readback))))
            {error=HrText(L"Readback buffer",hr);return false;}
        }
        if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&uploadAllocator)))||
           FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,frames[0].allocator.Get(),nullptr,IID_PPV_ARGS(&commandList))))
        {error=HrText(L"Command list",hr);return false;}
        if(FAILED(hr=commandList.As(&commandList1))){error=HrText(L"Command list 1",hr);return false;}
        commandList->Close();
        if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))){error=HrText(L"Fence",hr);return false;}
        fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);

        if(!CreateRenderTargets(error)||!CreatePipelines(error))return false;

        // The shadow map: written as depth, read by the model shader.
        {
            auto defaultHeap=HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width=desc.Height=ShadowMapSize;desc.DepthOrArraySize=1;desc.MipLevels=1;
            desc.Format=DXGI_FORMAT_R32_TYPELESS;
            desc.SampleDesc.Count=1;
            desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE clear{};
            clear.Format=DepthFormat;clear.DepthStencil.Depth=1.0f;
            if(FAILED(hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&shadowMap))))
            {error=HrText(L"Shadow map",hr);return false;}
            D3D12_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format=DepthFormat;
            view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            const auto dsvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
            device->CreateDepthStencilView(shadowMap.Get(),&view,D3D12_CPU_DESCRIPTOR_HANDLE{dsvHeap->GetCPUDescriptorHandleForHeapStart().ptr+dsvStride});
        }

        // 1x1 white stands in for every missing texture.
        ImageRgba8 image;
        image.levels.push_back({1,1,{255,255,255,255}});
        BeginUploads();
        const bool ok=UploadTexture(image,white);
        FinishUploads();
        if(!ok){error=T(L"既定テクスチャの作成に失敗しました。",L"Could not create the default texture.");return false;}
        return true;
    }

    bool MascotRenderer::CreateRenderTargets(std::wstring& error)
    {
        HRESULT hr;
        auto rtv=rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<FrameCount;++i)
        {
            if(FAILED(hr=swapChain->GetBuffer(i,IID_PPV_ARGS(&backBuffers[i])))){error=HrText(L"Back buffer",hr);return false;}
            D3D12_CPU_DESCRIPTOR_HANDLE handle{rtv.ptr+i*rtvStride};
            device->CreateRenderTargetView(backBuffers[i].Get(),nullptr,handle);
        }
        auto defaultHeap=HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=bufferWidth;desc.Height=bufferHeight;desc.DepthOrArraySize=1;desc.MipLevels=1;
        desc.SampleDesc.Count=sampleCount;
        if(sampleCount>1)
        {
            desc.Format=ColorFormat;
            desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format=ColorFormat;
            std::copy(std::begin(ClearColor),std::end(ClearColor),clear.Color);
            if(FAILED(hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&msaaTarget))))
            {error=HrText(L"MSAA target",hr);return false;}
            device->CreateRenderTargetView(msaaTarget.Get(),nullptr,D3D12_CPU_DESCRIPTOR_HANDLE{rtv.ptr+FrameCount*rtvStride});
        }
        {
            D3D12_RESOURCE_DESC target=desc;
            target.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            const auto make=[&](DXGI_FORMAT format,UINT samples,ComPtr<ID3D12Resource>& resource,UINT slot)
            {
                target.Format=clear.Format=format;
                target.SampleDesc.Count=samples;
                if(FAILED(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&target,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&resource))))
                    return false;
                device->CreateRenderTargetView(resource.Get(),nullptr,D3D12_CPU_DESCRIPTOR_HANDLE{rtv.ptr+(FrameCount+slot)*rtvStride});
                return true;
            };
            if(!make(ColorFormat,1,sceneColor,1)||
               (motionBlur&&(!make(MotionFormat,1,sceneMotion,2)||(sampleCount>1&&!make(MotionFormat,sampleCount,msaaMotion,3))||!make(ColorFormat,1,blurredColor,4))))
            {error=L"Drawing targets";return false;}
            WritePassDescriptors();
        }
        desc.Format=DepthFormat;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL|D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
        D3D12_CLEAR_VALUE depthClear{};
        depthClear.Format=DepthFormat;depthClear.DepthStencil.Depth=1.0f;
        if(FAILED(hr=device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&depthClear,IID_PPV_ARGS(&depthTarget))))
        {error=HrText(L"Depth target",hr);return false;}
        device->CreateDepthStencilView(depthTarget.Get(),nullptr,dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool MascotRenderer::CreatePipelines(std::wstring& error)
    {
        HRESULT hr;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors=3;
        range.BaseShaderRegister=1;
        D3D12_DESCRIPTOR_RANGE shadowRange{};
        shadowRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        shadowRange.NumDescriptors=1;
        shadowRange.BaseShaderRegister=4;
        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[1].Descriptor.ShaderRegister=1;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2].Descriptor.ShaderRegister=0;
        params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges=1;params[3].DescriptorTable.pDescriptorRanges=&range;
        params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges=1;params[4].DescriptorTable.pDescriptorRanges=&shadowRange;
        params[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        for(int i=0;i<2;++i)
        {
            auto& s=samplers[i];
            s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU=s.AddressV=s.AddressW=i==0?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s.MaxLOD=D3D12_FLOAT32_MAX;
            s.ShaderRegister=i;
            s.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        }
        // Shadow comparisons: outside the map counts as lit.
        samplers[2].Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[2].AddressU=samplers[2].AddressV=samplers[2].AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[2].BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[2].ComparisonFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[2].MaxLOD=D3D12_FLOAT32_MAX;
        samplers[2].ShaderRegister=2;
        samplers[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters=5;rs.pParameters=params;
        rs.NumStaticSamplers=3;rs.pStaticSamplers=samplers;
        rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob,errors;
        if(FAILED(hr=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
           FAILED(hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&rootSignature))))
        {error=HrText(L"Root signature",hr);return false;}

        const D3D12_INPUT_ELEMENT_DESC layout[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R32G32B32A32_SINT,1,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,1,28,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32_FLOAT,1,44,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32_UINT,1,56,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32_FLOAT,1,60,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature=rootSignature.Get();
        pso.InputLayout={layout,static_cast<UINT>(std::size(layout))};
        pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets=motionBlur?2:1;
        pso.RTVFormats[0]=ColorFormat;
        if(motionBlur)pso.RTVFormats[1]=MotionFormat;
        // The motion target takes the frontmost surface's motion as it is.
        pso.BlendState.IndependentBlendEnable=TRUE;
        pso.BlendState.RenderTarget[1].RenderTargetWriteMask=motionBlur?D3D12_COLOR_WRITE_ENABLE_ALL:0;
        pso.DSVFormat=DepthFormat;
        pso.SampleDesc.Count=sampleCount;
        pso.SampleMask=UINT_MAX;
        // Straight-alpha source over a premultiplied destination keeps the
        // back buffer premultiplied, as the composition swap chain expects.
        auto& blend=pso.BlendState.RenderTarget[0];
        blend.BlendEnable=TRUE;
        blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.DepthClipEnable=TRUE;
        pso.RasterizerState.MultisampleEnable=sampleCount>1;
        pso.DepthStencilState.DepthEnable=TRUE;
        pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;

        pso.VS={g_ModelVS,sizeof(g_ModelVS)};
        pso.PS={g_ModelPS,sizeof(g_ModelPS)};
        pso.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
        if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&modelCullPso)))){error=HrText(L"Model PSO",hr);return false;}
        pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&modelDoubleSidedPso)))){error=HrText(L"Model PSO",hr);return false;}
        pso.VS={g_EdgeVS,sizeof(g_EdgeVS)};
        pso.PS={g_EdgePS,sizeof(g_EdgePS)};
        pso.RasterizerState.CullMode=D3D12_CULL_MODE_FRONT;
        if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&edgePso)))){error=HrText(L"Edge PSO",hr);return false;}

        // Shadow map: depth only, both faces, biased against self-shadow acne.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC shadow=pso;
            shadow.VS={g_ShadowVS,sizeof(g_ShadowVS)};
            shadow.PS={g_ShadowPS,sizeof(g_ShadowPS)};
            shadow.NumRenderTargets=0;
            shadow.RTVFormats[0]=shadow.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;
            shadow.BlendState.RenderTarget[0].BlendEnable=FALSE;
            shadow.BlendState.RenderTarget[0].RenderTargetWriteMask=shadow.BlendState.RenderTarget[1].RenderTargetWriteMask=0;
            shadow.SampleDesc.Count=1;
            shadow.RasterizerState.MultisampleEnable=FALSE;
            shadow.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            shadow.RasterizerState.DepthBias=1000;
            shadow.RasterizerState.SlopeScaledDepthBias=2.0f;
            shadow.RasterizerState.DepthBiasClamp=0.01f;
            if(FAILED(hr=device->CreateGraphicsPipelineState(&shadow,IID_PPV_ARGS(&shadowPso)))){error=HrText(L"Shadow PSO",hr);return false;}
        }

        // Particles: no vertex input, premultiplied output, depth-tested against the model.
        pso.InputLayout={nullptr,0};
        pso.VS={g_ParticleVS,sizeof(g_ParticleVS)};
        pso.PS={g_ParticlePS,sizeof(g_ParticlePS)};
        pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        blend.SrcBlend=D3D12_BLEND_ONE;
        pso.BlendState.RenderTarget[1].RenderTargetWriteMask=0;   // sparkles leave the motion alone
        if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&particlePso)))){error=HrText(L"Particle PSO",hr);return false;}

        {
            // The smear and the scaling up: one triangle over the target, no blending.
            D3D12_GRAPHICS_PIPELINE_STATE_DESC post{};
            post.pRootSignature=rootSignature.Get();
            post.VS={g_PostVS,sizeof(g_PostVS)};
            post.PS={g_PostPS,sizeof(g_PostPS)};
            post.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            post.NumRenderTargets=1;
            post.RTVFormats[0]=ColorFormat;
            post.SampleDesc.Count=1;
            post.SampleMask=UINT_MAX;
            post.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            post.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
            post.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
            post.RasterizerState.DepthClipEnable=TRUE;
            if(motionBlur&&FAILED(hr=device->CreateGraphicsPipelineState(&post,IID_PPV_ARGS(&blurPso)))){error=HrText(L"Blur PSO",hr);return false;}
            post.PS={g_UpscalePS,sizeof(g_UpscalePS)};
            if(FAILED(hr=device->CreateGraphicsPipelineState(&post,IID_PPV_ARGS(&upscalePso)))){error=HrText(L"Upscale PSO",hr);return false;}
        }
        return true;
    }

    // The last passes' inputs, after the materials' textures and the shadow map, three to a
    // table (white fills the unused slots): the blur's (the drawn frame and its motion), the
    // scaling up's from the drawn frame, and from the smeared one.
    void MascotRenderer::WritePassDescriptors()
    {
        if(!srvHeap||!sceneColor||!white.resource)return;
        const auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();
        const size_t base=materialCount*3+1;
        ID3D12Resource* const slots[9]={
            sceneColor.Get(),sceneMotion?sceneMotion.Get():white.resource.Get(),white.resource.Get(),
            sceneColor.Get(),white.resource.Get(),white.resource.Get(),
            blurredColor?blurredColor.Get():white.resource.Get(),white.resource.Get(),white.resource.Get()};
        for(size_t i=0;i<9;++i)
            device->CreateShaderResourceView(slots[i],nullptr,D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr+(base+i)*srvStride});
    }

    void MascotRenderer::BeginUploads()
    {
        uploadAllocator->Reset();
        commandList->Reset(uploadAllocator.Get(),nullptr);
    }

    void MascotRenderer::FinishUploads()
    {
        commandList->Close();
        ID3D12CommandList* lists[]={commandList.Get()};
        queue->ExecuteCommandLists(1,lists);
        WaitIdle();
        pendingUploads.clear();
    }

    bool MascotRenderer::UploadBuffer(const void* data,size_t size,ComPtr<ID3D12Resource>& buffer,D3D12_RESOURCE_STATES finalState)
    {
        auto defaultHeap=HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        auto uploadHeap=HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        auto desc=BufferDesc(size);
        ComPtr<ID3D12Resource> staging;
        if(FAILED(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)))||
           FAILED(device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&staging))))
            return false;
        void* mapped=nullptr;
        if(FAILED(staging->Map(0,nullptr,&mapped)))return false;
        std::memcpy(mapped,data,size);
        staging->Unmap(0,nullptr);
        commandList->CopyBufferRegion(buffer.Get(),0,staging.Get(),0,size);
        auto barrier=Transition(buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,finalState);
        commandList->ResourceBarrier(1,&barrier);
        pendingUploads.push_back(staging);
        return true;
    }

    bool MascotRenderer::UploadTexture(const ImageRgba8& image,Texture& texture)
    {
        if(image.levels.empty())return false;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=image.levels[0].width;desc.Height=image.levels[0].height;
        desc.DepthOrArraySize=1;
        desc.MipLevels=static_cast<UINT16>(image.levels.size());
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;
        auto defaultHeap=HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if(FAILED(device->CreateCommittedResource(&defaultHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&texture.resource))))
            return false;

        const UINT count=desc.MipLevels;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> rowBytes(count);
        UINT64 total=0;
        device->GetCopyableFootprints(&desc,0,count,0,layouts.data(),rows.data(),rowBytes.data(),&total);

        auto uploadHeap=HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        auto bufferDesc=BufferDesc(total);
        ComPtr<ID3D12Resource> staging;
        std::uint8_t* mapped=nullptr;
        if(FAILED(device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&bufferDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&staging)))||
           FAILED(staging->Map(0,nullptr,reinterpret_cast<void**>(&mapped))))
            return false;
        for(UINT i=0;i<count;++i)
        {
            const auto& level=image.levels[i];
            for(UINT y=0;y<rows[i];++y)
                std::memcpy(mapped+layouts[i].Offset+static_cast<size_t>(y)*layouts[i].Footprint.RowPitch,
                            level.pixels.data()+static_cast<size_t>(y)*level.width*4,static_cast<size_t>(level.width)*4);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource=texture.resource.Get();
            dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex=i;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource=staging.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint=layouts[i];
            commandList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        staging->Unmap(0,nullptr);
        auto barrier=Transition(texture.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1,&barrier);
        pendingUploads.push_back(staging);
        texture.valid=true;
        return true;
    }

    bool MascotRenderer::LoadModel(const MascotModel& model,const std::wstring& toonDir,unsigned maxTextureSize,std::wstring& error)
    {
        WaitIdle();
        const auto& statics=model.StaticVertices();
        const auto& indices=model.Indices();
        const auto& materials=model.Materials();
        vertexCount=statics.size();
        boneCount=std::max<size_t>(model.BoneCount(),1);
        materialCount=materials.size();

        BeginUploads();
        bool ok=UploadBuffer(statics.data(),statics.size()*sizeof(StaticVertex),staticVertexBuffer,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)&&
                UploadBuffer(indices.data(),indices.size()*sizeof(std::uint32_t),indexBuffer,D3D12_RESOURCE_STATE_INDEX_BUFFER);

        // Only decode textures some material actually samples.
        const auto& paths=model.TexturePaths();
        textures.assign(paths.size(),{});
        std::vector<bool> used(paths.size(),false);
        auto markUsed=[&](int index){if(index>=0&&static_cast<size_t>(index)<used.size())used[index]=true;};
        for(const auto& m:materials)
        {
            markUsed(m.texture);
            if(m.sphereMode==1||m.sphereMode==2)markUsed(m.sphere);
            if(!m.sharedToon)markUsed(m.toon);
        }
        // The UV triangles drawn with each color texture (to fill its unused padding).
        std::vector<std::vector<std::array<float,2>>> uvTriangles(paths.size());
        {
            const auto& vertices=model.DynamicVertices();
            const auto& indices=model.Indices();
            for(const auto& m:materials)
            {
                if(m.texture<0||static_cast<size_t>(m.texture)>=paths.size())continue;
                auto& list=uvTriangles[m.texture];
                for(std::uint32_t k=m.indexStart;k<m.indexStart+m.indexCount&&k<indices.size();++k)
                    if(indices[k]<vertices.size())list.push_back(vertices[indices[k]].uv);
            }
        }
        for(size_t i=0;ok&&i<paths.size();++i)
        {
            if(!used[i])continue;
            ImageRgba8 image;
            if(LoadImageRgba8(paths[i],image,maxTextureSize,&uvTriangles[i]))
            {
                ok=UploadTexture(image,textures[i]);
                Log(L"  texture %s: %ux%u%s%s",paths[i].c_str(),image.levels[0].width,image.levels[0].height,
                    image.alphaIgnored?L", alpha 0 everywhere (taken as opaque)":image.hasAlpha?L", alpha":L"",ok?L"":L", upload failed");
            }
            else Log(L"  texture %s: could not be read (drawn white)",paths[i].c_str());
        }
        for(int i=0;ok&&i<10;++i)
        {
            sharedToons[i]={};
            ImageRgba8 image;
            wchar_t name[16]{};
            swprintf_s(name,L"toon%02d.bmp",i+1);
            const bool loaded=!toonDir.empty()&&LoadImageRgba8((std::filesystem::path(toonDir)/name).wstring(),image);
            if(!loaded)
            {
                // No MMD Data folder configured: approximate the shared toon as a soft gray ramp.
                image.levels.assign(1,{1,32,{}});
                for(int y=0;y<32;++y)
                {
                    const auto v=static_cast<std::uint8_t>(y<15?255:205);
                    image.levels[0].pixels.insert(image.levels[0].pixels.end(),{v,v,v,255});
                }
                GenerateMips(image);
            }
            ok=UploadTexture(image,sharedToons[i]);
        }
        FinishUploads();
        if(!ok){error=T(L"GPU リソースの作成に失敗しました。",L"Could not create GPU resources.");return false;}

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=static_cast<UINT>(materialCount*3+10);   // the materials' textures, the shadow map, the last passes' inputs
        heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        srvHeap.Reset();
        if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srvHeap)))){error=T(L"SRV ヒープの作成に失敗しました。",L"Could not create the SRV heap.");return false;}
        auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();
        auto pick=[&](int index)->ID3D12Resource*{
            return index>=0&&static_cast<size_t>(index)<textures.size()&&textures[index].valid?textures[index].resource.Get():nullptr;};
        for(size_t i=0;i<materialCount;++i)
        {
            const auto& m=materials[i];
            ID3D12Resource* slots[3]={
                pick(m.texture),
                (m.sphereMode==1||m.sphereMode==2)?pick(m.sphere):nullptr,
                m.sharedToon?(m.toon>=0&&m.toon<10?sharedToons[m.toon].resource.Get():nullptr):pick(m.toon)};
            for(int s=0;s<3;++s)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE handle{cpu.ptr+(i*3+s)*srvStride};
                device->CreateShaderResourceView(slots[s]?slots[s]:white.resource.Get(),nullptr,handle);
            }
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format=DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(shadowMap.Get(),&view,D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr+materialCount*3*srvStride});
        }
        WritePassDescriptors();
        history=false;

        // Per-frame upload ring: scene CB, material CBs, bone matrices (this frame's, then last
        // frame's for motion blur), morph-applied vertices.
        sceneOffset=0;
        materialOffset=Align256(sizeof(SceneConstants));
        boneOffset=Align256(materialOffset+materialCount*256);
        dynamicOffset=Align256(boneOffset+boneCount*64*2);
        particleOffset=Align256(dynamicOffset+vertexCount*sizeof(DynamicVertex));
        uploadSize=particleOffset+MaxParticles*sizeof(GpuParticle);
        auto uploadHeap=HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        auto desc=BufferDesc(uploadSize);
        for(auto& f:frames)
        {
            if(f.upload&&f.mapped)f.upload->Unmap(0,nullptr);
            f.upload.Reset();f.mapped=nullptr;f.dynamicRevision=0;
            if(FAILED(device->CreateCommittedResource(&uploadHeap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&f.upload)))||
               FAILED(f.upload->Map(0,nullptr,reinterpret_cast<void**>(&f.mapped))))
            {error=T(L"アップロードバッファの作成に失敗しました。",L"Could not create the upload buffer.");return false;}
        }
        return true;
    }

    bool MascotRenderer::Resize(unsigned w,unsigned h,std::wstring& error)
    {
        const unsigned ww=std::max(1u,w),wh=std::max(1u,h);
        w=std::max(1u,static_cast<unsigned>(std::lround(ww*resolutionScale)));
        h=std::max(1u,static_cast<unsigned>(std::lround(wh*resolutionScale)));
        if(w==width&&h==height&&ww==windowWidth&&wh==windowHeight)return true;
        clearWhole.fill(true);
        if(w<=bufferWidth&&h<=bufferHeight&&ww<=swapWidth&&wh<=swapHeight){width=w;height=h;windowWidth=ww;windowHeight=wh;return true;}
        WaitIdle();
        for(auto& b:backBuffers)b.Reset();
        msaaTarget.Reset();
        depthTarget.Reset();
        sceneColor.Reset();sceneMotion.Reset();msaaMotion.Reset();blurredColor.Reset();
        width=w;height=h;windowWidth=ww;windowHeight=wh;
        bufferWidth=std::max(bufferWidth,width+width/4);
        bufferHeight=std::max(bufferHeight,height+height/4);
        swapWidth=std::max(swapWidth,windowWidth+windowWidth/4);
        swapHeight=std::max(swapHeight,windowHeight+windowHeight/4);
        HRESULT hr;
        if(FAILED(hr=swapChain->ResizeBuffers(FrameCount,swapWidth,swapHeight,ColorFormat,0))){error=HrText(L"ResizeBuffers",hr);return false;}
        for(auto& f:frames){f.hitPoint={-1,-1};f.fenceValue=0;}
        lastAlpha=-1;
        return CreateRenderTargets(error);
    }

    void MascotRenderer::SetCamera(FXMVECTOR eye,FXMVECTOR target,float fovY)
    {
        XMStoreFloat4x4(&view,XMMatrixLookAtLH(eye,target,XMVectorSet(0,1,0,0)));
        XMStoreFloat3(&cameraEye,eye);
        XMStoreFloat3(&cameraTarget,target);
        cameraFovY=fovY;
        XMStoreFloat4x4(&projection,XMMatrixPerspectiveFovLH(fovY,static_cast<float>(windowWidth)/windowHeight,1.0f,1000.0f));
    }

    void MascotRenderer::SetLight(float azimuth,float elevation)
    {
        // The camera looks along +Z at the model's face, so the front is -Z and the viewer's right +X.
        const float a=XMConvertToRadians(azimuth),e=XMConvertToRadians(elevation);
        lightDir={-std::sin(a)*std::cos(e),-std::sin(e),std::cos(a)*std::cos(e)};
    }

    void MascotRenderer::SetVisualOffset(int x,int y){visualOffset={x,y};}

    void MascotRenderer::SetModelTransform(FXMMATRIX world){XMStoreFloat4x4(&modelWorld,world);}

    void MascotRenderer::SetParticles(const std::vector<Particle>& list)
    {
        particles.assign(list.begin(),list.begin()+static_cast<std::ptrdiff_t>(std::min(list.size(),MaxParticles)));
    }

    POINT MascotRenderer::Project(const std::array<float,3>& world) const
    {
        const XMMATRIX vp=XMLoadFloat4x4(&view)*XMLoadFloat4x4(&projection);
        const XMVECTOR ndc=XMVector3TransformCoord(XMVectorSet(world[0],world[1],world[2],1),vp);
        return {static_cast<LONG>((XMVectorGetX(ndc)*0.5f+0.5f)*windowWidth),static_cast<LONG>((0.5f-XMVectorGetY(ndc)*0.5f)*windowHeight)};
    }

    int MascotRenderer::AlphaAt(POINT p)
    {
        const UINT index=swapChain?swapChain->GetCurrentBackBufferIndex():0;
        frames[index].hitPoint=p;
        return lastAlpha;
    }

    void MascotRenderer::Render(const MascotModel& model,float edgeScale)
    {
        if(!srvHeap||!frames[0].mapped||deviceLost!=S_OK)return;
        const UINT index=swapChain->GetCurrentBackBufferIndex();
        auto& f=frames[index];
        WaitForFence(f.fenceValue);

        // The readback for this slot has completed; publish it for hit testing.
        {
            D3D12_RANGE range{0,4};
            std::uint8_t* pixel=nullptr;
            if(f.fenceValue&&SUCCEEDED(f.readback->Map(0,&range,reinterpret_cast<void**>(&pixel))))
            {
                lastAlpha=pixel[3];
                D3D12_RANGE none{0,0};
                f.readback->Unmap(0,&none);
            }
        }

        // Constants.
        SceneConstants scene{};
        const XMMATRIX v=XMLoadFloat4x4(&view),w=XMLoadFloat4x4(&modelWorld);
        // Sub-pixel shift applied after projection (in clip space, scaled by w), in drawn pixels.
        const XMFLOAT2 scale{static_cast<float>(width)/windowWidth,static_cast<float>(height)/windowHeight};
        const bool upscale=width!=windowWidth||height!=windowHeight;
        if(scale.x!=lastScale.x||scale.y!=lastScale.y)history=false;   // last frame was drawn at another resolution
        lastScale=scale;
        const XMFLOAT2 shiftPixels{pixelOffset.x*scale.x,pixelOffset.y*scale.y};
        const XMMATRIX p=XMLoadFloat4x4(&projection)*XMMatrixTranslation(2*shiftPixels.x/width,-2*shiftPixels.y/height,0);
        XMStoreFloat4x4(&scene.viewProj,XMMatrixTranspose(w*v*p));
        XMStoreFloat4x4(&scene.view,XMMatrixTranspose(w*v));
        XMStoreFloat4x4(&scene.particleViewProj,XMMatrixTranspose(v*p));
        // Camera right and up in world space: the first two columns of the (row-vector) view.
        scene.cameraRight={view._11,view._21,view._31,0};
        scene.cameraUp={view._12,view._22,view._32,0};
        scene.lightDir=lightDir;
        scene.lightColor={154/255.0f,154/255.0f,154/255.0f};
        scene.edgePixels=edgeScale*static_cast<float>(height)/600.0f;
        scene.viewportSize={static_cast<float>(width),static_cast<float>(height)};
        {
            // The light looks at the camera's target from far enough to see all the camera
            // sees (the model never leaves the window), straight down its direction.
            const XMVECTOR target=XMLoadFloat3(&cameraTarget);
            const float halfHeight=XMVectorGetX(XMVector3Length(XMVectorSubtract(XMLoadFloat3(&cameraEye),target)))*std::tan(cameraFovY*0.5f);
            const float halfWidth=halfHeight*static_cast<float>(width)/static_cast<float>(height);
            const float radius=std::max(1.0f,std::sqrt(halfHeight*halfHeight+halfWidth*halfWidth));
            const XMVECTOR toward=XMVector3Normalize(XMLoadFloat3(&scene.lightDir));
            const XMMATRIX lightView=XMMatrixLookAtLH(XMVectorSubtract(target,XMVectorScale(toward,2*radius)),target,XMVectorSet(0,1,0,0));
            const XMMATRIX lightProj=XMMatrixOrthographicLH(2*radius,2*radius,radius*0.5f,radius*3.5f);
            XMStoreFloat4x4(&scene.lightViewProj,XMMatrixTranspose(w*lightView*lightProj));
            scene.shadowInfo={selfShadow?1.0f:0.0f,1.0f/ShadowMapSize,0,0};
        }
        // Motion blur: last frame's camera and place on screen (the window moves by whole
        // pixels plus the sub-pixel shift). A jump (another monitor) or a first frame has none.
        const bool blur=motionBlur&&sceneMotion;
        const XMFLOAT2 screenOffset{visualOffset.x*scale.x+shiftPixels.x,visualOffset.y*scale.y+shiftPixels.y};
        XMFLOAT2 shift{screenOffset.x-lastScreenOffset.x,screenOffset.y-lastScreenOffset.y};
        if(!history||std::abs(shift.x)>width*0.5f||std::abs(shift.y)>height*0.5f)
        {
            lastViewProj=scene.viewProj;
            lastViewport=scene.viewportSize;
            shift={0,0};
            lastBones.assign(static_cast<const std::uint8_t*>(model.BoneMatrices()),
                             static_cast<const std::uint8_t*>(model.BoneMatrices())+(model.BoneMatrices()?std::min(model.BoneCount(),boneCount)*64:0));
        }
        scene.prevViewProj=lastViewProj;
        scene.motionInfo={shift.x,shift.y,lastViewport.x,lastViewport.y};
        // The smear is a share of one frame's worth of motion at the drawing rate: a late
        // frame (one that moved further) is not smeared further for it.
        LARGE_INTEGER now{},frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double sinceLast=lastRenderTime.QuadPart?static_cast<double>(now.QuadPart-lastRenderTime.QuadPart)/frequency.QuadPart:framePeriod;
        lastRenderTime=now;
        const float perFrame=static_cast<float>(std::clamp(framePeriod/std::max(sinceLast,1e-4),0.25,1.5));
        const float longest=std::min(BlurLongest*static_cast<float>(1/(30*framePeriod)),BlurLongestMax);
        scene.blurInfo={BlurShutter*perFrame,std::max(8.0f,std::max(BlurLongest,longest)*height),scale.x,scale.y};
        scene.blurFlags[0]=blur?1u:0u;
        scene.blurFlags[1]=static_cast<std::uint32_t>(boneCount);
        std::memcpy(f.mapped+sceneOffset,&scene,sizeof(scene));

        const auto& materials=model.Materials();
        for(size_t i=0;i<materialCount&&i<materials.size();++i)
        {
            const auto& m=materials[i];
            MaterialConstants c{};
            c.diffuse=m.diffuse;c.specular=m.specular;c.shininess=m.shininess;
            c.ambient=m.ambient;c.edgeSize=m.edgeSize;c.edgeColor=m.edgeColor;
            c.textureMul=m.textureMul;c.textureAdd=m.textureAdd;
            c.sphereMul=m.sphereMul;c.sphereAdd=m.sphereAdd;
            c.toonMul=m.toonMul;c.toonAdd=m.toonAdd;
            auto valid=[&](int t){return t>=0&&static_cast<size_t>(t)<textures.size()&&textures[t].valid;};
            c.useTexture=valid(m.texture)?1u:0u;
            c.sphereMode=(m.sphereMode==1||m.sphereMode==2)&&valid(m.sphere)?static_cast<std::uint32_t>(m.sphereMode):0u;
            c.useToon=(m.sharedToon?(m.toon>=0&&m.toon<10):valid(m.toon))?1u:0u;
            c.receiveShadow=(m.drawFlag&0x08)?1u:0u;
            std::memcpy(f.mapped+materialOffset+i*256,&c,sizeof(c));
        }
        if(const void* bones=model.BoneMatrices())
        {
            const size_t size=std::min(model.BoneCount(),boneCount)*64;
            std::memcpy(f.mapped+boneOffset,bones,size);
            if(blur)
            {
                std::memcpy(f.mapped+boneOffset+boneCount*64,lastBones.data(),std::min(size,lastBones.size()));
                lastBones.assign(static_cast<const std::uint8_t*>(bones),static_cast<const std::uint8_t*>(bones)+size);
            }
        }
        lastViewProj=scene.viewProj;
        lastViewport=scene.viewportSize;
        lastScreenOffset=screenOffset;
        history=true;
        for(size_t i=0;i<particles.size();++i)
        {
            const auto& q=particles[i];
            const GpuParticle g{{q.position[0],q.position[1],q.position[2]},q.size,{q.color[0],q.color[1],q.color[2],q.color[3]},q.angle,{}};
            std::memcpy(f.mapped+particleOffset+i*sizeof(GpuParticle),&g,sizeof(g));
        }
        if(f.dynamicRevision!=model.DynamicRevision())
        {
            std::memcpy(f.mapped+dynamicOffset,model.DynamicVertices().data(),vertexCount*sizeof(DynamicVertex));
            f.dynamicRevision=model.DynamicRevision();
        }

        // Commands.
        f.allocator->Reset();
        commandList->Reset(f.allocator.Get(),modelCullPso.Get());
        ID3D12Resource* backBuffer=backBuffers[index].Get();
        const auto rtvStart=rtvHeap->GetCPUDescriptorHandleForHeapStart();
        // Drawn into: the MSAA target, or sceneColor for a pass after it, else the back buffer.
        const bool offscreen=blur||upscale;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv{sampleCount>1?rtvStart.ptr+FrameCount*rtvStride:
                                              offscreen?rtvStart.ptr+(FrameCount+1)*rtvStride:rtvStart.ptr+index*rtvStride};
        const D3D12_CPU_DESCRIPTOR_HANDLE motionRtv{rtvStart.ptr+(FrameCount+(sampleCount>1?3:2))*rtvStride};
        const auto dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();
        if(sampleCount==1&&!offscreen)
        {
            auto b=Transition(backBuffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1,&b);
        }
        commandList->SetGraphicsRootSignature(rootSignature.Get());
        ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};
        commandList->SetDescriptorHeaps(1,heaps);
        const auto uploadVa=f.upload->GetGPUVirtualAddress();
        commandList->SetGraphicsRootConstantBufferView(0,uploadVa+sceneOffset);
        commandList->SetGraphicsRootShaderResourceView(2,uploadVa+boneOffset);
        D3D12_VERTEX_BUFFER_VIEW vbs[2]={
            {uploadVa+dynamicOffset,static_cast<UINT>(vertexCount*sizeof(DynamicVertex)),sizeof(DynamicVertex)},
            {staticVertexBuffer->GetGPUVirtualAddress(),static_cast<UINT>(vertexCount*sizeof(StaticVertex)),sizeof(StaticVertex)}};
        commandList->IASetVertexBuffers(0,2,vbs);
        D3D12_INDEX_BUFFER_VIEW ib{indexBuffer->GetGPUVirtualAddress(),static_cast<UINT>(model.Indices().size()*sizeof(std::uint32_t)),DXGI_FORMAT_R32_UINT};
        commandList->IASetIndexBuffer(&ib);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto srvGpu=srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetGraphicsRootDescriptorTable(4,D3D12_GPU_DESCRIPTOR_HANDLE{srvGpu.ptr+materialCount*3*srvStride});

        // Self-shadow map: the materials that cast one (PMX flag 0x04).
        if(selfShadow)
        {
            auto toDepth=Transition(shadowMap.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
            commandList->ResourceBarrier(1,&toDepth);
            const D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv{dsv.ptr+device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
            commandList->OMSetRenderTargets(0,nullptr,FALSE,&shadowDsv);
            commandList->ClearDepthStencilView(shadowDsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
            const D3D12_VIEWPORT shadowViewport{0,0,static_cast<float>(ShadowMapSize),static_cast<float>(ShadowMapSize),0,1};
            const D3D12_RECT shadowScissor{0,0,static_cast<LONG>(ShadowMapSize),static_cast<LONG>(ShadowMapSize)};
            commandList->RSSetViewports(1,&shadowViewport);
            commandList->RSSetScissorRects(1,&shadowScissor);
            commandList->SetPipelineState(shadowPso.Get());
            for(size_t i=0;i<materialCount&&i<materials.size();++i)
            {
                const auto& m=materials[i];
                if(m.indexCount==0||!(m.drawFlag&0x04)||m.diffuse[3]<0.98f)continue;
                commandList->SetGraphicsRootConstantBufferView(1,uploadVa+materialOffset+i*256);
                commandList->SetGraphicsRootDescriptorTable(3,D3D12_GPU_DESCRIPTOR_HANDLE{srvGpu.ptr+i*3*srvStride});
                commandList->DrawIndexedInstanced(m.indexCount,1,m.indexStart,0,0);
            }
            auto toRead=Transition(shadowMap.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            commandList->ResourceBarrier(1,&toRead);
            commandList->SetPipelineState(modelCullPso.Get());
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE targets[2]={rtv,motionRtv};
        commandList->OMSetRenderTargets(blur?2:1,targets,FALSE,&dsv);
        // Only the drawn area is cleared and resolved; the rest of a back buffer stays clear,
        // except right after the drawn area shrank (clearWhole).
        const D3D12_RECT drawn{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        const bool wholeBuffer=sampleCount==1&&!offscreen&&clearWhole[index];
        commandList->ClearRenderTargetView(rtv,ClearColor,wholeBuffer?0:1,wholeBuffer?nullptr:&drawn);
        if(blur)commandList->ClearRenderTargetView(motionRtv,ClearColor,1,&drawn);
        commandList->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,1,&drawn);
        if(wholeBuffer)clearWhole[index]=false;
        D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        commandList->RSSetViewports(1,&viewport);
        commandList->RSSetScissorRects(1,&scissor);

        ID3D12PipelineState* current=modelCullPso.Get();
        for(size_t i=0;i<materialCount&&i<materials.size();++i)
        {
            const auto& m=materials[i];
            if(m.indexCount==0||m.diffuse[3]<=0.0f)continue;
            ID3D12PipelineState* pso=(m.drawFlag&0x01)?modelDoubleSidedPso.Get():modelCullPso.Get();
            if(pso!=current){commandList->SetPipelineState(pso);current=pso;}
            commandList->SetGraphicsRootConstantBufferView(1,uploadVa+materialOffset+i*256);
            commandList->SetGraphicsRootDescriptorTable(3,D3D12_GPU_DESCRIPTOR_HANDLE{srvGpu.ptr+i*3*srvStride});
            commandList->DrawIndexedInstanced(m.indexCount,1,m.indexStart,0,0);
        }
        if(edgeScale>0.0f)
        {
            commandList->SetPipelineState(edgePso.Get());
            for(size_t i=0;i<materialCount&&i<materials.size();++i)
            {
                const auto& m=materials[i];
                if(m.indexCount==0||!(m.drawFlag&0x10)||m.edgeSize<=0.0f||m.edgeColor[3]<=0.0f||m.diffuse[3]<=0.0f)continue;
                commandList->SetGraphicsRootConstantBufferView(1,uploadVa+materialOffset+i*256);
                commandList->DrawIndexedInstanced(m.indexCount,1,m.indexStart,0,0);
            }
        }
        if(!particles.empty())
        {
            commandList->SetPipelineState(particlePso.Get());
            commandList->SetGraphicsRootShaderResourceView(2,uploadVa+particleOffset);
            commandList->DrawInstanced(6,static_cast<UINT>(particles.size()),0,0);
        }

        if(offscreen)
        {
            // Into sceneColor (and sceneMotion), resolved from MSAA ...
            std::vector<D3D12_RESOURCE_BARRIER> before,after;
            if(sampleCount>1)
            {
                before.push_back(Transition(msaaTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
                before.push_back(Transition(sceneColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST));
                if(blur)
                {
                    before.push_back(Transition(msaaMotion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE));
                    before.push_back(Transition(sceneMotion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_DEST));
                }
                commandList->ResourceBarrier(static_cast<UINT>(before.size()),before.data());
                D3D12_RECT source=drawn;
                commandList1->ResolveSubresourceRegion(sceneColor.Get(),0,0,0,msaaTarget.Get(),0,&source,ColorFormat,D3D12_RESOLVE_MODE_AVERAGE);
                if(blur)commandList1->ResolveSubresourceRegion(sceneMotion.Get(),0,0,0,msaaMotion.Get(),0,&source,MotionFormat,D3D12_RESOLVE_MODE_AVERAGE);
                std::vector<D3D12_RESOURCE_BARRIER> resolved{
                    Transition(msaaTarget.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET),
                    Transition(sceneColor.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
                if(blur)
                {
                    resolved.push_back(Transition(msaaMotion.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
                    resolved.push_back(Transition(sceneMotion.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
                }
                commandList->ResourceBarrier(static_cast<UINT>(resolved.size()),resolved.data());
            }
            else
            {
                before.push_back(Transition(sceneColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
                if(blur)before.push_back(Transition(sceneMotion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
                commandList->ResourceBarrier(static_cast<UINT>(before.size()),before.data());
            }
            after.push_back(Transition(sceneColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
            if(blur)after.push_back(Transition(sceneMotion.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
            const auto table=[&](size_t set){return D3D12_GPU_DESCRIPTOR_HANDLE{srvGpu.ptr+(materialCount*3+1+set*3)*srvStride};};
            // ... smeared at the drawn size (into blurredColor when it is scaled up after) ...
            if(blur&&upscale)
            {
                const D3D12_CPU_DESCRIPTOR_HANDLE blurredRtv{rtvStart.ptr+(FrameCount+4)*rtvStride};
                commandList->OMSetRenderTargets(1,&blurredRtv,FALSE,nullptr);
                commandList->SetPipelineState(blurPso.Get());
                commandList->SetGraphicsRootDescriptorTable(3,table(0));
                commandList->DrawInstanced(3,1,0,0);
                auto toRead=Transition(blurredColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commandList->ResourceBarrier(1,&toRead);
                after.push_back(Transition(blurredColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET));
            }
            // ... and into the back buffer, scaled up to the window's pixels or smeared there.
            auto toTarget=Transition(backBuffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1,&toTarget);
            const D3D12_CPU_DESCRIPTOR_HANDLE backRtv{rtvStart.ptr+index*rtvStride};
            if(clearWhole[index])
            {
                commandList->ClearRenderTargetView(backRtv,ClearColor,0,nullptr);
                clearWhole[index]=false;
            }
            commandList->OMSetRenderTargets(1,&backRtv,FALSE,nullptr);
            if(upscale)
            {
                const D3D12_VIEWPORT windowViewport{0,0,static_cast<float>(windowWidth),static_cast<float>(windowHeight),0,1};
                const D3D12_RECT windowScissor{0,0,static_cast<LONG>(windowWidth),static_cast<LONG>(windowHeight)};
                commandList->RSSetViewports(1,&windowViewport);
                commandList->RSSetScissorRects(1,&windowScissor);
                commandList->SetPipelineState(upscalePso.Get());
                commandList->SetGraphicsRootDescriptorTable(3,table(blur?2:1));
            }
            else
            {
                commandList->SetPipelineState(blurPso.Get());
                commandList->SetGraphicsRootDescriptorTable(3,table(0));
            }
            commandList->DrawInstanced(3,1,0,0);
            after.push_back(Transition(backBuffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE));
            commandList->ResourceBarrier(static_cast<UINT>(after.size()),after.data());
        }
        else if(sampleCount>1)
        {
            D3D12_RESOURCE_STATES backState=D3D12_RESOURCE_STATE_PRESENT;
            if(clearWhole[index])
            {
                auto toTarget=Transition(backBuffer,backState,D3D12_RESOURCE_STATE_RENDER_TARGET);
                commandList->ResourceBarrier(1,&toTarget);
                commandList->ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE{rtvStart.ptr+index*rtvStride},ClearColor,0,nullptr);
                backState=D3D12_RESOURCE_STATE_RENDER_TARGET;
                clearWhole[index]=false;
            }
            D3D12_RESOURCE_BARRIER b[2]={
                Transition(msaaTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                Transition(backBuffer,backState,D3D12_RESOURCE_STATE_RESOLVE_DEST)};
            commandList->ResourceBarrier(2,b);
            D3D12_RECT source=drawn;
            commandList1->ResolveSubresourceRegion(backBuffer,0,0,0,msaaTarget.Get(),0,&source,ColorFormat,D3D12_RESOLVE_MODE_AVERAGE);
            D3D12_RESOURCE_BARRIER a[2]={
                Transition(msaaTarget.Get(),D3D12_RESOURCE_STATE_RESOLVE_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(backBuffer,D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE)};
            commandList->ResourceBarrier(2,a);
        }
        else
        {
            auto b=Transition(backBuffer,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commandList->ResourceBarrier(1,&b);
        }

        // Copy the pixel under the cursor so click-through can follow the silhouette.
        const POINT hit{std::clamp<LONG>(f.hitPoint.x,0,static_cast<LONG>(windowWidth)-1),std::clamp<LONG>(f.hitPoint.y,0,static_cast<LONG>(windowHeight)-1)};
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=f.readback.Get();
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={ColorFormat,1,1,1,256};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=backBuffer;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_BOX box{static_cast<UINT>(hit.x),static_cast<UINT>(hit.y),0,static_cast<UINT>(hit.x)+1,static_cast<UINT>(hit.y)+1,1};
        commandList->CopyTextureRegion(&dst,0,0,0,&src,&box);

        auto toPresent=Transition(backBuffer,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        commandList->ResourceBarrier(1,&toPresent);
        commandList->Close();
        ID3D12CommandList* lists[]={commandList.Get()};
        queue->ExecuteCommandLists(1,lists);
        f.fenceValue=nextFenceValue++;
        queue->Signal(fence.Get(),f.fenceValue);
        lastRenderedIndex=index;
        // The frame's position in the window is a composition property: set it in the same
        // composition transaction as this frame, so the two never show apart.
        const bool moved=visualOffset.x!=appliedVisualOffset.x||visualOffset.y!=appliedVisualOffset.y;
        if(moved)
        {
            compositionVisual->SetOffsetX(static_cast<float>(visualOffset.x));
            compositionVisual->SetOffsetY(static_cast<float>(visualOffset.y));
            appliedVisualOffset=visualOffset;
        }
        if(const HRESULT hr=swapChain->Present(1,0);hr==DXGI_ERROR_DEVICE_REMOVED||hr==DXGI_ERROR_DEVICE_RESET)
        {
            const HRESULT reason=device->GetDeviceRemovedReason();
            deviceLost=FAILED(reason)?reason:hr;
            Log(L"GPU stopped: Present 0x%08X, reason 0x%08X",static_cast<unsigned>(hr),static_cast<unsigned>(reason));
            return;
        }
        if(moved)
        {
            // Commit the new position only once the GPU has finished this frame: committed
            // earlier, it can reach the screen a refresh before the frame does (a rare one-step
            // jump while walking).
            WaitForFence(f.fenceValue);
            composition->Commit();
        }
    }

    bool MascotRenderer::SaveLastFrame(const std::wstring& path)
    {
        WaitIdle();
        ID3D12Resource* source=backBuffers[lastRenderedIndex].Get();
        const UINT pitch=(windowWidth*4+255)&~255u;
        auto heap=HeapProperties(D3D12_HEAP_TYPE_READBACK);
        auto desc=BufferDesc(static_cast<UINT64>(pitch)*windowHeight);
        ComPtr<ID3D12Resource> readback;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return false;
        BeginUploads();
        auto toCopy=Transition(source,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1,&toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=readback.Get();
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={ColorFormat,windowWidth,windowHeight,1,pitch};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=source;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{0,0,0,windowWidth,windowHeight,1};
        commandList->CopyTextureRegion(&dst,0,0,0,&src,&box);
        auto back=Transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        commandList->ResourceBarrier(1,&back);
        FinishUploads();

        std::uint8_t* data=nullptr;
        if(FAILED(readback->Map(0,nullptr,reinterpret_cast<void**>(&data))))return false;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;
        const bool ok=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))&&
            SUCCEEDED(wic->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))&&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&
            SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&
            SUCCEEDED(frame->SetSize(windowWidth,windowHeight))&&SUCCEEDED(frame->SetPixelFormat(&format))&&
            SUCCEEDED(frame->WritePixels(windowHeight,pitch,pitch*windowHeight,data))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
        readback->Unmap(0,nullptr);
        return ok;
    }

    void MascotRenderer::WaitForFence(UINT64 value)
    {
        if(value&&fence->GetCompletedValue()<value)
        {
            fence->SetEventOnCompletion(value,fenceEvent);
            WaitForSingleObject(fenceEvent,INFINITE);
        }
    }

    void MascotRenderer::WaitIdle()
    {
        if(!queue||!fence)return;
        const UINT64 value=nextFenceValue++;
        queue->Signal(fence.Get(),value);
        WaitForFence(value);
    }
}
