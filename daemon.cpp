#include <windows.h>
#if _WIN32_WINNT >= 0x0602 /* Windows 8 and Windows Server 2012 */
# include <Processthreadsapi.h>
#endif
#ifdef __MINGW32__
// C++ Runtime Library (libstdc++) require C99 printf(), but we dont't use it at all
# define __USE_MINGW_ANSI_STDIO 0
#endif
#include <assert.h>
#include <direct.h>
#include <signal.h>
#include <stdio.h>
#include <tchar.h>
#include <time.h>

#include "ntservice.h"
#include "bsd_getopt.h"

#define min(a,b)            (((a) < (b)) ? (a) : (b))

#undef GOOGLE_ARRAYSIZE
#define GOOGLE_ARRAYSIZE(a) \
        ((sizeof(a) / sizeof(*(a))) / \
             static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))


TCHAR *app_dir = NULL;
TCHAR *app_name = NULL;
TCHAR log_path[MAX_PATH + 1] = {0};

#define LASTERROR(buf)  ErrorDetail(GetLastError(), buf, _countof(buf) - 1)

#define MAX_PROCESS_NUM (MAXIMUM_WAIT_OBJECTS - 1)

struct Process
{
    time_t start_time;
    TCHAR *cmd;
    TCHAR *cwd;
    PROCESS_INFORMATION pi;

    enum State
    {
        stop,
        runing,
    } state;
} processes[MAX_PROCESS_NUM] = {};

int process_count = 0;
HANDLE handles[MAX_PROCESS_NUM + 1];
const int idx_action = 0;
const int idx_process = 1;
HANDLE *process_handles = handles + idx_process;
volatile BOOL g_run = TRUE;
BOOL run_as_service = FALSE;

const int force_terminate_child_span = 5;
const int restart_child_span = 5;


#define TIME_FORMAT_LENGTH 24
#define TIME_FORMAT _T("%Y-%m-%d %H:%M:%S")

#define LOG_LEVEL_DEBUG 1
#define LOG_LEVEL_INFO 2
#define LOG_LEVEL_WARN 3
#define LOG_LEVEL_ERROR 4
#define LOG_LEVEL_FATAL 5

const TCHAR* ToLogLevel(int id)
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
	struct tm timeinfo;

	time(&now);
    errno_t err = localtime_s(&timeinfo, &now);

    if (0 == err)
	{
		_tcsftime(buffer, TIME_FORMAT_LENGTH, TIME_FORMAT, &timeinfo);
	}
	else
	{
		buffer[0] = '\0';
	}
}

void Log(int nLevel, const TCHAR *file, int lineno, const TCHAR *fmt, ...)
{
	TCHAR time_buf[TIME_FORMAT_LENGTH];
	Time(time_buf);

	va_list arglist;
	va_start(arglist, fmt); 
	TCHAR msg[4096] = {'\0'};
    _vsntprintf_s(msg, _TRUNCATE, fmt, arglist);
	va_end(arglist);

#ifdef  _UNICODE
	FILE *f = _tfopen(log_path, _T("a+, ccs=UTF-8"));
#else
	FILE *f = _tfopen(log_path, _T("a+"));
#endif
	if (f)
	{
		_ftprintf(f, _T("[%s]%s %s:%d %s\n"), ToLogLevel(nLevel), time_buf, file, lineno, msg);
		fclose(f);
        f = NULL;
	}

    if (!run_as_service)
    {
#if _UNICODE
        DWORD written = 0;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

        int len = lstrlen(msg);
        msg[len] = '\n';
        len += 1;

        if (INVALID_HANDLE_VALUE != out)
        {
            // convert unicode string to console 's code page
            UINT cp = GetConsoleOutputCP();
            DWORD mulit_byte_len = WideCharToMultiByte(cp, 0, msg, len, NULL, 0, NULL, NULL);

            char *multi_byte_msg = (char*)malloc(mulit_byte_len);

            WideCharToMultiByte(cp, 0, msg, len, multi_byte_msg, mulit_byte_len, NULL, NULL);

            WriteFile(out, multi_byte_msg, mulit_byte_len, &written, NULL);

            free(multi_byte_msg);
        }
#else
        _ftprintf(stdout, _T("%s\n"), msg);
#endif
    }
}

#if _WIN32
# define DIRECTORY_SEPARATOR_CHAR '\\'
#else
# define DIRECTORY_SEPARATOR_CHAR '/'
#endif

