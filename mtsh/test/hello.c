/* hello — demo program for mtsh.
 *
 * Usage: hello [interval_ms] [count]
 *
 * Prints "hello, world" once per interval (default 1000 ms),
 * forever unless a count is given. Background two copies and the
 * job tags are the only thing telling them apart — which is the
 * point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    long ms    = argc > 1 ? strtol(argv[1], NULL, 10) : 1000;
    long count = argc > 2 ? strtol(argv[2], NULL, 10) : -1;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    for (long i = 1; count < 0 || i <= count; i++) {
        printf("hello, world\n");
        fflush(stdout);
        nanosleep(&ts, NULL);
    }
    return 0;
}
