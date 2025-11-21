//***************************************************************************************
// SsaoApp.cpp by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "../../Common/Camera.h"
#include "FrameResource.h"
#include "ShadowMap.h"
#include "Ssao.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

// ------------------------------------------------------------
// Конвертирует SRGB формат в обычный UNORM
static DXGI_FORMAT ToNonSRGB(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return fmt;
    }
}

// ------------------------------------------------------------
// Инициализация/обновление TAA истории при первом кадре
static void InitOrRefreshTaaHistoryIfNeeded(
    ID3D12GraphicsCommandList* cmd,
    ID3D12Resource* srcBackbuffer,
    ID3D12Resource* dstHistory,
    ID3D12Resource* tmpCurrCopy,
    DXGI_FORMAT backbufferFormat)
{
    CD3DX12_RESOURCE_BARRIER pre[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(srcBackbuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(dstHistory,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        CD3DX12_RESOURCE_BARRIER::Transition(tmpCurrCopy,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    cmd->ResourceBarrier(_countof(pre), pre);

    cmd->CopyResource(dstHistory, srcBackbuffer);
    cmd->CopyResource(tmpCurrCopy, srcBackbuffer);

    CD3DX12_RESOURCE_BARRIER post[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(srcBackbuffer,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(dstHistory,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(tmpCurrCopy,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(_countof(post), post);
}

static float Halton(int index, int base)
{
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= base; r += f * (index % base); index /= base; }
    return r;
}


using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;


// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    RenderItem() = default;
    RenderItem(const RenderItem& rhs) = delete;

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

    // Dirty flag indicating the object data has changed and we need to update the constant buffer.
    // Because we have an object cbuffer for each FrameResource, we have to apply the
    // update to each FrameResource.  Thus, when we modify obect data we should set 
    // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
    int NumFramesDirty = gNumFrameResources;

    // Index into GPU constant buffer corresponding to the ObjectCB for this render item.
    UINT ObjCBIndex = -1;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

enum class RenderLayer : int
{
    Opaque = 0,
    Debug,
    Sky,
    Count
};

class SsaoApp : public D3DApp
{
public:
    SsaoApp(HINSTANCE hInstance);
    SsaoApp(const SsaoApp& rhs) = delete;
    SsaoApp& operator=(const SsaoApp& rhs) = delete;
    ~SsaoApp();

    virtual bool Initialize()override;
    float mTaaJitterPixels = 0.5f;

private:
    virtual void CreateRtvAndDsvDescriptorHeaps()override;
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMaterialBuffer(const GameTimer& gt);
    void UpdateShadowTransform(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateShadowPassCB(const GameTimer& gt);
    void UpdateSsaoCB(const GameTimer& gt);

    void LoadTextures();
    void BuildRootSignature();
    void BuildSsaoRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
    void BuildSkullGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void DrawSceneToShadowMap();
    void DrawNormalsAndDepth();

    CD3DX12_CPU_DESCRIPTOR_HANDLE GetCpuSrv(int index)const;
    CD3DX12_GPU_DESCRIPTOR_HANDLE GetGpuSrv(int index)const;
    CD3DX12_CPU_DESCRIPTOR_HANDLE GetDsv(int index)const;
    CD3DX12_CPU_DESCRIPTOR_HANDLE GetRtv(int index)const;

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GetStaticSamplers();
    DirectX::XMFLOAT4X4 mJitteredProj = MathHelper::Identity4x4();
    void ComputeTaaJitter();      // ??????? Halton, ?????????? mJitteredProj
    void TaaResolvePass();        // ????????? ?????? TAA (fullscreen)

    void CreateTaaResources();
    void AnimateSkull(const GameTimer& gt);
    void UpdateDebugWindowTitle(const GameTimer& gt);
private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12RootSignature> mSsaoRootSignature = nullptr;

    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mRitemLayer[(int)RenderLayer::Count];

    UINT mSkyTexHeapIndex = 0;
    UINT mShadowMapHeapIndex = 0;
    UINT mSsaoHeapIndexStart = 0;
    UINT mSsaoAmbientMapIndex = 0;

    UINT mNullCubeSrvIndex = 0;
    UINT mNullTexSrvIndex1 = 0;
    UINT mNullTexSrvIndex2 = 0;

    CD3DX12_GPU_DESCRIPTOR_HANDLE mNullSrv;

    PassConstants mMainPassCB;  // index 0 of pass cbuffer.
    PassConstants mShadowPassCB;// index 1 of pass cbuffer.

    Camera mCamera;

    std::unique_ptr<ShadowMap> mShadowMap;

    std::unique_ptr<Ssao> mSsao;

    DirectX::BoundingSphere mSceneBounds;

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    XMFLOAT3 mLightPosW;
    XMFLOAT4X4 mLightView = MathHelper::Identity4x4();
    XMFLOAT4X4 mLightProj = MathHelper::Identity4x4();
    XMFLOAT4X4 mShadowTransform = MathHelper::Identity4x4();

    float mLightRotationAngle = 0.0f;
    XMFLOAT3 mBaseLightDirections[3] = {
        XMFLOAT3(0.57735f, -0.57735f, 0.57735f),
        XMFLOAT3(-0.57735f, -0.57735f, 0.57735f),
        XMFLOAT3(0.0f, -0.707f, -0.707f)
    };
    XMFLOAT3 mRotatedLightDirections[3];

    POINT mLastMousePos;
    // TAA BEGIN: history resources + PSO
    Microsoft::WRL::ComPtr<ID3D12Resource> mTaaHistoryA;
    Microsoft::WRL::ComPtr<ID3D12Resource> mTaaHistoryB;
    bool mUseHistoryA = true;

    CD3DX12_CPU_DESCRIPTOR_HANDLE mTaaRTV[2];
    CD3DX12_GPU_DESCRIPTOR_HANDLE mTaaSRV[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mCurrColorCopy;
    ComPtr<ID3D12PipelineState> mTaaPSO;
    ComPtr<ID3DBlob> mTaaVS;
    ComPtr<ID3DBlob> mTaaPS;
    // TAA END
    UINT mTaaHeapIndexStart = 0; // t0..t2 ??? TAA
    bool mTaaEnabled = true;
    int  mTaaViewMode = 0; // 0..3
    UINT mTaaHeapIndexStartB = 0;
    // === Debug overlay ===

    RenderItem* mSkullRitem = nullptr;                 // указатель на skull RenderItem
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mTaaPSOStencil; // PSO для TAA со stencil==1
    float mSkullMoveTime = 0.0f;
    bool mTaaHistoryValid = false;
    bool mHasPrevViewProj = false;
    bool mShadowsEnabled = true;
    int  mShadowMode = 0; // 0 = Luna PCF, 1 = мягкие тени
    float mAtmosphereGlobalDensity = 0.035f; // было 0.02
    float mAtmosphereHeightFalloff = 0.06f;  // туман тянется выше
    float mAtmosphereCleanliness = 0.35f;  // стартуем с лёгкого смога
    float mAtmosphereIntensity = 2.0f;   // эффект заметнее
};



struct TaaState
{
    int HaltonIndex = 1;
    DirectX::XMFLOAT2 Jitter = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 PrevJitter = { 0.0f, 0.0f };
} gTaa;
// TAA END



int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        SsaoApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

SsaoApp::SsaoApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    // Estimate the scene bounding sphere manually since we know how the scene was constructed.
    // The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
    // the world space origin.  In general, you need to loop over every world space vertex
    // position and compute the bounding sphere.
    mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
    mSceneBounds.Radius = sqrtf(10.0f * 10.0f + 15.0f * 15.0f);
}

SsaoApp::~SsaoApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

bool SsaoApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCamera.SetPosition(0.0f, 2.0f, -15.0f);

    mShadowMap = std::make_unique<ShadowMap>(md3dDevice.Get(),
        2048, 2048);

    mSsao = std::make_unique<Ssao>(
        md3dDevice.Get(),
        mCommandList.Get(),
        mClientWidth, mClientHeight);

    LoadTextures();
    BuildRootSignature();
    BuildSsaoRootSignature();
    BuildDescriptorHeaps();
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
    BuildSkullGeometry();
    BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();
    BuildPSOs();

    mSsao->SetPSOs(mPSOs["ssao"].Get(), mPSOs["ssaoBlur"].Get());

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    return true;
}

void SsaoApp::CreateRtvAndDsvDescriptorHeaps()
{
    // Add +1 for screen normal map, +2 for ambient maps.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
    rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

    // Add +1 DSV for shadow map.
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
    dsvHeapDesc.NumDescriptors = 2;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
        &dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void SsaoApp::OnResize()
{
    D3DApp::OnResize();

    mCamera.SetLens(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);

    if (mSsao != nullptr)
    {
        mSsao->OnResize(mClientWidth, mClientHeight);
        mSsao->RebuildDescriptors(mDepthStencilBuffer.Get());
    }

    // === TAA: (re)create history A/B и копию текущего цвета ===
    CreateTaaResources();
    mTaaHistoryValid = false;
}


void SsaoApp::AnimateSkull(const GameTimer& gt)
{
    if (!mSkullRitem)
        return;

    mSkullMoveTime += gt.DeltaTime();

    // синусоидальное смещение по X
    const float amplitude = 2.5f;   // дальность
    const float speedHz = 0.35f;  // скорость
    const float offsetX = amplitude * sinf(2.0f * XM_PI * speedHz * mSkullMoveTime);

    XMMATRIX world =
        XMMatrixScaling(0.4f, 0.4f, 0.4f) *
        XMMatrixTranslation(offsetX, 1.0f, 0.0f);

    XMStoreFloat4x4(&mSkullRitem->World, world);
    mSkullRitem->NumFramesDirty = gNumFrameResources;
}


void SsaoApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);

    // === Frame resource sync ===
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();
    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    // === Light animation ===
    mLightRotationAngle += 0.1f * gt.DeltaTime();
    XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
    for (int i = 0; i < 3; ++i)
    {
        XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
        lightDir = XMVector3TransformNormal(lightDir, R);
        XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
    }

    // анимация skull (реальное движение)
    //AnimateSkull(gt);

    AnimateMaterials(gt);
    UpdateObjectCBs(gt);
    UpdateMaterialBuffer(gt);
    UpdateShadowTransform(gt);

    // === TAA jitter ===
    ComputeTaaJitter();

    // === Остальные апдейты ===
    UpdateMainPassCB(gt);
    UpdateShadowPassCB(gt);
    UpdateSsaoCB(gt);
    UpdateDebugWindowTitle(gt);
}


void SsaoApp::UpdateDebugWindowTitle(const GameTimer& gt)
{
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // TAA: ON/OFF + режим
    oss << L"TAA: " << (mTaaEnabled ? L"ON" : L"OFF");
    oss << L"  |  Mode: " << mTaaViewMode;
    oss << L"  |  Feedback: " << mMainPassCB.TaaFeedback;
    oss << L"  |  JitterPx: " << mTaaJitterPixels;

    // Атмосфера
    oss << L"  ||  Atmosphere  Cleanliness: " << mAtmosphereCleanliness;
    oss << L", Density: " << mAtmosphereGlobalDensity;
    oss << L", Intensity: " << mAtmosphereIntensity;

    // Тени
    oss << L"  ||  Shadows: "
        << (mShadowMode == 0 ? L"Luna PCF" : L"Soft pseudo RT");

    // Подсказка по кнопкам
    oss << L"  ||  [T: TAA] [Y: TAA view] [H: shadows] [Z/X: atmosphere]";

    SetWindowText(mhMainWnd, oss.str().c_str());
}




void SsaoApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
    ThrowIfFailed(cmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    //
    // Shadow map pass.
    //
    auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());
    mCommandList->SetGraphicsRootDescriptorTable(3, mNullSrv);
    mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    DrawSceneToShadowMap();

    //
    // Normal/depth pre-pass.
    //
    DrawNormalsAndDepth(); // тут мы уже чистим ТОЛЬКО stencil, depth сохраняем

    //
    // SSAO.
    //
    mCommandList->SetGraphicsRootSignature(mSsaoRootSignature.Get());
    mSsao->ComputeSsao(mCommandList.Get(), mCurrFrameResource, 3);

    //
    // Main pass -> BackBuffer.
    //
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());
    matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
    mCommandList->SetGraphicsRootShaderResourceView(2, matBuffer->GetGPUVirtualAddress());

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // backbuffer: PRESENT -> RENDER_TARGET
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // clear color. ВАЖНО: depth здесь НЕ чистим!
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);
    // НЕ вызывать ClearDepthStencilView с флагом DEPTH в main-pass.
    // Глубина из pre-pass должна сохраниться для теста EQUAL.

    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    // таблица текстур сцены
    mCommandList->SetGraphicsRootDescriptorTable(4, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

    CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(3, skyTexDescriptor);

    // --- Все opaque КРОМЕ skull: рисуем по pre-pass глубине (DepthFunc = EQUAL) ---
    mCommandList->SetPipelineState(mPSOs["opaque"].Get());
    {
        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        auto objectCB = mCurrFrameResource->ObjectCB->Resource();
        for (auto* ri : mRitemLayer[(int)RenderLayer::Opaque])
        {
            if (ri == mSkullRitem) continue;

            mCommandList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
            mCommandList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
            mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);

            D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
                objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

            mCommandList->SetGraphicsRootConstantBufferView(0, objCBAddress);
            mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        }
    }

    // --- skull: рисуем c записью stencil = 1, depth не пишем (оставляем как есть) ---
    // --- OPAQUE: НЕ рисуем ничего, КРОМЕ skull ---
    if (mSkullRitem)
    {
        // Сначала обычный цвет для skull? Вы у себя рисуете skull в PSO с записью stencil.
        // Этого достаточно (цвет+стенсил), depth не пишем — используем pre-pass.
        mCommandList->SetPipelineState(mPSOs["opaque_stencilWrite"].Get());
        mCommandList->OMSetStencilRef(1);

        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        auto objectCB = mCurrFrameResource->ObjectCB->Resource();
        auto* ri = mSkullRitem;

        mCommandList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        mCommandList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
            objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        mCommandList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }


    // sky (DepthFunc = LESS_EQUAL) — теперь не перекроет уже отрисованные объекты
    mCommandList->SetPipelineState(mPSOs["sky"].Get());
    DrawRenderItems(mCommandList.Get(), mRitemLayer[(int)RenderLayer::Sky]);

    //
    // TAA resolve (только по stencil == 1)
    //
    if (mTaaEnabled)
        TaaResolvePass();

    // backbuffer: RENDER_TARGET -> PRESENT
    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}





void SsaoApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void SsaoApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void SsaoApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        // Make each pixel correspond to a quarter of a degree.
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

        mCamera.Pitch(dy);
        mCamera.RotateY(dx);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void SsaoApp::OnKeyboardInput(const GameTimer& gt)
{
    const float dt = gt.DeltaTime();

    if (GetAsyncKeyState('W') & 0x8000) mCamera.Walk(10.0f * dt);
    if (GetAsyncKeyState('S') & 0x8000) mCamera.Walk(-10.0f * dt);
    if (GetAsyncKeyState('A') & 0x8000) mCamera.Strafe(-10.0f * dt);
    if (GetAsyncKeyState('D') & 0x8000) mCamera.Strafe(10.0f * dt);

    // === TAA debug controls ===
    if (GetAsyncKeyState('T') & 0x1)  mTaaEnabled = !mTaaEnabled;                   // ON/OFF
    if (GetAsyncKeyState('U') & 0x1)  mMainPassCB.TaaFeedback = min(0.99f, mMainPassCB.TaaFeedback + 0.05f); // +??? ???????
    if (GetAsyncKeyState('J') & 0x1)  mMainPassCB.TaaFeedback = max(0.0f, mMainPassCB.TaaFeedback - 0.05f); // -??? ???????

    // ????????? ???????? (? ????????)
    if (GetAsyncKeyState('I') & 0x1)  mTaaJitterPixels = min(8.0f, mTaaJitterPixels + 0.5f);
    if (GetAsyncKeyState('K') & 0x1)  mTaaJitterPixels = max(0.0f, mTaaJitterPixels - 0.5f);
    if (GetAsyncKeyState('Y') & 0x1)
        mTaaViewMode = (mTaaViewMode + 1) % 5; // 0..4 (4=DebugSkull)
    if (GetAsyncKeyState('H') & 0x1)
        mShadowMode = 1 - mShadowMode;  // 0 <-> 1
    // Чистота атмосферы: X = грязнее, Z = чище
    if (GetAsyncKeyState('Z') & 0x8000)
        mAtmosphereCleanliness = min(1.0f, mAtmosphereCleanliness + 0.5f * dt);

    if (GetAsyncKeyState('X') & 0x8000)
        mAtmosphereCleanliness = max(0.0f, mAtmosphereCleanliness - 0.5f * dt);
    mCamera.UpdateViewMatrix();
}


void SsaoApp::AnimateMaterials(const GameTimer& gt)
{

}

void SsaoApp::UpdateObjectCBs(const GameTimer& gt)
{
    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        // Only update the cbuffer data if the constants have changed.  
        // This needs to be tracked per frame resource.
        if (e->NumFramesDirty > 0)
        {
            XMMATRIX world = XMLoadFloat4x4(&e->World);
            XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
            objConstants.MaterialIndex = e->Mat->MatCBIndex;

            currObjectCB->CopyData(e->ObjCBIndex, objConstants);

            // Next FrameResource need to be updated too.
            e->NumFramesDirty--;
        }
    }
}

void SsaoApp::UpdateMaterialBuffer(const GameTimer& gt)
{
    auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
    for (auto& e : mMaterials)
    {
        // Only update the cbuffer data if the constants have changed.  If the cbuffer
        // data changes, it needs to be updated for each FrameResource.
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialData matData;
            matData.DiffuseAlbedo = mat->DiffuseAlbedo;
            matData.FresnelR0 = mat->FresnelR0;
            matData.Roughness = mat->Roughness;
            XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));
            matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
            matData.NormalMapIndex = mat->NormalSrvHeapIndex;

            currMaterialBuffer->CopyData(mat->MatCBIndex, matData);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }
}

