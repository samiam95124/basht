/* status — test helper: a progress bar repainted in place.
 *
 * Usage: status [interval_ms] [cells]
 *
 * Prints "status: [XXX_______________] 15%" and rewrites that same
 * line once per interval (default 1000 ms), the bar filling left to
 * right across CELLS cells (default 20), until it reaches 100% and
 * ends with a newline.
 *
 * The only cursor motion is a bare carriage return — no escape
 * sequences at all, not even an erase-to-end-of-line — because the
 * point is to exercise basht's one-line terminal emulation on the
 * plainest thing a progress bar can do: return to column 0 and
 * overwrite. The line only ever grows (the percentage widens 1 -> 2
 * -> 3 digits), so a correct display never shows a stale tail.
 *
 * In the foreground it should repaint the console bottom line in
 * place; in the background it should do the same under its own tag
 * while other tasks print through above it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAXCELLS 256

int main(int argc, char **argv)
{
    long ms    = argc > 1 ? strtol(argv[1], NULL, 10) : 1000;
    long cells = argc > 2 ? strtol(argv[2], NULL, 10) : 20;
    char bar[MAXCELLS + 1];
    struct timespec ts;
    long i, j;

    if (ms < 0)
        ms = 0;
    if (cells < 1)
        cells = 1;
    if (cells > MAXCELLS)
        cells = MAXCELLS;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    for (i = 0; i <= cells; i++) {
        for (j = 0; j < cells; j++)
            bar[j] = j < i ? 'X' : '_';
        bar[cells] = '\0';

        printf("\rstatus: [%s] %ld%%", bar, i * 100 / cells);
        fflush(stdout);

        if (i < cells)
            nanosleep(&ts, NULL);
    }
    printf("\n");
    return 0;
}
