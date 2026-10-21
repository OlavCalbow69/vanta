#include "input_recovery.hpp"
#include "medius_backend.hpp"
#include "makcu_c.h"
#include <Windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    constexpr DWORD kMagic=0x56494C38;
    struct RecoveryRecord
    {
        DWORD magic{};
        DWORD version{},recordBytes{};
        std::uint64_t session{};
        volatile LONG mask{};
        vanta::InputBackend backend{};vanta::MediusRecoverySettings medius{};
        wchar_t instance[512]{};
        wchar_t guardPath[1024]{};
    };
    static_assert(std::is_standard_layout_v<RecoveryRecord> && std::is_trivially_copyable_v<RecoveryRecord>);
    void Close(HANDLE& value) { if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=nullptr; }
    std::filesystem::path Executable()
    {
        std::array<wchar_t,32768> path{};
        const auto n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        return std::filesystem::path(std::wstring(path.data(),n));
    }
    std::wstring Widen(const std::string& text){return std::wstring(text.begin(),text.end());}
    // Exact PnP instance identity, never "first available COM port" recovery.
    std::wstring FindDevice(const std::wstring& wantedPort,const std::wstring& wantedInstance)
    {
        const HDEVINFO list=SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS,nullptr,nullptr,DIGCF_PRESENT);
        if(list==INVALID_HANDLE_VALUE)return {};
        std::wstring found;
        SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);
        for(DWORD i=0;SetupDiEnumDeviceInfo(list,i,&info);++i)
        {
            wchar_t instance[512]{};
            if(!SetupDiGetDeviceInstanceIdW(list,&info,instance,512,nullptr))continue;
            const HKEY key=SetupDiOpenDevRegKey(list,&info,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ);
            if(key==INVALID_HANDLE_VALUE)continue;
            wchar_t port[256]{};DWORD bytes=sizeof(port),kind{};
            const LONG result=RegQueryValueExW(key,L"PortName",nullptr,&kind,reinterpret_cast<BYTE*>(port),&bytes);
            RegCloseKey(key);
            if(result!=ERROR_SUCCESS || kind!=REG_SZ)continue;
            if(!wantedPort.empty() && _wcsicmp(port,wantedPort.c_str())==0){found=instance;break;}
            if(!wantedInstance.empty() && _wcsicmp(instance,wantedInstance.c_str())==0){found=port;break;}
        }
        SetupDiDestroyDeviceInfoList(list);return found;
    }
    std::filesystem::path GuardPath(const std::wstring& instance)
    {
        std::uint64_t hash=1469598103934665603ULL;
        for(const auto c:instance){hash^=static_cast<std::uint64_t>(towlower(c));hash*=1099511628211ULL;}
        wchar_t local[32768]{};
        const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
        auto directory=(n && n<32768?std::filesystem::path(local):Executable().parent_path())/L"Vanta"/L"input-guards";
        std::filesystem::create_directories(directory);
        std::wostringstream name;name<<std::hex<<hash<<L".guard";return directory/name.str();
    }
    HANDLE OpenGuard(const wchar_t* path)
    {
        return CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    }
    void RecoveryLog(const std::string& text)
    {
        std::ofstream out(Executable().parent_path()/L"vanta-input-recovery.log",std::ios::app);
        SYSTEMTIME t{};GetLocalTime(&t);
        out<<t.wYear<<'-'<<t.wMonth<<'-'<<t.wDay<<' '<<t.wHour<<':'<<t.wMinute<<':'<<t.wSecond
           <<" [PID "<<GetCurrentProcessId()<<"] "<<text<<'\n';
    }
    bool Neutralize(makcu_device_t* device,std::uint32_t mask)
    {
        bool ok=makcu_mouse_up(device,MAKCU_MOUSE_LEFT)==MAKCU_SUCCESS;
        for(const auto bit:{1U,2U,4U,8U,16U})if(mask&bit)ok=vanta::WriteMakcuLock(device,bit,false)&&ok;
        // A timed-out uncorrelated query poisons this session. No further reads.
        int held{};
        if(!vanta::QueryMakcuInteger(device,"km.left()",held) || (held&2))return false;
        for(const auto bit:{1U,2U,4U,8U,16U})if(mask&bit)
        {
            int value{};
            if(!vanta::QueryMakcuInteger(device,vanta::MakcuLockQuery(bit),value) || value!=0)return false;
        }
        return ok;
    }
}