void SsaoApp::UpdateShadowTransform(const GameTimer& gt)
{
    // Only the first "main" light casts a shadow.
    XMVECTOR lightDir = XMLoadFloat3(&mRotatedLightDirections[0]);
    XMVECTOR lightPos = -2.0f * mSceneBounds.Radius * lightDir;
    XMVECTOR targetPos = XMLoadFloat3(&mSceneBounds.Center);
    XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);

    XMStoreFloat3(&mLightPosW, lightPos);

    // Transform bounding sphere to light space.
    XMFLOAT3 sphereCenterLS;
    XMStoreFloat3(&sphereCenterLS, XMVector3TransformCoord(targetPos, lightView));

    // Ortho frustum in light space encloses scene.
    float l = sphereCenterLS.x - mSceneBounds.Radius;
    float b = sphereCenterLS.y - mSceneBounds.Radius;
    float n = sphereCenterLS.z - mSceneBounds.Radius;
    float r = sphereCenterLS.x + mSceneBounds.Radius;
    float t = sphereCenterLS.y + mSceneBounds.Radius;
    float f = sphereCenterLS.z + mSceneBounds.Radius;

    mLightNearZ = n;
    mLightFarZ = f;
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(l, r, b, t, n, f);

    // Transform NDC space [-1,+1]^2 to texture space [0,1]^2
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    XMMATRIX S = lightView * lightProj * T;
    XMStoreFloat4x4(&mLightView, lightView);
    XMStoreFloat4x4(&mLightProj, lightProj);
    XMStoreFloat4x4(&mShadowTransform, S);
}

void SsaoApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = mCamera.GetView();
    XMMATRIX proj = XMLoadFloat4x4(&mJitteredProj);   // джиттерная проекция
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);
    XMMATRIX shadowTransform = XMLoadFloat4x4(&mShadowTransform);

    // === prev ViewProj для TAA ===
    static XMFLOAT4X4 sPrevViewProj = MathHelper::Identity4x4();
    if (!mHasPrevViewProj)
    {
        XMStoreFloat4x4(&sPrevViewProj, XMMatrixTranspose(viewProj)); // первый кадр: prev = current
        mHasPrevViewProj = true;
    }
    mMainPassCB.PrevViewProj = sPrevViewProj;

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));
    XMStoreFloat4x4(&mMainPassCB.ShadowTransform, XMMatrixTranspose(shadowTransform));

    mMainPassCB.EyePosW = mCamera.GetPosition3f();

    // размер рендера
    mMainPassCB.RenderTargetSize =
        XMFLOAT2(static_cast<float>(mClientWidth), static_cast<float>(mClientHeight));

    XMFLOAT2 invRT(
        1.0f / max(1, mClientWidth),
        1.0f / max(1, mClientHeight));

    mMainPassCB.InvRenderTargetSize = invRT;
    mMainPassCB.InvRT = invRT;   // TAA-специфичные копии
    mMainPassCB.InvRT_dup = invRT;

    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 1000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    mMainPassCB.AmbientLight = { 0.7f, 0.7f, 0.7f, 1.0f }; // чуть ярче, на вкус
    mMainPassCB.Lights[0].Direction = mRotatedLightDirections[0];
    mMainPassCB.Lights[0].Strength = { 1.0f, 1.0f, 1.0f };
    mMainPassCB.Lights[1].Direction = mRotatedLightDirections[1];
    mMainPassCB.Lights[1].Strength = { 0.0f, 0.0f, 0.0f };
    mMainPassCB.Lights[2].Direction = mRotatedLightDirections[2];
    mMainPassCB.Lights[2].Strength = { 0.0f, 0.0f, 0.0f };


    // === TAA поля ===
    mMainPassCB.Jitter = gTaa.Jitter;
    mMainPassCB.PrevJitter = gTaa.PrevJitter;

    mMainPassCB.TaaMode = mTaaViewMode;
    mMainPassCB.TaaEnabledInt = mTaaEnabled ? 1 : 0;

    // дефолт для feedback, если вдруг мусор
    if (mMainPassCB.TaaFeedback <= 0.0f || mMainPassCB.TaaFeedback > 0.99f)
        mMainPassCB.TaaFeedback = 0.9f;

    mMainPassCB.TaaFeedback =
        MathHelper::Clamp(mMainPassCB.TaaFeedback, 0.0f, 0.99f);

    mMainPassCB.TaaDepthThresh = 0.001f;
    // НОВОЕ: режим теней в constant buffer
    mMainPassCB.ShadowMode = mShadowMode;
    // === НОВОЕ: атмосфера ===
    mMainPassCB.AtmosphereGlobalDensity = mAtmosphereGlobalDensity;
    mMainPassCB.AtmosphereHeightFalloff = mAtmosphereHeightFalloff;
    mMainPassCB.AtmosphereCleanliness = mAtmosphereCleanliness;
    mMainPassCB.AtmosphereIntensity = mAtmosphereIntensity;

    auto currPassCB = mCurrFrameResource->PassCB.get();

    // === debug-данные для skull ===
    if (mSkullRitem)
    {
        const auto& skullSub = mGeometries["skullGeo"]->DrawArgs["skull"];
        const XMFLOAT3 cLS = skullSub.Bounds.Center;
        const XMFLOAT3 eLS = skullSub.Bounds.Extents;

        XMMATRIX W = XMLoadFloat4x4(&mSkullRitem->World);

        XMVECTOR cWSv = XMVector3TransformCoord(XMLoadFloat3(&cLS), W);
        XMStoreFloat3(&mMainPassCB.SkullCenterWS, cWSv);

        float sx = XMVectorGetX(XMVector3Length(W.r[0]));
        float sy = XMVectorGetX(XMVector3Length(W.r[1]));
        float sz = XMVectorGetX(XMVector3Length(W.r[2]));
        XMFLOAT3 eWS = { eLS.x * sx, eLS.y * sy, eLS.z * sz };
        mMainPassCB.SkullRadius =
            std::sqrt(eWS.x * eWS.x + eWS.y * eWS.y + eWS.z * eWS.z);

        XMMATRIX invW = XMMatrixInverse(nullptr, W);
        XMStoreFloat4x4(&mMainPassCB.InvSkullWorld, XMMatrixTranspose(invW));
        mMainPassCB.SkullExtentsLS = eLS;
    }
    else
    {
        mMainPassCB.SkullCenterWS = { 0,0,0 };
        mMainPassCB.SkullRadius = 0.0f;
        XMStoreFloat4x4(&mMainPassCB.InvSkullWorld, XMMatrixIdentity());
        mMainPassCB.SkullExtentsLS = { 0,0,0 };
    }


    currPassCB->CopyData(0, mMainPassCB);

    // подготовка prev на следующий кадр
    XMStoreFloat4x4(&sPrevViewProj, XMMatrixTranspose(viewProj));
}







