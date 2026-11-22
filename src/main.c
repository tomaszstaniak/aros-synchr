/* aros-syncthing proof of concept: connect to one Syncthing device, take
 * part in the BEP handshake for one folder and report the folder's index.
 *
 *   stpoc [--brief] LOGFILE identity-create DIR
 *   stpoc LOGFILE identity-show DIR
 *   stpoc LOGFILE run DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID [SECONDS [UPDATES [NAME]]]
 *   stpoc LOGFILE sync DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID DEST [SECONDS [UPDATES [ABORT-AFTER-BLOCKS [STATE-FAULT [COMMIT-HOOK-DIR]]]]]
 *   stpoc LOGFILE twoway DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID FOLDER [SECONDS [RESCAN-SECONDS]]
 *   stpoc [--log-max BYTES] LOGFILE engine DIR IPV4 PORT PEER-DEVICE-ID FOLDER-ID FOLDER [RESCAN-SECONDS]
 *   stpoc [--log-max BYTES] LOGFILE engine DIR CONFIGURATION-FILE
 *   stpoc ctl COMMAND [ARGUMENT]      (to the running engine: src/ctl.h)
 *   stpoc manifest FOLDER OUTFILE     (SHA-256 of every file, for tests)
 *
 * run: stops after the first Index plus UPDATES IndexUpdate messages
 * (default 0), or after SECONDS (default 60). sync: the same, and also
 * downloads every file into the existing directory DEST, announcing each
 * one when written. The state of DEST is kept in DIR (next to the identity)
 * so unchanged files are not downloaded again. Test hooks: ABORT-AFTER-
 * BLOCKS stops the transfer like a crash after that many blocks;
 * STATE-FAULT 1 makes every state write fail. The log's last line is
 * "RESULT PASS" or "RESULT FAIL <reason>"; the exit code is 0 or 10.
 * twoway: FOLDER is synchronised in both directions (src/tw.h): local
 * new and changed files are published and served, the peer's newer
 * entries are downloaded; deletions and conflicts are held and reported
 * in this version. Stops when both indexes agree, or after SECONDS
 * (default 60); RESCAN-SECONDS > 0 reads the folder again that often.
 * engine: FOLDER is kept synchronised in both directions until the
 * program is stopped with Ctrl-C (src/engine.h): it reconnects when the
 * connection is lost and reads the folder again every RESCAN-SECONDS
 * (default 60). With a CONFIGURATION-FILE (src/conf.h) it does so for
 * several folders and devices, each folder with one device, and every
 * line says which device or folder it is about. The console shows only
 * the short lines. The log is
 * limited: when LOGFILE passes BYTES (--log-max, default 1048576) it is
 * renamed to LOGFILE.old, replacing the previous one, and started again.
 * When the previous log cannot be replaced, the log is not emptied: it
 * continues in the same file and says so. The last line is "RESULT PASS
 * STOPPED" after a stop request, or "RESULT FAIL STATE_NOT_SAVED" when
 * the last state write before the stop failed (exit code 10).
 * Nothing secret is logged: Device IDs are public identifiers.
 *
 * --status FILE: the same short lines are appended to FILE, which is
 * closed after each line so that a front end can follow it.
 * --brief: the console shows only short user-facing lines (connection,
 * folder, each file with its size and verification, a summary); the full
 * log still goes to LOGFILE. */
#include "conf.h"
#include "ctl.h"
#include "ctlport.h"
#include "devid.h"
#include "engine.h"
#include "net.h"
#include "prof.h"
#include "scan.h"
#include "session.h"
#include "tls.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ST_TARGET
#define ST_TARGET "host"
#endif

static FILE *logf;
static net_ms t0;
static int brief;
static const char *status_path; /* --status FILE */
static const char *log_path;
static long log_max; /* 0: no limit */
static long log_next; /* size at which the next rotation is attempted */
static net_ms log_reopen_at;

/* Keeps at most two files of log_max bytes: the log and its predecessor.
 * The log is never opened for truncation unless it was moved aside first:
 * when that fails, writing continues in the same file and the rotation is
 * tried again after another log_max bytes. */
