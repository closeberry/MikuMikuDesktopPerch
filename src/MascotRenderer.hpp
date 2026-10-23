#pragma once
#include "MascotModel.hpp"
#include "TextureLoader.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <array>
#include <string>
#include <vector>

namespace DesktopMascot
{
    // D3D12 renderer that draws one PMX model into a premultiplied-alpha
    // DirectComposition swap chain, so the window is per-pixel transparent.
    class MascotRenderer
    {
    public:
        // A camera-facing sparkle drawn over the model (warp effect). World units.
        struct Particle
        {
            std::array<float,3> position{};
            float size=0;                    // half the quad's side
            std::array<float,4> color{};     // straight alpha
            float angle=0;
        };
        static constexpr size_t MaxParticles=512;

        ~MascotRenderer();
        bool Initialize(HWND hwnd,unsigned width,unsigned height,int msaa,std::wstring& error);
        bool LoadModel(const MascotModel& model,const std::wstring& toonDir,unsigned maxTextureSize,std::wstring& error);
        // Recreate the swap chain buffers and render targets at a new size (call SetCamera afterwards).
        bool Resize(unsigned width,unsigned height,std::wstring& error);
        void SetCamera(DirectX::FXMVECTOR eye,DirectX::FXMVECTOR target,float fovY);
        // Display-only transform of the drawn model (identity normally); Project ignores it.
        void SetModelTransform(DirectX::FXMMATRIX world);
        void SetParticles(const std::vector<Particle>& list);
        // Shift the drawn frame by a fraction of a pixel (the window can only sit on whole
        // pixels); Project ignores it.
        void SetPixelOffset(float dx,float dy){pixelOffset={dx,dy};}
        // Where the frame sits inside the window, in whole pixels. Applied together with the next
        // Present, so the frame and its position change in the same composition.
        void SetVisualOffset(int x,int y);
        // World position to client-area pixels (y down).
        POINT Project(const std::array<float,3>& world) const;
        void Render(const MascotModel& model,float edgeScale);
        void WaitIdle();
        const std::wstring& AdapterName() const {return adapterName;}

        // Alpha (0-255) of the pixel under clientPoint, sampled a couple of frames ago.
        // Returns -1 while no sample is available.
        int AlphaAt(POINT clientPoint);

        // Diagnostics: read back the frame just rendered and save it as a PNG (premultiplied BGRA).
        bool SaveLastFrame(const std::wstring& path);

    private:
        static constexpr UINT FrameCount=2;
        struct Texture
        {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            bool valid=false;
        };
        struct Frame
        {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12Resource> upload;   // persistently mapped: CBs, bones, dynamic VB
            std::uint8_t* mapped=nullptr;
            Microsoft::WRL::ComPtr<ID3D12Resource> readback; // 1 pixel for click-through hit testing
            UINT64 fenceValue=0;
            std::uint64_t dynamicRevision=0;
            POINT hitPoint{-1,-1};
        };

        bool CreatePipelines(std::wstring& error);
        bool CreateRenderTargets(std::wstring& error);
        bool UploadBuffer(const void* data,size_t size,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,D3D12_RESOURCE_STATES finalState);
        bool UploadTexture(const ImageRgba8& image,Texture& texture);
        void BeginUploads();
        void FinishUploads();
        void WaitForFence(UINT64 value);

        HWND hwnd=nullptr;
        unsigned width=0,height=0;
        UINT sampleCount=1;
        std::wstring adapterName;

        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain;
        Microsoft::WRL::ComPtr<IDCompositionDevice> composition;
        Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget;
        Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent=nullptr;
        UINT64 nextFenceValue=1;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
        std::array<Frame,FrameCount> frames;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,FrameCount> backBuffers;
        Microsoft::WRL::ComPtr<ID3D12Resource> msaaTarget,depthTarget;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap,dsvHeap,srvHeap;
        UINT rtvStride=0,srvStride=0;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> modelCullPso,modelDoubleSidedPso,edgePso,particlePso;

        // Upload batching for load-time resources.
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAllocator;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingUploads;

        // Model resources.
        Microsoft::WRL::ComPtr<ID3D12Resource> staticVertexBuffer,indexBuffer;
        std::vector<Texture> textures;
        std::array<Texture,10> sharedToons;
        Texture white;
        size_t boneCount=0,vertexCount=0,materialCount=0;
        size_t sceneOffset=0,materialOffset=0,boneOffset=0,dynamicOffset=0,particleOffset=0,uploadSize=0;

        DirectX::XMFLOAT4X4 view{},projection{};
        DirectX::XMFLOAT4X4 modelWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::vector<Particle> particles;
        DirectX::XMFLOAT2 pixelOffset{};
        POINT visualOffset{},appliedVisualOffset{};
        int lastAlpha=-1;
        UINT lastRenderedIndex=0;
    };
}