void SsaoApp::TaaResolvePass()
{
    //
    // 0) Первичная инициализация истории после ресайза / первого кадра
    //    Делаем обе текстуры истории и копию текущего цвета валидными.
    //
    if (!mTaaHistoryValid)
    {
        auto src = CurrentBackBuffer();
        auto histA = mTaaHistoryA.Get();
        auto histB = mTaaHistoryB.Get();
        auto currCopy = mCurrColorCopy.Get();

        if (src && histA && histB && currCopy)
        {
            CD3DX12_RESOURCE_BARRIER preInit[] =
            {
                CD3DX12_RESOURCE_BARRIER::Transition(src,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(histA,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                CD3DX12_RESOURCE_BARRIER::Transition(histB,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                CD3DX12_RESOURCE_BARRIER::Transition(currCopy,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            };
            mCommandList->ResourceBarrier(_countof(preInit), preInit);

            // Заполняем обе истории и копию текущим кадром
            mCommandList->CopyResource(histA, src);
            mCommandList->CopyResource(histB, src);
            mCommandList->CopyResource(currCopy, src);

            CD3DX12_RESOURCE_BARRIER postInit[] =
            {
                CD3DX12_RESOURCE_BARRIER::Transition(src,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                CD3DX12_RESOURCE_BARRIER::Transition(histA,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(histB,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(currCopy,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            mCommandList->ResourceBarrier(_countof(postInit), postInit);

            mTaaHistoryValid = true;
            mUseHistoryA = true; // пусть сначала читаем из A, пишем в B
        }
    }

    //
    // 1) Копия текущего цвета: backbuffer -> mCurrColorCopy
    //
    {
        auto src = CurrentBackBuffer();
        auto dst = mCurrColorCopy.Get();
        if (src && dst)
        {
            CD3DX12_RESOURCE_BARRIER pre[] =
            {
                CD3DX12_RESOURCE_BARRIER::Transition(src,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                CD3DX12_RESOURCE_BARRIER::Transition(dst,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
            };
            mCommandList->ResourceBarrier(_countof(pre), pre);

            mCommandList->CopyResource(dst, src);

            CD3DX12_RESOURCE_BARRIER post[] =
            {
                CD3DX12_RESOURCE_BARRIER::Transition(src,
                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                CD3DX12_RESOURCE_BARRIER::Transition(dst,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            };
            mCommandList->ResourceBarrier(_countof(post), post);
        }
    }

    //
    // 2) Выбор текстур истории для ping-pong:
    //    histSrc — читаем, histDst — туда пишем resolved кадр.
    //
    ID3D12Resource* histSrc = mUseHistoryA ? mTaaHistoryA.Get() : mTaaHistoryB.Get();
    ID3D12Resource* histDst = mUseHistoryA ? mTaaHistoryB.Get() : mTaaHistoryA.Get();

    //
    // 3) Обновляем SRV t0..t2 = curr, history, depth
    //
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.ResourceMinLODClamp = 0.0f;
        sd.Format = ToNonSRGB(mBackBufferFormat); // SRV формат совместим с bb

        // t0: текущий цвет (копия backbuffer)
        md3dDevice->CreateShaderResourceView(
            mCurrColorCopy.Get(), &sd, GetCpuSrv(mTaaHeapIndexStart + 0));

        // t1: история (источник)
        md3dDevice->CreateShaderResourceView(
            histSrc, &sd, GetCpuSrv(mTaaHeapIndexStart + 1));

        // t2: depth из SSAO-кучи (mSsaoHeapIndexStart+1)
        md3dDevice->CopyDescriptorsSimple(
            1,
            GetCpuSrv(mTaaHeapIndexStart + 2),
            GetCpuSrv(mSsaoHeapIndexStart + 1),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    //
    // 4) Fullscreen TAA resolve
    //
    {
        mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

        // PassCB
        auto passCB = mCurrFrameResource->PassCB->Resource();
        mCommandList->SetGraphicsRootConstantBufferView(
            1, passCB->GetGPUVirtualAddress());

        // SRV t0..t2 = curr, history, depth
        mCommandList->SetGraphicsRootDescriptorTable(
            3, GetGpuSrv(mTaaHeapIndexStart));

        if (mTaaViewMode == 4)
        {
            // Debug: применять TAA только где stencil == 1
            auto dsv = DepthStencilView();
            mCommandList->OMSetRenderTargets(
                1, &CurrentBackBufferView(), TRUE, &dsv);
            mCommandList->SetPipelineState(mTaaPSOStencil.Get());
            mCommandList->OMSetStencilRef(1);
        }
        else
        {
            // Обычный full-screen resolve: без depth/stencil
            mCommandList->OMSetRenderTargets(
                1, &CurrentBackBufferView(), TRUE, nullptr);
            mCommandList->SetPipelineState(mTaaPSO.Get());
        }

        // fullscreen triangle
        mCommandList->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        mCommandList->DrawInstanced(3, 1, 0, 0);
    }

    //
    // 5) Обновляем историю: resolved backbuffer -> histDst
    //
    {
        auto src = CurrentBackBuffer();
        auto dst = histDst;

        CD3DX12_RESOURCE_BARRIER pre[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(src,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(dst,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        mCommandList->ResourceBarrier(_countof(pre), pre);

        mCommandList->CopyResource(dst, src);

        CD3DX12_RESOURCE_BARRIER post[] =
        {
            CD3DX12_RESOURCE_BARRIER::Transition(src,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
            CD3DX12_RESOURCE_BARRIER::Transition(dst,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        };
        mCommandList->ResourceBarrier(_countof(post), post);
    }

    // Меняем «активную» историю на следующий кадр
    mUseHistoryA = !mUseHistoryA;
}









void SsaoApp::UpdateShadowPassCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mLightView);
    XMMATRIX proj = XMLoadFloat4x4(&mLightProj);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    UINT w = mShadowMap->Width();
    UINT h = mShadowMap->Height();

    XMStoreFloat4x4(&mShadowPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mShadowPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mShadowPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mShadowPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mShadowPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mShadowPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    mShadowPassCB.EyePosW = mLightPosW;
    mShadowPassCB.RenderTargetSize = XMFLOAT2((float)w, (float)h);
    mShadowPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / w, 1.0f / h);
    mShadowPassCB.NearZ = mLightNearZ;
    mShadowPassCB.FarZ = mLightFarZ;

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(1, mShadowPassCB);
}

void SsaoApp::UpdateSsaoCB(const GameTimer& gt)
{
    SsaoConstants ssaoCB;

    // ?? ?? ?????????? ????????:
    XMMATRIX P = XMLoadFloat4x4(&mJitteredProj);

    // NDC[-1,1] -> tex[0,1]
    XMMATRIX T(
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f);

    // Proj / InvProj ????? ?? ???????? Pass (??? ??? ??????????)
    ssaoCB.Proj = mMainPassCB.Proj;
    ssaoCB.InvProj = mMainPassCB.InvProj;
    XMStoreFloat4x4(&ssaoCB.ProjTex, XMMatrixTranspose(P * T));

    mSsao->GetOffsetVectors(ssaoCB.OffsetVectors);

    auto blurWeights = mSsao->CalcGaussWeights(2.5f);
    ssaoCB.BlurWeights[0] = XMFLOAT4(&blurWeights[0]);
    ssaoCB.BlurWeights[1] = XMFLOAT4(&blurWeights[4]);
    ssaoCB.BlurWeights[2] = XMFLOAT4(&blurWeights[8]);

    ssaoCB.InvRenderTargetSize = XMFLOAT2(1.0f / mSsao->SsaoMapWidth(), 1.0f / mSsao->SsaoMapHeight());

    // Coordinates given in view space.
    ssaoCB.OcclusionRadius = 0.5f;
    ssaoCB.OcclusionFadeStart = 0.2f;
    ssaoCB.OcclusionFadeEnd = 1.0f;
    ssaoCB.SurfaceEpsilon = 0.05f;

    auto currSsaoCB = mCurrFrameResource->SsaoCB.get();
    currSsaoCB->CopyData(0, ssaoCB);
}



void SsaoApp::LoadTextures()
{
    std::vector<std::string> texNames =
    {
        "bricksDiffuseMap",
        "bricksNormalMap",
        "tileDiffuseMap",
        "tileNormalMap",
        "defaultDiffuseMap",
        "defaultNormalMap",
        "skyCubeMap"
    };

    std::vector<std::wstring> texFilenames =
    {
        L"../../Textures/bricks2.dds",
        L"../../Textures/bricks2_nmap.dds",
        L"../../Textures/tile.dds",
        L"../../Textures/tile_nmap.dds",
        L"../../Textures/white1x1.dds",
        L"../../Textures/default_nmap.dds",
        L"../../Textures/sunsetcube1024.dds"
    };

    for (int i = 0; i < (int)texNames.size(); ++i)
    {
        auto texMap = std::make_unique<Texture>();
        texMap->Name = texNames[i];
        texMap->Filename = texFilenames[i];
        ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
            mCommandList.Get(), texMap->Filename.c_str(),
            texMap->Resource, texMap->UploadHeap));

        mTextures[texMap->Name] = std::move(texMap);
    }
}

void SsaoApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 3, 0);

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[5];

    // Perfomance TIP: Order from most frequent to least frequent.
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsShaderResourceView(0, 1);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);


    auto staticSamplers = GetStaticSamplers();

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void SsaoApp::BuildSsaoRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable0;
    texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0);

    CD3DX12_DESCRIPTOR_RANGE texTable1;
    texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0);

    // Root parameter can be a table, root descriptor or root constants.
    CD3DX12_ROOT_PARAMETER slotRootParameter[4];

    // Perfomance TIP: Order from most frequent to least frequent.
    slotRootParameter[0].InitAsConstantBufferView(0);
    slotRootParameter[1].InitAsConstants(1, 1);
    slotRootParameter[2].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC depthMapSam(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
        0.0f,
        0,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    std::array<CD3DX12_STATIC_SAMPLER_DESC, 4> staticSamplers =
    {
        pointClamp, linearClamp, depthMapSam, linearWrap
    };

    // A root signature is an array of root parameters.
    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    // create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mSsaoRootSignature.GetAddressOf())));
}

void SsaoApp::BuildDescriptorHeaps()
{
    //
    // Create the SRV heap.
    //
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    // Увеличили запас под 2 тройки TAA (A и B): суммарно 24
    srvHeapDesc.NumDescriptors = 24;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    //
    // Fill out the heap with actual descriptors.
    //
    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    std::vector<ComPtr<ID3D12Resource>> tex2DList =
    {
        mTextures["bricksDiffuseMap"]->Resource,
        mTextures["bricksNormalMap"]->Resource,
        mTextures["tileDiffuseMap"]->Resource,
        mTextures["tileNormalMap"]->Resource,
        mTextures["defaultDiffuseMap"]->Resource,
        mTextures["defaultNormalMap"]->Resource
    };

    auto skyCubeMap = mTextures["skyCubeMap"]->Resource;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (UINT i = 0; i < (UINT)tex2DList.size(); ++i)
    {
        srvDesc.Format = tex2DList[i]->GetDesc().Format;
        srvDesc.Texture2D.MipLevels = tex2DList[i]->GetDesc().MipLevels;
        md3dDevice->CreateShaderResourceView(tex2DList[i].Get(), &srvDesc, hDescriptor);
        hDescriptor.Offset(1, mCbvSrvUavDescriptorSize);
    }

    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = skyCubeMap->GetDesc().MipLevels;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
    srvDesc.Format = skyCubeMap->GetDesc().Format;
    md3dDevice->CreateShaderResourceView(skyCubeMap.Get(), &srvDesc, hDescriptor);

    mSkyTexHeapIndex = (UINT)tex2DList.size();  // 6
    mShadowMapHeapIndex = mSkyTexHeapIndex + 1;    // 7
    mSsaoHeapIndexStart = mShadowMapHeapIndex + 1; // 8  (normal, randomVec, depthSRV)
    mSsaoAmbientMapIndex = mSsaoHeapIndexStart + 3; // 11 (ambientA)
    // РЕЗЕРВ ДЛЯ TAA: две тройки (A и B)
    mTaaHeapIndexStart = mSsaoAmbientMapIndex + 2; // 13..15 (A: curr, histA, depth)
    mTaaHeapIndexStartB = mTaaHeapIndexStart + 3;   // 16..18 (B: curr, histB, depth)

    mNullCubeSrvIndex = mTaaHeapIndexStartB + 3;  // 19
    mNullTexSrvIndex1 = mNullCubeSrvIndex + 1;    // 20
    mNullTexSrvIndex2 = mNullTexSrvIndex1 + 1;    // 21

    // Нулевые SRV (cube + 2d + 2d)
    auto nullSrv = GetCpuSrv(mNullCubeSrvIndex);
    mNullSrv = GetGpuSrv(mNullCubeSrvIndex);

    // null cube
    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);
    nullSrv.Offset(1, mCbvSrvUavDescriptorSize);

    // null 2D #1
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

    // null 2D #2
    nullSrv.Offset(1, mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(nullptr, &srvDesc, nullSrv);

    // ShadowMap дескрипторы
    mShadowMap->BuildDescriptors(
        GetCpuSrv(mShadowMapHeapIndex),
        GetGpuSrv(mShadowMapHeapIndex),
        GetDsv(1));

    // SSAO дескрипторы
    mSsao->BuildDescriptors(
        mDepthStencilBuffer.Get(),
        GetCpuSrv(mSsaoHeapIndexStart),
        GetGpuSrv(mSsaoHeapIndexStart),
        GetRtv(SwapChainBufferCount),
        mCbvSrvUavDescriptorSize,
        mRtvDescriptorSize);

    // ВНИМАНИЕ: SRV для TAA (две тройки) создадим в OnResize(),
    // когда уже пересозданы mCurrColorCopy / mTaaHistoryA / mTaaHistoryB.
}


void SsaoApp::BuildShadersAndInputLayout()
{
    const D3D_SHADER_MACRO alphaTestDefines[] =
    {
        "ALPHA_TEST", "1",
        NULL, NULL
    };

    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["shadowOpaquePS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", nullptr, "PS", "ps_5_1");
    mShaders["shadowAlphaTestedPS"] = d3dUtil::CompileShader(L"Shaders\\Shadows.hlsl", alphaTestDefines, "PS", "ps_5_1");

    mShaders["debugVS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["debugPS"] = d3dUtil::CompileShader(L"Shaders\\ShadowDebug.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["drawNormalsVS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["drawNormalsPS"] = d3dUtil::CompileShader(L"Shaders\\DrawNormals.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["ssaoVS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["ssaoPS"] = d3dUtil::CompileShader(L"Shaders\\Ssao.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["ssaoBlurVS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["ssaoBlurPS"] = d3dUtil::CompileShader(L"Shaders\\SsaoBlur.hlsl", nullptr, "PS", "ps_5_1");

    mShaders["skyVS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "VS", "vs_5_1");
    mShaders["skyPS"] = d3dUtil::CompileShader(L"Shaders\\Sky.hlsl", nullptr, "PS", "ps_5_1");

    // === TAA: ????????????? VS/PS ===
    mTaaVS = d3dUtil::CompileShader(L"Shaders\\TaaResolve.hlsl", nullptr, "VS_FullscreenTriangle", "vs_5_1");
    mTaaPS = d3dUtil::CompileShader(L"Shaders\\TaaResolve.hlsl", nullptr, "PS_TAA", "ps_5_1");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}


void SsaoApp::BuildShapeGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
    GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
    GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.3f, 3.0f, 20, 20);
    GeometryGenerator::MeshData quad = geoGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);

    //
    // We are concatenating all the geometry into one big vertex/index buffer.  So
    // define the regions in the buffer each submesh covers.
    //

    // Cache the vertex offsets to each object in the concatenated vertex buffer.
    UINT boxVertexOffset = 0;
    UINT gridVertexOffset = (UINT)box.Vertices.size();
    UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
    UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
    UINT quadVertexOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();

    // Cache the starting index for each object in the concatenated index buffer.
    UINT boxIndexOffset = 0;
    UINT gridIndexOffset = (UINT)box.Indices32.size();
    UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
    UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
    UINT quadIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();

    SubmeshGeometry boxSubmesh;
    boxSubmesh.IndexCount = (UINT)box.Indices32.size();
    boxSubmesh.StartIndexLocation = boxIndexOffset;
    boxSubmesh.BaseVertexLocation = boxVertexOffset;

    SubmeshGeometry gridSubmesh;
    gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
    gridSubmesh.StartIndexLocation = gridIndexOffset;
    gridSubmesh.BaseVertexLocation = gridVertexOffset;

    SubmeshGeometry sphereSubmesh;
    sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
    sphereSubmesh.StartIndexLocation = sphereIndexOffset;
    sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

    SubmeshGeometry cylinderSubmesh;
    cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
    cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
    cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

    SubmeshGeometry quadSubmesh;
    quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
    quadSubmesh.StartIndexLocation = quadIndexOffset;
    quadSubmesh.BaseVertexLocation = quadVertexOffset;

    //
    // Extract the vertex elements we are interested in and pack the
    // vertices of all the meshes into one vertex buffer.
    //

    auto totalVertexCount =
        box.Vertices.size() +
        grid.Vertices.size() +
        sphere.Vertices.size() +
        cylinder.Vertices.size() +
        quad.Vertices.size();

    std::vector<Vertex> vertices(totalVertexCount);

    UINT k = 0;
    for (size_t i = 0; i < box.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = box.Vertices[i].Position;
        vertices[k].Normal = box.Vertices[i].Normal;
        vertices[k].TexC = box.Vertices[i].TexC;
        vertices[k].TangentU = box.Vertices[i].TangentU;
    }

    for (size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = grid.Vertices[i].Position;
        vertices[k].Normal = grid.Vertices[i].Normal;
        vertices[k].TexC = grid.Vertices[i].TexC;
        vertices[k].TangentU = grid.Vertices[i].TangentU;
    }

    for (size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = sphere.Vertices[i].Position;
        vertices[k].Normal = sphere.Vertices[i].Normal;
        vertices[k].TexC = sphere.Vertices[i].TexC;
        vertices[k].TangentU = sphere.Vertices[i].TangentU;
    }

    for (size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = cylinder.Vertices[i].Position;
        vertices[k].Normal = cylinder.Vertices[i].Normal;
        vertices[k].TexC = cylinder.Vertices[i].TexC;
        vertices[k].TangentU = cylinder.Vertices[i].TangentU;
    }

    for (int i = 0; i < quad.Vertices.size(); ++i, ++k)
    {
        vertices[k].Pos = quad.Vertices[i].Position;
        vertices[k].Normal = quad.Vertices[i].Normal;
        vertices[k].TexC = quad.Vertices[i].TexC;
        vertices[k].TangentU = quad.Vertices[i].TangentU;
    }

    std::vector<std::uint16_t> indices;
    indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
    indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
    indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
    indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
    indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "shapeGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R16_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    geo->DrawArgs["box"] = boxSubmesh;
    geo->DrawArgs["grid"] = gridSubmesh;
    geo->DrawArgs["sphere"] = sphereSubmesh;
    geo->DrawArgs["cylinder"] = cylinderSubmesh;
    geo->DrawArgs["quad"] = quadSubmesh;

    mGeometries[geo->Name] = std::move(geo);
}

void SsaoApp::BuildSkullGeometry()
{
    std::ifstream fin("Models/skull.txt");

    if (!fin)
    {
        MessageBox(0, L"Models/skull.txt not found.", 0, 0);
        return;
    }

    UINT vcount = 0;
    UINT tcount = 0;
    std::string ignore;

    fin >> ignore >> vcount;
    fin >> ignore >> tcount;
    fin >> ignore >> ignore >> ignore >> ignore;

    XMFLOAT3 vMinf3(+MathHelper::Infinity, +MathHelper::Infinity, +MathHelper::Infinity);
    XMFLOAT3 vMaxf3(-MathHelper::Infinity, -MathHelper::Infinity, -MathHelper::Infinity);

    XMVECTOR vMin = XMLoadFloat3(&vMinf3);
    XMVECTOR vMax = XMLoadFloat3(&vMaxf3);

    std::vector<Vertex> vertices(vcount);
    for (UINT i = 0; i < vcount; ++i)
    {
        fin >> vertices[i].Pos.x >> vertices[i].Pos.y >> vertices[i].Pos.z;
        fin >> vertices[i].Normal.x >> vertices[i].Normal.y >> vertices[i].Normal.z;

        vertices[i].TexC = { 0.0f, 0.0f };

        XMVECTOR P = XMLoadFloat3(&vertices[i].Pos);

        XMVECTOR N = XMLoadFloat3(&vertices[i].Normal);

        // Generate a tangent vector so normal mapping works.  We aren't applying
        // a texture map to the skull, so we just need any tangent vector so that
        // the math works out to give us the original interpolated vertex normal.
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (fabsf(XMVectorGetX(XMVector3Dot(N, up))) < 1.0f - 0.001f)
        {
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(up, N));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }
        else
        {
            up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            XMVECTOR T = XMVector3Normalize(XMVector3Cross(N, up));
            XMStoreFloat3(&vertices[i].TangentU, T);
        }


        vMin = XMVectorMin(vMin, P);
        vMax = XMVectorMax(vMax, P);
    }

    BoundingBox bounds;
    XMStoreFloat3(&bounds.Center, 0.5f * (vMin + vMax));
    XMStoreFloat3(&bounds.Extents, 0.5f * (vMax - vMin));

    fin >> ignore;
    fin >> ignore;
    fin >> ignore;

    std::vector<std::int32_t> indices(3 * tcount);
    for (UINT i = 0; i < tcount; ++i)
    {
        fin >> indices[i * 3 + 0] >> indices[i * 3 + 1] >> indices[i * 3 + 2];
    }

    fin.close();

    //
    // Pack the indices of all the meshes into one index buffer.
    //

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);

    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::int32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "skullGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.Bounds = bounds;

    geo->DrawArgs["skull"] = submesh;

    mGeometries[geo->Name] = std::move(geo);
}

void SsaoApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC basePsoDesc;
    ZeroMemory(&basePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    basePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    basePsoDesc.pRootSignature = mRootSignature.Get();
    basePsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
      mShaders["standardVS"]->GetBufferSize() };
    basePsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
      mShaders["opaquePS"]->GetBufferSize() };
    basePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    basePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    basePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    basePsoDesc.SampleMask = UINT_MAX;
    basePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    basePsoDesc.NumRenderTargets = 1;
    basePsoDesc.RTVFormats[0] = mBackBufferFormat;
    basePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    basePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    basePsoDesc.DSVFormat = mDepthStencilFormat;

    // opaque (цветовой проход поверх depth pre-pass)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = basePsoDesc;
    opaquePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
    opaquePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));

    // shadow
    D3D12_GRAPHICS_PIPELINE_STATE_DESC smapPsoDesc = basePsoDesc;
    smapPsoDesc.RasterizerState.DepthBias = 100000;
    smapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    smapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
    smapPsoDesc.pRootSignature = mRootSignature.Get();
    smapPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["shadowVS"]->GetBufferPointer()),
      mShaders["shadowVS"]->GetBufferSize() };
    smapPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["shadowOpaquePS"]->GetBufferPointer()),
      mShaders["shadowOpaquePS"]->GetBufferSize() };
    smapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    smapPsoDesc.NumRenderTargets = 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&smapPsoDesc, IID_PPV_ARGS(&mPSOs["shadow_opaque"])));

    // debug
    D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc = basePsoDesc;
    debugPsoDesc.pRootSignature = mRootSignature.Get();
    debugPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["debugVS"]->GetBufferPointer()),
      mShaders["debugVS"]->GetBufferSize() };
    debugPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["debugPS"]->GetBufferPointer()),
      mShaders["debugPS"]->GetBufferSize() };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["debug"])));

    // drawNormals (pre-pass в нормали + depth)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawNormalsPsoDesc = basePsoDesc;
    drawNormalsPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["drawNormalsVS"]->GetBufferPointer()),
      mShaders["drawNormalsVS"]->GetBufferSize() };
    drawNormalsPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["drawNormalsPS"]->GetBufferPointer()),
      mShaders["drawNormalsPS"]->GetBufferSize() };
    drawNormalsPsoDesc.RTVFormats[0] = Ssao::NormalMapFormat;
    drawNormalsPsoDesc.SampleDesc.Count = 1;
    drawNormalsPsoDesc.SampleDesc.Quality = 0;
    drawNormalsPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&drawNormalsPsoDesc, IID_PPV_ARGS(&mPSOs["drawNormals"])));

    // ssao
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoPsoDesc = basePsoDesc;
    ssaoPsoDesc.InputLayout = { nullptr, 0 };
    ssaoPsoDesc.pRootSignature = mSsaoRootSignature.Get();
    ssaoPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["ssaoVS"]->GetBufferPointer()),
      mShaders["ssaoVS"]->GetBufferSize() };
    ssaoPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["ssaoPS"]->GetBufferPointer()),
      mShaders["ssaoPS"]->GetBufferSize() };
    ssaoPsoDesc.DepthStencilState.DepthEnable = false;
    ssaoPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    ssaoPsoDesc.RTVFormats[0] = Ssao::AmbientMapFormat;
    ssaoPsoDesc.SampleDesc.Count = 1;
    ssaoPsoDesc.SampleDesc.Quality = 0;
    ssaoPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoPsoDesc, IID_PPV_ARGS(&mPSOs["ssao"])));

    // ssao blur
    D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoBlurPsoDesc = ssaoPsoDesc;
    ssaoBlurPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["ssaoBlurVS"]->GetBufferPointer()),
      mShaders["ssaoBlurVS"]->GetBufferSize() };
    ssaoBlurPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["ssaoBlurPS"]->GetBufferPointer()),
      mShaders["ssaoBlurPS"]->GetBufferSize() };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ssaoBlurPsoDesc, IID_PPV_ARGS(&mPSOs["ssaoBlur"])));

    // sky
    D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc = basePsoDesc;
    skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    skyPsoDesc.pRootSignature = mRootSignature.Get();
    skyPsoDesc.VS =
    { reinterpret_cast<BYTE*>(mShaders["skyVS"]->GetBufferPointer()),
      mShaders["skyVS"]->GetBufferSize() };
    skyPsoDesc.PS =
    { reinterpret_cast<BYTE*>(mShaders["skyPS"]->GetBufferPointer()),
      mShaders["skyPS"]->GetBufferSize() };
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["sky"])));

    // TAA resolve (fullscreen, без depth/stencil)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC taaPsoDesc = {};
    taaPsoDesc.pRootSignature = mRootSignature.Get();
    taaPsoDesc.VS = { reinterpret_cast<BYTE*>(mTaaVS->GetBufferPointer()), mTaaVS->GetBufferSize() };
    taaPsoDesc.PS = { reinterpret_cast<BYTE*>(mTaaPS->GetBufferPointer()), mTaaPS->GetBufferSize() };
    taaPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    taaPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    taaPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    taaPsoDesc.DepthStencilState.DepthEnable = FALSE;
    taaPsoDesc.SampleMask = UINT_MAX;
    taaPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    taaPsoDesc.NumRenderTargets = 1;
    taaPsoDesc.RTVFormats[0] = mBackBufferFormat;
    taaPsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    taaPsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&taaPsoDesc, IID_PPV_ARGS(&mTaaPSO)));

    // === Opaque со записью stencil = 1 (только для skull). ВАЖНО: DepthFunc = LESS_EQUAL, запись Z отключена ===
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueStencilWrite = basePsoDesc;

    // читаем Z, но не пишем его
    opaqueStencilWrite.DepthStencilState.DepthEnable = TRUE;
    opaqueStencilWrite.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    opaqueStencilWrite.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // запись в stencil = 1
    opaqueStencilWrite.DepthStencilState.StencilEnable = TRUE;
    opaqueStencilWrite.DepthStencilState.StencilWriteMask = 0xFF;
    opaqueStencilWrite.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    opaqueStencilWrite.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
    opaqueStencilWrite.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    opaqueStencilWrite.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    opaqueStencilWrite.DepthStencilState.BackFace = opaqueStencilWrite.DepthStencilState.FrontFace;

    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &opaqueStencilWrite, IID_PPV_ARGS(&mPSOs["opaque_stencilWrite"])));

    // === TAA только там, где stencil == 1 ===
    D3D12_GRAPHICS_PIPELINE_STATE_DESC taaStencil = {};
    taaStencil.pRootSignature = mRootSignature.Get();
    taaStencil.VS = { reinterpret_cast<BYTE*>(mTaaVS->GetBufferPointer()), mTaaVS->GetBufferSize() };
    taaStencil.PS = { reinterpret_cast<BYTE*>(mTaaPS->GetBufferPointer()), mTaaPS->GetBufferSize() };
    taaStencil.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    taaStencil.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    taaStencil.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    taaStencil.DepthStencilState.DepthEnable = FALSE;       // resolve без глубины
    taaStencil.DepthStencilState.StencilEnable = TRUE;
    taaStencil.DepthStencilState.StencilReadMask = 0xFF;
    taaStencil.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    taaStencil.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    taaStencil.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    taaStencil.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    taaStencil.DepthStencilState.BackFace = taaStencil.DepthStencilState.FrontFace;
    taaStencil.SampleMask = UINT_MAX;
    taaStencil.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    taaStencil.NumRenderTargets = 1;
    taaStencil.RTVFormats[0] = mBackBufferFormat;
    taaStencil.DSVFormat = mDepthStencilFormat; // нужен DSV для stencil-теста
    taaStencil.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    taaStencil.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(
        &taaStencil, IID_PPV_ARGS(&mTaaPSOStencil)));
}




