#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <tchar.h>
#include <Sddl.h>

#include <VersionHelpers.h>

//
// Definitions
//

#define DRIVER_NAME     _T("\\\\.\\uxstyle")
#define SERVICE_NAME    _T("Unsigned Themes")
#define SERVICE_SLEEP   3600000 // 1 hour

#define IOCTL_PATCH_ADDR    0x226000  
#define IOCTL_UNPATCH_ADDR  0x222007
#define IOCTL_DUMP_PATCHED_ADDR 0x002200B
#define BUFFER_ADDR_COUNT   0

#define HMOD_UXTHEME        0
#define HMOD_SHSVCS         1
#define HMOD_THEMEUI        2

#define _WIN32_WINNT_WINTHRESHOLD 0x0604

VERSIONHELPERAPI IsWindows8Point4OrGreater()
{
    return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINTHRESHOLD), LOBYTE(_WIN32_WINNT_WINTHRESHOLD), 0);
}

//
// Globals
//

SERVICE_STATUS ServiceStatus;
SERVICE_STATUS_HANDLE hStatus;
HANDLE hDevice                  = INVALID_HANDLE_VALUE;
HANDLE hEvent					= INVALID_HANDLE_VALUE;
DWORD dwBytesReturned           = 0;
ULONG_PTR ulBuffer[4]           = { 0, 0x00, 0x00, 0x00 };
HMODULE hMods[3]                = { NULL, NULL, NULL };
BOOL bWindows7					= false; // [10/28/2008] Support changes in 6801+
BOOL bWindows8                  = false; // [09/02/2012] Support for Windows 8
BOOL bWindows81					= false; // [09/11/2013] Support for Windows 8.1
BOOL bWindowsThreshold          = false; // [09/14/2014] Support for Windows Threshold  (9838+)
BOOL bDemoMode					= false;
INT nDependencies               = 3;     // Default: Windows Vista and up.

//
// Prototypes
//

VOID WINAPI ServiceMain(DWORD, LPTSTR*);
ULONG_PTR WINAPI ControlHandlerEx(ULONG_PTR, ULONG_PTR, LPVOID, LPVOID);
ULONG_PTR GetIATPtrToImport(HMODULE, LPCSTR, BOOL, INT);
VOID SafeUnloadLibraries();
VOID DeclareFailure(BOOL);

#pragma warning(suppress: 28251)
INT WINAPI WinMain( HINSTANCE, HINSTANCE, LPSTR lpCmdLine, INT )
{

    #ifdef _DEBUG
    if (_stricmp("-demo", lpCmdLine) == 0) {
        OutputDebugString(_T("Take note, we're in demo mode."));
        bDemoMode = 1;
    } else if (_stricmp("-waitfordebugger", lpCmdLine) == 0) {
        OutputDebugString(_T("Waiting for debugger to attach..."));
        
        while(!IsDebuggerPresent())
            Sleep(2500);

        DebugBreak();
    }
    #else
    UNREFERENCED_PARAMETER(lpCmdLine);
    #endif

    // Determine what OS we're on first...
    
    if (IsWindows8Point4OrGreater()) {
        #ifdef _DEBUG
        OutputDebugStringW(L"Windows 8.4 mode.\r\n");
        #endif

        nDependencies = 3;
        bWindowsThreshold = true;
    } else if (IsWindows8Point1OrGreater()) {
        #ifdef _DEBUG
        OutputDebugStringW(L"Windows 8.1 mode.\r\n");
        #endif

        nDependencies = 3;
        bWindows81 = true;
    } else if (IsWindows8OrGreater()) {
        #ifdef _DEBUG
        OutputDebugStringW(L"Windows 8 mode.\r\n");
        #endif

        nDependencies = 3;
        bWindows8 = true;
    } else {
        return ERROR_NOT_SUPPORTED;
    }

    SERVICE_TABLE_ENTRY ServiceTable[2];

    ServiceTable[0].lpServiceName = SERVICE_NAME;
    ServiceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION)ServiceMain;
    ServiceTable[1].lpServiceName = NULL;
    ServiceTable[1].lpServiceProc = NULL;

    StartServiceCtrlDispatcher(ServiceTable);

    return ERROR_SUCCESS;
}

