#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

#define BIG_STRIDE 10000

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;
static int64_t next_tick_to_awake = INT64_MAX;  // 모든 blocked threads 가운데, 가장 작은 tick_to_awake값 / 가장 빨리 깨워줘야 하는 thread의 tick값
static struct list blocked_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

struct thread *get_thread_by_tid(tid_t tid);
static int next_thread_tickets = 1;
static int min_pass_num = 0;

enum scheduler_type current_scheduler = SCHED_ROUND_ROBIN;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */


void
thread_sleep (int64_t ticks)
{
  struct thread *cur = thread_current();
  enum intr_level old_level;

  ASSERT (!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread) {
    cur -> tick_to_awake = ticks;
    list_push_back(&blocked_list, &cur->elem);
  }

  update_next_tick_to_awake();

  cur->status = THREAD_BLOCKED;

  schedule();

  intr_set_level(old_level);
}

void
thread_awake (int64_t ticks)
{
  struct list_elem *e;

  e = list_begin (&blocked_list);

  while (e != list_end (&blocked_list)) {
    struct thread *t = list_entry(e, struct thread, elem);

    if (t->tick_to_awake <= ticks) {
      e = list_remove(e);
      t->tick_to_awake = 0;

      enum intr_level old_level;

      ASSERT(is_thread(t));

      old_level = intr_disable();
      ASSERT (t->status == THREAD_BLOCKED);
      list_push_back(&ready_list, &t->elem);
      t->status = THREAD_READY;
    }
    else {
      e = list_next(e);
    }
  }

  update_next_tick_to_awake();
}

void
update_next_tick_to_awake (void)
{
  struct list_elem *e;

  if (list_empty (&blocked_list)) {
    next_tick_to_awake = INT64_MAX;
  }

  for (e = list_begin(&blocked_list); e != list_end(&blocked_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);

    if (t->tick_to_awake < next_tick_to_awake) {
      next_tick_to_awake = t->tick_to_awake;
    }
  }
}

int64_t
get_next_tick_to_awake (void)
{
  if (list_empty(&blocked_list)) {
    next_tick_to_awake = INT64_MAX;
  }

  return next_tick_to_awake;
}


void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&blocked_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;  
  initial_thread->tid = allocate_tid ();

  initial_thread->tickets = 1;
  initial_thread->pass = 0;

  random_init(timer_ticks());  // 시드 초기화
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit (thread_current()->exit_status);
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

void
set_scheduler(enum scheduler_type type) {
  current_scheduler = type;
}

struct thread *pick_lottery_thread(void) {
  if (list_empty(&ready_list))
    return idle_thread;

  int total_tickets = 0;
  struct list_elem *e;

  // 1. 전체 티켓 수 계산
  for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    total_tickets += t->tickets;  // 기본 값은 1
  }

  if (total_tickets == 0)
    return list_entry(list_pop_front(&ready_list), struct thread, elem);

  // 2. 난수 추첨 (1~total_tickets)
  int winner = random_ulong() % total_tickets + 1;

  // 3. 추첨된 티켓을 가진 thread 선택
  for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (winner <= t->tickets) {
      list_remove(e);  // ready_list에서 꼭 제거해야 함!
      return t;
    }
    winner -= t->tickets;
  }

  // fallback
  return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

struct thread *
pick_stride_seq_thread(void) {
  if (list_empty(&ready_list))
    return idle_thread;

  struct list_elem *e;
  struct thread *min_t = NULL;

  /* 1) 최소 pass 찾기 */
  for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (!min_t || t->pass < min_t->pass)
      min_t = t;
  }
  ASSERT(min_t);

  /* 2) 리스트에서 제거 */
  list_remove(&min_t->elem);

  /* 3) pass 갱신 */
  min_t->pass += min_t->stride;  /* stride는 init_thread에서 BIG_STRIDE/tickets로 계산됨 */

  return min_t;
}

/* 비교 함수: elem a, b 를 갖는 스레드의 pass 값 비교 */
static bool
stride_less (const struct list_elem *a,
            const struct list_elem *b,
            void *aux UNUSED)
{
  const struct thread *t1 = list_entry (a, struct thread, elem);
  const struct thread *t2 = list_entry (b, struct thread, elem);
  return t1->pass < t2->pass;
}