void SsaoApp::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            2, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
    }
}

void SsaoApp::BuildMaterials()
{
    auto bricks0 = std::make_unique<Material>();
    bricks0->Name = "bricks0";
    bricks0->MatCBIndex = 0;
    bricks0->DiffuseSrvHeapIndex = 0;
    bricks0->NormalSrvHeapIndex = 1;
    bricks0->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    bricks0->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    bricks0->Roughness = 0.3f;

    auto tile0 = std::make_unique<Material>();
    tile0->Name = "tile0";
    tile0->MatCBIndex = 1;
    tile0->DiffuseSrvHeapIndex = 2;
    tile0->NormalSrvHeapIndex = 3;
    tile0->DiffuseAlbedo = XMFLOAT4(0.9f, 0.9f, 0.9f, 1.0f);
    tile0->FresnelR0 = XMFLOAT3(0.2f, 0.2f, 0.2f);
    tile0->Roughness = 0.1f;

    auto mirror0 = std::make_unique<Material>();
    mirror0->Name = "mirror0";
    mirror0->MatCBIndex = 2;
    mirror0->DiffuseSrvHeapIndex = 4;
    mirror0->NormalSrvHeapIndex = 5;
    mirror0->DiffuseAlbedo = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mirror0->FresnelR0 = XMFLOAT3(0.98f, 0.97f, 0.95f);
    mirror0->Roughness = 0.1f;

    auto skullMat = std::make_unique<Material>();
    skullMat->Name = "skullMat";
    skullMat->MatCBIndex = 3;
    skullMat->DiffuseSrvHeapIndex = 4;
    skullMat->NormalSrvHeapIndex = 5;
    skullMat->DiffuseAlbedo = XMFLOAT4(0.3f, 0.3f, 0.3f, 1.0f);
    skullMat->FresnelR0 = XMFLOAT3(0.6f, 0.6f, 0.6f);
    skullMat->Roughness = 0.2f;

    auto sky = std::make_unique<Material>();
    sky->Name = "sky";
    sky->MatCBIndex = 4;
    sky->DiffuseSrvHeapIndex = 6;
    sky->NormalSrvHeapIndex = 7;
    sky->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    sky->FresnelR0 = XMFLOAT3(0.1f, 0.1f, 0.1f);
    sky->Roughness = 1.0f;

    mMaterials["bricks0"] = std::move(bricks0);
    mMaterials["tile0"] = std::move(tile0);
    mMaterials["mirror0"] = std::move(mirror0);
    mMaterials["skullMat"] = std::move(skullMat);
    mMaterials["sky"] = std::move(sky);
}


