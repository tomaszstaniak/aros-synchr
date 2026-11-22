/* Reproducer for a TCP receive stall on AROS: connects to a host server
 * that sends BYTES in one burst, reads it all and logs when each 16 KiB
 * arrived. No TLS, no Syncthing code.
 *
 *   tcpburst IPV4 PORT BYTES RCVBUF LOGFILE      (RCVBUF 0: system default)
 */
#ifdef __AROS__
#include <proto/bsdsocket.h>
#define SOCK_ERRNO() ((int)Errno())
#define SOCK_CLOSE(s) CloseSocket(s)
#else
#define SOCK_ERRNO() errno
#define SOCK_CLOSE(s) close(s)
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
    if (argc != 6)
        return 20;
    long want = atol(argv[3]);
    int rcvbuf = atoi(argv[4]);
    FILE *log = fopen(argv[5], "w");
    if (!log)
        return 20;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(log, "socket failed (errno %d)\n", SOCK_ERRNO());
        return 20;
    }
    if (rcvbuf > 0 && setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf) != 0)
        fprintf(log, "SO_RCVBUF %d failed (errno %d)\n", rcvbuf, SOCK_ERRNO());
    int cur = 0;
    socklen_t cl = sizeof cur;
    getsockopt(s, SOL_SOCKET, SO_RCVBUF, &cur, &cl);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[2]));
    a.sin_addr.s_addr = inet_addr(argv[1]);
    long t0 = ms();
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(log, "connect failed (errno %d)\n", SOCK_ERRNO());
        return 20;
    }
    fprintf(log, "connected in %ld ms, SO_RCVBUF %d\n", ms() - t0, cur);
    t0 = ms();
    static char buf[65536];
    long got = 0, next = 16384;
    while (got < want) {
        ssize_t n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) {
            fprintf(log, "recv returned %ld (errno %d) after %ld bytes\n", (long)n, SOCK_ERRNO(), got);
            break;
        }
        got += n;
        while (got >= next) {
            fprintf(log, "%7ld bytes at %6ld ms\n", next, ms() - t0);
            next += 16384;
        }
    }
    fprintf(log, "total %ld bytes in %ld ms\n", got, ms() - t0);
    SOCK_CLOSE(s);
    fclose(log);
    printf("tcpburst: %ld bytes in %ld ms (log %s)\n", got, ms() - t0, argv[5]);
    return got == want ? 0 : 10;
}
