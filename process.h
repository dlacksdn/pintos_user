#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (int);
void process_activate (void);

/* Returns the file* for descriptor fd, or NULL if invalid. */
struct file *process_get_file (int fd);

/* Allocates a new descriptor for file, returns fd (>=2) or -1 on failure. */
int process_add_file (struct file *file);

/* Closes and removes descriptor fd; returns true on success, false otherwise. */
bool process_close_file (int fd);

#endif /* userprog/process.h */
