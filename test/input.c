/* input.c - simple line input test program.
   Prompts for a line, reads it from stdin, echoes it back. */

#include <stdio.h>
#include <string.h>

int
main (void)
{
  char line[4096];

  printf ("enter a line: ");
  fflush (stdout);

  if (fgets (line, sizeof (line), stdin) == NULL)
    {
      fprintf (stderr, "no input\n");
      return 1;
    }

  /* strip trailing newline */
  line[strcspn (line, "\n")] = '\0';

  printf ("here is your line: %s\n", line);

  return 0;
}
