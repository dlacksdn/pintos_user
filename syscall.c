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
  int *uargs = (int *) f->esp;
  if (!is_user_vaddr (uargs))
    process_exit (-1);

  switch (uargs[0]) 
    {
      case SYS_CREATE:
        syscall_create (f, uargs);
        break;

      case SYS_REMOVE:
        syscall_remove (f, uargs);
        break;
      
      case SYS_OPEN:
        syscall_open (f, uargs);
        break;

      case SYS_READ:
        syscall_read (f, uargs);
        break;

      case SYS_WRITE:
        syscall_write (f, uargs);
        break;

      case SYS_FILESIZE:
        syscall_filesize (f, uargs);
        break;

      case SYS_SEEK:
        syscall_seek (f, uargs);
        break;

      case SYS_TELL:
        syscall_tell (f, uargs);
        break;

      case SYS_CLOSE:
        syscall_close (f, uargs);
        break;

      case SYS_EXIT:
        syscall_exit (f, uargs);
        break;

      default:
          process_exit (-1);
    }
}

static void 
check_address(const void *addr,size_t size, struct intr_frame *f) // file을 인자로 받은 syscall 전용
{
     uint8_t *start = (uint8_t *) addr;
  for (size_t i = 0; i < size; i++) {
    void *check = start + i;
    if (check == NULL || !is_user_vaddr(check) ||
        pagedir_get_page(thread_current()->pagedir, check) == NULL) {

      printf("[!] Invalid user address access at %p\n", check);
      thread_current()->exit_status = -1; // 무언가가 잘못되어서 실행된 코드이니 status를 -1로 바꾼다
      f->eax = false;
      thread_exit();  
    }
  } 
}

/* 1. bool create(const char *file, unsigned initial_size) */
void 
syscall_create(struct intr_frame *f, int *uargs) 
{
    const char *file = uargs[1];
    check_address(file, strlen(file) + 1, f); // null까지 보려고 +1을 한다
    unsigned initial_size = uargs[2];
    f->eax = filesys_create(file, initial_size);
}

/* 2. bool remove(const char *file) */
// 여기서 file이라 함은 file 이름을 말하는 것 같다
void 
syscall_remove(struct intr_frame *f, int *uargs) 
{
    const char *file = uargs[1];
    check_address(file, strlen(file) + 1, f);
    f->eax = filesys_remove(file);
}

// int open (const char *file)
// 성공하면 fd, 실패하면 -1을 리턴 -> 이걸 eax로 한다
void
syscall_open (struct intr_frame *f, int *uargs)
{
  const char *fn = (const char *) uargs[1];
  if (!is_user_vaddr (fn))
    {
      f->eax = -1; // syscall의 반환값을 -1로 설정하라 -> open 실패 시 유저 프로그램에 -1을 돌려줘라
      thread_current()->exit_status = -1;
      thread_exit();
    }
  check_address(fn, strlen(fn) + 1); // 그럼 이게 필요 없긴 함 -> f->eax = -1이 뭔 뜻인지만 알면 결정 가능

  lock_acquire (&fs_lock);
  struct file *file = filesys_open (fn);
  if (file == NULL)
    f->eax = -1;
  else
    f->eax = process_add_file (file); // fd 인덱스를 리턴
  lock_release (&fs_lock);
}

// int read (int fd, void *buffer, unsigned size)
void
syscall_read (struct intr_frame *f, int *uargs)
{
  int    fd  = uargs[1];
  void  *buf = (void *) uargs[2];
  unsigned sz = (unsigned) uargs[3];

  if (!is_user_vaddr (buf) ||
      !is_user_vaddr ((uint8_t *)buf + sz - 1))
    {
      f->eax = -1; // eax가 뭔지만 알면 check_address를 쓰냐 마냐를 결정한다
      thread_current()->exit_status = -1;
      thread_exit();
    }

  if (fd == STDIN_FILENO) // fd가 0이라는게 버퍼에 입력한다는 뜻인가?
    {
      for (int i = 0; i < (int) sz; i++)
        ((char *) buf)[i] = input_getc ();
      f->eax = sz;
    }
  else // 이 둘은 무슨 차이인가?
    {
      lock_acquire (&fs_lock);
      struct file *file = process_get_file (fd);
      if (file != NULL)
        f->eax = file_read (file, buf, sz); // read한 byte수를 리턴
      else
        f->eax = -1;
      lock_release (&fs_lock);
    }
}

// int write (int fd, const void *buffer, unsigned size)
void
syscall_write (struct intr_frame *f, int *uargs)
{
  int    fd  = uargs[1];
  void  *buf = (void *) uargs[2];
  unsigned sz = (unsigned) uargs[3];

  if (!is_user_vaddr (buf) ||
      !is_user_vaddr ((uint8_t *)buf + sz - 1))
    {
      f->eax = -1;
      thread_exit(); // write는 실제 쓰인 byte 수를 리턴하니 0을 리턴 (exit_status 초기값이 0)
    }

  if (fd == STDOUT_FILENO) 
    {
      putbuf (buf, sz);
      f->eax = sz;
    }
  else 
    {
      lock_acquire (&fs_lock);
      struct file *file = process_get_file (fd);
      if (file != NULL)
        f->eax = file_write (file, buf, sz); // write된 byte 수를 리턴
      else
        f->eax = -1;
      lock_release (&fs_lock);
    }
}

// int filesize (int fd) 
void
syscall_filesize (struct intr_frame *f, int *uargs)
{
  int fd = uargs[1];
  struct file *file = process_get_file (fd);
  if (file == NULL)
    f->eax = -1; // eax로 결과를 보내나?
  else
    f->eax = file_length (file); // inode->data의 length를 잰다
}

// void seek (int fd, unsigned position) 
void
syscall_seek (struct intr_frame *f, int *uargs)
{
  int fd = uargs[1];
  unsigned position = uargs[2];
  struct file *fp = process_get_file (fd);
  if (fp != NULL) {
    file_seek(fp, position);
  }
}

/* 8. unsigned tell(int fd) */
void 
syscall_tell(struct intr_frame *f, int *uargs) 
{
    int fd = uargs[1];
    struct file *fp = process_get_file (fd);
    if (fp == NULL) {
      f->eax = -1;
      thread_current()->exit_status = -1;
      thread_exit();
    }
    f->eax = file_tell(fp);
}

// void close (int fd)
void
syscall_close (struct intr_frame *f, int *uargs)
{
  int fd = uargs[1];
  lock_acquire (&fs_lock);
  if (process_close_file (fd))
    f->eax = 0;
  else
    f->eax = -1;
  lock_release (&fs_lock);
}

/* SYS_EXIT */
void
syscall_exit (struct intr_frame *f, int *uargs)
{
  
  // printf ("%s: exit(%d)\n", thread_name(), status);
  //process_exit (uargs[1]);
  thread_exit();
}













