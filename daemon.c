#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <tchar.h>
#include <time.h>

#include "ntservice.h"


#undef GOOGLE_ARRAYSIZE
#define GOOGLE_ARRAYSIZE(a) \
        ((sizeof(a) / sizeof(*(a))) / \
             static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))


TCHAR *app_dir = NULL;
TCHAR *app_name = NULL;
TCHAR log_path[MAX_PATH + 1] = {0};

TCHAR *PACKAGE_NAME = NULL;
TCHAR *PACKAGE_DISPLAY_NAME = NULL;
TCHAR *PACKAGE_DESCRIPTION = NULL;

TCHAR *cmd = NULL;
HANDLE handles[2];
const int idx_process = 0;
const int idx_action = 1;
volatile BOOL g_run = TRUE;

const int force_terminate_child_span = 5;
const int restart_child_span = 5;


#define TIME_FORMAT_LENGTH 24
#define TIME_FORMAT _T("%Y-%m-%d %H:%M:%S")

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5

TCHAR *ToLogLevel(int id)
{
#define case_statement(x) case LOG_LEVEL_##x: return _T(#x);

	switch(id)
	{
	case_statement(DEBUG);
	case_statement(INFO);
	case_statement(WARN);
	case_statement(ERROR);
	case_statement(FATAL);
	default:
		return _T("UNKOWN");
	}

#undef case_statement
}

void Time(TCHAR* buffer)
{
	time_t now;
	struct tm* timeinfo = NULL;

	time(&now);
	timeinfo = localtime(&now);

	if(timeinfo != NULL)
	{
		_tcsftime(buffer, TIME_FORMAT_LENGTH, TIME_FORMAT, timeinfo);
	}
	else
	{
		buffer[0] = '\0';
	}
}

void Log(int nLevel, const TCHAR *fmt, ...)
{
    const TCHAR *file = log_path;

	TCHAR time_buf[TIME_FORMAT_LENGTH];
	Time(time_buf);

	va_list arglist;
	va_start(arglist, fmt); 
	TCHAR msg[4096] = {'\0'};
	_vsntprintf(msg, GOOGLE_ARRAYSIZE(msg) - 1, fmt, arglist);
	va_end(arglist);

#ifdef  _UNICODE
	FILE *f = _tfopen(file, _T("a+, ccs=UTF-8"));
#else
	FILE *f = _tfopen(file, _T("a+"));
#endif
	if (f)
	{
		_ftprintf(f, _T("[%s]%s %s\n"), ToLogLevel(nLevel), time_buf, msg);
		fclose(f);
	}
}

void LoadConfig(const TCHAR *filename)
{
    TCHAR file[4096] = {0};
    _sntprintf(file, 4096, _T("%s\\%s.ini"), app_dir, app_name);

    TCHAR buf[4096] = {0};

    if(GetPrivateProfileString(_T("Settings"), _T("ServiceName"), NULL, buf, 4096, file))
	{
        PACKAGE_NAME = _tcsdup(buf);
	}
    else
	{
		Log(LOG_LEVEL_FATAL, _T("ServiceName must be set"));
        exit(-1);
	}

    GetPrivateProfileString(_T("Settings"), _T("Description"), NULL, buf, 4096, file);
    PACKAGE_DESCRIPTION = _tcsdup(buf);

	GetPrivateProfileString(_T("Settings"), _T("DisplayName"), NULL, buf, 4096, file);
	PACKAGE_DISPLAY_NAME = _tcsdup(buf ? buf : PACKAGE_NAME);

    if(GetPrivateProfileString(_T("Process0"), _T("CommandLine"), NULL, buf, 4096, file))
    {
		cmd = _tcsdup(buf);
	}
    else
	{
		Log(LOG_LEVEL_FATAL, _T("CommandLine must be set"));
        exit(-1);
	}
}

void init_server()
{
    handles[idx_action] = CreateEvent( 
            NULL,   // default security attributes
            FALSE,  // auto-reset event object
            FALSE,  // initial state is nonsignaled
            NULL);  // unnamed object

	if(!GetConsoleWindow())
	{
		Log(LOG_LEVEL_INFO, _T("No window used by the console associated with the calling process"));
		if(AllocConsole())
		{
			Log(LOG_LEVEL_INFO, _T("Alloc Console successfully"));
		}
	}
}

void fini_server()
{
    // Close event
    CloseHandle(handles[idx_action]);
}