VOID WINAPI ServiceMain( DWORD, LPTSTR* )
{
    #ifdef _DEBUG
    OutputDebugString(_T("Oh hai, this is ServiceMain().\r\n"));
    #endif

    ServiceStatus.dwServiceType = SERVICE_WIN32; 
    ServiceStatus.dwControlsAccepted  = SERVICE_ACCEPT_STOP;
    ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    ServiceStatus.dwWin32ExitCode = 0; 
    ServiceStatus.dwServiceSpecificExitCode = 0; 
    ServiceStatus.dwCheckPoint = 0; 
    ServiceStatus.dwWaitHint = 0;

    hStatus = RegisterServiceCtrlHandlerEx(SERVICE_NAME, (LPHANDLER_FUNCTION_EX)ControlHandlerEx, NULL); 
    
    #ifdef _DEBUG
    OutputDebugString(_T("... RegisterServiceCtrlHandlerEx() called.\r\n"));
    #endif

    if(hStatus == NULL)
    {
        DeclareFailure(FALSE);
        return;
    }

    if(!bDemoMode) {

        //
        // Attempt to get handle to driver
        //
        hDevice = CreateFile(
            DRIVER_NAME,
            GENERIC_READ,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);

        //
        // Uh, oh. Something naughty happened.
        //
        if(hDevice == INVALID_HANDLE_VALUE)
        {
            DeclareFailure(FALSE);
            return;
        }

        //
        // Load the libraries into our array, we're going to persist them
        // until unload+unpatch time...
        //
        #ifdef _DEBUG
        OutputDebugString(_T("Attempting load of libraries...\r\n"));
        #endif

        hMods[HMOD_UXTHEME] = LoadLibrary( _T("uxtheme.dll") );

        if(nDependencies == 3)
        {
            if(bWindows7)
            {
                hMods[HMOD_SHSVCS] = LoadLibrary( _T("themeservice.dll") );
            }
            else if (bWindows8 || bWindows81 || bWindowsThreshold)
            {
                
                hMods[HMOD_SHSVCS] = LoadLibrary( _T("uxinit.dll") );
                hMods[HMOD_THEMEUI] = LoadLibrary( _T("themeui.dll") );
            }
            else
            {
                hMods[HMOD_SHSVCS] = LoadLibrary( _T("shsvcs.dll") );
                hMods[HMOD_THEMEUI] = LoadLibrary( _T("themeui.dll") );
            }

        }

        //
        // Verify we didn't fuck up...
        //
        #ifdef _DEBUG
        OutputDebugString(_T("Verifying all modules loaded...\r\n"));
        #endif

        for( int i = 0; i < nDependencies; i++ )
        {
            if(hMods[i] == NULL)
            {
                DeclareFailure(FALSE);
                return;
            }
        }

        //
        // Find patch target and pass for patching...
        //
        for( int i = 0; i < nDependencies; i++ )
        {
            ULONG_PTR ptr = 0;

            if (bWindowsThreshold) {
                ptr = GetIATPtrToImport(hMods[i], "CryptVerifySignatureW", 1, -2);
            } else if(bWindows7 || bWindows8) {
				INT nFuzzyAdjustments[3] = { -2, 1, 1 }; // [Sep. 2 2012] Support for Windows 8
				ptr = GetIATPtrToImport(hMods[i], "CryptVerifySignatureW", bWindows7 || (bWindows8 && i != HMOD_SHSVCS), nFuzzyAdjustments[i]);
			} else {
				ptr = GetIATPtrToImport(hMods[i], "CryptVerifySignatureW", (i != HMOD_SHSVCS), -2);
			}

            #ifdef _DEBUG
                wchar_t addr[12] = {0};
                wsprintfW(addr, L"%X", ptr);
                OutputDebugString(_T("Sending address for patching: "));
                OutputDebugStringW(addr);
                OutputDebugString(_T("\r\n"));
            #endif

            BOOL bSuccess = DeviceIoControl(
                hDevice,
                IOCTL_PATCH_ADDR,
                (LPVOID)&ptr,
                sizeof(LPVOID),
                NULL,
                NULL,
                &dwBytesReturned,
                NULL);

            if(bSuccess == FALSE)
            {
                //
                // Fire a flare, attempt to unpatch before we go down
                // with the ship
                //
                #ifdef _DEBUG
                OutputDebugString(_T("Oops. We need to bail. Patch failed.\r\n"));
                #endif

               DeviceIoControl(
                    hDevice,
                    IOCTL_UNPATCH_ADDR,
                    NULL,
                    0,
                    NULL,
                    NULL,
                    &dwBytesReturned,
                    NULL);

                DeclareFailure(FALSE);
                return;
            }
        }
    }

    //
    // Phew, we made it.
    //
    ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(hStatus, &ServiceStatus);

    //
    // Lets duck and cover, the majority of our code isn't going
    // to execute again.
    //
    SetProcessWorkingSetSize(
        GetCurrentProcess(),
        (size_t)-1,
        (size_t)-1);

    while(ServiceStatus.dwCurrentState == SERVICE_RUNNING)
    {
        Sleep(SERVICE_SLEEP);
    }

    return; 
}

ULONG_PTR WINAPI ControlHandlerEx( ULONG_PTR dwControl, ULONG_PTR, LPVOID, LPVOID )
{
    BOOL bSuccess = FALSE;

    switch(dwControl)
    {
        case SERVICE_CONTROL_STOP:
            
            if(!bDemoMode) {

                bSuccess = DeviceIoControl(
                    hDevice,
                    IOCTL_UNPATCH_ADDR,
                    NULL,
                    0,
                    NULL,
                    NULL,
                    &dwBytesReturned,
                    NULL);

                if(bSuccess == FALSE)
                {
                    DeclareFailure(TRUE);
                    return ERROR;
                }

                //
                // Unpatch was successful, we can now safely remove
                // the libraries and close up shop.
                //
                SafeUnloadLibraries();
            }

            ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(hStatus, &ServiceStatus);

            return NO_ERROR;
        
        //
        // Just in case, special controls that need NO_ERROR returned.
        //
        case SERVICE_CONTROL_INTERROGATE:
        case SERVICE_CONTROL_DEVICEEVENT:
        case SERVICE_CONTROL_HARDWAREPROFILECHANGE:
        case SERVICE_CONTROL_POWEREVENT:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

VOID DeclareFailure( BOOL bInControlHandler )
{
    SafeUnloadLibraries();

    ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    ServiceStatus.dwWin32ExitCode = GetLastError();
    
    if(!bInControlHandler)
        SetServiceStatus(hStatus, &ServiceStatus);
}

VOID SafeUnloadLibraries()
{
    for( int i = 0; i < nDependencies; i++ )
    {
        if(hMods[i] != NULL)
            FreeLibrary(hMods[i]);
    }
}