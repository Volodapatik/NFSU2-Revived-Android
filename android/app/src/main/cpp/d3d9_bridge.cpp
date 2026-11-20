#include "d3d9_bridge.h"
#include "guest_render_probe.h"
#include "guest_runtime.h"
#include "runtime_log.h"
#include "dxt_shadow.h"
#include "vertex_formats.h"
#include "graphics_cache.h"
#include <d3d9.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <cstring>
#include <stdexcept>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>

extern "C" IDirect3D9* Direct3DCreate9(UINT sdk);
extern "C" void dxvkAndroidSetWindow(ANativeWindow* window);
extern "C" HWND dxvkAndroidGetWindowHandle();
extern "C" void dxvkAndroidSetRenderSize(uint32_t width,uint32_t height);
static std::atomic<uint32_t> s_targetRenderWidth{0}, s_targetRenderHeight{0};
void setD3D9RenderSize(unsigned width,unsigned height){
    s_targetRenderWidth = width;
    s_targetRenderHeight = height;
    dxvkAndroidSetRenderSize(width,height);
}
#ifndef __ANDROID__
extern "C" ANativeWindow* dxvkCreateTestWindow();
extern "C" void dxvkDestroyTestWindow(ANativeWindow* window);
#endif
namespace {
std::atomic<uint64_t> s_debugCreateAttempts{0},s_debugCreateSuccesses{0},s_debugPresentCalls{0};
std::atomic<uint32_t> s_debugLastCreateResult{0xffffffffu},s_debugLastPresentResult{0xffffffffu};
std::atomic<uint32_t> s_backbufferProbeState{0},s_backbufferProbePoints{0},s_backbufferProbeNonBlack{0};
std::atomic<uint32_t> s_backbufferProbeWidth{0},s_backbufferProbeHeight{0},s_backbufferProbeFormat{0},s_backbufferProbeMs{0},s_backbufferProbeResult{0xffffffffu};
constexpr uint32_t renderStatCount=static_cast<uint32_t>(D3D9RenderStat::Count);
std::atomic<uint32_t> s_renderCalls[renderStatCount]{};
std::atomic<uint32_t> s_renderForwarded[renderStatCount]{};
std::atomic<uint32_t> s_renderCulled[renderStatCount]{};
std::atomic<uint32_t> s_renderLastCaller[renderStatCount]{};
std::atomic<uint32_t> s_renderLastResult[renderStatCount]{};
std::atomic<uint32_t> s_renderLastPath[renderStatCount]{};
std::atomic<uint32_t> s_activeTargetState{0},s_activeTargetWidth{0},s_activeTargetHeight{0};
std::atomic<uint32_t> s_activeTargetFormat{0},s_activeTargetUsage{0},s_activeTargetPool{0};
std::atomic<uint32_t> s_activeTargetMultisample{0},s_activeTargetMultisampleQuality{0},s_activeTargetType{0};
std::atomic<uint32_t> s_activeTargetIsImplicitBackbuffer{0},s_activeTargetCaller{0},s_activeTargetResult{0xffffffffu};
std::mutex s_probeMutex;
D3D9RenderTargetSnapshot s_helperTargets[2]{};
D3D9RenderSnapshot::GuestRenderDiagnostics s_guestRender{};
std::atomic<uint64_t> s_guestGlobal8383ACReads{0};
std::atomic<uint32_t> s_guestGlobal8383ACValue{0};
struct GuestFlowAtomic {
    std::atomic<uint64_t> hits{0},taken{0},notTaken{0};
    std::atomic<uint32_t> lastValue{0},lastAuxiliary{0},lastOutcome{NFS_GUEST_FLOW_NO_BRANCH};
};
GuestFlowAtomic s_guestFlow[NFS_GUEST_FLOW_EVENT_COUNT]{};
constexpr uint32_t kGuestStateTraceWriteLimit=512,kGuestStateTraceSnapshotLimit=64;
std::atomic<uint32_t> s_guestStateTraceWrites{0},s_guestStateTraceSnapshots{0};
std::atomic<bool> s_guestStateTraceWriteLimitNotified{false},s_guestStateTraceSnapshotLimitNotified{false};
std::atomic<uint64_t> s_guestStateTraceSnapshotKeys[3]{};
std::atomic<bool> s_guestStateTraceSnapshotSeen[3]{};
constexpr uint32_t bucketHelperCount=2,bucketBankCount=2,bucketCountPerBank=184;
std::atomic<uint64_t> s_bucketCalls[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketExistingSkips[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketPrimaryNull[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketAuxiliaryNull[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketSelections[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketLinkedNonNull[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketLinkedNull[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketInvalidAddress[bucketHelperCount]{};
std::atomic<uint64_t> s_bucketBankSelections[bucketHelperCount][bucketBankCount]{};
std::atomic<uint64_t> s_bucketBankLinked[bucketHelperCount][bucketBankCount]{};
std::atomic<uint64_t> s_bucketTouched[bucketHelperCount][bucketBankCount]{};
std::atomic<uint64_t> s_bucketHistogram[bucketHelperCount][bucketBankCount][bucketCountPerBank]{};
std::atomic<uint32_t> s_bucketLastAddress[bucketHelperCount]{};
std::atomic<uint32_t> s_bucketLastNode[bucketHelperCount]{};
std::atomic<uint32_t> s_bucketLastBank[bucketHelperCount]{};
std::atomic<uint32_t> s_bucketLastIndex[bucketHelperCount]{};
D3D9HelperUPSnapshot s_helperUP{};
std::atomic<uint32_t> s_helperUPCaptureState{0};
constexpr uint32_t methodBase=0x7e000000;
enum class Kind:uint32_t {D3D9=1,Device=2,Texture=3,Cube=4,Surface=5,VertexBuffer=6,IndexBuffer=7,
    VertexShader=8,PixelShader=9,Declaration=10,StateBlock=11,Query=12,SwapChain=13,VolumeTexture=14,Volume=15};
void recordRenderMethod(D3D9RenderStat method,uint32_t caller,HRESULT result,bool culled=false){
    const uint32_t index=static_cast<uint32_t>(method);
    s_renderCalls[index].fetch_add(1,std::memory_order_relaxed);
    (culled?s_renderCulled[index]:s_renderForwarded[index]).fetch_add(1,std::memory_order_relaxed);
    s_renderLastCaller[index].store(caller,std::memory_order_relaxed);
    s_renderLastResult[index].store(static_cast<uint32_t>(result),std::memory_order_relaxed);
    s_renderLastPath[index].store(culled?2u:1u,std::memory_order_relaxed);
}
void beginRenderAttempt(D3D9RenderStat method,uint32_t caller){
    const uint32_t index=static_cast<uint32_t>(method);
    s_renderCalls[index].fetch_add(1,std::memory_order_relaxed);
    s_renderLastCaller[index].store(caller,std::memory_order_relaxed);
    s_renderLastResult[index].store(0xffffffffu,std::memory_order_relaxed);
    s_renderLastPath[index].store(0u,std::memory_order_relaxed);
}
void finishRenderAttempt(D3D9RenderStat method,HRESULT result,bool culled){
    const uint32_t index=static_cast<uint32_t>(method);
    (culled?s_renderCulled[index]:s_renderForwarded[index]).fetch_add(1,std::memory_order_relaxed);
    s_renderLastResult[index].store(static_cast<uint32_t>(result),std::memory_order_relaxed);
    s_renderLastPath[index].store(culled?2u:1u,std::memory_order_relaxed);
}
void invalidateActiveTarget(){
    s_activeTargetResult.store(0xffffffffu,std::memory_order_relaxed);
    s_activeTargetState.store(0,std::memory_order_release);
}
void captureActiveTarget(IDirect3DDevice9* device,uint32_t caller,bool force=false) noexcept {
    if(force)invalidateActiveTarget();
    if(!force&&s_activeTargetState.load(std::memory_order_acquire)!=0)return;
    uint32_t expected=0;
    if(!s_activeTargetState.compare_exchange_strong(expected,1,std::memory_order_acq_rel,std::memory_order_relaxed))return;
    s_activeTargetCaller.store(caller,std::memory_order_relaxed);
    struct SurfaceGuard{IDirect3DSurface9* surface{};~SurfaceGuard(){if(surface)surface->Release();}} target,backbuffer;
    try{
        if(!device){s_activeTargetResult.store(E_POINTER,std::memory_order_relaxed);s_activeTargetState.store(3,std::memory_order_release);return;}
        HRESULT status=device->GetRenderTarget(0,&target.surface);
        if(FAILED(status)||!target.surface){s_activeTargetResult.store(static_cast<uint32_t>(FAILED(status)?status:E_FAIL),std::memory_order_relaxed);s_activeTargetState.store(3,std::memory_order_release);return;}
        D3DSURFACE_DESC desc{};
        status=target.surface->GetDesc(&desc);
        if(FAILED(status)){s_activeTargetResult.store(static_cast<uint32_t>(status),std::memory_order_relaxed);s_activeTargetState.store(3,std::memory_order_release);return;}
        uint32_t implicit=0;
        HRESULT backStatus=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer.surface);
        if(SUCCEEDED(backStatus)&&backbuffer.surface)implicit=backbuffer.surface==target.surface?1u:2u;
        s_activeTargetWidth.store(desc.Width,std::memory_order_relaxed);
        s_activeTargetHeight.store(desc.Height,std::memory_order_relaxed);
        s_activeTargetFormat.store(static_cast<uint32_t>(desc.Format),std::memory_order_relaxed);
        s_activeTargetUsage.store(desc.Usage,std::memory_order_relaxed);
        s_activeTargetPool.store(static_cast<uint32_t>(desc.Pool),std::memory_order_relaxed);
        s_activeTargetMultisample.store(static_cast<uint32_t>(desc.MultiSampleType),std::memory_order_relaxed);
        s_activeTargetMultisampleQuality.store(desc.MultiSampleQuality,std::memory_order_relaxed);
        s_activeTargetType.store(static_cast<uint32_t>(desc.Type),std::memory_order_relaxed);
        s_activeTargetIsImplicitBackbuffer.store(implicit,std::memory_order_relaxed);
        s_activeTargetResult.store(static_cast<uint32_t>(status),std::memory_order_relaxed);
        s_activeTargetState.store(2,std::memory_order_release);
    }catch(...){
        s_activeTargetResult.store(E_FAIL,std::memory_order_relaxed);
        s_activeTargetState.store(3,std::memory_order_release);
    }
}
struct Object {IUnknown* native;Kind kind;uint32_t references=1;CompressedView compressed;uint32_t parent=0;uint32_t children=0;};
std::unordered_map<uint32_t,Object> objects;
// Nearly every call of a frame targets the same object (the device). Element
// pointers of an unordered_map survive rehashing; only erasing an object
// invalidates them, so retireGuestObject/shutdown drop this one-entry cache.
uint32_t cachedObjectAddress=0;Object* cachedObject=nullptr;
Object* findObject(uint32_t address){
    if(address==cachedObjectAddress&&cachedObject)return cachedObject;
    auto found=objects.find(address);if(found==objects.end())return nullptr;
    cachedObjectAddress=address;cachedObject=&found->second;return cachedObject;
}
std::map<IUnknown*,uint32_t> addresses;
void* guestBase;
uint32_t (*allocateGuest)(uint32_t);
void (*freeGuest)(uint32_t);
std::atomic<ANativeWindow*> hostWindow{nullptr};
std::atomic<bool> surfaceRequested{false},surfaceAvailable{false};
void* pointer(uint32_t address){return address?static_cast<uint8_t*>(guestBase)+address:nullptr;}
void write(uint32_t address,uint32_t value){if(!address)throw std::runtime_error("Null D3D9 output");std::memcpy(pointer(address),&value,4);}
uint32_t wrap(IUnknown* native,Kind kind){
    if(!native)return 0;auto previous=addresses.find(native);if(previous!=addresses.end()){++objects.at(previous->second).references;return previous->second;}
    static constexpr uint32_t sizes[]={0,17,119,22,22,17,14,14,5,5,5,6,8,10,22,11};
    uint32_t count=sizes[static_cast<uint32_t>(kind)];
    uint32_t table=allocateGuest(count*4),object=allocateGuest(4);
    for(uint32_t i=0;i<count;++i)write(table+i*4,methodBase+static_cast<uint32_t>(kind)*4096+i*16);
    write(object,table);objects.emplace(object,Object{native,kind});addresses.emplace(native,object);return object;
}
// The guest handle's kind decides the interface: D3D9 interfaces form single
// inheritance chains from IUnknown, so once the kind matches, static_cast is
// exact. dynamic_cast on every SetTexture/SetStreamSource cost ~3% of a race.
template<class T>bool kindMatches(Kind kind){
    if constexpr(std::is_same_v<T,IDirect3DSurface9>)return kind==Kind::Surface;
    else if constexpr(std::is_same_v<T,IDirect3DBaseTexture9>)return kind==Kind::Texture||kind==Kind::Cube||kind==Kind::VolumeTexture;
    else if constexpr(std::is_same_v<T,IDirect3DTexture9>)return kind==Kind::Texture;
    else if constexpr(std::is_same_v<T,IDirect3DCubeTexture9>)return kind==Kind::Cube;
    else if constexpr(std::is_same_v<T,IDirect3DVertexBuffer9>)return kind==Kind::VertexBuffer;
    else if constexpr(std::is_same_v<T,IDirect3DIndexBuffer9>)return kind==Kind::IndexBuffer;
    else if constexpr(std::is_same_v<T,IDirect3DVertexShader9>)return kind==Kind::VertexShader;
    else if constexpr(std::is_same_v<T,IDirect3DPixelShader9>)return kind==Kind::PixelShader;
    else if constexpr(std::is_same_v<T,IDirect3DVertexDeclaration9>)return kind==Kind::Declaration;
    else return false;
}
template<class T>T* nativeObject(uint32_t address){
    if(!address)return nullptr;auto found=objects.find(address);if(found==objects.end())return nullptr;
    if(kindMatches<T>(found->second.kind))return static_cast<T*>(found->second.native);
    auto native=dynamic_cast<T*>(found->second.native);return native;
}
uint32_t wrapTexture(IDirect3DBaseTexture9* texture){if(!texture)return 0;switch(texture->GetType()){case D3DRTYPE_TEXTURE:return wrap(texture,Kind::Texture);case D3DRTYPE_CUBETEXTURE:return wrap(texture,Kind::Cube);case D3DRTYPE_VOLUMETEXTURE:return wrap(texture,Kind::VolumeTexture);default:throw std::runtime_error("Unknown D3D9 base texture type");}}
void retireGuestObject(uint32_t address){
    cachedObjectAddress=0;cachedObject=nullptr;
    auto found=objects.find(address);if(found==objects.end())return;
    static auto s_filter_retire = reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"nfs_d3d9_filter_notify_retired"));
    if(s_filter_retire) s_filter_retire();
    uint32_t table;std::memcpy(&table,pointer(address),4);
    freeGuest(table);freeGuest(address);addresses.erase(found->second.native);objects.erase(found);
}
void attachTextureSurface(uint32_t texture,uint32_t surface){
    if(!surface)return;
    auto& child=objects.at(surface);
    if(!child.parent){child.parent=texture;++objects.at(texture).children;}
    else if(child.parent!=texture)throw std::runtime_error("D3D9 subresource changed parent");
}
// Underground 2 copies its initial depth surface from 0x870854 into the first
// render surface descriptor at 0x86e774, then releases both on device Reset.
// The copy does not call AddRef, so account for that owned descriptor here.
void retainInitialDepthDescriptor(uint32_t outputAddress,uint32_t surfaceAddress){
    if(outputAddress!=0x870854||!surfaceAddress)return;
    auto& surface=objects.at(surfaceAddress);surface.native->AddRef();++surface.references;
}
float floatValue(uint32_t bits){float value;std::memcpy(&value,&bits,4);return value;}
uint32_t floatBits(float value){uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
CompressedView compressionView(uint32_t address){auto found=objects.find(address);return found==objects.end()?CompressedView{}:found->second.compressed;}
// DXVK lets the CPU run at most BackBufferCount+1 frames ahead of the GPU.
// Fewer back buffers (the game's request is logged) serialise a CPU-heavy frame
// with its GPU work (Present waited 7-10 ms with the GPU 67% busy). Two
// back buffers allow DXVK's default latency of 3; the game only ever reads
// back buffer 0, so the extra buffers are invisible to it.
// Launcher "CPU/GPU Sync": 2 (default) or 1 for the lowest input latency.
std::atomic<uint32_t> configuredBackBuffers{2};
// Launcher "Compressed textures: convert on CPU": DXT decoded to RGBA8 even when the
// driver reports BC support, for drivers whose BC sampling is broken.
std::atomic<bool> forceCpuTextures{false};
uint32_t hostBackBufferCount(uint32_t requested){
    uint32_t count=std::max(requested,configuredBackBuffers.load());
    static bool logged=false;
    if(!logged){logged=true;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 game back buffers %u -> host %u",requested,count);}
    return count;
}
D3DPRESENT_PARAMETERS presentation(uint32_t address){
    auto p=static_cast<const uint32_t*>(pointer(address));if(!p)throw std::runtime_error("Null D3D9 presentation parameters");
    D3DPRESENT_PARAMETERS result{};
    uint32_t targetW = s_targetRenderWidth.load(), targetH = s_targetRenderHeight.load();
    result.BackBufferWidth=(targetW>0)?targetW:p[0];
    result.BackBufferHeight=(targetH>0)?targetH:p[1];
    result.BackBufferFormat=static_cast<D3DFORMAT>(p[2]);result.BackBufferCount=hostBackBufferCount(p[3]);
    // Mobile GPUs: Clamp MultiSampleType to NONE to eliminate broken tile-based MSAA resolves (Issue #67)
    result.MultiSampleType=D3DMULTISAMPLE_NONE;result.MultiSampleQuality=0;result.SwapEffect=static_cast<D3DSWAPEFFECT>(p[6]);result.hDeviceWindow=dxvkAndroidGetWindowHandle();
    result.Windowed=FALSE;result.EnableAutoDepthStencil=p[9];result.AutoDepthStencilFormat=static_cast<D3DFORMAT>(p[10]);result.Flags=p[11];result.FullScreen_RefreshRateInHz=p[12];result.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;return result;
}
#if defined(NFS_D3D9_BACKBUFFER_PROBE_ONCE)
void sampleD3D9BackbufferOnce(IDirect3DDevice9* device){
    if(s_debugPresentCalls.load(std::memory_order_relaxed)<120)return;
    uint32_t expected=0;
    if(!s_backbufferProbeState.compare_exchange_strong(expected,4,std::memory_order_relaxed))return;
    const auto started=std::chrono::steady_clock::now();
    auto finish=[&](uint32_t state,HRESULT result){
        uint64_t elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
        s_backbufferProbeMs.store(static_cast<uint32_t>(std::min<uint64_t>(elapsed,UINT32_MAX)),std::memory_order_relaxed);
        s_backbufferProbeResult.store(static_cast<uint32_t>(result),std::memory_order_relaxed);
        s_backbufferProbeState.store(state,std::memory_order_release);
    };
    struct SurfaceGuard{
        IDirect3DSurface9* surface{};
        bool locked{};
        ~SurfaceGuard(){if(surface){if(locked)surface->UnlockRect();surface->Release();}}
    } guard;
    try{
        HRESULT status=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&guard.surface);
        if(FAILED(status)||!guard.surface){finish(2,FAILED(status)?status:E_FAIL);return;}
        D3DSURFACE_DESC desc{};
        status=guard.surface->GetDesc(&desc);
        if(FAILED(status)){finish(2,status);return;}
        s_backbufferProbeWidth.store(desc.Width,std::memory_order_relaxed);
        s_backbufferProbeHeight.store(desc.Height,std::memory_order_relaxed);
        s_backbufferProbeFormat.store(static_cast<uint32_t>(desc.Format),std::memory_order_relaxed);
        if(!desc.Width||!desc.Height){finish(2,D3DERR_INVALIDCALL);return;}
        uint32_t pixelBytes=0,mask32=0x00ffffffu,mask16=0xffffu;
        switch(desc.Format){
        case D3DFMT_A8R8G8B8:case D3DFMT_X8R8G8B8:case D3DFMT_A8B8G8R8:case D3DFMT_X8B8G8R8:pixelBytes=4;break;
        case D3DFMT_A2R10G10B10:case D3DFMT_A2B10G10R10:pixelBytes=4;mask32=0x3fffffffu;break;
        case D3DFMT_R8G8B8:pixelBytes=3;break;
        case D3DFMT_R5G6B5:pixelBytes=2;break;
        case D3DFMT_X1R5G5B5:case D3DFMT_A1R5G5B5:pixelBytes=2;mask16=0x7fffu;break;
        case D3DFMT_X4R4G4B4:case D3DFMT_A4R4G4B4:pixelBytes=2;mask16=0x0fffu;break;
        default:finish(3,D3DERR_NOTAVAILABLE);return;
        }
        D3DLOCKED_RECT locked{};
        status=guard.surface->LockRect(&locked,nullptr,D3DLOCK_READONLY);
        if(FAILED(status)||!locked.pBits||locked.Pitch<=0){finish(2,FAILED(status)?status:D3DERR_INVALIDCALL);return;}
        guard.locked=true;
        uint32_t nonBlack=0;
        constexpr uint32_t grid=16;
        for(uint32_t gy=0;gy<grid;++gy){
            uint32_t y=std::min(desc.Height-1,(2*gy+1)*desc.Height/(2*grid));
            const auto* row=static_cast<const uint8_t*>(locked.pBits)+size_t(y)*static_cast<uint32_t>(locked.Pitch);
            for(uint32_t gx=0;gx<grid;++gx){
                uint32_t x=std::min(desc.Width-1,(2*gx+1)*desc.Width/(2*grid));
                const uint8_t* pixel=row+size_t(x)*pixelBytes;
                bool lit=false;
                if(pixelBytes==4){uint32_t value;std::memcpy(&value,pixel,sizeof(value));lit=(value&mask32)!=0;}
                else if(pixelBytes==3)lit=(pixel[0]|pixel[1]|pixel[2])!=0;
                else{uint16_t value;std::memcpy(&value,pixel,sizeof(value));lit=(value&mask16)!=0;}
                if(lit)++nonBlack;
            }
        }
        status=guard.surface->UnlockRect();guard.locked=false;
        s_backbufferProbePoints.store(grid*grid,std::memory_order_relaxed);
        s_backbufferProbeNonBlack.store(nonBlack,std::memory_order_relaxed);
        finish(SUCCEEDED(status)?1:2,status);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 backbuffer one-shot sample: %u/%u non-black points, %ux%u format=%u, %u ms, result=%08x",
            nonBlack,grid*grid,desc.Width,desc.Height,unsigned(desc.Format),s_backbufferProbeMs.load(std::memory_order_relaxed),uint32_t(status));
    }catch(...){
        finish(2,E_FAIL);
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","D3D9 backbuffer one-shot sample failed with a native exception");
    }
}
#endif
#include "d3d9_locks.inc"
bool compressedFormat(D3DFORMAT format){return format>=D3DFMT_DXT1&&format<=D3DFMT_DXT5;}
D3DFORMAT nativeTextureFormat(IDirect3DDevice9* device,D3DFORMAT format,DWORD usage,D3DRESOURCETYPE type){
    if(!compressedFormat(format)||(usage&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL)))return format;
    if(forceCpuTextures.load())return D3DFMT_A8B8G8R8;
    IDirect3D9* d3d=nullptr;D3DDEVICE_CREATION_PARAMETERS parameters{};D3DDISPLAYMODE mode{};
    if(FAILED(device->GetDirect3D(&d3d)))return format;
    device->GetCreationParameters(&parameters);d3d->GetAdapterDisplayMode(parameters.AdapterOrdinal,&mode);
    HRESULT supported=d3d->CheckDeviceFormat(parameters.AdapterOrdinal,parameters.DeviceType,mode.Format,usage,type,format);d3d->Release();
    return SUCCEEDED(supported)?format:D3DFMT_A8B8G8R8;
}
void describeOriginal(uint32_t address,D3DSURFACE_DESC* desc){auto view=compressionView(address);if(view.texture&&desc)desc->Format=static_cast<D3DFORMAT>(view.texture->format);}
bool classifySceneBucket(uint32_t address,uint32_t& bank,uint32_t& index){
    constexpr uint32_t bases[2]={0x870D30u,0x8712F0u};
    for(uint32_t i=0;i<2;++i){
        const uint32_t end=bases[i]+bucketCountPerBank*8u;
        if(address>=bases[i]&&address<end&&((address-bases[i])%8u)==0){bank=i;index=(address-bases[i])/8u;return true;}
    }
    return false;
}
void captureFirstHelperUP(IDirect3DDevice9* device,uint32_t caller,uint32_t primitiveType,
        uint32_t primitiveCount,uint32_t vertexAddress,uint32_t stride) noexcept {
    if(caller!=0x005C4F70u)return;
    uint32_t expected=0;
    if(!s_helperUPCaptureState.compare_exchange_strong(expected,1,std::memory_order_acq_rel,std::memory_order_relaxed))return;
    D3D9HelperUPSnapshot result{};
    result.state=1;
    result.caller=caller;
    result.guestVertexAddress=vertexAddress;
    result.primitiveType=primitiveType;
    result.primitiveCount=primitiveCount;
    result.stride=stride;
    result.fvfResult=E_POINTER;
    result.textureResult=E_POINTER;
    if(device){
        try{
            DWORD fvf=0;
            HRESULT status=device->GetFVF(&fvf);
            result.fvfResult=static_cast<uint32_t>(status);
            if(SUCCEEDED(status))result.fvf=fvf;
            IDirect3DBaseTexture9* texture=nullptr;
            status=device->GetTexture(0,&texture);
            result.textureResult=static_cast<uint32_t>(status);
            if(SUCCEEDED(status)&&texture){result.textureBound=1;result.textureType=static_cast<uint32_t>(texture->GetType());}
            if(texture)texture->Release();
            auto readStage=[&](D3DTEXTURESTAGESTATETYPE state,uint32_t bit,uint32_t& out){
                DWORD value=0;
                if(SUCCEEDED(device->GetTextureStageState(0,state,&value))){out=value;result.stageStateValidMask|=bit;}
            };
            readStage(D3DTSS_TEXCOORDINDEX,1u<<0,result.texCoordIndex);
            readStage(D3DTSS_TEXTURETRANSFORMFLAGS,1u<<1,result.textureTransformFlags);
            readStage(D3DTSS_COLOROP,1u<<2,result.colorOp);
            readStage(D3DTSS_COLORARG1,1u<<3,result.colorArg1);
            readStage(D3DTSS_COLORARG2,1u<<4,result.colorArg2);
            readStage(D3DTSS_ALPHAOP,1u<<5,result.alphaOp);
            readStage(D3DTSS_ALPHAARG1,1u<<6,result.alphaArg1);
            readStage(D3DTSS_ALPHAARG2,1u<<7,result.alphaArg2);
        }catch(...){ }
    }
    if(primitiveType==6u&&primitiveCount==2u&&stride==32u){
        result.vertexCount=primitiveCount+2u;
        const uint64_t byteCount=uint64_t(result.vertexCount)*uint64_t(stride);
        if(byteCount<=1024u&&byteCount<=UINT32_MAX){
            result.byteCount=static_cast<uint32_t>(byteCount);
            if(guestBase&&vertexAddress&&guest_is_readable(vertexAddress,result.byteCount)){
                uint8_t vertices[128]{};
                std::memcpy(vertices,static_cast<const uint8_t*>(guestBase)+vertexAddress,result.byteCount);
                result.dataValidated=1;
                result.dataStatus=1;
                bool havePosition[3]{},haveTexcoord[4]{};
                bool haveColor[4]{};
                for(uint32_t i=0;i<result.vertexCount;++i){
                    const uint8_t* vertex=vertices+i*stride;
                    float position[3],texcoord[4];
                    uint32_t color=0;
                    std::memcpy(position,vertex,sizeof(position));
                    std::memcpy(&color,vertex+12,sizeof(color));
                    std::memcpy(texcoord,vertex+16,sizeof(texcoord));
                    const uint32_t channels[4]={(color>>16)&255u,(color>>8)&255u,color&255u,(color>>24)&255u};
                    for(uint32_t c=0;c<3;++c)if(std::isfinite(position[c])){
                        if(!havePosition[c]){result.positionMin[c]=result.positionMax[c]=position[c];havePosition[c]=true;}
                        else{result.positionMin[c]=std::min(result.positionMin[c],position[c]);result.positionMax[c]=std::max(result.positionMax[c],position[c]);}
                    }
                    for(uint32_t c=0;c<4;++c){
                        if(std::isfinite(texcoord[c])){
                            if(!haveTexcoord[c]){result.texcoordMin[c]=result.texcoordMax[c]=texcoord[c];haveTexcoord[c]=true;}
                            else{result.texcoordMin[c]=std::min(result.texcoordMin[c],texcoord[c]);result.texcoordMax[c]=std::max(result.texcoordMax[c],texcoord[c]);}
                        }
                        if(!haveColor[c]){result.colorMin[c]=result.colorMax[c]=channels[c];haveColor[c]=true;}
                        else{result.colorMin[c]=std::min(result.colorMin[c],channels[c]);result.colorMax[c]=std::max(result.colorMax[c],channels[c]);}
                    }
                }
                result.state=2;
            }else result.dataStatus=2;
        }
        else result.dataStatus=4;
    }else{result.state=3;result.dataStatus=3;}
    try{
        std::lock_guard<std::mutex> lock(s_probeMutex);
        s_helperUP=result;
    }catch(...){result.state=1;}
    s_helperUPCaptureState.store(result.state,std::memory_order_release);
}
}
void setD3D9Compatibility(bool cpuTextures,unsigned backBuffers){
    forceCpuTextures=cpuTextures;configuredBackBuffers=backBuffers<1?1u:backBuffers>3?3u:backBuffers;
}
void configureD3D9Bridge(void* base,uint32_t (*allocator)(uint32_t),void (*deallocator)(uint32_t)){
    guestBase=base;allocateGuest=allocator;freeGuest=deallocator;
    static auto s_tuner_env = reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"nfs_dxvk_tuner_apply_environment"));
    if(s_tuner_env) s_tuner_env();
#ifdef __ANDROID__
    // android_main provides a writable private cache before creating DXVK.
    // Other bring-up callers can still run without a writable working directory.
    if(!std::getenv("DXVK_STATE_CACHE_PATH"))setenv("DXVK_STATE_CACHE","0",1);
    setenv("DXVK_LOG_PATH","none",1);
#endif
#ifndef __ANDROID__
    if(std::getenv("NFS_D3D9_TEST_DISPLAY")){auto window=dxvkCreateTestWindow();if(!window)throw std::runtime_error("Xlib test display unavailable");setD3D9HostWindow(window);acknowledgeD3D9Surface(true);}
#endif
}
void shutdownD3D9Bridge(){
    for(auto& lock:resourceLocks)freeGuest(lock.second.guest);resourceLocks.clear();
    for(uint32_t kind=15;kind;--kind)for(auto it=objects.begin();it!=objects.end();){
        if(static_cast<uint32_t>(it->second.kind)!=kind){++it;continue;}
        auto object=it->second;for(uint32_t n=0;n<object.references;++n)object.native->Release();
        cachedObjectAddress=0;cachedObject=nullptr;
        uint32_t table;std::memcpy(&table,pointer(it->first),4);freeGuest(table);freeGuest(it->first);addresses.erase(object.native);it=objects.erase(it);
    }
    surfaceRequested=false;surfaceAvailable=false;
#ifndef __ANDROID__
    if(std::getenv("NFS_D3D9_TEST_DISPLAY")){auto window=hostWindow.load();setD3D9HostWindow(nullptr);dxvkDestroyTestWindow(window);}
#endif
}
void setD3D9HostWindow(ANativeWindow* window){
    hostWindow=window;
#ifdef __ANDROID__
    if(window){
        typedef int32_t (*PFN_ANativeWindow_setFrameRate)(ANativeWindow*, float, int8_t);
        static auto pfnSetFrameRate = reinterpret_cast<PFN_ANativeWindow_setFrameRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
        if(pfnSetFrameRate){
            pfnSetFrameRate(window, 60.0f, 0);
        }
    }
#endif
    dxvkAndroidSetWindow(window);
}
bool d3d9RequestsSurface(){return surfaceRequested;}
void acknowledgeD3D9Surface(bool available){surfaceAvailable=available;}
uint32_t createGuestD3D9(uint32_t sdk){
    auto native=Direct3DCreate9(sdk);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","DXVK native Direct3DCreate9 sdk=%u",sdk);
    return wrap(native,Kind::D3D9);
}
// Logs each vertex declaration type the first time the game uses it, flagging the
// ones this GPU cannot fetch (see vertex_formats.h): the evidence for stretched geometry.
void noteVertexDeclaration(const D3DVERTEXELEMENT9* elements){
    if(!elements) return;
    static auto s_hash_decl = reinterpret_cast<uint64_t(*)(const D3DVertexElement9Raw*)>(dlsym(RTLD_DEFAULT,"nfs_d3d9_hash_vertex_declaration"));
    static auto s_stride = reinterpret_cast<uint32_t(*)(const D3DVertexElement9Raw*, uint32_t)>(dlsym(RTLD_DEFAULT,"nfs_d3d9_get_stream_stride"));
    if(s_hash_decl){
        uint64_t hash = s_hash_decl(reinterpret_cast<const D3DVertexElement9Raw*>(elements));
        uint32_t stride0 = s_stride ? s_stride(reinterpret_cast<const D3DVertexElement9Raw*>(elements), 0) : 0;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 vertex decl parsed: hash=%016llx stream0_stride=%u", (unsigned long long)hash, stride0);
    }
    static bool seen[32]{};
    for(unsigned i=0;elements&&i<64&&elements[i].Stream!=0xff;++i){
        unsigned type=elements[i].Type;
        if(type>=32||seen[type])continue;
        seen[type]=true;
        const char* name="?";for(auto& entry:kD3DVertexFormats)if(entry.type==type)name=entry.name;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 game vertex type %s (usage %u)",name,unsigned(elements[i].Usage));
    }
}
// Set when DXVK could not rebuild the swap chain in Reset. DXVK's TestCooperativeLevel
// always reports D3D_OK, so the game would keep drawing on the half-reset device and
// crash (null dereference, Mali reports); until a Reset succeeds the device reads as
// lost and needing a reset, which the game handles as on Windows.
bool deviceResetPending=false;
void* getGameCoreHandle() {
    static void* s_core_handle = nullptr;
    if (!s_core_handle) {
        s_core_handle = dlopen("libgame_core.so", RTLD_NOLOAD);
        if (!s_core_handle) s_core_handle = dlopen("libgame_core.so", RTLD_NOW | RTLD_GLOBAL);
        if (s_core_handle) {
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU2", "Loaded libgame_core.so successfully with RTLD_GLOBAL");
        }
    }
    return s_core_handle;
}
void* getGraphicsCacheHandle() {
    static void* s_handle = nullptr;
    if (!s_handle) {
        s_handle = dlopen("libgraphics_cache.so", RTLD_NOLOAD);
        if (!s_handle) s_handle = dlopen("libgraphics_cache.so", RTLD_NOW | RTLD_GLOBAL);
        if (s_handle) {
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO, "NFSU2", "Loaded libgraphics_cache.so successfully with RTLD_GLOBAL");
        } else {
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN, "NFSU2", "dlopen libgraphics_cache.so: %s", dlerror());
        }
    }
    return s_handle;
}
void* resolveGraphicsCacheSymbol(const char* name) {
    void* h_core = getGameCoreHandle();
    void* sym = h_core ? dlsym(h_core, name) : nullptr;
    if (!sym) {
        void* h_gfx = getGraphicsCacheHandle();
        sym = h_gfx ? dlsym(h_gfx, name) : nullptr;
    }
    if (!sym) sym = dlsym(RTLD_DEFAULT, name);
    return sym;
}