static void log_rotate(void)
{
    char old[1100];
    if (log_max <= 0)
        return;
    if (!logf) {
        /* An earlier reopen failed: try again, appending, at most every 10 s. */
        if (net_now() < log_reopen_at)
            return;
        log_reopen_at = net_now() + 10000;
        logf = fopen(log_path, "a");
        if (!logf)
            return;
        fseek(logf, 0, SEEK_END);
        fprintf(logf, "[%7lld ms] log: reopened after a failure; lines in between are missing\n",
                (long long)(net_now() - t0));
    }
    if (log_next <= 0)
        log_next = log_max;
    if (ftell(logf) < log_next)
        return;
    if (snprintf(old, sizeof old, "%s.old", log_path) >= (int)sizeof old)
        return;
    fclose(logf);
    logf = NULL;
    const char *step = NULL;
    int e = 0;
    struct stat st;
    int had = stat(old, &st) == 0;
    /* rename does not replace an existing file on AROS */
    if (remove(old) != 0 && had) {
        step = "removing the previous log";
        e = errno;
    } else if (rename(log_path, old) != 0) {
        step = "renaming the log";
        e = errno;
    }
    logf = fopen(log_path, step ? "a" : "w");
    if (!logf) {
        log_reopen_at = net_now() + 10000;
        return;
    }
    if (step) {
        fseek(logf, 0, SEEK_END);
        log_next = ftell(logf) + log_max;
        fprintf(logf, "[%7lld ms] log: not rotated: %s failed (errno %d); continuing in this file\n",
                (long long)(net_now() - t0), step, e);
    } else {
        log_next = log_max;
        fprintf(logf, "[%7lld ms] log: continued from %s\n", (long long)(net_now() - t0), old);
    }
}

/* With several devices or folders (engine with a configuration file)
 * each line says which one it is about. */
static char scope_text[204];

static void set_scope(const char *text)
{
    if (text && *text)
        snprintf(scope_text, sizeof scope_text, "[%s] ", text);
    else
        scope_text[0] = 0;
}

static void plog(const char *fmt, ...)
{
    char line[900];
    va_list ap;
    size_t o = (size_t)snprintf(line, sizeof line, "%s", scope_text);
    va_start(ap, fmt);
    vsnprintf(line + o, sizeof line - o, fmt, ap);
    va_end(ap);
    long long t = (long long)(net_now() - t0);
    log_rotate();
    if (logf) {
        PROF_BEGIN(tf);
        fprintf(logf, "[%7lld ms] %s\n", t, line);
        fflush(logf);
        PROF_END(PROF_LOG_FILE, tf);
    }
    if (!brief) {
        PROF_BEGIN(tc);
        printf("[%7lld ms] %s\n", t, line);
        fflush(stdout);
        PROF_END(PROF_LOG_CONSOLE, tc);
    }
}

/* User-facing line: always on the console, and into the log. */
static void ui(const char *fmt, ...)
{
    char line[900];
    va_list ap;
    size_t o = (size_t)snprintf(line, sizeof line, "%s", scope_text);
    va_start(ap, fmt);
    vsnprintf(line + o, sizeof line - o, fmt, ap);
    va_end(ap);
    if (logf) {
        fprintf(logf, "[%7lld ms] ui: %s\n", (long long)(net_now() - t0), line);
        fflush(logf);
    }
    if (brief) {
        printf("%s\n", line);
        fflush(stdout);
    }
    /* Opened and closed for every line, so a front end can read the file
     * while this program runs (AROS locks a file that is open for writing). */
    if (status_path) {
        FILE *sf = fopen(status_path, "a");
        if (sf) {
            fprintf(sf, "%s\n", line);
            fclose(sf);
        }
    }
}

static int finish(int ok, const char *what)
{
    if (brief || status_path)
        ui("Result: %s%s%s", ok ? "PASS" : "FAIL", what && *what ? " " : "", what ? what : "");
    plog("RESULT %s%s%s", ok ? "PASS" : "FAIL", what && *what ? " " : "", what ? what : "");
    if (logf)
        fclose(logf);
    return ok ? 0 : 10;
}

static int show(const char *dir)
{
    tls_identity *id = tls_identity_load(dir, plog);
    if (!id)
        return 0;
    unsigned char raw[DEVID_BYTES];
    char text[DEVID_TEXT_SIZE];
    tls_identity_devid(id, raw);
    devid_format(raw, text);
    plog("identity: Device ID %s", text);
    tls_identity_free(id);
    return 1;
}

/* engine: either one device and one folder from the command line, or
 * everything from a configuration file (conf.h). */
