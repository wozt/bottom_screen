#ifndef BS_WIIU_PROC_H
#define BS_WIIU_PROC_H

#ifdef __cplusplus
extern "C" {
#endif

void proc_init(void);
int  proc_running(void);
int  proc_release_pending(void);
int  proc_release_and_wait(void);
void proc_stop(void);
void proc_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
