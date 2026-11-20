#ifndef NFSU2_GUEST_RENDER_PROBE_H
#define NFSU2_GUEST_RENDER_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NfsGuestRenderProbe {
    uint32_t bucketChecks;
    uint32_t bucketsEmpty;
    uint32_t bucketsNonempty;
    uint32_t lastBucketAddress;
    uint32_t lastBucketHead;
    uint32_t routeIndexed;
    uint32_t routeUp;
    uint32_t lastObjectAddress;
    uint32_t lastRouteValue;
    uint32_t indexedCountPositive;
    uint32_t indexedCountNonPositive;
    uint32_t lastIndexedCount;
    uint32_t maxIndexedCount;
    uint32_t upCountPositive;
    uint32_t upCountNonPositive;
    uint32_t lastUpCount;
    uint32_t maxUpCount;
} NfsGuestRenderProbe;

/* Called once at the end of the lifted helper, with values observed from its
 * already-loaded guest registers/locals. These functions only update snapshots. */
void nfsD3D9RecordGuestRenderProbe(const NfsGuestRenderProbe* probe);
void nfsD3D9RecordGuestGlobal8383AC(uint32_t value);
/* stage: 0 = immediately before sub_005CF3D0, 1 = immediately after. */
void nfsD3D9RecordGuestHelperTarget(uint32_t deviceToken, uint32_t tokenReadable,
                                    uint32_t caller, uint32_t stage);

/* Queue helper ids and event ids used only by generated-source telemetry. */
enum {
    NFS_GUEST_BUCKET_HELPER_CE9E0 = 0,
    NFS_GUEST_BUCKET_HELPER_CEAE0 = 1,
    NFS_GUEST_BUCKET_EVENT_CALL = 0,
    NFS_GUEST_BUCKET_EVENT_EXISTING_SKIP = 1,
    NFS_GUEST_BUCKET_EVENT_PRIMARY_ALLOC_NULL = 2,
    NFS_GUEST_BUCKET_EVENT_AUX_ALLOC_NULL = 3,
    NFS_GUEST_BUCKET_EVENT_BUCKET_SELECTED = 4,
    NFS_GUEST_BUCKET_EVENT_BUCKET_LINKED = 5
};
enum {
    NFS_GUEST_FLOW_GATE_5D2AD6 = 0,
    NFS_GUEST_FLOW_GATE_5D2B1D,
    NFS_GUEST_FLOW_CAA20_ENTRY,
    NFS_GUEST_FLOW_CAA20_870CD4,
    NFS_GUEST_FLOW_630DE0_ENTRY,
    NFS_GUEST_FLOW_630DE0_8654A4,
    NFS_GUEST_FLOW_630DE0_8026C8,
    NFS_GUEST_FLOW_630DE0_INPUT_COUNT,
    NFS_GUEST_FLOW_630DE0_INPUT_OBJECT,
    NFS_GUEST_FLOW_316D0_ENTRY,
    NFS_GUEST_FLOW_316D0_8026C8,
    NFS_GUEST_FLOW_316D0_INPUT_OBJECT,
    NFS_GUEST_FLOW_316D0_890080,
    NFS_GUEST_FLOW_162E0_ENTRY,
    NFS_GUEST_FLOW_20AE0_ENTRY,
    NFS_GUEST_FLOW_Q0_SITE_6173D1,
    NFS_GUEST_FLOW_Q0_SITE_621D96,
    NFS_GUEST_FLOW_Q0_SITE_621E79,
    NFS_GUEST_FLOW_Q0_SITE_6220B5,
    NFS_GUEST_FLOW_Q0_SITE_622EB2,
    NFS_GUEST_FLOW_C5880_ENTRY,
    NFS_GUEST_FLOW_C5880_INPUT_COUNT,
    NFS_GUEST_FLOW_C5880_QUALIFYING_RECORD,
    NFS_GUEST_FLOW_EVENT_COUNT
};
enum {
    NFS_GUEST_FLOW_NOT_TAKEN = 0,
    NFS_GUEST_FLOW_TAKEN = 1,
    NFS_GUEST_FLOW_NO_BRANCH = 2
};
typedef struct NfsGuestFlowSnapshot {
    uint64_t hits;
    uint64_t taken;
    uint64_t notTaken;
    uint32_t lastValue;
    uint32_t lastAuxiliary;
    uint32_t lastOutcome;
} NfsGuestFlowSnapshot;
void nfsD3D9RecordGuestFlowProbe(uint32_t event, uint32_t value,
                                uint32_t auxiliary, uint32_t outcome);
void nfsD3D9RecordGuestBucketProbe(uint32_t helper, uint32_t event,
                                   uint32_t bucketHead, uint32_t node);
enum {
    NFS_GUEST_STATE_TRACE_DIRECT_WRITE = 0,
    NFS_GUEST_STATE_TRACE_REGISTRY_COPY = 1,
    NFS_GUEST_STATE_TRACE_BA_ENTRY = 2,
    NFS_GUEST_STATE_TRACE_BA_EXIT = 3,
    NFS_GUEST_STATE_TRACE_CAA_ENTRY = 4,
    NFS_GUEST_STATE_TRACE_KIND_COUNT = 5
};
void nfsD3D9RecordGuestStateTrace(uint32_t kind, uint32_t writerPc,
                                  uint32_t oldValue, uint32_t newValue,
                                  uint32_t capabilityValue, uint32_t callerFunc,
                                  uint32_t guestEsp, uint32_t guestEbp,
                                  uint32_t guestStackTop);

#ifdef __cplusplus
}
#endif

#endif