void SsaoApp::BuildRenderItems()
{
    auto skyRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&skyRitem->World, XMMatrixScaling(5000.0f, 5000.0f, 5000.0f));
    skyRitem->TexTransform = MathHelper::Identity4x4();
    skyRitem->ObjCBIndex = 0;
    skyRitem->Mat = mMaterials["sky"].get();
    skyRitem->Geo = mGeometries["shapeGeo"].get();
    skyRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skyRitem->IndexCount = skyRitem->Geo->DrawArgs["sphere"].IndexCount;
    skyRitem->StartIndexLocation = skyRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
    skyRitem->BaseVertexLocation = skyRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

    mRitemLayer[(int)RenderLayer::Sky].push_back(skyRitem.get());
    mAllRitems.push_back(std::move(skyRitem));

    auto quadRitem = std::make_unique<RenderItem>();
    quadRitem->World = MathHelper::Identity4x4();
    quadRitem->TexTransform = MathHelper::Identity4x4();
    quadRitem->ObjCBIndex = 1;
    quadRitem->Mat = mMaterials["bricks0"].get();
    quadRitem->Geo = mGeometries["shapeGeo"].get();
    quadRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    quadRitem->IndexCount = quadRitem->Geo->DrawArgs["quad"].IndexCount;
    quadRitem->StartIndexLocation = quadRitem->Geo->DrawArgs["quad"].StartIndexLocation;
    quadRitem->BaseVertexLocation = quadRitem->Geo->DrawArgs["quad"].BaseVertexLocation;

    mRitemLayer[(int)RenderLayer::Debug].push_back(quadRitem.get());
    mAllRitems.push_back(std::move(quadRitem));

    auto boxRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(2.0f, 1.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
    XMStoreFloat4x4(&boxRitem->TexTransform, XMMatrixScaling(1.0f, 0.5f, 1.0f));
    boxRitem->ObjCBIndex = 2;
    boxRitem->Mat = mMaterials["bricks0"].get();
    boxRitem->Geo = mGeometries["shapeGeo"].get();
    boxRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
    boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
    boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;

    mRitemLayer[(int)RenderLayer::Opaque].push_back(boxRitem.get());
    mAllRitems.push_back(std::move(boxRitem));

    auto skullRitem = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&skullRitem->World, XMMatrixScaling(0.4f, 0.4f, 0.4f) * XMMatrixTranslation(0.0f, 1.0f, 0.0f));
    skullRitem->TexTransform = MathHelper::Identity4x4();
    skullRitem->ObjCBIndex = 3;
    skullRitem->Mat = mMaterials["skullMat"].get();
    skullRitem->Geo = mGeometries["skullGeo"].get();
    skullRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    skullRitem->IndexCount = skullRitem->Geo->DrawArgs["skull"].IndexCount;
    skullRitem->StartIndexLocation = skullRitem->Geo->DrawArgs["skull"].StartIndexLocation;
    skullRitem->BaseVertexLocation = skullRitem->Geo->DrawArgs["skull"].BaseVertexLocation;

    // сохраним указатель на skull
    mSkullRitem = skullRitem.get();

    mRitemLayer[(int)RenderLayer::Opaque].push_back(skullRitem.get());
    mAllRitems.push_back(std::move(skullRitem));

    auto gridRitem = std::make_unique<RenderItem>();
    gridRitem->World = MathHelper::Identity4x4();
    XMStoreFloat4x4(&gridRitem->TexTransform, XMMatrixScaling(8.0f, 8.0f, 1.0f));
    gridRitem->ObjCBIndex = 4;
    gridRitem->Mat = mMaterials["tile0"].get();
    gridRitem->Geo = mGeometries["shapeGeo"].get();
    gridRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;

    mRitemLayer[(int)RenderLayer::Opaque].push_back(gridRitem.get());
    mAllRitems.push_back(std::move(gridRitem));

    XMMATRIX brickTexTransform = XMMatrixScaling(1.5f, 2.0f, 1.0f);
    UINT objCBIndex = 5;
    for (int i = 0; i < 5; ++i)
    {
        auto leftCylRitem = std::make_unique<RenderItem>();
        auto rightCylRitem = std::make_unique<RenderItem>();
        auto leftSphereRitem = std::make_unique<RenderItem>();
        auto rightSphereRitem = std::make_unique<RenderItem>();

        XMMATRIX leftCylWorld = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
        XMMATRIX rightCylWorld = XMMatrixTranslation(+5.0f, 1.5f, -10.0f + i * 5.0f);

        XMMATRIX leftSphereWorld = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
        XMMATRIX rightSphereWorld = XMMatrixTranslation(+5.0f, 3.5f, -10.0f + i * 5.0f);

        XMStoreFloat4x4(&leftCylRitem->World, leftCylWorld);
        XMStoreFloat4x4(&leftCylRitem->TexTransform, brickTexTransform);
        leftCylRitem->ObjCBIndex = objCBIndex++;
        leftCylRitem->Mat = mMaterials["bricks0"].get();
        leftCylRitem->Geo = mGeometries["shapeGeo"].get();
        leftCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        leftCylRitem->IndexCount = leftCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        leftCylRitem->StartIndexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        leftCylRitem->BaseVertexLocation = leftCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

        XMStoreFloat4x4(&rightCylRitem->World, rightCylWorld);
        XMStoreFloat4x4(&rightCylRitem->TexTransform, brickTexTransform);
        rightCylRitem->ObjCBIndex = objCBIndex++;
        rightCylRitem->Mat = mMaterials["bricks0"].get();
        rightCylRitem->Geo = mGeometries["shapeGeo"].get();
        rightCylRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        rightCylRitem->IndexCount = rightCylRitem->Geo->DrawArgs["cylinder"].IndexCount;
        rightCylRitem->StartIndexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].StartIndexLocation;
        rightCylRitem->BaseVertexLocation = rightCylRitem->Geo->DrawArgs["cylinder"].BaseVertexLocation;

        XMStoreFloat4x4(&leftSphereRitem->World, leftSphereWorld);
        leftSphereRitem->TexTransform = MathHelper::Identity4x4();
        leftSphereRitem->ObjCBIndex = objCBIndex++;
        leftSphereRitem->Mat = mMaterials["mirror0"].get();
        leftSphereRitem->Geo = mGeometries["shapeGeo"].get();
        leftSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        leftSphereRitem->IndexCount = leftSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
        leftSphereRitem->StartIndexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
        leftSphereRitem->BaseVertexLocation = leftSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

        XMStoreFloat4x4(&rightSphereRitem->World, rightSphereWorld);
        rightSphereRitem->TexTransform = MathHelper::Identity4x4();
        rightSphereRitem->ObjCBIndex = objCBIndex++;
        rightSphereRitem->Mat = mMaterials["mirror0"].get();
        rightSphereRitem->Geo = mGeometries["shapeGeo"].get();
        rightSphereRitem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        rightSphereRitem->IndexCount = rightSphereRitem->Geo->DrawArgs["sphere"].IndexCount;
        rightSphereRitem->StartIndexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
        rightSphereRitem->BaseVertexLocation = rightSphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;

        mRitemLayer[(int)RenderLayer::Opaque].push_back(leftCylRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(rightCylRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(leftSphereRitem.get());
        mRitemLayer[(int)RenderLayer::Opaque].push_back(rightSphereRitem.get());

        mAllRitems.push_back(std::move(leftCylRitem));
        mAllRitems.push_back(std::move(rightCylRitem));
        mAllRitems.push_back(std::move(leftSphereRitem));
        mAllRitems.push_back(std::move(rightSphereRitem));
    }
}


