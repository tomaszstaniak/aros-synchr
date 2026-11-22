/* Test helper for AROS: keeps a file open across another program's
 * replacement of it and writes through the old handle afterwards.
 *
 *   holdopen FILE GOFILE WAIT-SECONDS LOGFILE
 *
 * Opens FILE for appending (fails if it does not exist), creates GOFILE
 * (the other program's go-ahead), waits, then writes one line through the
 * handle that was opened before, closes it and logs every result. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 5)
        return 20;
    FILE *log = fopen(argv[4], "w");
    if (!log)
        return 20;
    FILE *fp = fopen(argv[1], "r+");
    fprintf(log, "open %s for update: %s (errno %d)\n", argv[1], fp ? "ok" : "failed", fp ? 0 : errno);
    if (fp && fseek(fp, 0, SEEK_END) != 0)
        fprintf(log, "seek to end failed (errno %d)\n", errno);
    FILE *go = fopen(argv[2], "w");
    if (go)
        fclose(go);
    fprintf(log, "go-ahead %s created\n", argv[2]);
    fflush(log);
    sleep((unsigned)atoi(argv[3]));
    if (fp) {
        int w = fputs("written through a handle opened before the replacement\n", fp);
        int fl = fflush(fp);
        int e = errno;
        int c = fclose(fp);
        fprintf(log, "write %s, flush %s (errno %d), close %s\n", w >= 0 ? "ok" : "failed",
                fl == 0 ? "ok" : "failed", fl == 0 ? 0 : e, c == 0 ? "ok" : "failed");
    }
    fclose(log);
    return 0;
}