static IDirect3DDevice9* s_active_d3d9_device = nullptr;

extern "C" void nfs_d3d9_set_enhanced_contrast_ramp(int enable) {
    if (!s_active_d3d9_device) return;
    D3DGAMMARAMP ramp;
    if (enable) {
        static auto s_lr_ramp_fn = reinterpret_cast<int(*)(uint16_t*)>(resolveGraphicsCacheSymbol("nfs_lightroom_get_ramp"));
        if (s_lr_ramp_fn && s_lr_ramp_fn(reinterpret_cast<uint16_t*>(ramp.red)) == 0) {
            std::memcpy(ramp.green, ramp.red, sizeof(ramp.red));
            std::memcpy(ramp.blue, ramp.red, sizeof(ramp.red));
        } else {
            // Lightroom photographic tone reproduction:
            // EV+0.01, Contrast+15, Highlight-35, Shadow-12, White+16, Black+22, Dehaze+20
            for (int i = 0; i < 256; i++) {
                float c = i / 255.0f;
                c = std::clamp(c * 1.00695555f, 0.0f, 1.0f);
                float s_curve = c * c * (3.0f - 2.0f * c);
                c = c * 0.85f + s_curve * 0.15f;
                float th = std::clamp((c - 0.5f) / 0.5f, 0.0f, 1.0f);
                c += -0.35f * 0.20f * (th * th * (3.0f - 2.0f * th)) * (c - 0.5f);
                float tw = std::clamp((c - 0.75f) / 0.25f, 0.0f, 1.0f);
                c += 0.16f * 0.15f * (tw * tw * (3.0f - 2.0f * tw)) * (1.0f - c);
                float ts = std::clamp((0.5f - c) / 0.5f, 0.0f, 1.0f);
                c += -0.12f * 0.18f * (ts * ts * (3.0f - 2.0f * ts)) * (0.5f - c);
                float tb = std::clamp((0.25f - c) / 0.25f, 0.0f, 1.0f);
                c += 0.22f * 0.10f * (tb * tb * (3.0f - 2.0f * tb)) * c;
                c -= 0.20f * 0.12f * (1.0f - c) * c;
                WORD w = static_cast<WORD>(std::clamp(c, 0.0f, 1.0f) * 65535.0f);
                ramp.red[i] = w;
                ramp.green[i] = w;
                ramp.blue[i] = w;
            }
        }
    } else {
        for (int i = 0; i < 256; i++) {
            WORD w = static_cast<WORD>(i * 257);
            ramp.red[i] = w;
            ramp.green[i] = w;
            ramp.blue[i] = w;
        }
    }
    s_active_d3d9_device->SetGammaRamp(0, D3DSGR_NO_CALIBRATION, &ramp);
}