static int run_engine(int argc, char **argv, const char *dir)
{
    conf c;
    link_config one;
    link_folder one_folder;
    link_config *devs = NULL;
    link_folder *folders = NULL;
    size_t ndevs = 0;
    conf_init(&c);

    if (argc == 5) {
        char why[300];
        if (conf_load(argv[4], &c, why, sizeof why) != 0) {
            plog("engine: configuration %s: %s", argv[4], why);
            ui("Configuration not usable: %s", why);
            return finish(0, "BAD_CONFIGURATION");
        }
        plog("engine: configuration %s: %lu device(s), %lu folder(s), this device is \"%s\"", argv[4],
             (unsigned long)c.ndevices, (unsigned long)c.nfolders, c.name);
        devs = calloc(c.ndevices, sizeof *devs);
        folders = calloc(c.nfolders, sizeof *folders);
        if (!devs || !folders) {
            free(devs);
            free(folders);
            conf_free(&c);
            return finish(0, "OUT_OF_MEMORY");
        }
        /* The folders of one device follow one another in `folders`. */
        size_t at = 0;
        for (size_t d = 0; d < c.ndevices; d++) {
            size_t first = at;
            for (size_t i = 0; i < c.nfolders; i++)
                if (c.folders[i].device == d) {
                    folders[at].id = c.folders[i].id;
                    folders[at].path = c.folders[i].path;
                    folders[at].rescan_seconds = c.folders[i].rescan_seconds;
                    plog("engine: folder \"%s\" at %s with device \"%s\", read every %ld s", c.folders[i].id,
                         c.folders[i].path, c.devices[d].name, c.folders[i].rescan_seconds);
                    at++;
                }
            if (at == first) {
                plog("engine: device \"%s\" has no folder and is not contacted", c.devices[d].name);
                continue;
            }
            if (at - first > LINK_MAX_FOLDERS) {
                plog("engine: device \"%s\" has more than %d folders", c.devices[d].name, LINK_MAX_FOLDERS);
                free(devs);
                free(folders);
                conf_free(&c);
                return finish(0, "BAD_CONFIGURATION");
            }
            link_config *l = &devs[ndevs++];
            l->name = c.devices[d].name;
            l->ip = c.devices[d].ip;
            l->port = c.devices[d].port;
            memcpy(l->peer_id, c.devices[d].id, DEVID_BYTES);
            l->device_name = c.name;
            l->identity_dir = dir;
            l->folders = &folders[first];
            l->nfolders = at - first;
            l->log = plog;
            l->ui = ui;
            l->scope = set_scope;
        }
    } else if (argc >= 9) {
        long port = strtol(argv[5], NULL, 10);
        long rescan = argc > 9 ? strtol(argv[9], NULL, 10) : 60;
        memset(&one, 0, sizeof one);
        if (port < 1 || port > 65535 || rescan < 0 || rescan > 3600 || !*argv[7]) {
            plog("run: bad port, time limit, update count or folder");
            return finish(0, "BAD_ARGUMENT");
        }
        if (devid_parse(argv[6], one.peer_id) != 0) {
            plog("run: \"%s\" is not a valid Device ID", argv[6]);
            return finish(0, "BAD_ARGUMENT");
        }
        char peer_text[DEVID_TEXT_SIZE];
        devid_format(one.peer_id, peer_text);
        plog("engine: peer %s:%ld, expected Device ID %s, folder \"%s\", folder here %s", argv[4], port, peer_text,
             argv[7], argv[8]);
        one_folder.id = argv[7];
        one_folder.path = argv[8];
        one_folder.rescan_seconds = rescan;
        one.ip = argv[4];
        one.port = (unsigned)port;
        one.device_name = "aros";
        one.identity_dir = dir;
        one.folders = &one_folder;
        one.nfolders = 1;
        one.log = plog;
        one.ui = ui;
        devs = &one;
        ndevs = 1;
    } else {
        return finish(0, "BAD_ARGUMENT");
    }

    int rc = 1;
    const char *result = "IDENTITY_LOAD";
    tls_identity *id = show(dir) ? tls_identity_load(dir, plog) : NULL;
    if (id) {
        reach_options ro;
        memset(&ro, 0, sizeof ro);
        ro.listen_port = c.listen_port;
        ro.global = c.global_discovery;
        ro.relays = c.relays;
        ro.local_announce = c.local_announce;
        ro.lookup_url = ro.announce_url = c.discovery_server;
        ro.fixed_relay = c.relay;
#ifndef __AROS__
        /* Host tests: a relay pool of their own. */
        ro.pool_url = getenv("STPOC_RELAY_POOL");
#endif
        /* A configuration file has these; one device on the command line
         * (the older tests) has none. */
        engine_config ec = { devs, ndevs, plog, ui, argc == 5 ? &ro : NULL };
        rc = engine_run(&ec, id, &result);
        tls_identity_free(id);
    }
    set_scope(NULL);
    if (devs != &one)
        free(devs);
    free(folders);
    conf_free(&c);
    return finish(rc == 0, result);
}

