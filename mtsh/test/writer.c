/* writer — test helper (tsh_spec.md §9).
 *
 * Usage: writer name interval_ms count
 *
 * Writes numbered lines to stdout, every third line also to stderr,
 * one per interval. Used to exercise attribution, interleaving and
 * print-through in mtsh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: writer name interval_ms count\n");
        return 2;
    }
    const char *name = argv[1];
    long ms    = strtol(argv[2], NULL, 10);
    long count = strtol(argv[3], NULL, 10);
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    for (long i = 1; i <= count; i++) {
        printf("%s out %ld\n", name, i);
        if (i % 3 == 0)
            fprintf(stderr, "%s err %ld\n", name, i);
        fflush(NULL);
        nanosleep(&ts, NULL);
    }
    return 0;
}