uint32_t dispatchGuestD3D9(uint32_t token,const uint32_t* args,uint32_t* argumentCount,uint32_t guestCallerFunction){
    getGraphicsCacheHandle();
    static bool s_contrast_ramp_reg = false;
    if (!s_contrast_ramp_reg) {
        s_contrast_ramp_reg = true;
        auto reg_fn = reinterpret_cast<void(*)(void(*)(int))>(resolveGraphicsCacheSymbol("nfs_contrast_regulator_register_callback"));
        if (!reg_fn) reg_fn = reinterpret_cast<void(*)(void(*)(int))>(resolveGraphicsCacheSymbol("nfs_contrast_register_ramp_callback"));
        if (reg_fn) reg_fn(nfs_d3d9_set_enhanced_contrast_ramp);
    }
    uint32_t kind=(token-methodBase)/4096,method=((token-methodBase)%4096)/16;
    Object* entry=findObject(args[0]);
    if(!entry||static_cast<uint32_t>(entry->kind)!=kind){
        NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU2","Invalid D3D9 COM call object=%08x token=%08x kind=%u method=%u actualKind=%u",args[0],token,kind,method,
            entry?static_cast<uint32_t>(entry->kind):0);
        throw std::runtime_error("Invalid guest D3D9 COM object");
    }
    auto object=entry->native;
    // Android may replace ANativeWindow after backgrounding, even at the same
    // pointer address. Override the destination with this surface generation so
    // DXVK recreates its presenter while keeping the game's device/resources.
    if((kind==2&&method==17)||(kind==13&&method==3)){
        *argumentCount=kind==2?5:6;s_debugPresentCalls.fetch_add(1,std::memory_order_relaxed);auto window=dxvkAndroidGetWindowHandle();
        if(!window||deviceResetPending){s_debugLastPresentResult.store(uint32_t(D3DERR_DEVICELOST),std::memory_order_relaxed);return D3DERR_DEVICELOST;}
        if(kind==2)captureActiveTarget(static_cast<IDirect3DDevice9*>(object),guestCallerFunction);
        static void(*s_cam_restore)(void*) = nullptr;
        if(!s_cam_restore) s_cam_restore = reinterpret_cast<void(*)(void*)>(resolveGraphicsCacheSymbol("nfs_camera_stick_restore_frame"));
        if(s_cam_restore) s_cam_restore(guestBase);
        static void(*s_telemetry_present)(uint64_t) = nullptr;
        if(!s_telemetry_present) s_telemetry_present = reinterpret_cast<void(*)(uint64_t)>(resolveGraphicsCacheSymbol("nfs_telemetry_record_present"));
        if(s_telemetry_present) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            s_telemetry_present(now_ns);
        }
        static int(*s_contrast_post)(void*) = nullptr;
        if(!s_contrast_post) s_contrast_post = reinterpret_cast<int(*)(void*)>(resolveGraphicsCacheSymbol("nfs_contrast_regulator_post_frame"));
        if(s_contrast_post) s_contrast_post(guestBase);
        else {
            static uint32_t(*s_contrast_sync)(void*) = nullptr;
            if(!s_contrast_sync) s_contrast_sync = reinterpret_cast<uint32_t(*)(void*)>(resolveGraphicsCacheSymbol("nfs_contrast_sync_ingame"));
            if(s_contrast_sync) s_contrast_sync(guestBase);
        }

