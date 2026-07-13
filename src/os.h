#ifndef INC_1OOM_OS_H
#define INC_1OOM_OS_H

/* API to os/ */

#include <stdio.h>

#include "osdefs.h"
#include "options.h"
#include "types.h"

extern const char *idstr_os;

extern int os_early_init(void);
extern int os_init(void);
extern void os_shutdown(void);

extern const struct cmdline_options_s os_cmdline_options[];

extern const char **os_get_paths_data(void);
extern const char *os_get_path_data(void);
extern void os_set_path_data(const char *path);
extern const char *os_get_path_user(void);
extern void os_set_path_user(const char *path);
extern int os_make_path(const char *path);
extern int os_make_path_user(void);
extern int os_make_path_for(const char *filename);

extern const char *os_get_fname_save_slot(char *buf, size_t bufsize, int savei/*1..9*/);
extern const char *os_get_fname_save_year(char *buf, size_t bufsize, int year/*2300..*/);
extern const char *os_get_fname_cfg(char *buf, size_t bufsize, const char *gamestr, const char *uistr, const char *hwstr);
extern const char *os_get_fname_log(char *buf, size_t bufsize);

/* 1oom-mp: launch a background process (NULL-terminated argv, argv[0] = binary path). Returns a
   handle (>= 0) for os_spawn_kill, or -1. Used by the Multiplayer menu to start the local server. */
extern int os_spawn_bg(const char **argv);
extern void os_spawn_kill(int handle);
/* 1oom-mp: directory containing the running executable ("" if unknown), for finding sibling binaries
   (the client launches the server that sits next to it). */
extern const char *os_get_path_exe_dir(char *buf, size_t bufsize);

#endif