#define __FILENAME__ (_tcsrchr(_T(__FILE__), DIRECTORY_SEPARATOR_CHAR) ? \
        _tcsrchr(_T(__FILE__), DIRECTORY_SEPARATOR_CHAR) + 1 : _T(__FILE__))

#define LOG(prio, fmt, ...)                                                \
        Log(prio, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__))

#define LOGD(fmt, ...) Log(LOG_LEVEL_DEBUG, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__)
#define LOGI(fmt, ...) Log(LOG_LEVEL_INFO, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__)
#define LOGW(fmt, ...) Log(LOG_LEVEL_WARN, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__)
#define LOGE(fmt, ...) Log(LOG_LEVEL_ERROR, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__)
#define LOGF(fmt, ...) do { \
    Log(LOG_LEVEL_FATAL, __FILENAME__, __LINE__, fmt, ## __VA_ARGS__); \
    exit(EXIT_FAILURE); \
} while (0)

const TCHAR* ErrorDetail(DWORD code, TCHAR *szMsgBuf, size_t len)
{
    DWORD ret_len = 0;
    if (!(ret_len = FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        szMsgBuf,
        len, NULL)))
    {
        _sntprintf(szMsgBuf, len, _T("%ud"), code);
    }

    if (ret_len > 2 && szMsgBuf[ret_len - 2] == '\r' && szMsgBuf[ret_len - 1] == '\n')
    {
        szMsgBuf[ret_len - 2] = '\0';
    }

    return szMsgBuf;
}

void LoadConfig(const TCHAR *file)
{
    TCHAR buf[4096] = { 0 };
    TCHAR temp_file[4096] = {0};

    if (file == NULL || file[0] == '\0')
    {
        _sntprintf_s(temp_file, _TRUNCATE, _T("%s\\%s.ini"), app_dir, app_name);
        file = temp_file;
    }

    if(GetPrivateProfileString(_T("Settings"), _T("ServiceName"), NULL, buf, 4096, file))
	{
        PACKAGE_NAME = _tcsdup(buf);
	}
    else
	{
		LOGF(_T("ServiceName must be set"));
	}

    GetPrivateProfileString(_T("Settings"), _T("Description"), NULL, buf, 4096, file);
    PACKAGE_DESCRIPTION = _tcsdup(buf);

	GetPrivateProfileString(_T("Settings"), _T("DisplayName"), NULL, buf, 4096, file);
	PACKAGE_DISPLAY_NAME = _tcsdup(buf[0] ? buf : PACKAGE_NAME);

    for (int i = 0; i < MAX_PROCESS_NUM; ++i)
    {
        TCHAR appname[256] = {};
        _sntprintf_s(appname, _TRUNCATE, _T("Process%d"), i);
        if (GetPrivateProfileString(appname, _T("CommandLine"), NULL, buf, 4096, file))
        {
            processes[process_count].cmd = _tcsdup(buf);
            if (GetPrivateProfileString(appname, _T("Directory"), NULL, buf, 4096, file))
            {
                processes[process_count].cwd = _tcsdup(buf);
            }
            ++process_count;
        }
    }
    
    if (process_count <= 0)
	{
		LOGF(_T("CommandLine must be set"));
	}

	GetPrivateProfileString(_T("Settings"), _T("ServiceStartName"), NULL, buf, 4096, file);
    PACKAGE_START_NAME = _tcsdup(buf);
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
		LOGI(_T("No window used by the console associated with the calling process"));
		if(AllocConsole())
		{
			LOGI(_T("Alloc Console successfully"));
		}
	}
}

void fini_server()
{
    // Close event
    CloseHandle(handles[idx_action]);
}

void kill_child(struct Process *p)
{
    TCHAR buf[4096] = {'\0'};
	DWORD pid = GetProcessId(p->pi.hProcess);
	if(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid))
	{
		// wait child to exit
        DWORD event = WaitForSingleObject(p->pi.hProcess, 1000 * force_terminate_child_span);

		switch(event)
		{
		case WAIT_OBJECT_0:
			// child exit normally
			LOGI(_T("Process [%d] exited normally with CTRL_BREAK_EVENT"), pid);
			return;

		case WAIT_TIMEOUT:

			break;
		}
	}
	else
	{
		LOGI(_T("GenerateConsoleCtrlEvent failed (%s)"), LASTERROR(buf));
	}

    if (!TerminateProcess(p->pi.hProcess, 0))
	{
        LOGE(_T("TerminateProcess failed (%s)"), LASTERROR(buf));
	}
	else
	{
		LOGI(_T("Process [%d] was forced to exit"), pid);
	}

    // Close process and thread handles. 
    CloseHandle(p->pi.hProcess);
    CloseHandle(p->pi.hThread);
}