#if defined(NFS_D3D9_BACKBUFFER_PROBE_ONCE)
        if(kind==2)sampleD3D9BackbufferOnce(static_cast<IDirect3DDevice9*>(object));
#endif
        auto source=static_cast<const RECT*>(pointer(args[1])),destination=static_cast<const RECT*>(pointer(args[2]));
        auto dirty=static_cast<const RGNDATA*>(pointer(args[4]));
        HRESULT status=kind==2?static_cast<IDirect3DDevice9*>(object)->Present(source,destination,window,dirty):
            static_cast<IDirect3DSwapChain9*>(object)->Present(source,destination,window,dirty,args[5]);
        s_debugLastPresentResult.store(uint32_t(status),std::memory_order_relaxed);return status;
    }
    // Log the first calls of each method; tokens span 0x7e000000..0x7e00ffff.
    static uint8_t methodCalls[4096];auto& calls=methodCalls[((token-methodBase)/16)&4095];
    uint32_t renderTraceOrdinal=0;
    const bool traceRenderMethod=kind==2&&(method==41||method==42||method==43||method==82);
    if(calls<3&&++calls){
        if(traceRenderMethod)renderTraceOrdinal=calls;
        else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 COM kind=%u method=%u",kind,method);
    }
    // Counts belong to the guest COM wrapper. DXVK texture subresources
    // delegate AddRef/Release to their parent texture, so native counts can
    // include ownership through a different guest object. Returning them made
    // the game's release-until-zero loop call an already freed surface wrapper.
    if(method==1){*argumentCount=1;object->AddRef();return ++entry->references;}
    if(method==2){*argumentCount=1;
        // The game's detail rebuild releases the cube parent after every face
        // lookup, although only its first Release owns a parent reference.
        // Keep the callable wrapper until the acquired faces are released.
        if(!entry->references){
            if(entry->children)return 0;
            throw std::runtime_error("D3D9 COM Release without owned reference");
        }
        auto nativeReferences=object->Release();uint32_t references=--entry->references;
        if(kind==5)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Surface Release object=%08x guestRefs=%u nativeRefs=%u",args[0],references,uint32_t(nativeReferences));
        if(!references){
            if(entry->children)return uint32_t(nativeReferences);
            auto parent=entry->parent;retireGuestObject(args[0]);
            if(parent){auto owner=objects.find(parent);if(owner!=objects.end()&&!--owner->second.children&&!owner->second.references)retireGuestObject(parent);}
        }
        return references;}
    if(method==0){
        *argumentCount=3;IUnknown* result=nullptr;HRESULT status=object->QueryInterface(*static_cast<const GUID*>(pointer(args[1])),reinterpret_cast<void**>(&result));
        if(SUCCEEDED(status))write(args[2],wrap(result,entry->kind));else write(args[2],0);return status;
    }
    if(kind==1){
        auto d3d=static_cast<IDirect3D9*>(object);
        switch(method){
        case 3:*argumentCount=2;return D3DERR_NOTAVAILABLE;
        case 4:*argumentCount=1;return d3d->GetAdapterCount();
        case 5:*argumentCount=4;return d3d->GetAdapterIdentifier(args[1],args[2],static_cast<D3DADAPTER_IDENTIFIER9*>(pointer(args[3])));
        case 6:*argumentCount=3;return d3d->GetAdapterModeCount(args[1],static_cast<D3DFORMAT>(args[2]));
        case 7:*argumentCount=5;return d3d->EnumAdapterModes(args[1],static_cast<D3DFORMAT>(args[2]),args[3],static_cast<D3DDISPLAYMODE*>(pointer(args[4])));
        case 8:*argumentCount=3;return d3d->GetAdapterDisplayMode(args[1],static_cast<D3DDISPLAYMODE*>(pointer(args[2])));
        case 9:*argumentCount=6;return d3d->CheckDeviceType(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]),args[5]);
        case 10:{*argumentCount=7;auto format=static_cast<D3DFORMAT>(args[6]);auto type=static_cast<D3DRESOURCETYPE>(args[5]);
            HRESULT status=d3d->CheckDeviceFormat(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),args[4],type,format);
            if(FAILED(status)&&compressedFormat(format)&&!(args[4]&(D3DUSAGE_RENDERTARGET|D3DUSAGE_DEPTHSTENCIL))&&(type==D3DRTYPE_TEXTURE||type==D3DRTYPE_CUBETEXTURE||type==D3DRTYPE_SURFACE))
                status=d3d->CheckDeviceFormat(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),args[4],type,D3DFMT_A8B8G8R8);
            return status;}
        case 11:*argumentCount=7;if(pointer(args[6]))*static_cast<DWORD*>(pointer(args[6]))=0;return D3DERR_NOTAVAILABLE;
        case 12:*argumentCount=6;return d3d->CheckDepthStencilMatch(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]),static_cast<D3DFORMAT>(args[5]));
        case 13:*argumentCount=5;return d3d->CheckDeviceFormatConversion(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DFORMAT>(args[3]),static_cast<D3DFORMAT>(args[4]));
        case 14:*argumentCount=4;return d3d->GetDeviceCaps(args[1],static_cast<D3DDEVTYPE>(args[2]),static_cast<D3DCAPS9*>(pointer(args[3])));
        case 15:*argumentCount=2;return d3d->GetAdapterMonitor(args[1])?0xf0000300:0;
        case 16:{
            *argumentCount=7;auto p=static_cast<const uint32_t*>(pointer(args[5]));if(!p||!args[6])return D3DERR_INVALIDCALL;
            if(!hostWindow.load())throw std::runtime_error("D3D9 CreateDevice requires Android surface");
            surfaceRequested=true;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(!surfaceAvailable){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Android surface handover timed out");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            auto window=dxvkAndroidGetWindowHandle();if(!window)return D3DERR_DEVICELOST;
            uint32_t targetW = s_targetRenderWidth.load(), targetH = s_targetRenderHeight.load();
            D3DPRESENT_PARAMETERS parameters{};
            parameters.BackBufferWidth=(targetW>0)?targetW:p[0];
            parameters.BackBufferHeight=(targetH>0)?targetH:p[1];
            parameters.BackBufferFormat=static_cast<D3DFORMAT>(p[2]);parameters.BackBufferCount=hostBackBufferCount(p[3]);
            // Mobile GPUs: Clamp MultiSampleType to NONE to eliminate broken tile-based MSAA resolves (Issue #67)
            parameters.MultiSampleType=D3DMULTISAMPLE_NONE;parameters.MultiSampleQuality=0;parameters.SwapEffect=static_cast<D3DSWAPEFFECT>(p[6]);parameters.hDeviceWindow=window;
            parameters.Windowed=FALSE;parameters.EnableAutoDepthStencil=p[9];parameters.AutoDepthStencilFormat=static_cast<D3DFORMAT>(p[10]);parameters.Flags=p[11];parameters.FullScreen_RefreshRateInHz=p[12];parameters.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
            IDirect3DDevice9* device=nullptr;s_debugCreateAttempts.fetch_add(1,std::memory_order_relaxed);HRESULT status=d3d->CreateDevice(args[1],static_cast<D3DDEVTYPE>(args[2]),window,args[4],&parameters,&device);
            if (FAILED(status)) {
                // Device creation fallback for PowerVR GE8322 / low-end mobile Vulkan implementations:
                // Fall back to 16-bit depth buffer (D3DFMT_D16) and disable multisampling
                D3DPRESENT_PARAMETERS fallback = parameters;
                fallback.AutoDepthStencilFormat = D3DFMT_D16;
                fallback.MultiSampleType = D3DMULTISAMPLE_NONE;
                fallback.MultiSampleQuality = 0;
                s_debugCreateAttempts.fetch_add(1,std::memory_order_relaxed);HRESULT fbStatus = d3d->CreateDevice(args[1],static_cast<D3DDEVTYPE>(args[2]),window,args[4],&fallback,&device);
                if (SUCCEEDED(fbStatus)) {
                    status = fbStatus;
                    parameters = fallback;
                    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 CreateDevice succeeded with D16 depth fallback");
                }
            }
            s_debugLastCreateResult.store(uint32_t(status),std::memory_order_relaxed);
            if(SUCCEEDED(status))s_debugCreateSuccesses.fetch_add(1,std::memory_order_relaxed);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 backbuffer %ux%u CreateDevice result=%08x",parameters.BackBufferWidth,parameters.BackBufferHeight,uint32_t(status));
            // Without a device the game carries on and faults later in its own
            // code (PowerVR GE8322 reports): after its retry also fails, stop
            // here with the reason instead.
            static unsigned failures=0;failures=FAILED(status)?failures+1:0;
            if(FAILED(status)&&failures<2)return status;
            if(FAILED(status))throw std::runtime_error("The GPU could not create the graphics device (Vulkan, CreateDevice "+
                std::to_string(uint32_t(status))+"). Use \"Check driver\" in the launcher to see what is missing.");
            invalidateActiveTarget();
            write(args[6],wrap(device,Kind::Device));return status;
        }
        }
    }
    if(kind==2){
        static auto s_filter_state = []() -> int(*)(uint32_t,const uint32_t*,uint32_t*) {
            void* h = getGraphicsCacheHandle();
            auto fn = h ? reinterpret_cast<int(*)(uint32_t,const uint32_t*,uint32_t*)>(dlsym(h,"nfs_d3d9_filter_state")) : nullptr;
            if(!fn) fn = reinterpret_cast<int(*)(uint32_t,const uint32_t*,uint32_t*)>(dlsym(RTLD_DEFAULT,"nfs_d3d9_filter_state"));
            return fn;
        }();
        if(s_filter_state && s_filter_state(method, args, argumentCount)){
            return D3D_OK;
        }
        auto device=static_cast<IDirect3DDevice9*>(object);
        s_active_d3d9_device = device;
        switch(method){
        case 9:{*argumentCount=2;D3DDEVICE_CREATION_PARAMETERS p{};HRESULT status=device->GetCreationParameters(&p);if(SUCCEEDED(status)){write(args[1],p.AdapterOrdinal);write(args[1]+4,p.DeviceType);write(args[1]+8,0xf0000301);write(args[1]+12,p.BehaviorFlags);}return status;}
        case 13:{*argumentCount=3;auto p=presentation(args[1]);IDirect3DSwapChain9* result=nullptr;HRESULT status=device->CreateAdditionalSwapChain(&p,&result);write(args[2],SUCCEEDED(status)?wrap(result,Kind::SwapChain):0);return status;}
        case 16:{*argumentCount=2;auto p=presentation(args[1]);HRESULT status=D3DERR_DEVICELOST;
            static auto s_filter_forget = reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"nfs_d3d9_filter_forget_shadow"));
            if(s_filter_forget) s_filter_forget();
            static auto s_rs_reset = reinterpret_cast<void(*)()>(dlsym(RTLD_DEFAULT,"nfs_rain_splatter_reset"));
            if(s_rs_reset) s_rs_reset();
            // DXVK throws (DxvkError) when it cannot rebuild the swap chain; Windows
            // would report a lost device, which the game retries (Mali G615 report).
            // The old Android window can stay busy for a moment: retry once before giving up.
            for(int attempt=0;attempt<2;++attempt){
                try{status=device->Reset(&p);break;}catch(...){
                    NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU2","D3D9 Reset raised a DXVK error (attempt %d)",attempt+1);status=D3DERR_DEVICELOST;
                    if(!attempt)std::this_thread::sleep_for(std::chrono::milliseconds(100));}
            }
            deviceResetPending=status==D3DERR_DEVICELOST;
            invalidateActiveTarget();
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 Reset backbuffer %ux%u result=%08x",p.BackBufferWidth,p.BackBufferHeight,uint32_t(status));return status;}
        case 23:{*argumentCount=9;if(args[8])throw std::runtime_error("Shared texture handle pending");if(!args[7])return D3DERR_INVALIDCALL;
            auto original=static_cast<D3DFORMAT>(args[5]),format=nativeTextureFormat(device,original,args[4],D3DRTYPE_TEXTURE);IDirect3DTexture9* texture=nullptr;
            HRESULT status=device->CreateTexture(args[1],args[2],args[3],args[4],format,static_cast<D3DPOOL>(args[6]),&texture,nullptr);
            uint32_t address=SUCCEEDED(status)?wrap(texture,Kind::Texture):0;
            if(address&&format!=original){objects.at(address).compressed.texture=std::make_shared<CompressedTexture>(original,args[1],args[2],texture->GetLevelCount(),1);NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","CPU DXT fallback texture %ux%u format=%u",args[1],args[2],original);}
            write(args[7],address);return status;}
        case 25:{*argumentCount=8;if(args[7])throw std::runtime_error("Shared cube handle pending");if(!args[6])return D3DERR_INVALIDCALL;
            auto original=static_cast<D3DFORMAT>(args[4]),format=nativeTextureFormat(device,original,args[3],D3DRTYPE_CUBETEXTURE);IDirect3DCubeTexture9* texture=nullptr;
            HRESULT status=device->CreateCubeTexture(args[1],args[2],args[3],format,static_cast<D3DPOOL>(args[5]),&texture,nullptr);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","D3D9 CreateCubeTexture edge=%u levels=%u usage=%08x format=%u nativeFormat=%u pool=%u status=%08x output=%08x",args[1],args[2],args[3],unsigned(original),unsigned(format),args[5],uint32_t(status),args[6]);
            uint32_t address=SUCCEEDED(status)?wrap(texture,Kind::Cube):0;
            if(address&&format!=original)objects.at(address).compressed.texture=std::make_shared<CompressedTexture>(original,args[1],args[1],texture->GetLevelCount(),6);
            write(args[6],address);return status;}
#include "d3d9_device_methods.inc"
    }}
    if(kind==3){auto texture=static_cast<IDirect3DTexture9*>(object);
        if(method==17){*argumentCount=3;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[2]));HRESULT status=texture->GetLevelDesc(args[1],desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==18){*argumentCount=3;IDirect3DSurface9* surface=nullptr;HRESULT status=texture->GetSurfaceLevel(args[1],&surface);uint32_t address=SUCCEEDED(status)?wrap(surface,Kind::Surface):0;if(address){attachTextureSurface(args[0],address);auto view=compressionView(args[0]);view.level=args[1];objects.at(address).compressed=view;}write(args[2],address);return status;}
        if(method==19){*argumentCount=5;return lockTexture(args[0],args[1],args[2],static_cast<const RECT*>(pointer(args[3])),args[4]);}
        if(method==20){*argumentCount=2;copyLockBack(args[0],args[1]);HRESULT status=texture->UnlockRect(args[1]);releaseLock(args[0],args[1]);return status;}}
    if(kind==4){auto texture=static_cast<IDirect3DCubeTexture9*>(object);
        if(method==17){*argumentCount=3;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[2]));HRESULT status=texture->GetLevelDesc(args[1],desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==18){*argumentCount=4;IDirect3DSurface9* surface=nullptr;HRESULT status=texture->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(args[1]),args[2],&surface);uint32_t address=SUCCEEDED(status)?wrap(surface,Kind::Surface):0;if(address){attachTextureSurface(args[0],address);auto view=compressionView(args[0]);view.face=args[1];view.level=args[2];objects.at(address).compressed=view;}write(args[3],address);return status;}
        if(method==19){*argumentCount=6;return lockCube(args[0],args[1],args[2],args[3],static_cast<const RECT*>(pointer(args[4])),args[5]);}
        if(method==20){*argumentCount=3;uint32_t subresource=args[1]*32+args[2];copyLockBack(args[0],subresource);HRESULT status=texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(args[1]),args[2]);releaseLock(args[0],subresource);return status;}}
    if(kind==5){auto surface=static_cast<IDirect3DSurface9*>(object);
        if(method==12){*argumentCount=2;auto desc=static_cast<D3DSURFACE_DESC*>(pointer(args[1]));HRESULT status=surface->GetDesc(desc);if(SUCCEEDED(status))describeOriginal(args[0],desc);return status;}
        if(method==13){*argumentCount=4;return lockSurface(args[0],args[1],static_cast<const RECT*>(pointer(args[2])),args[3]);}
        if(method==14){*argumentCount=1;copyLockBack(args[0],0);HRESULT status=surface->UnlockRect();releaseLock(args[0],0);return status;}}
    if(kind==6||kind==7){
        if(method==11){*argumentCount=5;return kind==6?lockBuffer<IDirect3DVertexBuffer9,D3DVERTEXBUFFER_DESC>(args[0],args[1],args[2],args[3],args[4]):lockBuffer<IDirect3DIndexBuffer9,D3DINDEXBUFFER_DESC>(args[0],args[1],args[2],args[3],args[4]);}
        if(method==12){*argumentCount=1;copyLockBack(args[0],0);HRESULT status=kind==6?static_cast<IDirect3DVertexBuffer9*>(object)->Unlock():static_cast<IDirect3DIndexBuffer9*>(object)->Unlock();releaseLock(args[0],0);return status;}}
