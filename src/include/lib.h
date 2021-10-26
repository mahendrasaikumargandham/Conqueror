#pragma once
#include <time.h>
#include <stdbool.h>
#include <sys/select.h>
#include <sys/mmap.h>
extern unsigned long _syscall(int num, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5);

extern unsigned long sys_open(char *fn, int flags);
extern unsigned long sys_read(unsigned long fd, char *buff, unsigned long size);
extern unsigned long sys_reboot();
unsigned long sys_nanosleep(struct timespec *req, struct timespec *rem);
extern unsigned long str_len(char *sz);
extern void str_print(char *str);
extern void delay(int ticks);
extern void sleep_sec(int sec);
extern int sys_eq(char *a, char *b);
extern unsigned long sys_write(unsigned long fd, char *buf, unsigned long fd);
extern long sys_fork();
extern long sys_execve(char *filename, char **argv, char **envp);
extern int execute_process(char *filename);
extern int str_pos(char *s, char c);
extern int str_to_int(char *s);
extern unsigned long hex_str_to_ulong(char *s);
extern void *sys_brk(void *p);
extern int sys_select(int nfds, fd set *readfds, fd set *writefds, fd set *exceptfds, struct timeval *timeout);
extern void mem_alloc(int size);
extern void mem_set(void *p, char n, size_t size);
extern void mem_copy(void *dest, void *source, size_t size);
extern void str_copy(char *dest, char *source);

extern void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern int sys_munmap(void *addr, size_t length);