/* manifest: one line "<sha256>  ./<name>" for every file the scanner
 * reports under folder (the format of shasum over find's output), in the
 * scanner's order, then "end <files> files, <unreadable> unreadable". A
 * manifest without that last line was cut short. The folder is compared
 * on the other side from this small file instead of from a copy of it. */
static int manifest(const char *folder, const char *out_path)
{
    static unsigned char buf[65536];
    scan_result r;
    memset(&r, 0, sizeof r);
    if (scan_folder(folder, &r) != 0) {
        fprintf(stderr, "cannot read %s\n", folder);
        return 10;
    }
    FILE *out = fopen(out_path, "w");
    if (!out) {
        fprintf(stderr, "cannot write %s\n", out_path);
        scan_free(&r);
        return 10;
    }
    long files = 0, unreadable = r.failed_dirs;
    size_t flen = strlen(folder);
    const char *sep = flen && (folder[flen - 1] == '/' || folder[flen - 1] == ':') ? "" : "/";
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    for (size_t i = 0; c && i < r.n; i++) {
        if (r.items[i].kind != SCAN_FILE)
            continue;
        size_t n = flen + strlen(r.items[i].name) + 2;
        char *path = malloc(n);
        FILE *f = NULL;
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        int ok = 0;
        if (path) {
            snprintf(path, n, "%s%s%s", folder, sep, r.items[i].name);
            f = fopen(path, "rb");
        }
        if (f && EVP_DigestInit_ex(c, EVP_sha256(), NULL)) {
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, f)) > 0)
                EVP_DigestUpdate(c, buf, got);
            ok = !ferror(f) && EVP_DigestFinal_ex(c, md, &len) && len == 32;
        }
        if (f)
            fclose(f);
        free(path);
        if (!ok) {
            unreadable++;
            fprintf(out, "unreadable  ./%s\n", r.items[i].name);
            continue;
        }
        for (unsigned int k = 0; k < len; k++)
            fprintf(out, "%02x", md[k]);
        fprintf(out, "  ./%s\n", r.items[i].name);
        files++;
    }
    EVP_MD_CTX_free(c);
    fprintf(out, "end %ld files, %ld unreadable\n", files, c ? unreadable : -1);
    int bad = ferror(out);
    bad |= fclose(out) != 0;
    scan_free(&r);
    return !c || bad || unreadable ? 10 : 0;
}