void SsaoApp::CreateTaaResources()
{
    auto device = md3dDevice.Get();

    // ВАЖНО: формат истории/копии = формат бэкбуфера, иначе CopyResource не сработает
    DXGI_FORMAT fmt = mBackBufferFormat;

    auto makeTex = [&](ComPtr<ID3D12Resource>& tex)
        {
            if (mClientWidth == 0 || mClientHeight == 0) return;

            CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
            CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
                fmt, mClientWidth, mClientHeight, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&tex)));
        };

    makeTex(mTaaHistoryA);
    makeTex(mTaaHistoryB);
    makeTex(mCurrColorCopy);

    // история стала невалидной после ресайза — пересоздадим на первом кадре
    mTaaHistoryValid = false;
}




void SsaoApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();

    // For each render item...
    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;

        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void SsaoApp::DrawSceneToShadowMap()
{
    mCommandList->RSSetViewports(1, &mShadowMap->Viewport());
    mCommandList->RSSetScissorRects(1, &mShadowMap->ScissorRect());

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mShadowMap->Resource(), D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

    mCommandList->ClearDepthStencilView(mShadowMap->Dsv(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    mCommandList->OMSetRenderTargets(0, nullptr, false, &mShadowMap->Dsv());

    // PassCB для shadow-прохода.
    const UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));
    auto passCB = mCurrFrameResource->PassCB->Resource();
    D3D12_GPU_VIRTUAL_ADDRESS passCBAddress = passCB->GetGPUVirtualAddress() + 1 * passCBByteSize;
    mCommandList->SetGraphicsRootConstantBufferView(1, passCBAddress);

    mCommandList->SetPipelineState(mPSOs["shadow_opaque"].Get());

    // --- ТОЛЬКО SKULL ---
    if (mSkullRitem)
    {
        UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
        auto objectCB = mCurrFrameResource->ObjectCB->Resource();
        auto* ri = mSkullRitem;

        mCommandList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        mCommandList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
            objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        mCommandList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        mCommandList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        mShadowMap->Resource(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));
}

