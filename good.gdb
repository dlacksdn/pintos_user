set print pretty on
set pagination off
source /home/dlacksdn/pintos/src/misc/gdb-macros

define bre
    break process_execute
    break start_process
end

define connect
    target remote localhost:1234
end

define sarray
    if $argc == 0
        printf "Usage: sarray ARRAY_NAME\n"
        return
    end

    set $arr = $arg0
    set $i = 0

    while $arr[$i]
        printf "%s[%d] = %s\n", "$arg0", $i, $arr[$i]
        set $i = $i + 1
    end
end
document sarray
Print all elements of a null-terminated char** array.
Usage: sarray ARRAY_NAME
End.


# 현재 running_thread 정보 출력
define running
    p *(struct thread *) ((int)$esp & ~0xfff)
end

# ready_list 내부 thread 모든 정보 출력
define ready
    set $re = ready_list.head.next
    while($re != &ready_list.tail)
        set $rt = (struct thread *)((char *) $re - 48)
        p $re
        p $rt
        p *$rt
        set $re = $re->next
    end
end

# simple_ready라는 의미 / ready_list안에 어떤 thread가 있는지만 check
define sready
    set $e = ready_list.head.next
    while ($e != &ready_list.tail)
        set $t = (struct thread *)((char *)$e - 48)
        printf "%-5d %-16s %-8d\n", $t->tid, $t->name, $t->status
        set $e = $e->next
    end
end

define blocked
    set $be = blocked_list.head.next
    while($be != &blocked_list.tail)
        set $bt = (struct thread *)((char *) $be - 48)
        p $be
        p $bt
        p *$bt
        set $be = $be->next
    end
end

define sblocked
    set $e = blocked_list.head.next
    while ($e != &blocked_list.tail)
        set $t = (struct thread *)((char *)$e - 48)
        printf "%-5d %-16s %-8d %-5d\n", $t->tid, $t->name, $t->status, $t->tick_to_awake
        set $e = $e->next
    end
end

define all
    set $ae = all_list.head.next
    while($ae != &all_list.tail)
        set $at = (struct thread *)((char *) $ae - 32)
        p $ae
        p $at
        p *$at
        set $ae = $ae->next
    end
end

define sall
    set $e = all_list.head.next
    while ($e != &all_list.tail)
        set $t = (struct thread *)((char *)$e - 32)
        printf "%-5d %-16s", $t->tid, $t->name

        if $t->status == THREAD_RUNNING
            printf " running\n"
        else
            if $t->status == THREAD_READY
                printf " ready\n"
            else
                if $t->status == THREAD_BLOCKED
                    printf " blocked\n"
                else
                    printf " unknown(%d)\n", $t->status
                end
            end
        end

        set $e = $e->next
    end
end

define sall_test
    printf "%-5s %-10s %8s %8s %6s %8s\n", \
        "tid", "name", "tickets", "stride", "pass", "status"

    set $e = all_list.head.next
    while ($e != &all_list.tail)
        set $t = (struct thread *)((char *)$e - 32)
        printf "%-5d %-8s %8d %8d %8d  ", $t->tid, $t->name, $t->tickets, $t->stride, $t->pass

        if $t->status == THREAD_RUNNING
            printf " running\n"
        else
            if $t->status == THREAD_READY
                printf " ready\n"
            else
                if $t->status == THREAD_BLOCKED
                    printf " blocked\n"
                else
                    printf " unknown(%d)\n", $t->status
                end
            end
        end

        set $e = $e->next
    end

    # set $cur = (struct thread *)((unsigned long)$esp & ~0xfff)
    # printf "current_running -> %s\n", $cur->name
end

define size
    printf "all_list_size: %d\n", list_size(&all_list)
    printf "blocked_list_size: %d\n", list_size(&blocked_list)
    printf "ready_list_size: %d\n", list_size(&ready_list)
end



define sizeof
    printf "%-15s : %d\n", "tid",             sizeof(((struct thread*)0)->tid)
    printf "%-15s : %d\n", "status",          sizeof(((struct thread*)0)->status)
    printf "%-15s : %d\n", "name[16]",        sizeof(((struct thread*)0)->name)
    printf "%-15s : %d\n", "stack",           sizeof(((struct thread*)0)->stack)
    printf "%-15s : %d\n", "priority",        sizeof(((struct thread*)0)->priority)
    printf "%-15s : %d\n", "allelem",         sizeof(((struct thread*)0)->allelem)
    printf "%-15s : %d\n", "tick_to_awake",   sizeof(((struct thread*)0)->tick_to_awake)
    printf "%-15s : %d\n", "elem",            sizeof(((struct thread*)0)->elem)
    printf "%-15s : %d\n", "magic",           sizeof(((struct thread*)0)->magic)
