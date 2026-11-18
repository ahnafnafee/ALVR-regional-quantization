#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AttentionBridge.h"
#include "AttentionWorker.h"
#include <iostream>
#include <stdexcept>

using namespace gvl::streaming::alvr;
using namespace gvl::streaming::scene;
int checks=0;
void Check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
uint64_t Now(){LARGE_INTEGER t;QueryPerformanceCounter(&t);return uint64_t(t.QuadPart);}
int main(){
    void* worker=nullptr;
    try {
        HANDLE existing=OpenFileMappingW(FILE_MAP_READ,FALSE,MappingName);
        if(existing){CloseHandle(existing);throw std::runtime_error("Stop the live encoder before isolated bridge tests");}
        SetEnvironmentVariableW(L"FLYINGBIKE_ALVR_ATTENTION",L"1");
        auto& bridge=AttentionBridge::Instance();bridge.SetSceneProcess(GetCurrentProcessId());
        Geometry geometry{};geometry.width=2368;geometry.height=1184;geometry.block=32;
        bridge.Configure(geometry);Check(bridge.Enabled(),"bridge enabled");
        worker=FBA_Create(sizeof(Frame),sizeof(Shape),MappingName,MutexName);Check(worker!=nullptr,"worker and bridge agree on protocol");
        Frame frame{};frame.bytes=sizeof(frame);frame.abi=Abi;frame.flags=Active|Selective|Predictive;
        frame.tracked=frame.orientation={0,0,0,1};frame.nearPlane=.1f;frame.count=1;
        LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);frame.frequency=uint64_t(frequency.QuadPart);
        frame.settings={70,3,75,.0125f,.01f,.25f,.25f,.06f,10,150,.1f};frame.time=1;
        for(auto& matrix:frame.matrices){matrix.v[0]=1;matrix.v[5]=1;matrix.v[11]=1;}
        Shape shape{};shape.count=4;shape.kind=42;shape.fullUntil=1;shape.fadeUntil=2;shape.minimum=1;
        shape.points[0]={-1,-1,5};shape.points[1]={1,-1,5};shape.points[2]={1,1,5};shape.points[3]={-1,1,5};
        frame.qpc=Now();Check(FBA_Submit(worker,&frame,&shape)!=0,"submitted");
        Stats stats{};for(int i=0;i<1000;++i){FBA_Stats(worker,&stats);if(stats.sourceQpc==frame.qpc)break;Sleep(1);}
        Check(stats.sourceQpc==frame.qpc,"published");
        bridge.Capture(100,1);Check(bridge.Build(100,20),"ALVR accepts frame-matched packet");
        Check(!bridge.Build(101,20),"ALVR rejects a different queued frame");
        // Once captured, the old queued frame retains its own mask even if a newer request arrives.
        bridge.Capture(102,1);FBA_TestDelay(worker,30);frame.qpc=Now();frame.time+=1.0/72;
        FBA_Submit(worker,&frame,&shape);Check(bridge.Build(102,20),"captured queued frame retains its snapshot");
        bridge.Capture(103,1);Check(!bridge.Build(103,20),"pending current frame cannot consume a previous mask");
        Sleep(45);bridge.Capture(104,1);Check(!bridge.Build(104,20),"expired worker result never revives attention");
        FBA_TestDelay(worker,0);frame.flags=0;frame.qpc=Now();FBA_Submit(worker,&frame,&shape);
        for(int i=0;i<1000;++i){FBA_Stats(worker,&stats);if(stats.sourceQpc==frame.qpc)break;Sleep(1);}
        bridge.Capture(105,1);Check(!bridge.Build(105,20),"empty newest packet clears importance");
        FBA_Destroy(worker);worker=nullptr;
        bridge.Capture(106,1);Check(!bridge.Build(106,20),"stopped producer stays invalid");
        // Exercise the actual feedback mailbox, shared configuration, controller and published diagnostics.
        HANDLE shared=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,MappingName);
        auto* view=static_cast<uint8_t*>(MapViewOfFile(shared,FILE_MAP_ALL_ACCESS,0,0,8192));
        HANDLE lock=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,MutexName);
        gvl::streaming::QualityConfig config{};config.mode=2;config.producerPid=GetCurrentProcessId();config.updatedQpc=Now();
        WaitForSingleObject(lock,INFINITE);
        std::memcpy(view+gvl::streaming::QualityOffset+32,&config,sizeof(config));ReleaseMutex(lock);
        bridge.Build(107,20); // Reads the host configuration under its normal status lock.
        float budget=20;
        for(uint64_t i=1;i<=120;i++){
            gvl::streaming::QualityFeedback feedback{};
            feedback.sourceTimestamp=i;feedback.networkMs=5;feedback.decoderQueueMs=i>5?35.0f:1.0f;
            feedback.frameIntervalMs=14;feedback.frameBytes=35000;
            bridge.ReportNetworkFeedback(feedback);budget=bridge.UpdateQuality(20);bridge.Build(107+i,20);Sleep(14);
        }
        Check(budget<20&&budget>=13,"feedback mailbox did not cap NVENC budget within configured limits");
        WaitForSingleObject(lock,INFINITE);
        gvl::streaming::QualityStatus status{};std::memcpy(&status,view+gvl::streaming::QualityOffset+96,sizeof(status));
        config.mode=1;std::memcpy(view+gvl::streaming::QualityOffset+32,&config,sizeof(config));ReleaseMutex(lock);
        Check(status.feedbackCount==120&&status.appliedExtraQp>0&&status.fresh,"shared feedback diagnostics disagree with policy");
        bridge.Build(300,20);Check(bridge.UpdateQuality(20)==20,"observe mode kept encoder bitrate capped");
        config.mode=2;
        WaitForSingleObject(lock,INFINITE);std::memcpy(view+gvl::streaming::QualityOffset+32,&config,sizeof(config));ReleaseMutex(lock);
        bridge.Build(301,20);Check(bridge.UpdateQuality(20)<20,"adaptive mode did not resume its live recommendation");
        bridge.SetSceneProcess(GetCurrentProcessId()+1);
        Check(bridge.UpdateQuality(20)==20,"exited application's policy leaked into another game");
        UnmapViewOfFile(view);CloseHandle(lock);CloseHandle(shared);
        std::cout<<"{\"checks\":"<<checks<<",\"backend\":\"ALVR AttentionBridge + native worker, isolated process\"}\n";return 0;
    }catch(const std::exception& error){FBA_Destroy(worker);std::cerr<<error.what()<<"\n";return 1;}
}