void kill_child(HANDLE process)
{
	DWORD pid = GetProcessId(process);
	if(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid))
	{
		// wait child to exit
		DWORD event = WaitForSingleObject(process, 1000 * force_terminate_child_span);

		switch(event)
		{
		case WAIT_OBJECT_0:
			// child exit normally
			Log(LOG_LEVEL_INFO, _T("Process [%d] exited normally with CTRL_BREAK_EVENT"), pid);
			return;

		case WAIT_TIMEOUT:

			break;
		}
	}
	else
	{
		Log(LOG_LEVEL_INFO, _T("GenerateConsoleCtrlEvent failed (%d)"), GetLastError());
	}

	if(!TerminateProcess(process, 0))
	{
		Log(LOG_LEVEL_ERROR, _T("TerminateProcess failed (%d)"), GetLastError());
	}
	else
	{
		Log(LOG_LEVEL_INFO, _T("Process [%d] was forced to exit"), pid);
	}
}


int run_child()
{
    int retval;

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    DWORD ec = 0;

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    // Start the child process. 
    if( !CreateProcess( NULL,   // No module name (use command line)
        cmd,            // Command line
        NULL,           // Process handle not inheritable
        NULL,           // Thread handle not inheritable
        FALSE,          // Set handle inheritance to FALSE
		CREATE_NEW_PROCESS_GROUP,              // No creation flags
        NULL,           // Use parent's environment block
        NULL,           // Use parent's starting directory 
        &si,            // Pointer to STARTUPINFO structure
        &pi )           // Pointer to PROCESS_INFORMATION structure
    ) 
    {
		Log(LOG_LEVEL_ERROR, _T("CreateProcess failed (%d): %s"), GetLastError(), cmd);
        return false;
    }

    handles[idx_process] = pi.hProcess;

    // Wait until child process exits.
    DWORD event = WaitForMultipleObjects(GOOGLE_ARRAYSIZE(handles), handles, FALSE, INFINITE);

    switch(event)
    {
        case WAIT_OBJECT_0 + idx_process:
            if(GetExitCodeProcess(handles[idx_process], &ec))
            {
                Log(LOG_LEVEL_INFO, _T("Process [%u] have exited with exit code (%u)\n"), pi.dwProcessId, ec);
            }
            retval = true;

            break;

        case WAIT_OBJECT_0 + idx_action:
			kill_child(handles[idx_process]);
            retval = false;

            break;
    }

    // Close process and thread handles. 
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return retval;
}

void run_server()
{
   while(g_run)
   {
       run_child();
       Sleep(1000 * restart_child_span);
   }
}

void stop_server()
{
    g_run = false;

    if(!SetEvent(handles[idx_action]))
    {
        Log(LOG_LEVEL_ERROR, _T("SetEvent failed (%d)\n"), GetLastError());
    }
}

void Usage(TCHAR *prog)
{
    printf("Usage: %s [cmdline]\n", prog);
	printf(
		"		-d		run service\n"
		"		-i		install service\n"
		"		-u		uninstall service\n"
		"		-r		start service\n"
		"		-k		kill service\n");
}

const TCHAR* GetAppPath()
{
    static TCHAR szPath[MAX_PATH + 1] = {0};

    GetModuleFileName(NULL, szPath, GOOGLE_ARRAYSIZE(szPath));
    TCHAR *pos = _tcsrchr(szPath, '\\');

    if(pos)
    {
        *pos = '\0';
    }

    return szPath;
}

void InitApp()
{
	static TCHAR szPath[MAX_PATH + 1] = {0};

	DWORD size = GetModuleFileName(NULL, szPath, GOOGLE_ARRAYSIZE(szPath));
	TCHAR *pos = _tcsrchr(szPath, '\\');

	if(pos)
	{
		*pos = '\0';
		app_dir = szPath;
	}
	else
		exit(-1);

	app_name = ++pos;

	szPath[size - 4] = '\0';

    if(LOBYTE(LOWORD(GetVersion())) >= 6)
    {
        _sntprintf(log_path, MAX_PATH, _T("%s\\%s.log"), _tgetenv(_T("TEMP")), app_name); // vista and later
    }
    else
    {
        _sntprintf(log_path, MAX_PATH, _T("%s\\%s.log"), app_dir, app_name);
    }
}

int _tmain(int argc, TCHAR *argv[])
{
	InitApp();
	_tchdir(app_dir);
	LoadConfig(NULL);

    if(argv[1] && argv[1][0] == '-')
    {
        switch(argv[1][1])
        {
            case 'i':
                ServiceInstall();
                printf("install\n");
                break;

            case 'u':
                ServiceUninstall();
                printf("uninstall\n");
                break;

            case 'r':
                ServiceStart();
                printf("start\n");
                break;

            case 'k':
                ServiceStop();
                printf("stop\n");
                break;

            case 'd':
                {
                    init_server();

                    ServiceSetFunc(run_server, NULL, NULL, stop_server);
                    ServiceRun();

					fini_server();
                }
                break;

                // ServiceRestart();

            default:
                Usage(argv[0]);
        }
    }
    else
    {
        Usage(argv[0]);
    }

    return 0;
}