void SsaoApp::ComputeTaaJitter()
{
    // Halton 2,3 в [-0.5,+0.5]
    float jx = (Halton(gTaa.HaltonIndex, 2) - 0.5f);
    float jy = (Halton(gTaa.HaltonIndex, 3) - 0.5f);

    // Короткая последовательность для стабильности
    gTaa.HaltonIndex = (gTaa.HaltonIndex % 8) + 1;

    // В UV с учётом амплитуды (в пикселях)
    XMFLOAT2 jitterUV(
        (jx * mTaaJitterPixels) / max(1, mClientWidth),
        (jy * mTaaJitterPixels) / max(1, mClientHeight));

    gTaa.PrevJitter = gTaa.Jitter;
    gTaa.Jitter = jitterUV;

    // Смещение проекции в clip-space (Y инвертирован)
    mCamera.SetLens(0.25f * XM_PI, AspectRatio(), 1.0f, 1000.0f);
    XMMATRIX proj = mCamera.GetProj();
    proj.r[2].m128_f32[0] += jitterUV.x * 2.0f;
    proj.r[2].m128_f32[1] += jitterUV.y * -2.0f;
    XMStoreFloat4x4(&mJitteredProj, proj);
}




void SsaoApp::DrawNormalsAndDepth()
{
    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    auto normalMap = mSsao->NormalMap();
    auto normalMapRtv = mSsao->NormalMapRtv();

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        normalMap, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

    const float clearValue[] = { 0.0f, 0.0f, 1.0f, 0.0f };
    mCommandList->ClearRenderTargetView(normalMapRtv, clearValue, 0, nullptr);

    // Чистим DEPTH|STENCIL тут
    mCommandList->ClearDepthStencilView(
        DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, nullptr);

    mCommandList->OMSetRenderTargets(1, &normalMapRtv, TRUE, &DepthStencilView());

    auto passCB = mCurrFrameResource->PassCB->Resource();
    mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

    mCommandList->SetPipelineState(mPSOs["drawNormals"].Get());

    // Рисуем все opaque (у нас это плейн + череп)
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    auto objectCB = mCurrFrameResource->ObjectCB->Resource();

    for (auto* ri : mRitemLayer[(int)RenderLayer::Opaque])
    {
        mCommandList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        mCommandList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        mCommandList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress =
            objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        mCommandList->SetGraphicsRootConstantBufferView(0, objCBAddress);

        mCommandList->DrawIndexedInstanced(
            ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }

    mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        normalMap, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
}




CD3DX12_CPU_DESCRIPTOR_HANDLE SsaoApp::GetCpuSrv(int index)const
{
    auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    srv.Offset(index, mCbvSrvUavDescriptorSize);
    return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE SsaoApp::GetGpuSrv(int index)const
{
    auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    srv.Offset(index, mCbvSrvUavDescriptorSize);
    return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE SsaoApp::GetDsv(int index)const
{
    auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
    dsv.Offset(index, mDsvDescriptorSize);
    return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE SsaoApp::GetRtv(int index)const
{
    auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtv.Offset(index, mRtvDescriptorSize);
    return rtv;
}


std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> SsaoApp::GetStaticSamplers()
{
    // Applications usually only need a handful of samplers.  So just define them all up front
    // and keep them available as part of the root signature.  

    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
        0.0f,                              // mipLODBias
        8);                                // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC shadow(
        6, // shaderRegister
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
        0.0f,                               // mipLODBias
        16,                                 // maxAnisotropy
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp,
        shadow
    };
}