end



# idle_thread 포함
# 함수를 지정하면 그 함수가 호출되는 타이밍의 running_thread를 출력한다
define trace_run
    
    b $arg0 
    commands
    silent
    end
    c
    set $i = 0

    while($i < 100000)
        set $cur = (struct thread *)((int)$esp & ~0xfff)

        if($cur != -1)
            p ::ticks
            printf "RUNNING THREAD: tid= %-5d name= %-10s\n", $cur->tid, $cur->name
        end

        set $i = $i + 1
        c
    end
end

# idle_thread 배제
# 함수를 지정하면 그 함수가 호출되는 타이밍의 running_thread를 출력한다
define test_trace

    b $arg0 
    commands
    silent
    end
    c
    set $i = 0

    # initialize counter
    set $c0 = 0      
    set $c1 = 0      
    set $c2 = 0      
    set $c3 = 0      
    set $c4 = 0      

    while($i < 100000)
        set $cur = (struct thread *)((int)$esp & ~0xfff)

    
        if($cur->tid != 2)
            p ::ticks
            printf "RUNNING THREAD: tid= %-5d name= %-10s\n", $cur->tid, $cur->name

            if $_streq($cur->name, "thread 0")
                # count thread 0
                set $c0 = $c0 + 1    
            end
            if $_streq($cur->name, "thread 1")
                # count thread 1
                set $c1 = $c1 + 1    
            end
            if $_streq($cur->name, "thread 2")
                # count thread 2
                set $c2 = $c2 + 1   
            end
            if $_streq($cur->name, "thread 3")
                # count thread 3
                set $c3 = $c3 + 1    
            end
            if $_streq($cur->name, "thread 4")
                # count thread 4
                set $c4 = $c4 + 1    
            end

            # exit loop when ticks >= 500 and main is running
            if ::ticks >= 7000 && $_streq($cur->name, "main")
                set $i = 100000    
            end
        end

        set $i = $i + 1
        c
    end

    set $total = $c0 + $c1 + $c2 + $c3 + $c4

    printf "\n=== COUNT RESULTS ===\n"
    printf "thread 0 ran -> %d times (%.1f%%)\n", $c0, 100.0 * $c0 / $total
    printf "thread 1 ran -> %d times (%.1f%%)\n", $c1, 100.0 * $c1 / $total
    printf "thread 2 ran -> %d times (%.1f%%)\n", $c2, 100.0 * $c2 / $total
    printf "thread 3 ran -> %d times (%.1f%%)\n", $c3, 100.0 * $c3 / $total
    printf "thread 4 ran -> %d times (%.1f%%)\n", $c4, 100.0 * $c4 / $total    
end

# 각 thread의 backtrace 출력
# 결함이 많음. 수정 필요
# 모든 thread가 한 번씩 blocked 되어야하고, exit된 thread가 없어야 에러가 안 뜸
define abtthread
    printf "main_thread\n"
    btthread 0xc000e000
    printf "\n"

    printf "idle_thread\n"
    btthread 0xc0103000
    printf "\n"

    printf "thread 0\n"
    btthread 0xc0106000
    printf "\n"

    printf "thread 1\n"
    btthread 0xc0107000
    printf "\n"

    printf "thread 2\n"
    btthread 0xc0108000
    printf "\n"

    printf "thread 3\n"
    btthread 0xc0109000
    printf "\n"

    printf "thread 4\n"
    btthread 0xc010a000
    printf "\n"
end

# 각 thread의 시작주소 출력
define address
    printf "main_thread : 0xc000e000\n"
    printf "idle_thread : 0xc0103000\n"

    set $e = all_list.head.next.next.next
    set $i = 0

    while ($e != &all_list.tail)
        set $t = (struct thread *)((char *)$e - 32)
        printf "thread %d : %p\n", $i, $t
        set $e = $e->next
        set $i = $i + 1
    end
end

    