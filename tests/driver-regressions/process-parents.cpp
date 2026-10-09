#include <Windows.h>
#include <QMap>
#include <iostream>
struct ProcessInfo {DWORD ProcessId,ParentProcessId;FILETIME CreationTime;};
struct Log {Log& info(){return *this;}template<class T> Log& operator<<(const T&){return *this;}} logger;
int prune(QMap<DWORD,ProcessInfo>& processes) {
#include "production-process-parents.inc"
  return staleParents;
}
void check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
int main(){
  QMap<DWORD,ProcessInfo> processes;
  processes[1]={1,0,{10,0}};processes[2]={2,1,{20,0}};
  processes[3]={3,4,{30,0}};processes[4]={4,0,{40,0}};
  processes[5]={5,99,{50,0}};processes[6]={6,6,{60,0}};
  processes[7]={7,1,{0,0}};processes[8]={8,0,{0,0}};processes[9]={9,8,{90,0}};
  check(prune(processes)==5,"remove missing, recycled, self, unknown child and unknown parent links");
  check(processes[2].ParentProcessId==1,"preserve valid inheritance");
  for(DWORD pid:{3,5,6,7,9})check(processes[pid].ParentProcessId==0,"unsafe parent must not reach kernel driver");
  check(processes[3].CreationTime.dwLowDateTime==30&&processes.size()==9,"keep every process record and creation time");
  std::cout<<"PASS: valid ancestry, recycled PID, missing parent, self-parent, unknown times\n";
}
