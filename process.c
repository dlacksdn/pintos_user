#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define MAX_ARGS 128 // new
#define FDCOUNT_LIMIT 128 // new

/* Forward declaration of our helper. */
static void setup_user_stack (void **esp, char **argv, int argc); // new

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
// The job of process_execute() is to spawn a new Pintos thread that will load and run that program.
// file_name is the user-supplied command line (e.g. "grep foo bar").

/* Modified process_execute(): copy full cmd_line, extract thread name, pass full line */
tid_t
process_execute (const char *cmd_line) 
{
  char *fn_copy, *thread_name;
  char *save_ptr;
  tid_t tid;

  /* 1) Copy full command line into a fresh page. */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, cmd_line, PGSIZE); 

  /* 2) Extract first token as thread name (in its own page). */
  thread_name = palloc_get_page (0);
  if (thread_name == NULL)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  strlcpy (thread_name, fn_copy, PGSIZE);
  strtok_r (thread_name, " \t\r\n", &save_ptr);

  /* 3) Create the thread, passing fn_copy (full cmd line) as aux. */
  // start_process()의 인자로 fn_copy가 들어간다
  tid = thread_create (thread_name, PRI_DEFAULT, // "echo" "31" "start_process" "echo foo bar"
                       start_process, fn_copy);

  #ifdef USERPROG
  if (tid != TID_ERROR)
    {
      struct thread *child = get_thread_by_tid (tid);
      child->parent = thread_current ();
      list_push_back (&thread_current()->children,
                      &child->child_elem);
    }
  #endif

  /* 4) We no longer need the standalone name page. */
  palloc_free_page (thread_name);

  /* 5) On failure, free fn_copy to avoid leak. */
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
  
/* Modified start_process(): parse args → load → setup stack → intr_exit jump */
static void
start_process (void *file_name_) // file_name_ = "echo foo bar" 의 주소값
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;

  /* --- 1) Initialize interrupt frame --- */
  memset (&if_, 0, sizeof if_); // intr_frame의 모든 필드 if_가 0으로 설정된 후 필요한 필드만 명시적으로 초기화합니다
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG; // User data selector
  if_.cs = SEL_UCSEG; // User code selector.
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* --- 2) Tokenize the command line in-place --- */
  char *argv[MAX_ARGS];
  int argc = 0;
  char *save_ptr, *token;
  for (token = strtok_r (file_name, " \t\r\n", &save_ptr);
       token != NULL;
       token = strtok_r (NULL, " \t\r\n", &save_ptr))
    {
      if (argc < MAX_ARGS)
        argv[argc++] = token;
    }

  /* --- 3) Load the executable using argv[0] as program name --- */
  success = load (argv[0], &if_.eip, &if_.esp);
  if (!success) 
    {
      palloc_free_page (file_name);
      thread_exit ();
    }

  /* --- 4) Build the user stack with argc/argv --- */
  setup_user_stack (&if_.esp, argv, argc);

  /* --- 5) Clean up and transfer to user mode --- */
  palloc_free_page (file_name);
  asm volatile ("movl %0, %%esp; jmp intr_exit"
                : : "g" (&if_) : "memory");

  NOT_REACHED ();
}

/* Helper to push argc, argv strings + pointers onto user stack. */
static void
setup_user_stack (void **esp, char **argv, int argc) 
{
  int i;
  void *arg_addrs[MAX_ARGS];

  /* 1) Push each argument string bytes onto the stack (in reverse). */
  for (i = argc - 1; i >= 0; i--) 
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (uint8_t *) *esp - len;
      memcpy (*esp, argv[i], len);
      arg_addrs[i] = *esp;
    }

  /* 2) Word-align stack pointer to multiple of 4 bytes. */
  uintptr_t sp = (uintptr_t) *esp;
  size_t align = sp % 4;
  if (align != 0) 
    {
      *esp = (uint8_t *) *esp - align;
      memset (*esp, 0, align);
    }

  /* 3) Null sentinel for argv[argc]. */
  *esp = (uint8_t *) *esp - sizeof (char *);
  *(char **) *esp = NULL;

  /* 4) Push addresses of each argument (in reverse). */
  for (i = argc - 1; i >= 0; i--) 
    {
      *esp = (uint8_t *) *esp - sizeof (char *);
      memcpy (*esp, &arg_addrs[i], sizeof (char *));
    }

  /* 5) Push argv (i.e. address of argv[0]). */
  void *argv0 = *esp;
  *esp = (uint8_t *) *esp - sizeof (char **);
  memcpy (*esp, &argv0, sizeof (char **));

  /* 6) Push argc. */
  *esp = (uint8_t *) *esp - sizeof (int);
  memcpy (*esp, &argc, sizeof (int));

  /* 7) Push fake return address. */
  *esp = (uint8_t *) *esp - sizeof (void *);
  memset (*esp, 0, sizeof (void *));
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  /* Find the child in our list */
  for (e = list_begin (&cur->children);
       e != list_end (&cur->children);
       e = list_next (e)) 
    {
      struct thread *child = list_entry (e, struct thread, child_elem);
      if (child->tid == child_tid) 
        {
          /* If child hasn’t exited yet, block until it does. */
          if (!child->has_exited)
            sema_down (&child->wait_sema);
          int status = child->exit_status;
          /* Clean up the child record. */
          list_remove (&child->child_elem);
          return status;
        }
    }

  /* No such child. */
  return -1;
}


/* Free the current process's resources. */
void
process_exit (int status)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Print exit status just before tearing down */
  printf ("%s: exit(%d)\n", thread_name(), status);

  #ifdef USERPROG
  /* --- Signal parent (if any) that we’re done. --- */
  cur->exit_status = status;       /* already set, but safe to reaffirm */
  cur->has_exited = true;          /* mark that we have exited */
  sema_up (&cur->wait_sema);       /* unblock the parent in process_wait() */
  #endif

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* Retrieve the struct file* for this thread’s fd, or NULL. */
struct file *
process_get_file (int fd) 
{
  struct thread *cur = thread_current ();
  if (fd < 0 || fd >= FDCOUNT_LIMIT)
    return NULL;
  return cur->fd_table[fd];
}

/* Install file in the first free slot ≥ 2, return that fd or –1. */
int
process_add_file (struct file *file) 
{
  struct thread *cur = thread_current ();
  for (int fd = 2; fd < FDCOUNT_LIMIT; fd++) 
    {
      if (cur->fd_table[fd] == NULL) 
        {
          cur->fd_table[fd] = file;
          return fd;
        }
    }
  return -1;
}

/* Close and remove an fd; return true on success. */
bool
process_close_file (int fd) 
{
  struct thread *cur = thread_current ();
  if (fd < 2 || fd >= FDCOUNT_LIMIT)
    return false;
  struct file *f = cur->fd_table[fd];
  if (f == NULL)
    return false;
  file_close (f);
  cur->fd_table[fd] = NULL;
  return true;
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK; // <---- PGMASK에 덮여서 중복이 발생?
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page, // 문제 발생
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  /* Start reading from offset OFS in FILE. */
  file_seek (file, ofs);

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Allocate a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Read the requested bytes from FILE. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      /* Zero the remainder. */
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* If UPAGE is already mapped, just discard kpage. */
      if (pagedir_get_page (thread_current ()->pagedir, upage) != NULL)
        {
          palloc_free_page (kpage);
        }
      else
        {
          /* Otherwise install it. */
          if (!install_page (upage, kpage, writable))
            {
              palloc_free_page (kpage);
              return false;
            }
        }

      /* Advance to next page in segment. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL // <----- 이놈이 false임
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