void run_server()
{
    time_t now;
    time_t wait_until;

    struct Process *processes_to_wait[MAX_PROCESS_NUM];
    int processes_to_wait_count = 0;

    struct Process *processes_to_start[MAX_PROCESS_NUM];
    int processes_to_start_count = 0;
    TCHAR buf[4096] = {'\0'};

    DWORD ec = 0;

    // initialize processes to create
    now = time(NULL);
    for (int i = 0; i < process_count; ++i)
    {
        processes_to_start[i] = processes +i;
        processes_to_start[i]->start_time = now;
        processes_to_start[i]->state = Process::stop;
    }
    processes_to_start_count = process_count;


    while (g_run)
    {
        wait_until = INT_MAX;

        // 1. create child processes and calculate how long to wait
        for (int i = 0; i < processes_to_start_count; )
        {
            assert(processes_to_start[i]);
            assert(processes_to_start[i]->state == Process::stop);

            if (processes_to_start[i]->start_time <= now)
            {
                STARTUPINFO si;
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ZeroMemory(&processes_to_start[i]->pi, sizeof(processes_to_start[i]->pi));

                // Start the child process. 
                if (!CreateProcess(NULL,   // No module name (use command line)
                    processes_to_start[i]->cmd,            // Command line
                    NULL,           // Process handle not inheritable
                    NULL,           // Thread handle not inheritable
                    FALSE,          // Set handle inheritance to FALSE
                    CREATE_NEW_PROCESS_GROUP,              // new process group to limit CtrlEvent scope
                    NULL,           // Use parent's environment block
                    processes_to_start[i]->cwd,           // Set Current Directory
                    &si,            // Pointer to STARTUPINFO structure
                    &processes_to_start[i]->pi)           // Pointer to PROCESS_INFORMATION structure
                    )
                {
                    LOGE(_T("CreateProcess failed (%s): %s"), LASTERROR(buf), processes_to_start[i]->cmd);
                    // retry
                    processes_to_start[i]->start_time = now + restart_child_span;
                }
                else
                {
                    LOGI(_T("CreateProcess pid %u: %s"), processes_to_start[i]->pi.dwProcessId, processes_to_start[i]->cmd);
                    processes_to_start[i]->state = Process::runing;
                    process_handles[processes_to_wait_count] = processes_to_start[i]->pi.hProcess; // 
                    processes_to_wait[processes_to_wait_count] = processes_to_start[i];
                    processes_to_wait_count += 1;

                    if (processes_to_start_count - 1 > i)
                    {
                        processes_to_start[i] = processes_to_start[processes_to_start_count - 1];
                        processes_to_start[processes_to_start_count - 1] = NULL;
                    }
                    else
                    {
                        processes_to_start[i] = NULL;
                    }
                    processes_to_start_count -= 1;

                    continue; // iterate i again
                }
            }

            assert(processes_to_start[i]->state == Process::stop);
            wait_until = min(processes_to_start[i]->start_time, wait_until);

            ++i;
        }

        assert(wait_until >= now);

        // 2. Wait until child process exits.
        DWORD event = WaitForMultipleObjects(idx_process + processes_to_wait_count, handles, FALSE,
                (wait_until != INT_MAX ? 1000 * (wait_until - now) : INFINITE));
        now = time(NULL);

        if (event == WAIT_OBJECT_0 + idx_action)
        {
            // 3.1 kill all child
            for (int i = 0; i < processes_to_wait_count; ++i)
            {
                kill_child(processes_to_wait[i]);
            }
        }
        else if (event >= WAIT_OBJECT_0 + idx_process && event <= WAIT_OBJECT_0 + idx_process + processes_to_wait_count)
        {
            // 3.2 child process have been exited
            int idx = event - (WAIT_OBJECT_0 + idx_process);

            assert(processes_to_wait[idx]->pi.hProcess == process_handles[idx]);
            if (GetExitCodeProcess(process_handles[idx], &ec))
            {
                LOGI(_T("Process [%u] have exited with exit code (%u)"), processes_to_wait[idx]->pi.dwProcessId, ec);
            }

            // Close process and thread handles. 
            CloseHandle(processes_to_wait[idx]->pi.hProcess);
            CloseHandle(processes_to_wait[idx]->pi.hThread);

            // prepare to restart child
            processes_to_wait[idx]->start_time = now + restart_child_span;
            processes_to_wait[idx]->state = Process::stop;

            processes_to_start[processes_to_start_count] = processes_to_wait[idx];
            processes_to_start_count += 1;
            if (idx < processes_to_wait_count - 1)
            {
                processes_to_wait[idx] = processes_to_wait[processes_to_wait_count - 1];
                processes_to_wait[processes_to_wait_count - 1] = NULL;
                process_handles[idx] = process_handles[processes_to_wait_count - 1];

                assert(processes_to_wait[idx]->pi.hProcess == process_handles[idx]);
            }
            else
            {
                processes_to_wait[idx] = NULL;
            }
            processes_to_wait_count -= 1;
        }
    }
}

