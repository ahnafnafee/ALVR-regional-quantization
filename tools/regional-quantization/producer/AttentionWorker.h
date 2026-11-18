#pragma once
#include "AttentionScene.h"
#ifdef FBA_EXPORTS
#define FBA_API extern "C" __declspec(dllexport)
#else
#define FBA_API extern "C"
#endif
namespace gvl::streaming::scene
{
struct Stats
{
    uint64_t submitted, completed, published, superseded, expired, busy, sourceQpc;
    uint32_t regions, connected;
    float computeMs, readyAgeMs;
};
static_assert(sizeof(Stats) == 72, "Worker statistics ABI changed");
} // namespace gvl::streaming::scene
FBA_API void *__cdecl FBA_Create(uint32_t frameBytes, uint32_t shapeBytes, const wchar_t *mapping,
                                 const wchar_t *mutex);
FBA_API int __cdecl FBA_Submit(void *context, const gvl::streaming::scene::Frame *frame,
                               const gvl::streaming::scene::Shape *shapes);
FBA_API int __cdecl FBA_Stats(void *context, gvl::streaming::scene::Stats *stats);
FBA_API void __cdecl FBA_Destroy(void *context);
// Backend-independent numeric processor for same-frame rendering masks. No IPC or worker wait.
FBA_API void *__cdecl FBA_ProcessorCreate(uint32_t frameBytes, uint32_t shapeBytes, uint32_t packetBytes);
FBA_API int __cdecl FBA_ProcessorRun(void *context, const gvl::streaming::scene::Frame *frame,
    const gvl::streaming::scene::Shape *shapes, gvl::streaming::Packet *packet);
FBA_API void __cdecl FBA_ProcessorDestroy(void *context);
#ifdef FBA_TESTING
FBA_API void __cdecl FBA_TestDelay(void *context, uint32_t milliseconds);
#endif
