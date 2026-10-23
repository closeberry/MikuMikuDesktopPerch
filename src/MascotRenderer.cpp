#define NOMINMAX
#include "MascotRenderer.hpp"
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

namespace DesktopMascot
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
        };
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
            std::uint32_t useTexture,sphereMode,useToon,pad;
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
    }

    MascotRenderer::~MascotRenderer()
    {
        if(device)WaitIdle();
        for(auto& f:frames)if(f.upload&&f.mapped)f.upload->Unmap(0,nullptr);
        if(fenceEvent)CloseHandle(fenceEvent);
    }

    bool MascotRenderer::Initialize(HWND window,unsigned w,unsigned h,int msaa,std::wstring& error)
    {
        hwnd=window;width=std::max(1u,w);height=std::max(1u,h);
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
        if(FAILED(hr=CreateDXGIFactory2(factoryFlags,IID_PPV_ARGS(&factory)))){error=HrText(L"DXGI factory",hr);return false;}

        // A mascot should stay off the discrete GPU when an integrated one exists.
        ComPtr<IDXGIAdapter1> adapter;
        for(UINT i=0;factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_MINIMUM_POWER,IID_PPV_ARGS(&adapter))!=DXGI_ERROR_NOT_FOUND;++i)
        {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE){adapter.Reset();continue;}
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))){adapterName=desc.Description;break;}
            adapter.Reset();
        }
        if(!device){error=L"D3D12 に対応した GPU が見つかりません。";return false;}

        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(hr=device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)))){error=HrText(L"Command queue",hr);return false;}

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width=width;sc.Height=height;
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

        D3D12_DESCRIPTOR_HEAP_DESC rtv{};
        rtv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rtv.NumDescriptors=FrameCount+1;
        D3D12_DESCRIPTOR_HEAP_DESC dsv{};
        dsv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dsv.NumDescriptors=1;
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
        commandList->Close();
        if(FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)))){error=HrText(L"Fence",hr);return false;}
        fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);

        if(!CreateRenderTargets(error)||!CreatePipelines(error))return false;

        // 1x1 white stands in for every missing texture.
        ImageRgba8 image;
        image.levels.push_back({1,1,{255,255,255,255}});
        BeginUploads();
        const bool ok=UploadTexture(image,white);
        FinishUploads();
        if(!ok){error=L"既定テクスチャの作成に失敗しました。";return false;}
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
        desc.Width=width;desc.Height=height;desc.DepthOrArraySize=1;desc.MipLevels=1;
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
        D3D12_ROOT_PARAMETER params[4]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[1].Descriptor.ShaderRegister=1;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;params[2].Descriptor.ShaderRegister=0;
        params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        params[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges=1;params[3].DescriptorTable.pDescriptorRanges=&range;
        params[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{};
        for(int i=0;i<2;++i)
        {
            auto& s=samplers[i];
            s.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU=s.AddressV=s.AddressW=i==0?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s.MaxLOD=D3D12_FLOAT32_MAX;
            s.ShaderRegister=i;
            s.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rs{};
        rs.NumParameters=4;rs.pParameters=params;
        rs.NumStaticSamplers=2;rs.pStaticSamplers=samplers;
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
        pso.NumRenderTargets=1;
        pso.RTVFormats[0]=ColorFormat;
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

        // Particles: no vertex input, premultiplied output, depth-tested against the model.
        pso.InputLayout={nullptr,0};
        pso.VS={g_ParticleVS,sizeof(g_ParticleVS)};
        pso.PS={g_ParticlePS,sizeof(g_ParticlePS)};
        pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pso.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        blend.SrcBlend=D3D12_BLEND_ONE;
        if(FAILED(hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&particlePso)))){error=HrText(L"Particle PSO",hr);return false;}
        return true;
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
        for(size_t i=0;ok&&i<paths.size();++i)
        {
            if(!used[i])continue;
            ImageRgba8 image;
            if(LoadImageRgba8(paths[i],image,maxTextureSize))ok=UploadTexture(image,textures[i]);
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
        if(!ok){error=L"GPU リソースの作成に失敗しました。";return false;}

        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=static_cast<UINT>(std::max<size_t>(materialCount*3,3));
        heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        srvHeap.Reset();
        if(FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srvHeap)))){error=L"SRV ヒープの作成に失敗しました。";return false;}
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

        // Per-frame upload ring: scene CB, material CBs, bone matrices, morph-applied vertices.
        sceneOffset=0;
        materialOffset=Align256(sizeof(SceneConstants));
        boneOffset=Align256(materialOffset+materialCount*256);
        dynamicOffset=Align256(boneOffset+boneCount*64);
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
            {error=L"アップロードバッファの作成に失敗しました。";return false;}
        }
        return true;
    }

    bool MascotRenderer::Resize(unsigned w,unsigned h,std::wstring& error)
    {
        w=std::max(1u,w);h=std::max(1u,h);
        if(w==width&&h==height)return true;
        WaitIdle();
        for(auto& b:backBuffers)b.Reset();
        msaaTarget.Reset();
        depthTarget.Reset();
        width=w;height=h;
        HRESULT hr;
        if(FAILED(hr=swapChain->ResizeBuffers(FrameCount,width,height,ColorFormat,0))){error=HrText(L"ResizeBuffers",hr);return false;}
        for(auto& f:frames){f.hitPoint={-1,-1};f.fenceValue=0;}
        lastAlpha=-1;
        return CreateRenderTargets(error);
    }

    void MascotRenderer::SetCamera(FXMVECTOR eye,FXMVECTOR target,float fovY)
    {
        XMStoreFloat4x4(&view,XMMatrixLookAtLH(eye,target,XMVectorSet(0,1,0,0)));
        XMStoreFloat4x4(&projection,XMMatrixPerspectiveFovLH(fovY,static_cast<float>(width)/height,1.0f,1000.0f));
    }

    void MascotRenderer::SetModelTransform(FXMMATRIX world){XMStoreFloat4x4(&modelWorld,world);}

    void MascotRenderer::SetParticles(const std::vector<Particle>& list)
    {
        particles.assign(list.begin(),list.begin()+static_cast<std::ptrdiff_t>(std::min(list.size(),MaxParticles)));
    }

    POINT MascotRenderer::Project(const std::array<float,3>& world) const
    {
        const XMMATRIX vp=XMLoadFloat4x4(&view)*XMLoadFloat4x4(&projection);
        const XMVECTOR ndc=XMVector3TransformCoord(XMVectorSet(world[0],world[1],world[2],1),vp);
        return {static_cast<LONG>((XMVectorGetX(ndc)*0.5f+0.5f)*width),static_cast<LONG>((0.5f-XMVectorGetY(ndc)*0.5f)*height)};
    }

    int MascotRenderer::AlphaAt(POINT p)
    {
        const UINT index=swapChain?swapChain->GetCurrentBackBufferIndex():0;
        frames[index].hitPoint=p;
        return lastAlpha;
    }

    void MascotRenderer::Render(const MascotModel& model,float edgeScale)
    {
        if(!srvHeap||!frames[0].mapped)return;
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
        const XMMATRIX v=XMLoadFloat4x4(&view),p=XMLoadFloat4x4(&projection),w=XMLoadFloat4x4(&modelWorld);
        XMStoreFloat4x4(&scene.viewProj,XMMatrixTranspose(w*v*p));
        XMStoreFloat4x4(&scene.view,XMMatrixTranspose(w*v));
        XMStoreFloat4x4(&scene.particleViewProj,XMMatrixTranspose(v*p));
        // Camera right and up in world space: the first two columns of the (row-vector) view.
        scene.cameraRight={view._11,view._21,view._31,0};
        scene.cameraUp={view._12,view._22,view._32,0};
        scene.lightDir={-0.5f,-1.0f,0.5f};
        scene.lightColor={154/255.0f,154/255.0f,154/255.0f};
        scene.edgePixels=edgeScale*static_cast<float>(height)/600.0f;
        scene.viewportSize={static_cast<float>(width),static_cast<float>(height)};
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
            std::memcpy(f.mapped+materialOffset+i*256,&c,sizeof(c));
        }
        if(const void* bones=model.BoneMatrices())
            std::memcpy(f.mapped+boneOffset,bones,std::min(model.BoneCount(),boneCount)*64);
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
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{sampleCount>1?rtvStart.ptr+FrameCount*rtvStride:rtvStart.ptr+index*rtvStride};
        const auto dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();
        if(sampleCount==1)
        {
            auto b=Transition(backBuffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            commandList->ResourceBarrier(1,&b);
        }
        commandList->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
        commandList->ClearRenderTargetView(rtv,ClearColor,0,nullptr);
        commandList->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
        D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        commandList->RSSetViewports(1,&viewport);
        commandList->RSSetScissorRects(1,&scissor);
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

        if(sampleCount>1)
        {
            D3D12_RESOURCE_BARRIER b[2]={
                Transition(msaaTarget.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                Transition(backBuffer,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RESOLVE_DEST)};
            commandList->ResourceBarrier(2,b);
            commandList->ResolveSubresource(backBuffer,0,msaaTarget.Get(),0,ColorFormat);
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
        const POINT hit{std::clamp<LONG>(f.hitPoint.x,0,static_cast<LONG>(width)-1),std::clamp<LONG>(f.hitPoint.y,0,static_cast<LONG>(height)-1)};
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
        swapChain->Present(1,0);
    }

    bool MascotRenderer::SaveLastFrame(const std::wstring& path)
    {
        WaitIdle();
        ID3D12Resource* source=backBuffers[lastRenderedIndex].Get();
        const UINT pitch=(width*4+255)&~255u;
        auto heap=HeapProperties(D3D12_HEAP_TYPE_READBACK);
        auto desc=BufferDesc(static_cast<UINT64>(pitch)*height);
        ComPtr<ID3D12Resource> readback;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return false;
        BeginUploads();
        auto toCopy=Transition(source,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1,&toCopy);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource=readback.Get();
        dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={ColorFormat,width,height,1,pitch};
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource=source;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commandList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
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
            SUCCEEDED(frame->SetSize(width,height))&&SUCCEEDED(frame->SetPixelFormat(&format))&&
            SUCCEEDED(frame->WritePixels(height,pitch,pitch*height,data))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
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