int main(int argc, char **argv)
{
    t0 = net_now();
    for (;;) {
        if (argc > 1 && strcmp(argv[1], "--brief") == 0) {
            brief = 1;
            argv++;
            argc--;
        } else if (argc > 2 && strcmp(argv[1], "--status") == 0) {
            status_path = argv[2];
            remove(status_path);
            argv += 2;
            argc -= 2;
        } else if (argc > 2 && strcmp(argv[1], "--log-max") == 0) {
            log_max = strtol(argv[2], NULL, 10);
            argv += 2;
            argc -= 2;
        } else {
            break;
        }
    }
    if (argc > 2 && strcmp(argv[1], "ctl") == 0) {
        /* A command for the running engine (src/ctl.h); no log file. */
        static char line[400], answer[CTL_ANSWER_MAX];
        size_t o = 0;
        for (int i = 2; i < argc && o < sizeof line - 1; i++)
            o += (size_t)snprintf(line + o, sizeof line - o, "%s%s", i > 2 ? " " : "", argv[i]);
        int rc = ctlport_send(line, answer, sizeof answer);
        if (*answer)
            printf("%s\n", answer);
        return rc;
    }
    if (argc == 4 && strcmp(argv[1], "manifest") == 0)
        return manifest(argv[2], argv[3]);
    if (argc < 4) {
        fprintf(stderr, "usage: see the comment at the top of src/main.c\n");
        return 20;
    }
    if (!(logf = fopen(argv[1], "w"))) {
        fprintf(stderr, "cannot open log %s\n", argv[1]);
        return 20;
    }
    const char *cmd = argv[2], *dir = argv[3];
    int engine = strcmp(cmd, "engine") == 0;
    log_path = argv[1];
    if (engine) {
        engine_stop_init(); /* before anything slow: a stop request must not be lost */
        brief = 1;
        if (log_max <= 0)
            log_max = 1048576;
    } else {
        log_max = 0;
    }
    plog("aros-syncthing PoC, target %s, %s", ST_TARGET, OpenSSL_version(OPENSSL_VERSION));
    if (brief || status_path)
        ui("aros-syncthing 0.0.1 - proof of concept (%s)",
           engine ? "engine, first version" : strcmp(argv[2], "twoway") == 0 ? "two-way, first version"
                                                                             : "receive-only");
    plog("command %s", cmd);
    if (net_init() != 0)
        return finish(0, "NET_INIT");
    if (tls_rng_init(plog) != 0)
        return finish(0, "RNG_FAILED");

    if (strcmp(cmd, "identity-create") == 0) {
        if (tls_identity_create(dir, plog) != 0)
            return finish(0, "IDENTITY_CREATE");
        int ok = show(dir);
        return finish(ok, ok ? "" : "IDENTITY_LOAD");
    }
    if (strcmp(cmd, "identity-show") == 0)
        return finish(show(dir), "");
    if (engine)
        return run_engine(argc, argv, dir);
    int two_way = strcmp(cmd, "twoway") == 0;
    int sync = two_way || strcmp(cmd, "sync") == 0;
    if ((!sync && strcmp(cmd, "run") != 0) || argc < (sync ? 9 : 8))
        return finish(0, "BAD_ARGUMENT");

    session_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.peer_ip = argv[4];
    long port = strtol(argv[5], NULL, 10);
    cfg.folder_id = argv[7];
    int a = sync ? 9 : 8; /* first optional argument */
    if (sync)
        cfg.dest = argv[8];
    cfg.seconds = argc > a ? strtol(argv[a], NULL, 10) : 60;
    cfg.want_updates = argc > a + 1 ? (int)strtol(argv[a + 1], NULL, 10) : 0;
    if (two_way) {
        cfg.two_way = 1;
        cfg.want_updates = 0;
        cfg.rescan_seconds = argc > a + 1 ? strtol(argv[a + 1], NULL, 10) : 0;
        cfg.identity_dir = dir;
    } else if (sync) {
        cfg.abort_after_blocks = argc > a + 2 ? strtol(argv[a + 2], NULL, 10) : 0;
        cfg.state_fault = argc > a + 3 ? (int)strtol(argv[a + 3], NULL, 10) : 0;
        cfg.commit_hook = argc > a + 4 && strcmp(argv[a + 4], "-") != 0 ? argv[a + 4] : NULL;
        cfg.identity_dir = dir;
    }
    cfg.device_name = !sync && argc > a + 2 ? argv[a + 2] : "aros";
    cfg.log = plog;
    cfg.ui = brief || status_path ? ui : NULL;
    if (port < 1 || port > 65535 || cfg.seconds < 1 || cfg.seconds > 3600 || cfg.want_updates < 0
        || cfg.want_updates > 1000 || cfg.rescan_seconds < 0 || cfg.rescan_seconds > 3600 || !*cfg.folder_id) {
        plog("run: bad port, time limit, update count or folder");
        return finish(0, "BAD_ARGUMENT");
    }
    cfg.peer_port = (unsigned)port;
    if (devid_parse(argv[6], cfg.peer_id) != 0) {
        plog("run: \"%s\" is not a valid Device ID", argv[6]);
        return finish(0, "BAD_ARGUMENT");
    }
    char peer_text[DEVID_TEXT_SIZE];
    devid_format(cfg.peer_id, peer_text);
    plog("%s: peer %s:%ld, expected Device ID %s, folder \"%s\", limit %ld s, updates wanted %d%s%s",
         cmd, cfg.peer_ip, port, peer_text, cfg.folder_id, cfg.seconds, cfg.want_updates,
         sync ? ", destination " : "", sync ? cfg.dest : "");
    if (cfg.abort_after_blocks > 0)
        plog("%s: TEST HOOK: the transfer stops after %ld block(s)", cmd, cfg.abort_after_blocks);
    if (cfg.state_fault)
        plog("%s: TEST HOOK: every state write fails", cmd);

    if (!show(dir))
        return finish(0, "IDENTITY_LOAD");
    tls_identity *id = tls_identity_load(dir, plog);
    if (!id)
        return finish(0, "IDENTITY_LOAD");
    const char *result = NULL;
    int rc = session_run(&cfg, id, &result);
    tls_identity_free(id);
    return finish(rc == 0, rc == 0 ? "" : result);
}
