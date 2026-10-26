#pragma once
#include "MascotModel.hpp"
#include "TextureLoader.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace MikuMikuDesktopPerch
{
    // D3D12 renderer that draws one PMX model into a premultiplied-alpha
    // DirectComposition swap chain, so the window is per-pixel transparent. One renderer per
    // window; the renderers of a process share the GPU device.
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
        // motionBlur: smear what moves along its motion since the last frame (a pass after the
        // model; see PostPS.hlsl), so low frame rates look less choppy.
        bool Initialize(HWND hwnd,unsigned width,unsigned height,int msaa,bool motionBlur,std::wstring& error);
        // The drawing rate: the smear is a share of one frame's worth of motion at this rate.
        void SetFrameRate(int fps){framePeriod=1.0/std::max(fps,1);}
        // Draw at this share of the window's pixels (at most 1) and scale the frame up to the
        // window in a last pass: very large sizes cost little more than the size this
        // resolution is capped at. Takes effect with the next Resize (or Initialize). Sizes and
        // points given to and returned from the renderer stay in window pixels.
        void SetResolutionScale(float scale){resolutionScale=std::clamp(scale,0.1f,1.0f);}
        bool LoadModel(const MascotModel& model,const std::wstring& toonDir,unsigned maxTextureSize,std::wstring& error);
        // Change the drawn size (call SetCamera afterwards). The buffers keep some headroom and
        // are only reallocated when the size outgrows them: a reallocation can show for a frame
        // out of step with the frame's position, a smaller drawn area never does.
        bool Resize(unsigned width,unsigned height,std::wstring& error);
        void SetCamera(DirectX::FXMVECTOR eye,DirectX::FXMVECTOR target,float fovY);
        // MMD's self-shadow: the model shades itself from the light (a shadow map pass).
        void SetSelfShadow(bool on){selfShadow=on;}
        // Where the light comes from, in degrees: around (0 in front, + to the viewer's
        // right) and up from the horizon.
        void SetLight(float azimuth,float elevation);
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
        UINT SampleCount() const {return sampleCount;}   // MSAA samples actually drawn with (1: none)
        // The GPUs that can draw (the software one left out), as (id, name). The id is the
        // vendor and device ("10DE:249D"): it stays the same when the adapters' order changes.
        static std::vector<std::pair<std::wstring,std::wstring>> Gpus();
        // Draw on the GPU with this id; empty, or one not found: the one Windows picks for this
        // app (its graphics settings decide). Takes effect when the shared device is made.
        static void PreferGpu(std::wstring id){preferredGpu=std::move(id);}
        // Diagnostics: act as if on an old Intel driver ("old-driver") or a GPU that can only
        // resolve whole MSAA targets ("whole-resolve"). Before the shared device is made.
        static void SimulateQuirk(const std::wstring& quirk){simulatedQuirks+=quirk+L";";}
        // Why the GPU stopped taking work (TDR, a driver update or crash), S_OK while it runs.
        // Once set, nothing more is drawn: the device would have to be made again.
        HRESULT DeviceLost() const {return deviceLost;}
        // An Intel GPU with a driver too old to trust with MSAA (see SharedGpu): its id and
        // driver version ("8086:0412 20.19.15.4531"), empty otherwise. Anti-aliasing is off then.
        const std::wstring& OldDriver() const {return gpu?gpu->oldDriver:none;}

        // Alpha (0-255) of the pixel under clientPoint, sampled a couple of frames ago.
        // Returns -1 while no sample is available.
        int AlphaAt(POINT clientPoint);

        // Diagnostics: read back the frame just rendered and save it as a PNG (premultiplied BGRA).
        bool SaveLastFrame(const std::wstring& path);
        // The frame just rendered, read back: premultiplied BGRA rows, width*4 bytes each.
        bool ReadLastFrame(std::vector<std::uint8_t>& pixels,UINT& width,UINT& height);

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
        void WritePassDescriptors();
        bool CreateRenderTargets(std::wstring& error);
        bool UploadBuffer(const void* data,size_t size,Microsoft::WRL::ComPtr<ID3D12Resource>& buffer,D3D12_RESOURCE_STATES finalState);
        bool UploadTexture(const ImageRgba8& image,Texture& texture);
        void BeginUploads();
        void FinishUploads();
        void WaitForFence(UINT64 value);

        HWND hwnd=nullptr;
        unsigned width=0,height=0;                 // drawn area, at the top left of the buffers
        unsigned windowWidth=0,windowHeight=0;     // the window's size (the back buffer's drawn area)
        float resolutionScale=1;
        unsigned bufferWidth=0,bufferHeight=0;     // allocated size of the drawing targets (>= width, height)
        unsigned swapWidth=0,swapHeight=0;         // allocated size of the back buffers (>= windowWidth, windowHeight)
        std::array<bool,FrameCount> clearWhole{};  // back buffer still holds a larger frame
        UINT sampleCount=1;
        std::wstring adapterName;
        HRESULT deviceLost=S_OK;

        // The GPU device, shared by every renderer in the process (one per character): each
        // character's own device would hold its own copy of the driver's state.
        struct Gpu
        {
            Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            std::wstring adapterName;
            std::wstring oldDriver;
            bool regionResolve=true;   // ResolveSubresourceRegion can be used (else ResolveSubresource)
        };
        static inline const std::wstring none;
        static std::shared_ptr<Gpu> SharedGpu(std::wstring& error);
        static inline std::wstring preferredGpu,simulatedQuirks;
        std::shared_ptr<Gpu> gpu;
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
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList1> commandList1;   // for ResolveSubresourceRegion
        std::array<Frame,FrameCount> frames;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,FrameCount> backBuffers;
        Microsoft::WRL::ComPtr<ID3D12Resource> msaaTarget,depthTarget;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap,dsvHeap,srvHeap;
        UINT rtvStride=0,srvStride=0;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> modelCullPso,modelDoubleSidedPso,edgePso,particlePso,shadowPso,blurPso,upscalePso;
        // Self-shadow map (depth from the light), sampled by the model shader.
        static constexpr UINT ShadowMapSize=2048;
        Microsoft::WRL::ComPtr<ID3D12Resource> shadowMap;
        bool selfShadow=true;
        DirectX::XMFLOAT3 lightDir{-0.5f,-1.0f,0.5f};   // the way the light travels (MMD's default)

        // With motion blur or a scaled-up frame, the model is drawn into sceneColor (through
        // msaaTarget with MSAA), with its screen motion in sceneMotion (msaaMotion) for the blur,
        // then smeared and/or scaled up into the back buffer (blurredColor holds the smeared
        // frame when both). The motion is measured against last frame's camera, bones and place
        // on screen.
        static constexpr DXGI_FORMAT MotionFormat=DXGI_FORMAT_R16G16_FLOAT;
        bool motionBlur=false;
        Microsoft::WRL::ComPtr<ID3D12Resource> sceneColor,sceneMotion,msaaMotion,blurredColor;
        DirectX::XMFLOAT2 lastScale{1,1};   // drawn pixels per window pixel last frame
        bool history=false;   // last frame's state below is usable
        DirectX::XMFLOAT4X4 lastViewProj{};
        DirectX::XMFLOAT2 lastViewport{};
        DirectX::XMFLOAT2 lastScreenOffset{};
        std::vector<std::uint8_t> lastBones;
        double framePeriod=1/30.0;
        LARGE_INTEGER lastRenderTime{};

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
        DirectX::XMFLOAT3 cameraEye{},cameraTarget{};
        float cameraFovY=0.5f;
        DirectX::XMFLOAT4X4 modelWorld{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::vector<Particle> particles;
        DirectX::XMFLOAT2 pixelOffset{};
        POINT visualOffset{},appliedVisualOffset{};
        int lastAlpha=-1;
        UINT lastRenderedIndex=0;
    };
}