namespace vanta
{
    bool QueryMakcuInteger(makcu_device_t* device,const char* command,int& value)
    {
        std::array<char,256> reply{};
        if(makcu_query_text(device,command,reply.data(),reply.size(),20)!=MAKCU_SUCCESS)return false;
        std::string_view text(reply.data());
        while(!text.empty() && (text.front()==' ' || text.front()=='\r' || text.front()=='\n'))text.remove_prefix(1);
        while(!text.empty() && (text.back()==' ' || text.back()=='\r' || text.back()=='\n'))text.remove_suffix(1);
        const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
        return result.ec==std::errc{} && result.ptr==text.data()+text.size();
    }
    const char* MakcuLockQuery(std::uint32_t bit)
    {
        switch(bit){case InputLockX:return "km.lock_mx()";case InputLockY:return "km.lock_my()";
        case InputLockLeft:return "km.lock_ml()";case InputLockSide1:return "km.lock_ms1()";
        case InputLockSide2:return "km.lock_ms2()";default:return "";}
    }
    bool WriteMakcuLock(makcu_device_t* d,std::uint32_t bit,bool locked)
    {
        makcu_error_t result=MAKCU_ERROR_INVALID_PARAMETER;
        switch(bit){case InputLockX:result=makcu_lock_mouse_x(d,locked);break;
        case InputLockY:result=makcu_lock_mouse_y(d,locked);break;
        case InputLockLeft:result=makcu_lock_mouse_left(d,locked);break;
        case InputLockSide1:result=makcu_lock_mouse_side1(d,locked);break;
        case InputLockSide2:result=makcu_lock_mouse_side2(d,locked);break;}
        return result==MAKCU_SUCCESS;
    }
    struct InputRecoverySession::Implementation
    {
        HANDLE map{},parent{},ready{},finish{},handoff{},done{},process{},guard{};
        RecoveryRecord* record{};
        void CloseAll()
        {
            if(record)UnmapViewOfFile(record);record=nullptr;
            for(auto* h:{&map,&parent,&ready,&finish,&handoff,&done,&process,&guard})Close(*h);
        }
    };
    InputRecoverySession::InputRecoverySession():state_(std::make_unique<Implementation>()){}
    InputRecoverySession::~InputRecoverySession(){Finish(false);}
    bool InputRecoverySession::Start(const std::string& port,std::string& error,InputBackend backend,const MediusRecoverySettings* medius)
    {
        Finish(false);auto& s=*state_;
        try
        {
            const auto instance=FindDevice(Widen(port),{});
            if(instance.empty()){error="Cannot identify the MAKCU Windows device instance";return false;}
            const auto path=GuardPath(instance);
            s.guard=OpenGuard(path.c_str());
            if(s.guard==INVALID_HANDLE_VALUE){s.guard=nullptr;error="This MAKCU is owned by another Vanta/recovery session";return false;}
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            s.map=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,sizeof(RecoveryRecord),nullptr);
            if(!s.map)throw std::runtime_error("Recovery shared memory failed");
            s.record=static_cast<RecoveryRecord*>(MapViewOfFile(s.map,FILE_MAP_ALL_ACCESS,0,0,sizeof(RecoveryRecord)));
            if(!s.record)throw std::runtime_error("Recovery shared memory view failed");
            *s.record={};s.record->magic=kMagic;s.record->version=kInputRecoveryRecordVersion;s.record->recordBytes=sizeof(RecoveryRecord);
            s.record->backend=backend;if(medius)s.record->medius=*medius;
            if(backend==InputBackend::medius)s.record->mask=64; // Arm option recovery before the first mutation.
            LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);
            s.record->session=static_cast<std::uint64_t>(stamp.QuadPart)^(static_cast<std::uint64_t>(GetCurrentProcessId())<<32);
            if(!s.record->session)s.record->session=1;
            if(instance.size()>=std::size(s.record->instance) || path.native().size()>=std::size(s.record->guardPath))
                throw std::runtime_error("Recovery device identity or guard path is too long");
            wcscpy_s(s.record->instance,instance.c_str());wcscpy_s(s.record->guardPath,path.c_str());
            DWORD bytes{};
            if(!WriteFile(s.guard,&s.record->session,sizeof(s.record->session),&bytes,nullptr) || bytes!=sizeof(s.record->session))
                throw std::runtime_error("Recovery session token could not be saved");
            FlushFileBuffers(s.guard);
            s.ready=CreateEventW(&security,TRUE,FALSE,nullptr);s.finish=CreateEventW(&security,TRUE,FALSE,nullptr);
            s.handoff=CreateEventW(&security,TRUE,FALSE,nullptr);s.done=CreateEventW(&security,TRUE,FALSE,nullptr);
            if(!s.ready || !s.finish || !s.handoff || !s.done)throw std::runtime_error("Recovery events failed");
            if(!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&s.parent,SYNCHRONIZE,TRUE,0))
                throw std::runtime_error("Recovery parent handle failed");
            std::array<HANDLE,6> handles{s.map,s.parent,s.ready,s.finish,s.handoff,s.done};
            SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);
            std::vector<std::byte> storage(size);
            auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if(!InitializeProcThreadAttributeList(attributes,1,0,&size))throw std::runtime_error("Recovery handle list failed");
            if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles.data(),sizeof(handles),nullptr,nullptr))
            {DeleteProcThreadAttributeList(attributes);throw std::runtime_error("Recovery handle inheritance failed");}
            const auto executable=Executable();std::wostringstream command;
            command<<L'"'<<executable.wstring()<<L"\" --input-recovery";
            for(const auto h:handles)command<<L' '<<reinterpret_cast<std::uintptr_t>(h);
            command<<L' '<<s.record->session;
            auto arguments=command.str();STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.lpAttributeList=attributes;
            PROCESS_INFORMATION child{};
            const BOOL created=CreateProcessW(executable.c_str(),arguments.data(),nullptr,nullptr,TRUE,
                CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,executable.parent_path().c_str(),&start.StartupInfo,&child);
            DeleteProcThreadAttributeList(attributes);
            if(!created)throw std::runtime_error("Recovery helper could not start");
            s.process=child.hProcess;CloseHandle(child.hThread);
            if(WaitForSingleObject(s.ready,2000)!=WAIT_OBJECT_0 || !Ready())throw std::runtime_error("Recovery helper did not become ready");
            return true;
        }
        catch(const std::exception& e){error=e.what();Finish(true);return false;}
    }
    void InputRecoverySession::Mark(std::uint32_t mask) noexcept
    {if(state_->record)InterlockedExchange(&state_->record->mask,static_cast<LONG>(mask | (state_->record->backend==InputBackend::medius?64U:0U)));}
    bool InputRecoverySession::Ready() const noexcept
    {return state_->process && WaitForSingleObject(state_->process,0)==WAIT_TIMEOUT;}
    void InputRecoverySession::Finish(bool clean)
    {
        auto& s=*state_;if(!s.map && !s.guard)return;
        if(clean){if(s.finish)SetEvent(s.finish);}
        else {if(s.handoff)SetEvent(s.handoff);Close(s.guard);}
        if(clean && s.done && s.process){const HANDLE wait[]{s.done,s.process};WaitForMultipleObjects(2,wait,FALSE,2000);}
        s.CloseAll();
    }
    bool RunInputRecoveryMode(int argc,wchar_t** argv,int& exitCode)
    {
        if(argc<2 || wcscmp(argv[1],L"--input-recovery")!=0)return false;
        exitCode=2;if(argc!=9)return true;
        std::array<HANDLE,6> handles{};
        for(std::size_t i=0;i<handles.size();++i)
        {
            wchar_t* end{};const auto n=wcstoull(argv[i+2],&end,10);
            if(!n || !end || *end)return true;handles[i]=reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(n));
        }
        wchar_t* end{};const auto token=wcstoull(argv[8],&end,10);if(!token || !end || *end)return true;
        auto* record=static_cast<RecoveryRecord*>(MapViewOfFile(handles[0],FILE_MAP_ALL_ACCESS,0,0,sizeof(RecoveryRecord)));
        if(!record)return true;
        if(record->magic!=kMagic || !RecoveryLayoutMatches(record->version,record->recordBytes,sizeof(RecoveryRecord)) ||
            record->session!=token || !record->instance[0] || !record->guardPath[0] ||
            record->instance[std::size(record->instance)-1] || record->guardPath[std::size(record->guardPath)-1] ||
            (record->backend!=InputBackend::makcu && record->backend!=InputBackend::medius))
        {UnmapViewOfFile(record);return true;}
        SetEvent(handles[2]);
        const HANDLE triggers[]{handles[3],handles[1],handles[4]};
        const DWORD reason=WaitForMultipleObjects(3,triggers,FALSE,INFINITE);
        if(reason==WAIT_OBJECT_0){exitCode=0;}
        else
        {
            const auto mask=static_cast<std::uint32_t>(InterlockedCompareExchange(&record->mask,0,0));
            bool clean=mask==0;
            const ULONGLONG deadline=GetTickCount64()+8000;
            RecoveryLog("Recovering session "+std::to_string(token)+", possibly active inputs "+std::to_string(mask));
            while(!clean && GetTickCount64()<deadline)
            {
                HANDLE guard=OpenGuard(record->guardPath);
                if(guard==INVALID_HANDLE_VALUE){WaitForSingleObject(handles[3],50);continue;}
                std::uint64_t current{};DWORD bytes{};
                const bool own=ReadFile(guard,&current,sizeof(current),&bytes,nullptr) && bytes==sizeof(current) && RecoverySessionMatches(token,current);
                if(!own){CloseHandle(guard);RecoveryLog("Newer device session owns recovery; stale helper stopped");clean=true;break;}
                const auto port=FindDevice({},record->instance);
                if(port.empty()){CloseHandle(guard);WaitForSingleObject(handles[3],50);continue;}
                std::string narrow;for(const wchar_t c:port)narrow.push_back(static_cast<char>(c));
                if(record->backend==InputBackend::medius)clean=RecoverMedius(narrow,record->medius);
                else
                {
                    auto* device=makcu_device_create();
                    if(device){if(makcu_connect(device,narrow.c_str())==MAKCU_SUCCESS)clean=Neutralize(device,mask&InputManagedLocks);
                        makcu_disconnect(device);makcu_device_destroy(device);}
                }
                CloseHandle(guard);
                if(!clean)WaitForSingleObject(handles[3],100);
            }
            RecoveryLog(clean?"Recovery complete":"Recovery incomplete: device unavailable or firmware readback failed");
            exitCode=clean?0:3;
        }
        SetEvent(handles[5]);UnmapViewOfFile(record);
        for(auto& h:handles)Close(h);return true;
    }
}