struct thread *
pick_stride_sort_thread(void) {
  struct thread *next;

   if (list_empty (&ready_list))
     return idle_thread;

   /* 1) ready_list 를 pass 오름차순으로 정렬 */
   list_sort (&ready_list, stride_less, NULL);

   /* 2) 맨 앞 스레드를 꺼내고 */
   next = list_entry (list_pop_front (&ready_list),
                      struct thread, elem);

   /* 3) 실행 후 새로운 pass 값을 설정 */
   next->pass += next->stride;

   return next;
}

struct thread *
pick_stride_seq_new_thread(void) {
  if (list_empty(&ready_list))
    return idle_thread;

  struct list_elem *e;
  struct thread *min_t = NULL;

  /* 1) 최소 pass 찾기 */
  for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (!min_t || t->pass < min_t->pass)
      min_t = t;
  }
  ASSERT(min_t);
  
  /* 2) 리스트에서 제거 */
  list_remove(&min_t->elem);

  /* 3) pass 갱신 */
  /* stride는 init_thread에서 BIG_STRIDE/tickets로 계산됨 */
  if (min_t -> tid != 1) {
    min_pass_num = min_t -> pass;
    min_t -> pass += min_t->stride;  
  }
  
  return min_t;
}

struct thread *
pick_stride_seq_priority_thread(void) {
  if (list_empty(&ready_list))
    return idle_thread;

  struct list_elem *e;
  struct thread *min_t = NULL;

  /* 1) 최소 pass 찾기 */
  for (e = list_begin(&ready_list); e != list_end(&ready_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (!min_t || t->pass < min_t->pass)
      min_t = t;
  }
  ASSERT(min_t);

  /* 2) 리스트에서 제거 */
  list_remove(&min_t->elem);

  /* 3) pass 갱신 */
  min_t->pass += min_t->stride;  /* stride는 init_thread에서 BIG_STRIDE/tickets로 계산됨 */

  return min_t;
}


tid_t thread_create_lottery(const char *name, int priority, int tickets,
                            thread_func *function, void *aux) {
  next_thread_tickets = tickets;  // 다음 스레드가 생성될 때 사용할 티켓 수 설정
  tid_t tid = thread_create(name, priority, function, aux);
  next_thread_tickets = 1;        // 다음 thread는 기본값 (1장)으로 초기화
  return tid;
}

tid_t thread_create_stride(const char *name, int priority, int tickets,
                            thread_func *function, void *aux) {
  next_thread_tickets = tickets;  // 다음 스레드가 생성될 때 사용할 티켓 수 설정
  tid_t tid = thread_create(name, priority, function, aux);
  next_thread_tickets = 1;        // 다음 thread는 기본값 (1장)으로 초기화
  return tid;
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->tick_to_awake = INT64_MAX;
  t->magic = THREAD_MAGIC;

  t->tickets = next_thread_tickets;
  if(current_scheduler == SCHED_STRIDE_SEQ_PRIORITY) {
    t->tickets *= t->priority;
  }

  t->stride = BIG_STRIDE / t->tickets;

  if(!is_late_arrival)
    t->pass = 0;
  else
    t->pass = min_pass_num;

  t->perf_id=0;

  #ifdef USERPROG
  /* Initialize our per-process fd table and index. */
  for (int i = 0; i < FDCOUNT_LIMIT; i++)
    t->fd_table[i] = NULL;
  t->next_fd = 2;            /* 0 and 1 are console */
  t->exit_status = 0;
  #endif
  

  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  
  if (current_scheduler == SCHED_ROUND_ROBIN)
    return list_entry(list_pop_front(&ready_list), struct thread, elem);

  if (current_scheduler == SCHED_LOTTERY) {
    struct thread *next = pick_lottery_thread();
    count[next->perf_id]++;

    return next;
  }

  if (current_scheduler == SCHED_STRIDE_SEQ) {
    struct thread *next = pick_stride_seq_thread();
    count_stride[next->perf_id]++;

    return next;
  }

  if (current_scheduler == SCHED_STRIDE_SORT) {
    struct thread *next = pick_stride_sort_thread();
    count_stride_sort[next->perf_id]++;

    return next;
  }

  if (current_scheduler == SCHED_STRIDE_SEQ_NEW) {
    struct thread *next = pick_stride_seq_new_thread();
    count_stride_new[next->perf_id]++;

    return next;
  }

  if (current_scheduler == SCHED_STRIDE_SEQ_PRIORITY) {
    struct thread *next = pick_stride_seq_priority_thread();
    count_stride_priority[next->perf_id]++;

    return next;
  }
  
  return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