#include "d3d9_resource_methods.inc"
    throw std::runtime_error("D3D9 guest method pending: kind="+std::to_string(kind)+" slot="+std::to_string(method));
}

extern "C" void nfsD3D9GetDebugStats(uint64_t* createAttempts,uint64_t* createSuccesses,
        uint32_t* lastCreateResult,uint64_t* presentCalls,uint32_t* lastPresentResult){
    if(createAttempts)*createAttempts=s_debugCreateAttempts.load(std::memory_order_relaxed);
    if(createSuccesses)*createSuccesses=s_debugCreateSuccesses.load(std::memory_order_relaxed);
    if(lastCreateResult)*lastCreateResult=s_debugLastCreateResult.load(std::memory_order_relaxed);
    if(presentCalls)*presentCalls=s_debugPresentCalls.load(std::memory_order_relaxed);
    if(lastPresentResult)*lastPresentResult=s_debugLastPresentResult.load(std::memory_order_relaxed);
}

extern "C" void nfsD3D9RecordGuestRenderProbe(const NfsGuestRenderProbe* probe){
    if(!probe)return;
    try{
        std::lock_guard<std::mutex> lock(s_probeMutex);
        auto& d=s_guestRender;
        ++d.calls;
        d.bucketChecks+=probe->bucketChecks;
        d.bucketsEmpty+=probe->bucketsEmpty;
        d.bucketsNonempty+=probe->bucketsNonempty;
        d.routeIndexed+=probe->routeIndexed;
        d.routeUp+=probe->routeUp;
        d.indexedCountPositive+=probe->indexedCountPositive;
        d.indexedCountNonPositive+=probe->indexedCountNonPositive;
        d.upCountPositive+=probe->upCountPositive;
        d.upCountNonPositive+=probe->upCountNonPositive;
        d.lastBucketAddress=probe->lastBucketAddress;
        d.lastBucketHead=probe->lastBucketHead;
        d.lastObjectAddress=probe->lastObjectAddress;
        d.lastRouteValue=probe->lastRouteValue;
        d.lastIndexedCount=probe->lastIndexedCount;
        d.maxIndexedCount=std::max(d.maxIndexedCount,probe->maxIndexedCount);
        d.lastUpCount=probe->lastUpCount;
        d.maxUpCount=std::max(d.maxUpCount,probe->maxUpCount);
    }catch(...){ }
}
extern "C" void nfsD3D9RecordGuestGlobal8383AC(uint32_t value){
    s_guestGlobal8383ACValue.store(value,std::memory_order_relaxed);
    s_guestGlobal8383ACReads.fetch_add(1,std::memory_order_relaxed);
}
extern "C" void nfsD3D9RecordGuestFlowProbe(uint32_t event,uint32_t value,uint32_t auxiliary,uint32_t outcome){
    if(event>=NFS_GUEST_FLOW_EVENT_COUNT)return;
    auto& stat=s_guestFlow[event];
    stat.hits.fetch_add(1,std::memory_order_relaxed);
    if(outcome==NFS_GUEST_FLOW_TAKEN)stat.taken.fetch_add(1,std::memory_order_relaxed);
    else if(outcome==NFS_GUEST_FLOW_NOT_TAKEN)stat.notTaken.fetch_add(1,std::memory_order_relaxed);
    stat.lastValue.store(value,std::memory_order_relaxed);
    stat.lastAuxiliary.store(auxiliary,std::memory_order_relaxed);
    stat.lastOutcome.store(outcome,std::memory_order_relaxed);
}
extern "C" void nfsD3D9RecordGuestStateTrace(uint32_t kind,uint32_t writerPc,uint32_t oldValue,uint32_t newValue,uint32_t capabilityValue,uint32_t callerFunc,uint32_t guestEsp,uint32_t guestEbp,uint32_t guestStackTop){
    if(kind>=NFS_GUEST_STATE_TRACE_KIND_COUNT)return;
    static const char* const names[NFS_GUEST_STATE_TRACE_KIND_COUNT]={"direct_write","registry_copy","ba_entry","ba_exit","caa_entry"};
    const bool isWrite=kind==NFS_GUEST_STATE_TRACE_DIRECT_WRITE||kind==NFS_GUEST_STATE_TRACE_REGISTRY_COPY;
    if(isWrite){
        if(s_guestStateTraceWrites.fetch_add(1,std::memory_order_relaxed)>=kGuestStateTraceWriteLimit){
            if(!s_guestStateTraceWriteLimitNotified.exchange(true,std::memory_order_relaxed))
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","B24_STATE_TRACE write limit reached (%u); later writes omitted",kGuestStateTraceWriteLimit);
            return;
        }
    }else{
        const uint32_t index=kind-NFS_GUEST_STATE_TRACE_BA_ENTRY;
        const uint64_t key=(uint64_t(newValue)<<32)|capabilityValue;
        if(s_guestStateTraceSnapshotSeen[index].load(std::memory_order_relaxed)&&s_guestStateTraceSnapshotKeys[index].load(std::memory_order_relaxed)==key)return;
        s_guestStateTraceSnapshotKeys[index].store(key,std::memory_order_relaxed);
        s_guestStateTraceSnapshotSeen[index].store(true,std::memory_order_relaxed);
        if(s_guestStateTraceSnapshots.fetch_add(1,std::memory_order_relaxed)>=kGuestStateTraceSnapshotLimit){
            if(!s_guestStateTraceSnapshotLimitNotified.exchange(true,std::memory_order_relaxed))
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","B24_STATE_TRACE snapshot limit reached (%u); later snapshots omitted",kGuestStateTraceSnapshotLimit);
            return;
        }
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","B24_STATE_TRACE kind=%s writer_pc=%08x old=%08x new=%08x cap_87099c=%08x caller_func=%08x guest_esp=%08x guest_ebp=%08x stack_top=%08x",names[kind],writerPc,oldValue,newValue,capabilityValue,callerFunc,guestEsp,guestEbp,guestStackTop);
}
extern "C" void nfsD3D9RecordGuestBucketProbe(uint32_t helper,uint32_t event,uint32_t bucketHead,uint32_t node){
    if(helper>=bucketHelperCount)return;
    uint32_t bank=UINT32_MAX,index=UINT32_MAX;
    const bool validBucket=classifySceneBucket(bucketHead,bank,index);
    switch(event){
        case NFS_GUEST_BUCKET_EVENT_CALL:
            s_bucketCalls[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        case NFS_GUEST_BUCKET_EVENT_EXISTING_SKIP:
            s_bucketExistingSkips[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        case NFS_GUEST_BUCKET_EVENT_PRIMARY_ALLOC_NULL:
            s_bucketPrimaryNull[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        case NFS_GUEST_BUCKET_EVENT_AUX_ALLOC_NULL:
            s_bucketAuxiliaryNull[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        case NFS_GUEST_BUCKET_EVENT_BUCKET_SELECTED:
            s_bucketSelections[helper].fetch_add(1,std::memory_order_relaxed);
            s_bucketLastAddress[helper].store(bucketHead,std::memory_order_relaxed);
            s_bucketLastNode[helper].store(node,std::memory_order_relaxed);
            s_bucketLastBank[helper].store(validBucket?bank:UINT32_MAX,std::memory_order_relaxed);
            s_bucketLastIndex[helper].store(validBucket?index:UINT32_MAX,std::memory_order_relaxed);
            if(validBucket){
                s_bucketBankSelections[helper][bank].fetch_add(1,std::memory_order_relaxed);
                if(s_bucketHistogram[helper][bank][index].fetch_add(1,std::memory_order_relaxed)==0)
                    s_bucketTouched[helper][bank].fetch_add(1,std::memory_order_relaxed);
            }else s_bucketInvalidAddress[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        case NFS_GUEST_BUCKET_EVENT_BUCKET_LINKED:
            if(node){
                s_bucketLinkedNonNull[helper].fetch_add(1,std::memory_order_relaxed);
                if(validBucket)s_bucketBankLinked[helper][bank].fetch_add(1,std::memory_order_relaxed);
            }else s_bucketLinkedNull[helper].fetch_add(1,std::memory_order_relaxed);
            break;
        default:
            break;
    }
}
extern "C" void nfsD3D9RecordGuestHelperTarget(uint32_t deviceToken,uint32_t tokenReadable,uint32_t caller,uint32_t stage){
    if(stage>1)return;
    D3D9RenderTargetSnapshot result{};
    result.lastCaller=caller;
    result.lastResult=E_FAIL;
    try{
        {
            std::lock_guard<std::mutex> lock(s_probeMutex);
            D3D9RenderTargetSnapshot pending{};
            pending.state=1;
            pending.lastCaller=caller;
            pending.lastResult=0xffffffffu;
            s_helperTargets[stage]=pending;
        }
        if(!tokenReadable){
            result.state=3;
            result.lastResult=E_POINTER;
        }else{
            Object* entry=findObject(deviceToken);
            if(!entry||entry->kind!=Kind::Device||!entry->native){
                result.state=3;
                result.lastResult=D3DERR_INVALIDCALL;
            }else{
                auto* device=static_cast<IDirect3DDevice9*>(entry->native);
                struct SurfaceGuard{IDirect3DSurface9* surface{};~SurfaceGuard(){if(surface)surface->Release();}} target,backbuffer;
                try{
                    HRESULT status=device->GetRenderTarget(0,&target.surface);
                    if(FAILED(status)||!target.surface){
                        result.state=3;
                        result.lastResult=static_cast<uint32_t>(FAILED(status)?status:E_FAIL);
                    }else{
                        D3DSURFACE_DESC desc{};
                        status=target.surface->GetDesc(&desc);
                        if(FAILED(status)){
                            result.state=3;
                            result.lastResult=static_cast<uint32_t>(status);
                        }else{
                            uint32_t implicit=0;
                            HRESULT backStatus=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer.surface);
                            if(SUCCEEDED(backStatus)&&backbuffer.surface)implicit=backbuffer.surface==target.surface?1u:2u;
                            result.state=2;
                            result.width=desc.Width;
                            result.height=desc.Height;
                            result.format=static_cast<uint32_t>(desc.Format);
                            result.usage=desc.Usage;
                            result.pool=static_cast<uint32_t>(desc.Pool);
                            result.multisample=static_cast<uint32_t>(desc.MultiSampleType);
                            result.multisampleQuality=desc.MultiSampleQuality;
                            result.type=static_cast<uint32_t>(desc.Type);
                            result.isImplicitBackbuffer=implicit;
                            result.lastResult=static_cast<uint32_t>(status);
                        }
                    }
                }catch(...){
                    result.state=3;
                    result.lastResult=E_FAIL;
                }
            }
        }
    }catch(...){
        result.state=3;
        result.lastResult=E_FAIL;
    }
    try{
        std::lock_guard<std::mutex> lock(s_probeMutex);
        s_helperTargets[stage]=result;
    }catch(...){ }
}
extern "C" void nfsD3D9GetRenderSnapshot(D3D9RenderSnapshot* snapshot){
    if(!snapshot)return;
    for(uint32_t i=0;i<renderStatCount;++i){
        auto& method=snapshot->methods[i];
        method.calls=s_renderCalls[i].load(std::memory_order_relaxed);
        method.forwarded=s_renderForwarded[i].load(std::memory_order_relaxed);
        method.culled=s_renderCulled[i].load(std::memory_order_relaxed);
        method.lastCaller=s_renderLastCaller[i].load(std::memory_order_relaxed);
        method.lastResult=method.calls?s_renderLastResult[i].load(std::memory_order_relaxed):0xffffffffu;
        method.lastPath=method.calls?s_renderLastPath[i].load(std::memory_order_relaxed):0u;
    }
    auto& target=snapshot->activeTarget;
    target.state=s_activeTargetState.load(std::memory_order_acquire);
    target.width=target.state==2?s_activeTargetWidth.load(std::memory_order_relaxed):0;
    target.height=target.state==2?s_activeTargetHeight.load(std::memory_order_relaxed):0;
    target.format=target.state==2?s_activeTargetFormat.load(std::memory_order_relaxed):0;
    target.usage=target.state==2?s_activeTargetUsage.load(std::memory_order_relaxed):0;
    target.pool=target.state==2?s_activeTargetPool.load(std::memory_order_relaxed):0;
    target.multisample=target.state==2?s_activeTargetMultisample.load(std::memory_order_relaxed):0;
    target.multisampleQuality=target.state==2?s_activeTargetMultisampleQuality.load(std::memory_order_relaxed):0;
    target.type=target.state==2?s_activeTargetType.load(std::memory_order_relaxed):0;
    target.isImplicitBackbuffer=target.state==2?s_activeTargetIsImplicitBackbuffer.load(std::memory_order_relaxed):0;
    target.lastCaller=s_activeTargetCaller.load(std::memory_order_relaxed);
    target.lastResult=s_activeTargetResult.load(std::memory_order_relaxed);
    for(uint32_t event=0;event<NFS_GUEST_FLOW_EVENT_COUNT;++event){
        auto& out=snapshot->guestFlow[event];
        const auto& stat=s_guestFlow[event];
        out.hits=stat.hits.load(std::memory_order_relaxed);
        out.taken=stat.taken.load(std::memory_order_relaxed);
        out.notTaken=stat.notTaken.load(std::memory_order_relaxed);
        out.lastValue=stat.lastValue.load(std::memory_order_relaxed);
        out.lastAuxiliary=stat.lastAuxiliary.load(std::memory_order_relaxed);
        out.lastOutcome=stat.lastOutcome.load(std::memory_order_relaxed);
    }
    try{
        std::lock_guard<std::mutex> lock(s_probeMutex);
        snapshot->helperTargetBefore=s_helperTargets[0];
        snapshot->helperTargetAfter=s_helperTargets[1];
        snapshot->guestRender=s_guestRender;
        snapshot->helperUP=s_helperUP;
    }catch(...){
        snapshot->helperTargetBefore.state=3;
        snapshot->helperTargetAfter.state=3;
    }
    snapshot->helperUP.state=s_helperUPCaptureState.load(std::memory_order_acquire);
    for(uint32_t helper=0;helper<bucketHelperCount;++helper){
        auto& out=snapshot->bucketHelpers[helper];
        out.calls=s_bucketCalls[helper].load(std::memory_order_relaxed);
        out.existingNodeSkips=s_bucketExistingSkips[helper].load(std::memory_order_relaxed);
        out.primaryAllocatorNull=s_bucketPrimaryNull[helper].load(std::memory_order_relaxed);
        out.auxiliaryAllocatorNull=s_bucketAuxiliaryNull[helper].load(std::memory_order_relaxed);
        out.bucketSelections=s_bucketSelections[helper].load(std::memory_order_relaxed);
        out.linkedNonNull=s_bucketLinkedNonNull[helper].load(std::memory_order_relaxed);
        out.linkedNull=s_bucketLinkedNull[helper].load(std::memory_order_relaxed);
        out.invalidBucketAddresses=s_bucketInvalidAddress[helper].load(std::memory_order_relaxed);
        out.lastBucketAddress=s_bucketLastAddress[helper].load(std::memory_order_relaxed);
        out.lastNode=s_bucketLastNode[helper].load(std::memory_order_relaxed);
        out.lastBank=s_bucketLastBank[helper].load(std::memory_order_relaxed);
        out.lastBucketIndex=s_bucketLastIndex[helper].load(std::memory_order_relaxed);
        for(uint32_t bank=0;bank<bucketBankCount;++bank){
            out.bankSelections[bank]=s_bucketBankSelections[helper][bank].load(std::memory_order_relaxed);
            out.bankLinkedNonNull[bank]=s_bucketBankLinked[helper][bank].load(std::memory_order_relaxed);
            out.touchedBuckets[bank]=s_bucketTouched[helper][bank].load(std::memory_order_relaxed);
            uint64_t topHits=0;
            for(uint32_t index=0;index<bucketCountPerBank;++index){
                const uint64_t hits=s_bucketHistogram[helper][bank][index].load(std::memory_order_relaxed);
                if(hits>topHits){topHits=hits;out.hottestBucket[bank]=index;}
            }
            out.hottestBucketHits[bank]=static_cast<uint32_t>(std::min<uint64_t>(topHits,UINT32_MAX));
        }
    }
    snapshot->guestGlobal8383ACReads=s_guestGlobal8383ACReads.load(std::memory_order_relaxed);
    snapshot->guestGlobal8383ACValue=s_guestGlobal8383ACValue.load(std::memory_order_relaxed);
}

extern "C" void nfsD3D9GetBackbufferProbe(uint32_t* state,uint32_t* points,uint32_t* nonBlack,
        uint32_t* width,uint32_t* height,uint32_t* format,uint32_t* elapsedMs,uint32_t* result){
    if(state)*state=s_backbufferProbeState.load(std::memory_order_acquire);
    if(points)*points=s_backbufferProbePoints.load(std::memory_order_relaxed);
    if(nonBlack)*nonBlack=s_backbufferProbeNonBlack.load(std::memory_order_relaxed);
    if(width)*width=s_backbufferProbeWidth.load(std::memory_order_relaxed);
    if(height)*height=s_backbufferProbeHeight.load(std::memory_order_relaxed);
    if(format)*format=s_backbufferProbeFormat.load(std::memory_order_relaxed);
    if(elapsedMs)*elapsedMs=s_backbufferProbeMs.load(std::memory_order_relaxed);
    if(result)*result=s_backbufferProbeResult.load(std::memory_order_relaxed);
}
