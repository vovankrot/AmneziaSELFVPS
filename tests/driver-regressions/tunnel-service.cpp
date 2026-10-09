#include <Windows.h>
#include <QByteArray>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QString>
#include <QTimer>
#include <iostream>
struct State {
    bool pending=false,timeout=false,zeroRead=false,stopOk=true,queryOk=true;
    DWORD serviceState=SERVICE_RUNNING;
    DWORD lastError=0, flags=0;int cancelled=0,drained=0,closes=0,writes=0;
    QByteArray response="errno=0\n\n",written;
} state;
HANDLE fakeCreateFile(LPCTSTR,DWORD,DWORD,void*,DWORD,DWORD flags,HANDLE){state.flags=flags;return HANDLE(1);}
HANDLE fakeCreateEvent(void*,BOOL,BOOL,LPCTSTR){return HANDLE(2);}
BOOL fakeCloseHandle(HANDLE){++state.closes;return TRUE;}
BOOL fakeWaitNamedPipe(LPCTSTR,DWORD){return TRUE;}
BOOL fakeSetNamedPipeHandleState(HANDLE,DWORD*,DWORD*,DWORD*){return TRUE;}
BOOL fakeResetEvent(HANDLE){return TRUE;}
BOOL fakeWriteFile(HANDLE,const void *data,DWORD size,DWORD *count,OVERLAPPED *operation){
    if(!operation){std::exit(2);} ++state.writes;*count=qMin<DWORD>(size,3);
    state.written.append(static_cast<const char*>(data),*count);
    operation->InternalHigh=*count;state.lastError=ERROR_IO_PENDING;return state.pending?FALSE:TRUE;
}
BOOL fakeReadFile(HANDLE,void *data,DWORD size,DWORD *count,OVERLAPPED *operation){
    if(!operation){std::exit(3);} *count=state.zeroRead?0:qMin<DWORD>(size,state.response.size());
    memcpy(data,state.response.constData(),*count);state.response.remove(0,*count);
    operation->InternalHigh=*count;state.lastError=ERROR_IO_PENDING;return state.pending?FALSE:TRUE;
}
DWORD fakeGetLastError(){return state.lastError;}
BOOL fakeQueryServiceStatus(SC_HANDLE,SERVICE_STATUS *status){status->dwCurrentState=state.serviceState;return state.queryOk;}
DWORD fakeWaitForSingleObject(HANDLE,DWORD){return state.timeout?WAIT_TIMEOUT:WAIT_OBJECT_0;}
BOOL fakeCancelIoEx(HANDLE,OVERLAPPED*){++state.cancelled;return TRUE;}
BOOL fakeGetOverlappedResult(HANDLE,OVERLAPPED *operation,DWORD *count,BOOL wait){
    if(wait){++state.drained;} *count=DWORD(operation->InternalHigh);return TRUE;
}
#undef CreateFile
#define CreateFile fakeCreateFile
#undef CreateEvent
#define CreateEvent fakeCreateEvent
#define CloseHandle fakeCloseHandle
#define CloseServiceHandle fakeCloseHandle
#undef WaitNamedPipe
#define WaitNamedPipe fakeWaitNamedPipe
#define SetNamedPipeHandleState fakeSetNamedPipeHandleState
#define ResetEvent fakeResetEvent
#define WriteFile fakeWriteFile
#define ReadFile fakeReadFile
#define GetLastError fakeGetLastError
#define QueryServiceStatus fakeQueryServiceStatus
#define WaitForSingleObject fakeWaitForSingleObject
#define CancelIoEx fakeCancelIoEx
#define GetOverlappedResult fakeGetOverlappedResult
#define TUNNEL_NAMED_PIPE "test"
namespace WindowsUtils {void windowsLog(const char*){}}
bool stopAndDeleteTunnelService(SC_HANDLE){return state.stopOk;}
struct Thread {int stops=0;void quit(){++stops;}void wait(){}};
class WindowsTunnelService {
public:
    void *m_service=reinterpret_cast<void*>(3),*m_logworker=reinterpret_cast<void*>(4);
    QTimer m_timer;Thread m_logthread;
    QString uapiCommand(const QString&);bool stop();bool isStopped();
};
#include "production-uapi.inc"
#include "production-service-stop.inc"
#include "production-service-stopped.inc"
void check(bool ok,const char *message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    WindowsTunnelService tunnel;
    check(!tunnel.isStopped(),"running service is not stopped");
    state.queryOk=false;
    check(!tunnel.isStopped(),"failed status query must not prove service absence");
    state.queryOk=true;state.serviceState=SERVICE_STOPPED;
    check(tunnel.isStopped(),"confirmed stopped service permits cleanup without IPC");
    for(bool pending:{false,true}){
        state={};state.pending=pending;
        check(tunnel.uapiCommand("set=1\n")=="errno=0","valid complete UAPI reply");
        check(state.flags==FILE_FLAG_OVERLAPPED&&state.written=="set=1\n\n"&&state.writes>1&&state.closes==2,
            "partial write and handle ownership");
    }
    state={};state.pending=state.timeout=true;
    check(tunnel.uapiCommand("set=1").isEmpty()&&state.cancelled==1&&state.drained==1&&state.closes==2,
        "timeout must cancel and drain pending operation before closing handles");
    state={};state.zeroRead=true;
    check(tunnel.uapiCommand("set=1").isEmpty()&&state.closes==2,"zero byte reply cannot spin or succeed");
    state={};state.response="errno=0\n";
    check(tunnel.uapiCommand("set=1").isEmpty(),"truncated reply cannot become success");
    state={};state.stopOk=false;
    check(!tunnel.stop()&&tunnel.m_service&&tunnel.m_logworker&&!state.closes&&!tunnel.m_logthread.stops,
        "failed service stop preserves handles for retry");
    state.stopOk=true;
    check(tunnel.stop()&&!tunnel.m_service&&!tunnel.m_logworker&&state.closes==1&&tunnel.m_logthread.stops==1,
        "successful stop releases ownership");
    check(tunnel.stop()&&state.closes==1,"repeated stop is idempotent");
    std::cout<<"PASS: partial and pending IO, timeout cancellation, zero/truncated response, failed stop and retry\n";
}
