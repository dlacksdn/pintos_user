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

      case SYS_EXEC:
        syscall_exec (f, uargs);

      case SYS_HALT:
        syscall_halt ();

      case SYS_WAIT:
        syscall_wait (f, uargs);

      default:
          process_exit (-1);
    }
}

// null pointer | virtual memory와 mapping되지 않은 pointer | kernel virtual address에 있는 pointer (PHYS_BASE 위) 를 막는 함수
static void
check_address_buffer(const void *addr, size_t size) 
{
  uint8_t *start = (uint8_t *) addr;
  for (size_t i = 0; i < size; i++) {
    void *check = start + i;
    if (check == NULL || !is_user_vaddr(check) ||
        pagedir_get_page(thread_current()->pagedir, check) == NULL) {

        thread_current()->exit_status = -1;
        thread_exit();
    }
  }
}

static void
check_address_file(const char *addr)
{
  while (true) 
    {
      /* 1) 널 체크, user 영역 체크, 매핑 체크 */
      if (addr == NULL
          || !is_user_vaddr (addr)
          || pagedir_get_page (thread_current ()->pagedir, addr) == NULL) {

            thread_current()->exit_status = -1;
            thread_exit();
          }

      /* 2) 널 문자 만나면 성공적으로 끝 */
      if (*addr == '\0')
        return;

      /* 3) 다음 문자 검사 */
      addr++;
    }
}

/* 1. bool create(const char *file, unsigned initial_size) */
void 
syscall_create(struct intr_frame *f, int *uargs) 
{
  const char *file = (const char *) uargs[1];
  check_address_file(file);  

  unsigned initial_size = uargs[2];
  f->eax = filesys_create(file, initial_size);
}

/* 2. bool remove(const char *file) */
// 여기서 file이라 함은 file 이름을 말하는 것 같다
void 
syscall_remove(struct intr_frame *f, int *uargs) 
{
    const char *file = (const char *) uargs[1];
    check_address_file(file);
    
    f->eax = filesys_remove(file);
}

// int open (const char *file)
// 성공하면 fd, 실패하면 -1을 리턴 -> 이걸 eax로 한다
void
syscall_open (struct intr_frame *f, int *uargs)
{
  const char *fn = (const char *) uargs[1];
  check_address_file(fn);

  lock_acquire (&fs_lock);
  struct file *file = filesys_open (fn);
  if (file == NULL)
    f->eax = -1;
  else
    f->eax = process_add_file (file); // fd 인덱스를 리턴
  lock_release (&fs_lock);
}

// int read (int fd, void *buffer, unsigned size)
// 성공 : 실제 읽은 byte 수 리턴 (EOF면 0도 가능) | 실패 : -1 리턴 (EOF제외한 다른 이유로 인해 읽지 못한 경우)
void
syscall_read (struct intr_frame *f, int *uargs)
{
  int    fd  = uargs[1];
  void  *buf = (void *) uargs[2];
  unsigned sz = (unsigned) uargs[3];

  // file이 아니라 buffer이기 때문에 null이 없어서 +1을 하지 않는다
  check_address_buffer(buf, sz);

  if (fd == STDIN_FILENO) { // 파일시스템이 아니라 콘솔 장치(키보드)에서 직접 가져온다
    for (int i = 0; i < (int) sz; i++)
      ((char *) buf)[i] = input_getc ();

    f->eax = sz;
  }
  else { 
      lock_acquire (&fs_lock);

      struct file *file = process_get_file (fd);
      if (file != NULL)
        f->eax = file_read (file, buf, sz); // read한 byte수를 리턴
      else
        f->eax = -1; // process를 종료시킬 것까지는 없을거라 판단 

      lock_release (&fs_lock);
  }
}

// int write (int fd, const void *buffer, unsigned size)
// 성공 : 실제로 write한 byte 수 (전부 write가 안 되서 size보다 작을 수 있다) (0도 가능)
void
syscall_write (struct intr_frame *f, int *uargs)
{
  // printf("syscall_write\n");
  int    fd  = uargs[1];
  void  *buf = (void *) uargs[2];
  unsigned sz = (unsigned) uargs[3];

  check_address_buffer(buf, sz); // 이건 아예 주소가 잘못 되서 process를 종료 시키는 것

  if (fd == STDOUT_FILENO) {
    putbuf (buf, sz);
    f->eax = sz;
  }
  else {
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
    f->eax = -1; 
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
  if (fp != NULL) { // void syscall이라 fp가 NULL일 때는 처리 안 해줘도 된다 (eax로 넘길 필요가 없다)
    file_seek(fp, position);
  }
}

/* 8. unsigned tell(int fd) */
void 
syscall_tell(struct intr_frame *f, int *uargs) 
{
  int fd = uargs[1];
  struct file *fp = process_get_file (fd);
  // if (fp == NULL) {
  //   f->eax = -1;
  // }
  // else 
    f->eax = file_tell(fp);
}

// void close (int fd)
void
syscall_close (struct intr_frame *f, int *uargs)
{
  int fd = uargs[1];

  lock_acquire (&fs_lock);
  process_close_file (fd);
  lock_release (&fs_lock);


  // lock_acquire (&fs_lock);
  // if (process_close_file (fd))
  //   f->eax = 0;
  // else
  //   f->eax = -1;
  // lock_release (&fs_lock);
}

// void exit (int status)
void
syscall_exit (struct intr_frame *f, int *uargs)
{
  // printf("syscall_exit\n");
  // syscall_exit가 바로 호출되면 exit_status를 바꿔주지 못하므로 코드를 추가했다
  int status = uargs[1];

  thread_current()->exit_status = status;

  // printf ("%s: exit(%d)\n", thread_name(), status);

  thread_exit();
  NOT_REACHED ();
}


// pid_t exec (const char *file)
// 현재 프로세스를 cmd_line에서 지정된 인수를 전달하여 이름이 지정된 실행 파일로 변경
void
syscall_exec (struct intr_frame *f, int *uargs)
{

}

// void halt (void) 
void
syscall_halt() 
{
  shutdown_power_off ();
}

// int wait (pid_t pid)
void 
syscall_wait(struct intr_frame *f, int *uargs)
{

}