void stop_server()
{
    TCHAR buf[4096] = {'\0'};
    g_run = false;

    if (!SetEvent(handles[idx_action]))
    {
        // In signal, we cannot call printf(), fread(), malloc(), time() ...
        if (!run_as_service) 
        {
            exit(EXIT_FAILURE);
        }
        else 
        {
            LOGE(_T("SetEvent failed (%d)"), LASTERROR(buf));
        }
    }
}

BOOL WINAPI console_ctrl_handler(DWORD event)
{
	switch (event)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	{
        stop_server();
	}
	return TRUE;

	default:
		return FALSE;
	}
}

void Usage(TCHAR *prog)
{
    TCHAR configration_file[MAX_PATH + 1] = {};
    _sntprintf_s(configration_file, _TRUNCATE, _T("%s\\%s.ini"), app_dir, app_name);

    _ftprintf(stdout, _T("\nUsage: %s [cmdline]\n"), prog);
    _ftprintf(stdout,
        _T("  -c	configuration file, default %s\n")
        _T("  -f	run foreground\n")
		_T("  -d	run as a background service\n")
		_T("  -i	install service\n")
		_T("  -u	uninstall service\n")
		_T("  -r	start service\n")
		_T("  -k	kill service\n"),
        configration_file);
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

	if (pos)
	{
		*pos = '\0';
		app_dir = szPath;
	}
	else
		exit(-1);

	app_name = ++pos;

	szPath[size - 4] = '\0';

    if (LOBYTE(LOWORD(GetVersion())) >= 6)
    {
        _sntprintf_s(log_path, _TRUNCATE, _T("%s\\%s.log"), _tgetenv(_T("TEMP")), app_name); // vista and later
    }
    else
    {
        _sntprintf_s(log_path, _TRUNCATE, _T("%s\\%s.log"), app_dir, app_name);
    }
}

extern "C" int _tmain(int argc, TCHAR *argv[])
{
	bool show_usage = false;
    int action_num = 0;
    const TCHAR *configration_file = NULL;
	int ret = 0;

	InitApp();

	int opt;

	while ((opt = getopt(argc, argv, _T("c:hiurkdf"))) != -1) 
	{
        switch (opt)
        {
        case 'c':
            configration_file = optarg;
            break;

        case 'i':
        case 'u':
        case 'r':
        case 'k':
        case 'd':
        case 'f':
            action_num = opt;
            break;

        case 'h':
            ret = 1;
        case '?': //_tprintf(_T("unknown option character")); // getopt() output error message directly
        default:
            show_usage = true;
        }
    }

	if (argc == 1 || show_usage || action_num == 0)
	{
		Usage(argv[0]);
	}


    if (action_num)
    {
        const TCHAR *action = NULL;
        LoadConfig(configration_file);
        _tchdir(app_dir);

        switch (action_num)
        {
        case 'i':
            ret = ServiceInstall();
            action = _T("install");
            break;

        case 'u':
            ret = ServiceUninstall();
            action = _T("uninstall");
            break;

        case 'r':
            ret = ServiceStart();
            action = _T("start");
            break;

        case 'k':
            ret = ServiceStop();
            action = _T("stop");
            break;

        case 'd':
        {
            run_as_service = TRUE;
            init_server();

            ServiceSetFunc(run_server, NULL, NULL, stop_server);
            ret = ServiceRun();

            fini_server();
        }
        break;

        case 'f':
        {
			SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
            init_server();

            run_server();

            fini_server();
        }
        break;
        }

        if (action)
        {
            _ftprintf(stdout, _T("%s service %s\n"), action, (ret == 1 ? _T("successful") : _T("failed")));
        }
    }


	if (ret == 1)
	{
		return 0;
	}	

	return -1;
}


