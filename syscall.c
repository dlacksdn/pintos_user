#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
// new
#include "threads/synch.h"        /* for struct lock */
#include "lib/kernel/console.h"   /* for putbuf() */
#include "userprog/process.h"     /* for process_exit() */
#include "threads/vaddr.h"        /* for is_user_vaddr() */

#define STDIN_FILENO   0   /* for SYS_READ */
#define STDOUT_FILENO  1   /* for SYS_WRITE */

static void syscall_handler (struct intr_frame *);


/* A single global lock protecting all file‐system calls. */
static struct lock fs_lock;

/* Called once at startup to register the syscall handler and init the lock. */
void
syscall_init (void) 
{
  lock_init (&fs_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  printf("in SYS_WRITE_test1\n");
  int *uargs = (int *) f->esp;
  if (!is_user_vaddr (uargs))
    process_exit (-1);

  switch (uargs[0]) 
    {
      case SYS_EXIT:
        process_exit (uargs[1]);
        break;

      case SYS_WRITE:
        {
          printf("in SYS_WRITE_test1\n");
          int fd    = uargs[1];
          void *buf = (void *) uargs[2];
          unsigned sz = (unsigned) uargs[3];

          

          if (!is_user_vaddr (buf) ||
              !is_user_vaddr ((uint8_t *)buf + sz - 1))
            {
              f->eax = -1;
              break;
            }

          if (fd == STDOUT_FILENO) 
            {
              /* Console writes—no lock needed for console itself. */
              putbuf (buf, sz);
              f->eax = sz;
            }
          else 
            {
              /* Any real file access must be serialized. */
              lock_acquire (&fs_lock);
              struct file *file = process_get_file (fd);
              if (file != NULL)
                f->eax = file_write (file, buf, sz);
              else
                f->eax = -1;
              lock_release (&fs_lock);
            }
        }
        break;

      case SYS_OPEN:
        {
          const char *fn = (const char *) uargs[1];
          if (!is_user_vaddr (fn))
            { f->eax = -1; break; }

          lock_acquire (&fs_lock);
          struct file *file = filesys_open (fn);
          if (file == NULL)
            f->eax = -1;
          else
            {
              int fd = process_add_file (file);
              f->eax = fd;
            }
          lock_release (&fs_lock);
        }
        break;

      case SYS_READ:
        {
          int fd    = uargs[1];
          void *buf = (void *) uargs[2];
          unsigned sz = (unsigned) uargs[3];

          if (!is_user_vaddr (buf) ||
              !is_user_vaddr ((uint8_t *)buf + sz - 1))
            { f->eax = -1; break; }

          if (fd == STDIN_FILENO)
            {
              /* Keyboard input—no fs lock. */
              int i;
              for (i = 0; i < (int)sz; i++)
                ((char *)buf)[i] = input_getc ();
              f->eax = sz;
            }
          else
            {
              lock_acquire (&fs_lock);
              struct file *file = process_get_file (fd);
              if (file != NULL)
                f->eax = file_read (file, buf, sz);
              else
                f->eax = -1;
              lock_release (&fs_lock);
            }
        }
        break;

      case SYS_CLOSE:
        {
          int fd = uargs[1];
          lock_acquire (&fs_lock);
          if (process_close_file (fd))
            f->eax = 0;
          else
            f->eax = -1;
          lock_release (&fs_lock);
        }
        break;

      /* … other syscalls (filesize, seek, tell, remove…) all go here,
         each wrapped by lock_acquire/lock_release around filesys_* calls. */

      default:
        process_exit (-1);
    }
}
