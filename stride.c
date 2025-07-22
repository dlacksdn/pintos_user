#include <stdbool.h>           /* bool, true/false */
#include "threads/thread.h"    /* N, set_scheduler, thread_create_stride 등 */

#define lottery 5
#define sequential_search 100
#define sort 100
#define priority 5
#define new 5


int count[lottery]; // lottery
int count_stride[sequential_search]; // sequential-search
int count_stride_sort[sort]; // sort
int count_stride_priority[priority]; // priority
int count_stride_new[new]; // new
bool is_late_arrival; // new




