#define WIN32_LEAN_AND_MEAN
#include "AttentionWorker.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <string>
#include <windows.h>

namespace
{
using namespace gvl::streaming;
using namespace gvl::streaming::scene;
uint64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}
struct Worker
{
    HANDLE mapping = nullptr, mutex = nullptr, event = nullptr, lease = nullptr;
    bool ownsLease = false;
    uint8_t *view = nullptr;
    uint64_t frequency = 0;
    std::thread thread;
    SRWLOCK queueLock = SRWLOCK_INIT;
    Frame pending{};
    std::array<Shape, MaxShapes> shapes{};
    bool hasPending = false;
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> submitted{0}, completed{0}, published{0}, superseded{0}, expired{0}, busy{0}, sourceQpc{0};
    std::atomic<uint32_t> regions{0}, connected{0};
    std::atomic<float> computeMs{0}, readyAgeMs{0};
#ifdef FBA_TESTING
    std::atomic<uint32_t> testDelay{0};
#endif
    volatile LONG64 *Fence() const
    {
        return reinterpret_cast<volatile LONG64 *>(view + offsetof(Control, requestedQpc));
    }
    uint64_t Requested() const
    {
        return uint64_t(InterlockedCompareExchange64(Fence(), 0, 0));
    }
    void Request(uint64_t qpc)
    {
        InterlockedExchange64(Fence(), LONG64(qpc));
    }
    bool Open(const wchar_t *name, const wchar_t *lockName)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = uint64_t(f.QuadPart);
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName);
        if (mapping)
            view = static_cast<uint8_t *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, MappingBytes));
        if (!view || !mutex)
            return false;
        const DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            return false;
        Control control{};
        std::memcpy(&control, view, sizeof(control));
        ReleaseMutex(mutex);
        if (wait == WAIT_ABANDONED || control.magic != Magic || control.version != ControlVersion || !control.enabled)
            return false;
        const std::wstring leaseName = std::wstring(name) + L".Producer." + std::to_wstring(GetCurrentProcessId());
        lease = CreateSemaphoreW(nullptr, 1, 1, leaseName.c_str());
        ownsLease = lease && WaitForSingleObject(lease, 0) == WAIT_OBJECT_0;
        if (!ownsLease) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return false;
        thread = std::thread([this] { Run(); });
        return true;
    }
    bool Submit(const Frame &f, const Shape *input)
    {
        if (f.bytes != sizeof(Frame) || f.abi != Abi || f.count > MaxShapes || !input || f.frequency != frequency ||
            f.qpc == 0 || f.qpc > Now() || stopping.load())
            return false;
        if (!OwnsScene()) { connected.store(0); return false; }
        // Invalidate earlier frames even when the queue or driver mutex is busy.
        // The driver checks this atomic fence at composition submission.
        Request(f.qpc);
        ++submitted;
        if (!TryAcquireSRWLockExclusive(&queueLock))
        {
            ++busy;
            return false;
        }
        if (hasPending)
            ++superseded;
        pending = f;
        std::memcpy(shapes.data(), input, size_t(f.count) * sizeof(Shape));
        hasPending = true;
        ReleaseSRWLockExclusive(&queueLock);
        SetEvent(event);
        return true;
    }
    void Run()
    {
        Processor processor;
        Frame frame{};
        std::array<Shape, MaxShapes> input{};
        Packet packet{};
        uint64_t sequence = 0;
        for (;;)
        {
            WaitForSingleObject(event, INFINITE);
            if (stopping.load())
                break;
            AcquireSRWLockExclusive(&queueLock);
            const bool available = hasPending;
            if (available)
            {
                frame = pending;
                std::memcpy(input.data(), shapes.data(), size_t(frame.count) * sizeof(Shape));
                hasPending = false;
            }
            ReleaseSRWLockExclusive(&queueLock);
            if (!available)
                continue;
#ifdef FBA_TESTING
            if (testDelay.load())
                Sleep(testDelay.load());
#endif
            if (Requested() != frame.qpc)
            {
                ++superseded;
                continue;
            }
            const uint64_t start = Now();
            processor.Process(frame, input.data(), GetCurrentProcessId(), ++sequence, packet);
            const uint64_t finished = Now();
            ++completed;
            computeMs.store(float(1000.0 * double(finished - start) / double(frequency)));
            const float age = float(1000.0 * double(finished - frame.qpc) / double(frequency));
            readyAgeMs.store(age);
            // The original sample clock is retained. Queue delay is never hidden by re-stamping.
            if (age > std::min(8.0f, packet.maxAgeMs))
            {
                ++expired;
                continue;
            }
            if (stopping.load() || Requested() != frame.qpc)
            {
                ++superseded;
                continue;
            }
            const DWORD wait = WaitForSingleObject(mutex, 0);
            if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            {
                ++busy;
                continue;
            }
            Control control{};
            std::memcpy(&control, view, sizeof(control));
            const uint64_t now = Now();
            const bool live = wait != WAIT_ABANDONED && control.magic == Magic && control.version == ControlVersion &&
                              control.enabled && now >= control.heartbeat &&
                              (control.scenePid == 0 || control.scenePid == GetCurrentProcessId()) &&
                              double(now - control.heartbeat) / double(frequency) < 1;
            connected.store(live ? 1u : 0u);
            if (live && !stopping.load() && Requested() == frame.qpc)
            {
                std::memcpy(view + ControlBytes + (packet.sequence % Slots) * sizeof(Packet), &packet, sizeof(packet));
                regions.store(packet.count);
                sourceQpc.store(frame.qpc);
                ++published;
            }
            ReleaseMutex(mutex);
        }
    }
    bool OwnsScene() const
    {
        const uint32_t scene = uint32_t(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(view + offsetof(Control, scenePid)), 0, 0));
        return scene == 0 || scene == GetCurrentProcessId();
    }
    ~Worker()
    {
        stopping.store(true);
        // Leave a newer invalidation fence behind until the next producer takes ownership.
        // Do not affect the mapping when Open failed before this worker started.
        if (thread.joinable())
        {
            if (OwnsScene()) Request(Now());
            SetEvent(event);
            thread.join();
        }
        if (event)
            CloseHandle(event);
        if (view)
            UnmapViewOfFile(view);
        if (mapping)
            CloseHandle(mapping);
        if (mutex)
            CloseHandle(mutex);
        if (ownsLease) ReleaseSemaphore(lease, 1, nullptr);
        if (lease) CloseHandle(lease);
    }
};
} // namespace
void *__cdecl FBA_Create(uint32_t frameBytes, uint32_t shapeBytes, const wchar_t *mapping, const wchar_t *mutex)
{
    if (frameBytes != sizeof(gvl::streaming::scene::Frame) || shapeBytes != sizeof(gvl::streaming::scene::Shape) ||
        !mapping || !mutex)
        return nullptr;
    Worker *w = nullptr;
    try
    {
        w = new Worker;
        if (w->Open(mapping, mutex))
            return w;
    }
    catch (...)
    {
    }
    delete w;
    return nullptr;
}
int __cdecl FBA_Submit(void *context, const gvl::streaming::scene::Frame *frame,
                       const gvl::streaming::scene::Shape *shapes)
{
    return context && frame && static_cast<Worker *>(context)->Submit(*frame, shapes) ? 1 : 0;
}
int __cdecl FBA_Stats(void *context, gvl::streaming::scene::Stats *out)
{
    if (!context || !out)
        return 0;
    auto &w = *static_cast<Worker *>(context);
    *out = {w.submitted.load(), w.completed.load(), w.published.load(), w.superseded.load(),
            w.expired.load(),   w.busy.load(),      w.sourceQpc.load(), w.regions.load(),
            w.connected.load(), w.computeMs.load(), w.readyAgeMs.load()};
    return 1;
}
void __cdecl FBA_Destroy(void *context)
{
    delete static_cast<Worker *>(context);
}
void *__cdecl FBA_ProcessorCreate(uint32_t frameBytes, uint32_t shapeBytes, uint32_t packetBytes)
{
    using namespace gvl::streaming;
    if (frameBytes != sizeof(scene::Frame) || shapeBytes != sizeof(scene::Shape) || packetBytes != sizeof(Packet)) return nullptr;
    try { return new scene::Processor; } catch (...) { return nullptr; }
}
int __cdecl FBA_ProcessorRun(void *context, const gvl::streaming::scene::Frame *frame,
    const gvl::streaming::scene::Shape *shapes, gvl::streaming::Packet *packet)
{
    if (!context || !frame || !packet || frame->bytes != sizeof(*frame) || frame->abi != gvl::streaming::scene::Abi) return 0;
    static_cast<gvl::streaming::scene::Processor *>(context)->Process(*frame, shapes, GetCurrentProcessId(), frame->frame, *packet);
    return 1;
}
void __cdecl FBA_ProcessorDestroy(void *context)
{
    delete static_cast<gvl::streaming::scene::Processor *>(context);
}
#ifdef FBA_TESTING
void __cdecl FBA_TestDelay(void *context, uint32_t milliseconds)
{
    static_cast<Worker *>(context)->testDelay.store(milliseconds);
}
#endif
