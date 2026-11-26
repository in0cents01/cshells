/* Force a real pipe(2) syscall.
 *
 * GCC treats pipe() as a builtin. When the results are not used in a way it
 * can prove necessary, it deletes the call entirely, which silently breaks
 * every shell pipeline. An assembly wrapper it cannot analyse stops that. */
#include <unistd.h>

__attribute__((noinline, noclone, optimize("no-optimize-sibling-calls")))
int cs_pipe(int fds[2])
{
    int r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(22), "D"(fds)        /* SYS_pipe on x86-64 */
                     : "rcx", "r11", "memory");
    return r;
}
