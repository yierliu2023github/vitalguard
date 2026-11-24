/*
 * vghog — CPU hog for the fault-injection demo.
 *
 * A deliberate busy loop at lowered scheduling priority (nice 10).
 * On the host this demonstrates that the (higher-priority work of
 * the) pipeline keeps flowing under load; on QNX the same role is
 * played by a SCHED_FIFO low-priority spinner, and the alarm path's
 * latency under the hog is exactly what tracelogger measures.
 *
 * Runs until killed (SIGTERM default disposition).
 */
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

int main(void)
{
    volatile unsigned long long sink = 0;
    unsigned long long i;

    setpriority(PRIO_PROCESS, 0, 10);
    printf("vghog: spinning at nice 10, pid=%d\n", (int)getpid());
    fflush(stdout);
    for (;;) {
        for (i = 0; i < 1000000ull; i++)
            sink += i ^ (sink << 3);
    }
    return (int)sink;
}
