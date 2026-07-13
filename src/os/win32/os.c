#include "config.h"

#include <stdlib.h>
#include <windows.h>
#include <io.h>

#include "os.h"
#include "options.h"
#include "lib.h"
#include "util.h"

/* -------------------------------------------------------------------------- */

const struct cmdline_options_s os_cmdline_options[] = {
    { NULL, 0, NULL, NULL, NULL, NULL }
};

/* -------------------------------------------------------------------------- */

static char *data_path = NULL;
static char *user_path = NULL;
static char *all_data_paths[] = { NULL, NULL, NULL, NULL };

/* -------------------------------------------------------------------------- */

const char *idstr_os = "win32";

int os_early_init(void)
{
    return 0;
}

int os_init(void)
{
    return 0;
}

void os_shutdown(void)
{
    lib_free(data_path);
    data_path = NULL;
    lib_free(user_path);
    user_path = NULL;
}

const char **os_get_paths_data(void)
{
    int i = 0;
    if (data_path) {
        all_data_paths[i++] = data_path;
    }
    all_data_paths[i++] = ".";
    all_data_paths[i++] = ".\\data";
    all_data_paths[i] = NULL;
    return (const char **)all_data_paths;
}

const char *os_get_path_data(void)
{
    return data_path;
}

void os_set_path_data(const char *path)
{
    data_path = lib_stralloc(path);
}

const char *os_get_path_user(void)
{
    if (user_path == NULL) {
        user_path = lib_stralloc(".");
    }
    return user_path;
}

void os_set_path_user(const char *path)
{
    if (user_path) {
        lib_free(user_path);
        user_path = NULL;
    }
    user_path = lib_stralloc(path);
}

int os_make_path(const char *path)
{
    if ((path == NULL) || ((path[0] == '.') && (path[1] == '\0'))) {
        return 0;
    }
    return mkdir(path);
}

int os_make_path_user(void)
{
    return os_make_path(os_get_path_user());
}

int os_make_path_for(const char *filename)
{
    int res = 0;
    char *path;
    util_fname_split(filename, &path, NULL);
    if (path != NULL) {
        res = os_make_path(path);
        lib_free(path);
    }
    return res;
}

const char *os_get_fname_save_slot(char *buf, size_t bufsize, int savei/*1..9*/)
{
    return NULL;
}

const char *os_get_fname_save_year(char *buf, size_t bufsize, int year/*2300..*/)
{
    return NULL;
}

const char *os_get_fname_cfg(char *buf, size_t bufsize, const char *gamestr, const char *uistr, const char *hwstr)
{
    return NULL;
}

const char *os_get_fname_log(char *buf, size_t bufsize)
{
    if (buf) {
        lib_strcpy(buf, "1oom_log.txt", bufsize);
        return buf;
    }
    return "1oom_log.txt";
}

/* -------------------------------------------------------------------------- */
/* 1oom-mp: background process spawn (the Multiplayer menu starts the local server with this). */

static HANDLE s_spawned[8] = { NULL };

int os_spawn_bg(const char **argv)
{
    char cmdline[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    size_t o = 0;
    int slot = -1;
    for (int i = 0; i < 8; ++i) { if (!s_spawned[i]) { slot = i; break; } }
    if (slot < 0) { return -1; }
    for (int i = 0; argv[i]; ++i) {   /* quote each arg (paths may contain spaces) */
        size_t n = strlen(argv[i]);
        if ((o + n + 4) >= sizeof(cmdline)) { return -1; }
        if (i) { cmdline[o++] = ' '; }
        cmdline[o++] = '"';
        memcpy(cmdline + o, argv[i], n); o += n;
        cmdline[o++] = '"';
    }
    cmdline[o] = '\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    s_spawned[slot] = pi.hProcess;
    return slot;
}

void os_spawn_kill(int handle)
{
    if ((handle < 0) || (handle >= 8) || !s_spawned[handle]) { return; }
    TerminateProcess(s_spawned[handle], 0);
    WaitForSingleObject(s_spawned[handle], 3000);
    CloseHandle(s_spawned[handle]);
    s_spawned[handle] = NULL;
}

const char *os_get_path_exe_dir(char *buf, size_t bufsize)
{
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsize);
    char *sl;
    if ((n == 0) || (n >= bufsize)) { buf[0] = '\0'; return buf; }
    sl = strrchr(buf, '\\');
    if (!sl) { sl = strrchr(buf, '/'); }
    if (sl) { *sl = '\0'; } else { buf[0] = '\0'; }
    return buf;
}
