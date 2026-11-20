#pragma once
#include <cstdint>
#include "guest_render_probe.h"
struct ANativeWindow;

enum class D3D9RenderStat : uint32_t {
    BeginScene = 0,
    Clear = 1,
    EndScene = 2,
    SetRenderTarget = 3,
    StretchRect = 4,
    DrawIndexedPrimitive = 5,
    DrawPrimitiveUP = 6,
    Count = 7
};

struct D3D9RenderMethodSnapshot {
    uint32_t calls;
    uint32_t forwarded;
    uint32_t culled;
    uint32_t lastCaller;
    uint32_t lastResult;
    uint32_t lastPath;
};

struct D3D9RenderTargetSnapshot {
    uint32_t state;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint32_t pool;
    uint32_t multisample;
    uint32_t multisampleQuality;
    uint32_t type;
    uint32_t isImplicitBackbuffer;
    uint32_t lastCaller;
    uint32_t lastResult;
};

struct D3D9GuestBucketHelperSnapshot {
    uint64_t calls;
    uint64_t existingNodeSkips;
    uint64_t primaryAllocatorNull;
    uint64_t auxiliaryAllocatorNull;
    uint64_t bucketSelections;
    uint64_t linkedNonNull;
    uint64_t linkedNull;
    uint64_t invalidBucketAddresses;
    uint64_t bankSelections[2];
    uint64_t bankLinkedNonNull[2];
    uint64_t touchedBuckets[2];
    uint32_t hottestBucket[2];
    uint32_t hottestBucketHits[2];
    uint32_t lastBucketAddress;
    uint32_t lastNode;
    uint32_t lastBank;
    uint32_t lastBucketIndex;
};

struct D3D9HelperUPSnapshot {
    uint32_t state; /* 0=not reached, 1=metadata only, 2=validated summary, 3=arguments rejected */
    uint32_t caller;
    uint32_t guestVertexAddress;
    uint32_t primitiveType;
    uint32_t primitiveCount;
    uint32_t vertexCount;
    uint32_t stride;
    uint32_t byteCount;
    uint32_t dataValidated;
    uint32_t dataStatus; /* 0=not checked, 1=validated, 2=unreadable pointer, 3=unexpected draw shape, 4=oversize */
    uint32_t fvfResult;
    uint32_t fvf;
    uint32_t textureResult;
    uint32_t textureBound;
    uint32_t textureType;
    uint32_t stageStateValidMask;
    uint32_t texCoordIndex;
    uint32_t textureTransformFlags;
    uint32_t colorOp;
    uint32_t colorArg1;
    uint32_t colorArg2;
    uint32_t alphaOp;
    uint32_t alphaArg1;
    uint32_t alphaArg2;
    float positionMin[3];
    float positionMax[3];
    float texcoordMin[4];
    float texcoordMax[4];
    uint32_t colorMin[4];
    uint32_t colorMax[4];
};

struct D3D9RenderSnapshot {
    D3D9RenderMethodSnapshot methods[static_cast<uint32_t>(D3D9RenderStat::Count)];
    D3D9RenderTargetSnapshot activeTarget;
    D3D9RenderTargetSnapshot helperTargetBefore;
    D3D9RenderTargetSnapshot helperTargetAfter;
    D3D9GuestBucketHelperSnapshot bucketHelpers[2];
    D3D9HelperUPSnapshot helperUP;
    struct GuestRenderDiagnostics {
        uint64_t calls;
        uint64_t bucketChecks;
        uint64_t bucketsEmpty;
        uint64_t bucketsNonempty;
        uint64_t routeIndexed;
        uint64_t routeUp;
        uint64_t indexedCountPositive;
        uint64_t indexedCountNonPositive;
        uint64_t upCountPositive;
        uint64_t upCountNonPositive;
        uint32_t lastBucketAddress;
        uint32_t lastBucketHead;
        uint32_t lastObjectAddress;
        uint32_t lastRouteValue;
        uint32_t lastIndexedCount;
        uint32_t maxIndexedCount;
        uint32_t lastUpCount;
        uint32_t maxUpCount;
    } guestRender;
    uint64_t guestGlobal8383ACReads;
    uint32_t guestGlobal8383ACValue;
    NfsGuestFlowSnapshot guestFlow[NFS_GUEST_FLOW_EVENT_COUNT];
};

void configureD3D9Bridge(void* guestBase,uint32_t (*allocator)(uint32_t),void (*deallocator)(uint32_t));
void shutdownD3D9Bridge();
void setD3D9HostWindow(ANativeWindow* window);
void setD3D9RenderSize(unsigned width,unsigned height);
// Launcher compatibility options; call before the game creates its device.
void setD3D9Compatibility(bool cpuTextures,unsigned backBuffers);
bool d3d9RequestsSurface();
void acknowledgeD3D9Surface(bool available);
uint32_t createGuestD3D9(uint32_t sdk);
uint32_t dispatchGuestD3D9(uint32_t token,const uint32_t* args,uint32_t* argumentCount,uint32_t guestCallerFunction);
extern "C" void nfsD3D9GetRenderSnapshot(D3D9RenderSnapshot* snapshot);
