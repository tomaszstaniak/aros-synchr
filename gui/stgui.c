/* The window of Synchr (Zune; the project's working name is aros-syncthing): devices and folders are set up
 * here, the engine (stpoc, src/engine.h) is started and stopped by this
 * program and asked for its state through its control port (src/ctl.h).
 * Nothing has to be typed in a Shell.
 *
 * Files, all in the drawer Data next to the program:
 *   cert.pem, key.pem   this device's identity (made at the first start)
 *   device-id.txt       its Device ID, as text
 *   engine.conf         devices and folders (src/conf.h), written here
 *   engine.log          the engine's log
 *   *.state             the engine's state files
 * The engine is the program SynchrEngine (stpoc, built from src/) in the
 * same drawer as this one.
 *
 * Start with the system: a project icon SYS:WBStartup/Synchr whose default
 * tool is this program, with the tool type HIDE. Started with HIDE (tool
 * type, or the switch HIDE in a Shell) the program opens no window: it
 * starts the engine when there is a folder to synchronise and ends.
 *
 * A change of devices or folders is written to engine.conf and the
 * engine is started again with it. The engine runs while there is at
 * least one folder. Closing the window leaves it running: the window
 * can be opened again by starting the program again. The menu item
 * "Stop synchronising and quit" ends both. */
#include "../src/conf.h"
#include "../src/ctlport.h"

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <libraries/asl.h>
#include <libraries/iffparse.h>
#include <libraries/mui.h>
#include <rexx/rxslib.h>
#include <rexx/storage.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/iffparse.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/rexxsyslib.h>
#include <proto/utility.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUI_VERSION "0.2"
#define GUI_DATE "20.11.2026"
#define ENGINE_NAME "SynchrEngine"
#define START_ICON "SYS:WBStartup/Synchr"
#define TICK_SECONDS 2
#define RESTART_SECONDS 30

struct Library *MUIMasterBase;
struct Library *IFFParseBase;
struct Library *IconBase;
struct RxsLib *RexxSysBase;

enum {
    GID_ADDDEV = 1, GID_REMDEV, GID_ADDFOL, GID_REMFOL, GID_PAUSE, GID_SCAN, GID_COPY, GID_NAME,
    GID_DEVOK, GID_DEVCANCEL, GID_FOLOK, GID_FOLCANCEL, GID_STOPQUIT, GID_EDITDEV, GID_EDITFOL, GID_AUTOSTART, GID_PUBLIC
};

static Object *app, *win, *devlist, *follist, *status, *idtext, *namestr, *autostart, *public_servers;
static Object *devwin, *dname, *did, *daddr, *dport;
static Object *folwin, *fid, *fpath, *fwith;

static conf C;
static char dev_state[CONF_MAX_DEVICES][120];
static char dev_found[CONF_MAX_DEVICES][40]; /* the address the engine uses, for a device found automatically */
static char fol_state[CONF_MAX_FOLDERS][160];
static int fol_paused[CONF_MAX_FOLDERS];
static char progdir[600], datadir[700], confpath[800], logpath[800], idpath[800], stpoc[700];
static char device_id[DEVID_TEXT_SIZE + 1];
static long edit_dev = -1, edit_fol = -1; /* the entry being changed, -1 when one is added */
static char progpath[700]; /* this program, for the start icon */
static long restart_wait; /* seconds until the engine may be started again after it was found gone */

static void say(const char *fmt, ...)
{
    static char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    set(status, MUIA_Text_Contents, (IPTR)msg);
}

static const char *text_of(Object *o)
{
    IPTR v = 0;
    get(o, MUIA_String_Contents, &v);
    return v ? (const char *)v : "";
}

/* ---- the engine ---- */

static int engine_running(void)
{
    Forbid();
    int up = FindPort((CONST_STRPTR)CTLPORT_NAME) != NULL;
    Permit();
    return up;
}

/* One command to the engine (the client half of src/ctlport.c, which is
 * not linked here because its other half needs the network code).
 * Returns the command's return code, 20 when there is no engine. */
static int engine_send(const char *line, char *out, size_t n)
{
    int rc = 20;
    out[0] = 0;
    struct MsgPort *reply = CreateMsgPort();
    struct RexxMsg *rm = reply ? CreateRexxMsg(reply, NULL, NULL) : NULL;
    if (rm && (rm->rm_Args[0] = (IPTR)CreateArgstring((STRPTR)line, strlen(line))) != 0) {
        rm->rm_Action = RXCOMM | RXFF_RESULT;
        Forbid();
        struct MsgPort *to = FindPort((CONST_STRPTR)CTLPORT_NAME);
        if (to)
            PutMsg(to, (struct Message *)rm);
        Permit();
        if (to) {
            WaitPort(reply);
            GetMsg(reply);
            rc = (int)rm->rm_Result1;
            if (rc == 0 && rm->rm_Result2) {
                snprintf(out, n, "%s", (const char *)rm->rm_Result2);
                DeleteArgstring((STRPTR)rm->rm_Result2);
            }
        }
        DeleteArgstring((STRPTR)rm->rm_Args[0]);
    }
    if (rm)
        DeleteRexxMsg(rm);
    if (reply)
        DeleteMsgPort(reply);
    return rc;
}

/* Appends ARG to cmd, quoted. 0 when it holds a character that cannot be
 * passed (a double quote, '*', a line break). */
static int add_arg(char *cmd, size_t cap, const char *arg)
{
    if (strpbrk(arg, "\"\n\r*"))
        return 0;
    size_t n = strlen(cmd);
    int w = snprintf(cmd + n, cap - n, "%s\"%s\"", n ? " " : "", arg);
    return w > 0 && (size_t)w < cap - n;
}

/* Runs a command line; wait != 0: until it ends, returning its code. */
static LONG run(const char *cmd, int wait)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out) {
        if (in)
            Close(in);
        if (out)
            Close(out);
        return -1;
    }
    LONG rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, SYS_Asynch, wait ? FALSE : TRUE,
                         NP_StackSize, 262144, TAG_DONE);
    if (wait || rc == -1) { /* an asynchronous command closes them itself */
        Close(in);
        Close(out);
    }
    return rc;
}

static int engine_start(void)
{
    static char cmd[3200];
    cmd[0] = 0;
    if (!(add_arg(cmd, sizeof cmd, stpoc) && add_arg(cmd, sizeof cmd, "--log-max") && add_arg(cmd, sizeof cmd, "400000")
          && add_arg(cmd, sizeof cmd, logpath) && add_arg(cmd, sizeof cmd, "engine") && add_arg(cmd, sizeof cmd, datadir)
          && add_arg(cmd, sizeof cmd, confpath)))
        return 0;
    return run(cmd, 0) != -1;
}

static void engine_stop(void)
{
    char out[100];
    if (!engine_running())
        return;
    engine_send("STOP", out, sizeof out);
    for (int i = 0; i < 150 && engine_running(); i++) /* it says goodbye to its devices first */
        Delay(5);
}

/* ---- the lists ---- */

AROS_UFH3S(IPTR, dev_display, AROS_UFHA(struct Hook *, h, A0), AROS_UFHA(char **, col, A2), AROS_UFHA(APTR, e, A1))
{
    AROS_USERFUNC_INIT
    static char addr[80];
    (void)h;
    if (!e) {
        col[0] = "\33bDevice";
        col[1] = "\33bAddress";
        col[2] = "\33bState";
    } else {
        size_t i = (size_t)(IPTR)e - 1;
        if (C.devices[i].ip)
            snprintf(addr, sizeof addr, "%s:%u", C.devices[i].ip, C.devices[i].port);
        else
            snprintf(addr, sizeof addr, "%s", dev_found[i][0] ? dev_found[i] : "automatic");
        col[0] = C.devices[i].name;
        col[1] = addr;
        col[2] = dev_state[i];
    }
    return 0;
    AROS_USERFUNC_EXIT
}

AROS_UFH3S(IPTR, fol_display, AROS_UFHA(struct Hook *, h, A0), AROS_UFHA(char **, col, A2), AROS_UFHA(APTR, e, A1))
{
    AROS_USERFUNC_INIT
    (void)h;
    if (!e) {
        col[0] = "\33bFolder ID";
        col[1] = "\33bDrawer";
        col[2] = "\33bWith";
        col[3] = "\33bState";
    } else {
        size_t i = (size_t)(IPTR)e - 1;
        col[0] = C.folders[i].id;
        col[1] = C.folders[i].path;
        col[2] = C.devices[C.folders[i].device].name;
        col[3] = fol_state[i];
    }
    return 0;
    AROS_USERFUNC_EXIT
}

static struct Hook dev_hook, fol_hook;

static void fill_lists(void)
{
    set(devlist, MUIA_List_Quiet, TRUE);
    DoMethod(devlist, MUIM_List_Clear);
    for (size_t i = 0; i < C.ndevices; i++)
        DoMethod(devlist, MUIM_List_InsertSingle, (IPTR)(i + 1), MUIV_List_Insert_Bottom);
    set(devlist, MUIA_List_Quiet, FALSE);
    set(follist, MUIA_List_Quiet, TRUE);
    DoMethod(follist, MUIM_List_Clear);
    for (size_t i = 0; i < C.nfolders; i++)
        DoMethod(follist, MUIM_List_InsertSingle, (IPTR)(i + 1), MUIV_List_Insert_Bottom);
    set(follist, MUIA_List_Quiet, FALSE);
}

static const char *reason_text(const char *code)
{
    static const struct { const char *code, *text; } t[] = {
        { "NO_ADDRESS", "not found yet; waiting for it to connect" },
        { "RELAY_UNREACHABLE", "its relay does not answer" },
        { "RELAY_TIMEOUT", "no connection through its relay" },
        { "RELAY_DEVICE_NOT_THERE", "the device is not at its relay any more" },
        { "RELAY_REFUSED", "its relay refused the connection" },
        { "RELAY_ID_MISMATCH", "its relay is not the one announced" },
        { "RELAY_PROTOCOL_ERROR", "its relay sent something not understood" },
        { "REPLACED_BY_DIRECT", "a direct connection replaces the one through a relay" },
        { "CONNECT_FAILED", "no answer at this address" },
        { "CONNECT_TIMEOUT", "no answer at this address" },
        { "TLS_FAILED", "the secure connection failed" },
        { "WRONG_DEVICE", "another device answers at this address" },
        { "PEER_DISCONNECTED", "the device closed the connection: accept this device there" },
        { "PEER_CLOSED", "the device closed the connection: has it accepted this device?" },
        { "CONNECTION_LOST", "the connection was lost" },
        { "NO_FOLDERS", "no folder is shared with it by both sides yet" },
        { "ALL_FOLDERS_PAUSED", "all its folders are paused" },
        { "FOLDER_PAUSED_HERE", "restarting without the paused folder" },
        { "FOLDER_RESUMED", "restarting with the resumed folder" },
        { "FOLDER_AVAILABLE", "restarting with a folder that is back" },
        { "TIMEOUT", "no answer at this address" },
        { "HELLO_TIMEOUT", "the device did not answer: is Syncthing running there?" },
        { "TLS_HANDSHAKE_FAILED", "the secure connection failed: has it accepted this device?" },
        { "PEER_ID_MISMATCH", "another device answers at this address" },
        { "PEER_CERTIFICATE_INVALID", "the device's identity is not valid" },
        { "SEND_TIMEOUT", "the connection stalled" },
        { "BAD_DESTINATION", "a folder's drawer is missing or cannot be used" },
        { "FOLDER_UNAVAILABLE", "a folder's drawer is missing or cannot be used" },
        { "FOLDER_NOT_SHARED", "the device does not share the folder with this one yet" },
        { "FOLDER_PAUSED", "the folder is paused on that device" },
        { "CLUSTER_CONFIG_CHANGED", "the device changed its shared folders" },
        { "INDEX_ID_RENEWED", "the device started its list again" },
        { "STATE_NOT_SAVED", "what was received could not be saved; is the disk full?" },
        { "IO_ERROR", "a file could not be read or written" },
        { "OUT_OF_MEMORY", "not enough memory" },
        { "PROTOCOL_ERROR", "the device sent something not understood" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (strcmp(code, t[i].code) == 0)
            return t[i].text;
    return code;
}

static void no_engine_states(const char *text)
{
    for (size_t i = 0; i < C.ndevices; i++) {
        snprintf(dev_state[i], sizeof dev_state[i], "%s", text);
        dev_found[i][0] = 0;
    }
    for (size_t i = 0; i < C.nfolders; i++) {
        snprintf(fol_state[i], sizeof fol_state[i], "%s", text);
        fol_paused[i] = 0;
    }
}

/* Takes the engine's STATUS answer into the state columns. */
static void take_status(char *text)
{
    for (char *line = text, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        if ((nl = strchr(line, '\n')))
            *nl = 0;
        char name[200], state[40], a[200], b[200];
        long n1 = 0, n2 = 0, held = 0, failed = 0, conflicts = 0;
        if (sscanf(line, "device %199s %39s sessions=%ld retry=%ld last=%199s", name, state, &n1, &n2, a) == 5) {
            for (size_t i = 0; i < C.ndevices; i++) {
                if (strcmp(C.devices[i].name, name) != 0)
                    continue;
                char *s = dev_state[i];
                size_t cap = sizeof dev_state[i];
                const char *ad = strstr(line, " address=");
                dev_found[i][0] = 0;
                if (ad && ad[9] != '-')
                    sscanf(ad + 9, "%39s", dev_found[i]);
                const char *via = strstr(line, " via=");
                int relayed = via && strncmp(via + 5, "relay", 5) == 0;
                if (strcmp(state, "connected") == 0)
                    snprintf(s, cap, relayed ? "Connected through a relay" : "Connected");
                else if (strcmp(state, "connecting") == 0)
                    snprintf(s, cap, "Connecting...");
                else if (strcmp(state, "reading") == 0)
                    snprintf(s, cap, "Reading the folders...");
                else if (strcmp(a, "-") == 0 || !*a)
                    snprintf(s, cap, "Not connected");
                else if (n2 <= 0)
                    snprintf(s, cap, "Not connected: %.90s", reason_text(a));
                else
                    snprintf(s, cap, "Not connected: %.60s (next try in %ld s)", reason_text(a), n2);
            }
        } else if (sscanf(line, "folder %199s %39s device=%199s why=%199s received=%ld deleted=%ld conflicts=%ld held=%ld failed=%ld",
                          name, state, a, b, &n1, &n2, &conflicts, &held, &failed) == 9) {
            for (size_t i = 0; i < C.nfolders; i++) {
                if (strcmp(C.folders[i].id, name) != 0)
                    continue;
                static const struct { const char *state, *text; } t[] = {
                    { "idle", "Waiting for the device" },
                    { "reading", "Reading the drawer" },
                    { "index", "Receiving the list of files" },
                    { "scanning", "Reading the drawer" },
                    { "syncing", "Synchronising" },
                    { "in-sync", "Up to date" },
                    { "paused-here", "Paused" },
                    { "paused", "Paused on the other device" },
                    { "not-shared", "Not shared by the other device yet" },
                    { "unusable", "Cannot be used" },
                };
                const char *txt = state;
                for (size_t k = 0; k < sizeof t / sizeof *t; k++)
                    if (strcmp(state, t[k].state) == 0)
                        txt = t[k].text;
                fol_paused[i] = strcmp(state, "paused-here") == 0;
                char *s = fol_state[i];
                size_t cap = sizeof fol_state[i], o;
                o = (size_t)snprintf(s, cap, "%s", txt);
                if (strcmp(state, "unusable") == 0 && strcmp(b, "-") != 0 && o < cap)
                    o += (size_t)snprintf(s + o, cap - o, ": %.100s",
                                          strcmp(b, "BAD_DESTINATION") == 0 || strcmp(b, "FOLDER_UNAVAILABLE") == 0
                                              ? "the drawer or its .stfolder marker is missing"
                                              : reason_text(b));
                if ((n1 || n2) && o < cap)
                    o += (size_t)snprintf(s + o, cap - o, "; %ld received, %ld deleted", n1, n2);
                if (conflicts && o < cap)
                    o += (size_t)snprintf(s + o, cap - o, "; %ld conflict copies", conflicts);
                if ((held || failed) && o < cap)
                    snprintf(s + o, cap - o, "; %ld not synchronised", held + failed);
            }
        }
    }
}

static void refresh(void)
{
    static char answer[8192];
    if (C.nfolders == 0) {
        no_engine_states(C.ndevices ? "No folder yet" : "");
    } else if (engine_running()) {
        restart_wait = 0;
        if (engine_send("STATUS", answer, sizeof answer) == 0)
            take_status(answer);
    } else {
        no_engine_states("Not running");
        if (restart_wait > 0) {
            restart_wait -= TICK_SECONDS;
        } else {
            restart_wait = RESTART_SECONDS;
            if (!engine_start())
                say("The synchronisation program %s could not be started", stpoc);
        }
    }
    DoMethod(devlist, MUIM_List_Redraw, MUIV_List_Redraw_All);
    DoMethod(follist, MUIM_List_Redraw, MUIV_List_Redraw_All);
}

/* Writes the configuration and has the engine use it. */
static void apply(void)
{
    char why[300];
    set(app, MUIA_Application_Sleep, TRUE);
    if (conf_save(confpath, &C, why, sizeof why) != 0) {
        say("The settings could not be saved: %s", why);
    } else {
        engine_stop();
        no_engine_states(C.nfolders ? "Starting..." : "No folder yet");
        restart_wait = 0;
        if (C.nfolders && !engine_start())
            say("The synchronisation program %s could not be started", stpoc);
    }
    fill_lists();
    set(app, MUIA_Application_Sleep, FALSE);
}

/* ---- changes ---- */

static char *dupstr(const char *s)
{
    char *d = malloc(strlen(s) + 1);
    if (d)
        strcpy(d, s);
    return d;
}

static int one_word(const char *s)
{
    if (!*s || *s == '#')
        return 0;
    for (; *s; s++)
        if (isspace((unsigned char)*s))
            return 0;
    return 1;
}

static int ipv4_ok(const char *s)
{
    unsigned a, b, c, d;
    char rest;
    return sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &rest) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
}

static void add_device(void)
{
    const char *name = text_of(dname), *id = text_of(did), *addr = text_of(daddr);
    long port = strtol(text_of(dport), NULL, 10);
    unsigned char raw[DEVID_BYTES];
    char idtext_[200];
    size_t o = 0;
    for (const char *p = id; *p && o < sizeof idtext_ - 1; p++) /* pasted IDs often carry spaces */
        if (!isspace((unsigned char)*p))
            idtext_[o++] = (char)toupper((unsigned char)*p);
    idtext_[o] = 0;
    if (!one_word(name)) {
        say("The name must be one word without spaces");
        return;
    }
    for (size_t i = 0; i < C.ndevices; i++)
        if ((long)i != edit_dev && strcmp(C.devices[i].name, name) == 0) {
            say("There is already a device named %s", name);
            return;
        }
    if (devid_parse(idtext_, raw) != 0) {
        say("This is not a Device ID: copy it from the other device (Actions, Show ID)");
        return;
    }
    for (size_t i = 0; i < C.ndevices; i++)
        if ((long)i != edit_dev && memcmp(C.devices[i].id, raw, DEVID_BYTES) == 0) {
            say("This Device ID is already in the list as %s", C.devices[i].name);
            return;
        }
    int automatic = !*addr;
    if (!automatic && !ipv4_ok(addr)) {
        say("The address must be numeric, like 192.168.1.10, or empty to find the device");
        return;
    }
    if (!automatic && (port < 1 || port > 65535)) {
        say("The port is a number from 1 to 65535 (Syncthing uses 22000)");
        return;
    }
    if (edit_dev >= 0 && (size_t)edit_dev < C.ndevices) {
        conf_device *d = &C.devices[edit_dev];
        char *n = dupstr(name), *ip = automatic ? NULL : dupstr(addr);
        if (!n || (!automatic && !ip)) {
            free(n);
            free(ip);
            say("Out of memory");
            return;
        }
        free(d->name);
        free(d->ip);
        d->name = n;
        d->ip = ip;
        d->port = automatic ? 0 : (unsigned)port;
        memcpy(d->id, raw, DEVID_BYTES);
        set(devwin, MUIA_Window_Open, FALSE);
        say("Device %s changed", name);
        long keep = edit_dev;
        edit_dev = -1;
        apply();
        set(devlist, MUIA_List_Active, keep);
        return;
    }
    if (C.ndevices >= CONF_MAX_DEVICES) {
        say("No more than %d devices", CONF_MAX_DEVICES);
        return;
    }
    conf_device *nd = realloc(C.devices, (C.ndevices + 1) * sizeof *nd);
    char *n = dupstr(name), *ip = automatic ? NULL : dupstr(addr);
    if (!nd || !n || (!automatic && !ip)) {
        if (nd)
            C.devices = nd;
        free(n);
        free(ip);
        say("Out of memory");
        return;
    }
    C.devices = nd;
    memset(&nd[C.ndevices], 0, sizeof *nd);
    nd[C.ndevices].name = n;
    nd[C.ndevices].ip = ip;
    nd[C.ndevices].port = automatic ? 0 : (unsigned)port;
    memcpy(nd[C.ndevices].id, raw, DEVID_BYTES);
    C.ndevices++;
    set(devwin, MUIA_Window_Open, FALSE);
    say("Device %s added. Now add this device on %s (Device ID above).", name, name);
    apply();
    set(devlist, MUIA_List_Active, MUIV_List_Active_Bottom);
}

static void remove_device(void)
{
    LONG a = -1;
    get(devlist, MUIA_List_Active, &a);
    if (a < 0 || (size_t)a >= C.ndevices) {
        say("Select a device first");
        return;
    }
    for (size_t i = 0; i < C.nfolders; i++)
        if (C.folders[i].device == (size_t)a) {
            say("Folder %s is shared with %s: remove the folder first", C.folders[i].id, C.devices[a].name);
            return;
        }
    free(C.devices[a].name);
    free(C.devices[a].ip);
    memmove(&C.devices[a], &C.devices[a + 1], (C.ndevices - (size_t)a - 1) * sizeof *C.devices);
    C.ndevices--;
    for (size_t i = 0; i < C.nfolders; i++)
        if (C.folders[i].device > (size_t)a)
            C.folders[i].device--;
    say("Device removed");
    apply();
}

/* The drawer and its .stfolder marker, made when missing. */
static int make_folder(const char *path)
{
    char marker[1100];
    BPTR l = Lock((CONST_STRPTR)path, ACCESS_READ);
    if (!l && !(l = CreateDir((CONST_STRPTR)path)))
        return 0;
    UnLock(l);
    snprintf(marker, sizeof marker, "%s", path);
    if (!AddPart((STRPTR)marker, (CONST_STRPTR)".stfolder", sizeof marker))
        return 0;
    if (!(l = Lock((CONST_STRPTR)marker, ACCESS_READ)) && !(l = CreateDir((CONST_STRPTR)marker)))
        return 0;
    UnLock(l);
    return 1;
}

static void add_folder(void)
{
    const char *id = text_of(fid), *path = text_of(fpath);
    LONG a = -1;
    get(devlist, MUIA_List_Active, &a);
    if (edit_fol >= 0 && (size_t)edit_fol < C.nfolders)
        a = (LONG)C.folders[edit_fol].device;
    if (a < 0 || (size_t)a >= C.ndevices) {
        say("Select the device to share the folder with first");
        return;
    }
    if (!one_word(id)) {
        say("The folder ID must be one word: the same ID as on the other device");
        return;
    }
    for (size_t i = 0; i < C.nfolders; i++)
        if ((long)i != edit_fol && strcmp(C.folders[i].id, id) == 0) {
            say("There is already a folder with the ID %s", id);
            return;
        }
    if (!*path || strpbrk(path, "\"*\n\r") || isspace((unsigned char)*path)
        || isspace((unsigned char)path[strlen(path) - 1])) {
        say("Choose the drawer to synchronise");
        return;
    }
    for (size_t i = 0; i < C.nfolders; i++)
        if ((long)i != edit_fol && Stricmp((CONST_STRPTR)C.folders[i].path, (CONST_STRPTR)path) == 0) {
            say("This drawer already belongs to folder %s", C.folders[i].id);
            return;
        }
    if (edit_fol < 0 && C.nfolders >= CONF_MAX_FOLDERS) {
        say("No more than %d folders", CONF_MAX_FOLDERS);
        return;
    }
    if (!make_folder(path)) {
        say("The drawer %s cannot be used", path);
        return;
    }
    if (edit_fol >= 0 && (size_t)edit_fol < C.nfolders) {
        conf_folder *f = &C.folders[edit_fol];
        char *i2 = dupstr(id), *p2 = dupstr(path);
        if (!i2 || !p2) {
            free(i2);
            free(p2);
            say("Out of memory");
            return;
        }
        free(f->id);
        free(f->path);
        f->id = i2;
        f->path = p2;
        set(folwin, MUIA_Window_Open, FALSE);
        say("Folder %s changed", id);
        long keep = edit_fol;
        edit_fol = -1;
        apply();
        set(follist, MUIA_List_Active, keep);
        return;
    }
    conf_folder *nf = realloc(C.folders, (C.nfolders + 1) * sizeof *nf);
    char *i2 = dupstr(id), *p2 = dupstr(path);
    if (!nf || !i2 || !p2) {
        if (nf)
            C.folders = nf;
        free(i2);
        free(p2);
        say("Out of memory");
        return;
    }
    C.folders = nf;
    memset(&nf[C.nfolders], 0, sizeof *nf);
    nf[C.nfolders].id = i2;
    nf[C.nfolders].path = p2;
    nf[C.nfolders].device = (size_t)a;
    nf[C.nfolders].rescan_seconds = CONF_RESCAN_DEFAULT;
    C.nfolders++;
    set(folwin, MUIA_Window_Open, FALSE);
    say("Folder %s added. Share it with this device on %s.", id, C.devices[a].name);
    apply();
}

static void remove_folder(void)
{
    LONG a = -1;
    get(follist, MUIA_List_Active, &a);
    if (a < 0 || (size_t)a >= C.nfolders) {
        say("Select a folder first");
        return;
    }
    free(C.folders[a].id);
    free(C.folders[a].path);
    memmove(&C.folders[a], &C.folders[a + 1], (C.nfolders - (size_t)a - 1) * sizeof *C.folders);
    C.nfolders--;
    say("Folder removed from the list; its files stay in the drawer");
    apply();
}

static void folder_command(int pause)
{
    char cmd[300], out[200];
    LONG a = -1;
    get(follist, MUIA_List_Active, &a);
    if (a < 0 || (size_t)a >= C.nfolders) {
        say("Select a folder first");
        return;
    }
    snprintf(cmd, sizeof cmd, "%s %s", !pause ? "SCAN" : fol_paused[a] ? "RESUME" : "PAUSE", C.folders[a].id);
    int rc = engine_send(cmd, out, sizeof out);
    if (rc == 20)
        say("The synchronisation program is not running");
    else if (rc != 0)
        say("Not done (%s)", cmd);
    else
        say(!pause ? "Folder %s is being read again" : fol_paused[a] ? "Folder %s resumed" : "Folder %s paused",
            C.folders[a].id);
    refresh();
}

static void open_device(long i)
{
    char idt[DEVID_TEXT_SIZE + 1], port[12];
    edit_dev = i;
    if (i >= 0) {
        devid_format(C.devices[i].id, idt);
        snprintf(port, sizeof port, "%u", C.devices[i].ip ? C.devices[i].port : 22000u);
    }
    set(devwin, MUIA_Window_Title, (IPTR)(i >= 0 ? "Change a device" : "Add a device"));
    set(dname, MUIA_String_Contents, (IPTR)(i >= 0 ? C.devices[i].name : ""));
    set(did, MUIA_String_Contents, (IPTR)(i >= 0 ? idt : ""));
    set(daddr, MUIA_String_Contents, (IPTR)(i >= 0 && C.devices[i].ip ? C.devices[i].ip : ""));
    set(dport, MUIA_String_Contents, (IPTR)(i >= 0 ? port : "22000"));
    set(devwin, MUIA_Window_Open, TRUE);
    set(devwin, MUIA_Window_ActiveObject, (IPTR)dname);
}

static void open_folder(long i)
{
    LONG a = -1;
    if (i < 0) {
        get(devlist, MUIA_List_Active, &a);
        if (C.ndevices == 1) {
            a = 0;
            set(devlist, MUIA_List_Active, 0);
        }
        if (a < 0 || (size_t)a >= C.ndevices) {
            say(C.ndevices ? "Select the device to share the folder with first" : "Add a device first");
            return;
        }
    } else {
        a = (LONG)C.folders[i].device;
    }
    edit_fol = i;
    set(folwin, MUIA_Window_Title, (IPTR)(i >= 0 ? "Change a folder" : "Add a folder"));
    set(fwith, MUIA_Text_Contents, (IPTR)C.devices[a].name);
    set(fid, MUIA_String_Contents, (IPTR)(i >= 0 ? C.folders[i].id : ""));
    set(fpath, MUIA_String_Contents, (IPTR)(i >= 0 ? C.folders[i].path : ""));
    set(folwin, MUIA_Window_Open, TRUE);
    set(folwin, MUIA_Window_ActiveObject, (IPTR)fid);
}

static long selected(Object *list, size_t n)
{
    LONG a = -1;
    get(list, MUIA_List_Active, &a);
    return a >= 0 && (size_t)a < n ? (long)a : -1;
}

static void change_name(void)
{
    const char *n = text_of(namestr);
    if (!one_word(n)) {
        say("The name must be one word without spaces");
        return;
    }
    char *d = dupstr(n);
    if (!d)
        return;
    free(C.name);
    C.name = d;
    say("This device is now called %s", n);
    apply();
}

/* ---- this device's identity ---- */

/* The first Device ID found in a text file. */
static int id_from_file(const char *path, char out[DEVID_TEXT_SIZE + 1])
{
    static char buf[4096];
    unsigned char raw[DEVID_BYTES];
    BPTR fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return 0;
    LONG got = Read(fh, buf, sizeof buf - 1);
    Close(fh);
    if (got <= 0)
        return 0;
    buf[got] = 0;
    for (char *p = buf; (size_t)(buf + got - p) >= 63; p++) {
        if (p[7] != '-' || p[15] != '-')
            continue;
        char cand[64];
        memcpy(cand, p, 63);
        cand[63] = 0;
        if (devid_parse(cand, raw) == 0) {
            devid_format(raw, out);
            return 1;
        }
    }
    return 0;
}

static int have_file(const char *dir, const char *name)
{
    char p[900];
    snprintf(p, sizeof p, "%s", dir);
    if (!AddPart((STRPTR)p, (CONST_STRPTR)name, sizeof p))
        return 0;
    BPTR l = Lock((CONST_STRPTR)p, ACCESS_READ);
    if (l)
        UnLock(l);
    return l != 0;
}

/* Makes the identity at the first start and finds the Device ID. */
static int identity(void)
{
    static char cmd[2400], tmplog[900];
    if (id_from_file(idpath, device_id))
        return 1;
    snprintf(tmplog, sizeof tmplog, "%s", datadir);
    AddPart((STRPTR)tmplog, (CONST_STRPTR)"identity.log", sizeof tmplog);
    int exists = have_file(datadir, "cert.pem") && have_file(datadir, "key.pem");
    say(exists ? "Reading this device's identity..." : "Making this device's identity (once; it can take a minute)...");
    cmd[0] = 0;
    if (!(add_arg(cmd, sizeof cmd, stpoc) && add_arg(cmd, sizeof cmd, tmplog)
          && add_arg(cmd, sizeof cmd, exists ? "identity-show" : "identity-create") && add_arg(cmd, sizeof cmd, datadir)))
        return 0;
    set(app, MUIA_Application_Sleep, TRUE);
    LONG rc = run(cmd, 1);
    set(app, MUIA_Application_Sleep, FALSE);
    if (rc != 0 || !id_from_file(tmplog, device_id))
        return 0;
    BPTR fh = Open((CONST_STRPTR)idpath, MODE_NEWFILE);
    if (fh) {
        FPuts(fh, (CONST_STRPTR)device_id);
        FPuts(fh, (CONST_STRPTR)"\n");
        Close(fh);
    }
    DeleteFile((CONST_STRPTR)tmplog);
    return 1;
}

static void copy_id(void)
{
    struct IFFHandle *iff = IFFParseBase ? AllocIFF() : NULL;
    int ok = 0;
    if (iff) {
        if ((iff->iff_Stream = (IPTR)OpenClipboard(0)) != 0) {
            InitIFFasClip(iff);
            if (OpenIFF(iff, IFFF_WRITE) == 0) {
                ok = PushChunk(iff, MAKE_ID('F', 'T', 'X', 'T'), ID_FORM, IFFSIZE_UNKNOWN) == 0
                     && PushChunk(iff, 0, MAKE_ID('C', 'H', 'R', 'S'), IFFSIZE_UNKNOWN) == 0
                     && WriteChunkBytes(iff, device_id, strlen(device_id)) == (LONG)strlen(device_id)
                     && PopChunk(iff) == 0 && PopChunk(iff) == 0;
                CloseIFF(iff);
            }
            CloseClipboard((struct ClipboardHandle *)iff->iff_Stream);
        }
        FreeIFF(iff);
    }
    say(ok ? "The Device ID is on the clipboard" : "The Device ID could not be copied");
}

/* ---- start with the system (the same way as Jumpstart: a project icon
 * in SYS:WBStartup whose default tool is this program) ---- */

static APTR quiet(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR was = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    return was;
}

static void loud(APTR was)
{
    ((struct Process *)FindTask(NULL))->pr_WindowPtr = was;
}

static int program_path(const struct WBStartup *wb, char *out, size_t cap)
{
    BPTR dir;
    const char *name;
    char progname[256];
    if (wb) {
        if (wb->sm_NumArgs < 1)
            return 0;
        dir = wb->sm_ArgList[0].wa_Lock;
        name = (const char *)wb->sm_ArgList[0].wa_Name;
    } else {
        if (!GetProgramName((STRPTR)progname, sizeof progname))
            return 0;
        dir = GetProgramDir();
        name = (const char *)FilePart((CONST_STRPTR)progname);
    }
    if (!dir || !name || !*name || !NameFromLock(dir, (STRPTR)out, (LONG)cap))
        return 0;
    return AddPart((STRPTR)out, (CONST_STRPTR)name, (ULONG)cap) != 0;
}

static int autostart_on(void)
{
    APTR was = quiet();
    BPTR lock = Lock((CONST_STRPTR)START_ICON ".info", SHARED_LOCK);
    loud(was);
    if (lock)
        UnLock(lock);
    return lock != 0;
}

static int autostart_set(int on)
{
    static STRPTR types[] = { (STRPTR)"DONOTWAIT", (STRPTR)"HIDE", NULL };
    struct DiskObject *icon;
    APTR was = quiet();
    int ok = 0;
    if (!IconBase) {
        ok = 0;
    } else if (!on) {
        ok = !autostart_on() || DeleteDiskObject((STRPTR)START_ICON);
    } else if (*progpath && (icon = GetDefDiskObject(WBPROJECT))) {
        STRPTR tool = icon->do_DefaultTool;
        STRPTR *tt = icon->do_ToolTypes;
        LONG x = icon->do_CurrentX, y = icon->do_CurrentY;
        icon->do_DefaultTool = (STRPTR)progpath;
        icon->do_ToolTypes = types;
        icon->do_CurrentX = icon->do_CurrentY = NO_ICON_POSITION;
        ok = PutDiskObject((CONST_STRPTR)START_ICON, icon) != 0;
        icon->do_DefaultTool = tool; /* the object is the library's: give it back as it was */
        icon->do_ToolTypes = tt;
        icon->do_CurrentX = x;
        icon->do_CurrentY = y;
        FreeDiskObject(icon);
    }
    loud(was);
    return ok;
}

/* 1 when an icon the program was started with carries the tool type HIDE
 * (the start icon in SYS:WBStartup does). */
static int wb_hide(const struct WBStartup *wb)
{
    int hide = 0;
    if (!IconBase)
        return 0;
    for (LONG i = 0; i < wb->sm_NumArgs && !hide; i++) {
        BPTR old = CurrentDir(wb->sm_ArgList[i].wa_Lock);
        struct DiskObject *d = GetDiskObject((CONST_STRPTR)wb->sm_ArgList[i].wa_Name);
        if (d) {
            hide = FindToolType((STRPTR *)d->do_ToolTypes, (STRPTR)"HIDE") != NULL;
            FreeDiskObject(d);
        }
        CurrentDir(old);
    }
    return hide;
}

/* ---- start ---- */

static int paths(void)
{
    if (!NameFromLock(GetProgramDir(), (STRPTR)progdir, sizeof progdir))
        return 0;
    snprintf(datadir, sizeof datadir, "%s", progdir);
    snprintf(stpoc, sizeof stpoc, "%s", progdir);
    if (!AddPart((STRPTR)datadir, (CONST_STRPTR)"Data", sizeof datadir)
        || !AddPart((STRPTR)stpoc, (CONST_STRPTR)ENGINE_NAME, sizeof stpoc))
        return 0;
    snprintf(confpath, sizeof confpath, "%s", datadir);
    snprintf(logpath, sizeof logpath, "%s", datadir);
    snprintf(idpath, sizeof idpath, "%s", datadir);
    if (!AddPart((STRPTR)confpath, (CONST_STRPTR)"engine.conf", sizeof confpath)
        || !AddPart((STRPTR)logpath, (CONST_STRPTR)"engine.log", sizeof logpath)
        || !AddPart((STRPTR)idpath, (CONST_STRPTR)"device-id.txt", sizeof idpath))
        return 0;
    BPTR l = Lock((CONST_STRPTR)datadir, ACCESS_READ);
    if (!l && !(l = CreateDir((CONST_STRPTR)datadir)))
        return 0;
    UnLock(l);
    return 1;
}

static Object *button(const char *label)
{
    return SimpleButton((IPTR)label);
}

/* Started with HIDE: the engine is started when there is something to
 * synchronise, and nothing else is done. */
static int start_hidden(void)
{
    char why[300];
    conf c;
    memset(&c, 0, sizeof c);
    if (!paths() || !have_file(progdir, ENGINE_NAME) || !have_file(datadir, "engine.conf"))
        return 5;
    if (conf_load(confpath, &c, why, sizeof why) != 0)
        return 5;
    int any = c.nfolders > 0;
    conf_free(&c);
    if (any && !engine_running() && !engine_start())
        return 10;
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 20;
    char why[300];
    struct WBStartup *wb = argc == 0 ? (struct WBStartup *)argv : NULL;
    int hide = 0;
    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 0);
    if (wb) {
        hide = wb_hide(wb);
    } else {
        IPTR a[1] = { 0 };
        struct RDArgs *rda = ReadArgs((CONST_STRPTR)"HIDE/S", (SIPTR *)a, NULL);
        if (rda) {
            hide = a[0] != 0;
            FreeArgs(rda);
        }
    }
    if (!program_path(wb, progpath, sizeof progpath))
        progpath[0] = 0;
    if (hide) {
        rc = start_hidden();
        if (IconBase)
            CloseLibrary(IconBase);
        return rc;
    }
    if (!(MUIMasterBase = OpenLibrary((CONST_STRPTR)"muimaster.library", 0))) {
        if (IconBase)
            CloseLibrary(IconBase);
        return 20;
    }
    IFFParseBase = OpenLibrary((CONST_STRPTR)"iffparse.library", 0);
    RexxSysBase = (struct RxsLib *)OpenLibrary((CONST_STRPTR)"rexxsyslib.library", 0);
    dev_hook.h_Entry = (HOOKFUNC)dev_display;
    fol_hook.h_Entry = (HOOKFUNC)fol_display;

    Object *badddev, *bremdev, *baddfol, *bremfol, *bpause, *bscan, *bcopy, *bdevok, *bdevcancel, *bfolok, *bfolcancel;
    Object *beditdev, *beditfol, *devlv, *follv;
    Object *strip, *mstop, *mhide;
    strip = MenustripObject,
        Child, MenuObject, MUIA_Menu_Title, (IPTR)"Project",
            Child, mhide = MenuitemObject, MUIA_Menuitem_Title, (IPTR)"Close window (keeps synchronising)",
                MUIA_Menuitem_Shortcut, (IPTR)"W", End,
            Child, mstop = MenuitemObject, MUIA_Menuitem_Title, (IPTR)"Stop synchronising and quit",
                MUIA_Menuitem_Shortcut, (IPTR)"Q", End,
        End,
    End;
    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"Synchr",
        MUIA_Application_Version, (IPTR)"$VER: Synchr " GUI_VERSION " (" GUI_DATE ")",
        MUIA_Application_Description, (IPTR)"Synchronises drawers with Syncthing devices",
        MUIA_Application_Base, (IPTR)"SYNCHRGUI",
        MUIA_Application_SingleTask, TRUE,
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (IPTR)"Synchr",
            MUIA_Window_Menustrip, (IPTR)strip,
            MUIA_Window_ID, MAKE_ID('S', 'T', 'M', 'N'),
            WindowContents, VGroup,
                Child, ColGroup(2), GroupFrameT("This device"),
                    Child, Label2("Name"),
                    Child, namestr = StringObject, StringFrame, MUIA_String_MaxLen, 60, MUIA_CycleChain, 1, End,
                    Child, Label2("Device ID"),
                    Child, HGroup,
                        Child, idtext = TextObject, TextFrame,
                            MUIA_Text_Contents, (IPTR)"MMMMMMM-MMMMMMM-MMMMMMM-MMMMMMM-MMMMMMM-MMMMMMM-MMMMMMM-MMMMMMM", End,
                        Child, bcopy = button("_Copy"),
                    End,
                    Child, HSpace(0),
                    Child, HGroup,
                        Child, autostart = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_Start with the system"),
                        Child, LLabel1("_Start with the system (synchronises without opening this window)"),
                        Child, HSpace(0),
                    End,
                    Child, HSpace(0),
                    Child, HGroup,
                        Child, public_servers = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_Use Syncthing's public servers"),
                        Child, LLabel1("_Use Syncthing's public servers (find devices anywhere, connect through a relay)"),
                        Child, HSpace(0),
                    End,
                End,
                Child, VGroup, GroupFrameT("Devices to synchronise with"),
                    Child, devlv = ListviewObject,
                        MUIA_Listview_List, devlist = ListObject, InputListFrame,
                            MUIA_List_Format, (IPTR)"BAR,BAR,",
                            MUIA_List_Title, TRUE,
                            MUIA_List_DisplayHook, (IPTR)&dev_hook,
                        End,
                        MUIA_CycleChain, 1,
                    End,
                    Child, HGroup,
                        Child, badddev = button("_Add device..."),
                        Child, beditdev = button("_Edit device..."),
                        Child, bremdev = button("_Remove device"),
                        Child, HSpace(0),
                    End,
                End,
                Child, VGroup, GroupFrameT("Folders"),
                    Child, follv = ListviewObject,
                        MUIA_Listview_List, follist = ListObject, InputListFrame,
                            MUIA_List_Format, (IPTR)"BAR,BAR,BAR,",
                            MUIA_List_Title, TRUE,
                            MUIA_List_DisplayHook, (IPTR)&fol_hook,
                        End,
                        MUIA_CycleChain, 1,
                    End,
                    Child, HGroup,
                        Child, baddfol = button("Add _folder..."),
                        Child, beditfol = button("Ch_ange folder..."),
                        Child, bremfol = button("Re_move folder"),
                        Child, bpause = button("_Pause / resume"),
                        Child, bscan = button("Read a_gain now"),
                        Child, HSpace(0),
                    End,
                End,
                Child, status = TextObject, TextFrame, MUIA_Text_Contents, (IPTR)"", MUIA_Text_SetMin, FALSE, End,
            End,
        End,
        SubWindow, devwin = WindowObject,
            MUIA_Window_Title, (IPTR)"Add a device",
            MUIA_Window_ID, MAKE_ID('S', 'T', 'D', 'V'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, Label2("Name"),
                    Child, dname = StringObject, StringFrame, MUIA_String_MaxLen, 60, MUIA_CycleChain, 1, End,
                    Child, Label2("Device ID"),
                    Child, did = StringObject, StringFrame, MUIA_String_MaxLen, 120, MUIA_CycleChain, 1, End,
                    Child, Label2("Address"),
                    Child, daddr = StringObject, StringFrame, MUIA_String_MaxLen, 20, MUIA_CycleChain, 1,
                        MUIA_String_Accept, (IPTR)"0123456789.", End,
                    Child, Label2("Port"),
                    Child, dport = StringObject, StringFrame, MUIA_String_MaxLen, 12, MUIA_CycleChain, 1,
                        MUIA_String_Accept, (IPTR)"0123456789", MUIA_String_Contents, (IPTR)"22000", End,
                End,
                Child, TextObject, MUIA_Text_Contents,
                    (IPTR)"The Device ID is shown on the other device under Actions, Show ID.\n"
                          "Leave the address empty to find the device on the local network.", End,
                Child, HGroup,
                    Child, bdevok = button("_OK"),
                    Child, HSpace(0),
                    Child, bdevcancel = button("_Cancel"),
                End,
            End,
        End,
        SubWindow, folwin = WindowObject,
            MUIA_Window_Title, (IPTR)"Add a folder",
            MUIA_Window_ID, MAKE_ID('S', 'T', 'F', 'L'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, Label2("Shared with"),
                    Child, fwith = TextObject, TextFrame, MUIA_Text_Contents, (IPTR)"", End,
                    Child, Label2("Folder ID"),
                    Child, fid = StringObject, StringFrame, MUIA_String_MaxLen, 64, MUIA_CycleChain, 1, End,
                    Child, Label2("Drawer"),
                    Child, PopaslObject,
                        MUIA_Popstring_String, fpath = StringObject, StringFrame, MUIA_String_MaxLen, 500,
                            MUIA_CycleChain, 1, End,
                        MUIA_Popstring_Button, PopButton(MUII_PopDrawer),
                        ASLFR_TitleText, (IPTR)"Drawer to synchronise",
                        ASLFR_DrawersOnly, TRUE,
                    End,
                End,
                Child, TextObject, MUIA_Text_Contents,
                    (IPTR)"The folder ID must be the same as on the other device\n"
                          "(there: the folder's Edit window, Folder ID).", End,
                Child, HGroup,
                    Child, bfolok = button("_OK"),
                    Child, HSpace(0),
                    Child, bfolcancel = button("_Cancel"),
                End,
            End,
        End,
    End;
    if (!app)
        goto out;

#define PRESS(b, id) DoMethod(b, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, id)
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);
    DoMethod(devwin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID, GID_DEVCANCEL);
    DoMethod(folwin, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID, GID_FOLCANCEL);
    DoMethod(namestr, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, GID_NAME);
    DoMethod(mhide, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);
    DoMethod(mstop, MUIM_Notify, MUIA_Menuitem_Trigger, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, GID_STOPQUIT);
    PRESS(bcopy, GID_COPY);
    PRESS(beditdev, GID_EDITDEV);
    PRESS(beditfol, GID_EDITFOL);
    DoMethod(devlv, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID, GID_EDITDEV);
    DoMethod(follv, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID, GID_EDITFOL);
    DoMethod(autostart, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, GID_AUTOSTART);
    DoMethod(public_servers, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)app, 2, MUIM_Application_ReturnID, GID_PUBLIC);
    /* Started again while running: this window comes to the front. */
    DoMethod(app, MUIM_Notify, MUIA_Application_DoubleStart, TRUE, (IPTR)win, 3, MUIM_Set, MUIA_Window_Open, TRUE);
    DoMethod(app, MUIM_Notify, MUIA_Application_DoubleStart, TRUE, (IPTR)win, 1, MUIM_Window_ToFront);
    PRESS(badddev, GID_ADDDEV);
    PRESS(bremdev, GID_REMDEV);
    PRESS(baddfol, GID_ADDFOL);
    PRESS(bremfol, GID_REMFOL);
    PRESS(bpause, GID_PAUSE);
    PRESS(bscan, GID_SCAN);
    PRESS(bdevok, GID_DEVOK);
    PRESS(bdevcancel, GID_DEVCANCEL);
    PRESS(bfolok, GID_FOLOK);
    PRESS(bfolcancel, GID_FOLCANCEL);

    set(win, MUIA_Window_Open, TRUE);

    int usable = 0;
    if (!RexxSysBase) {
        say("rexxsyslib.library cannot be opened: the program cannot talk to its engine");
    } else if (!paths()) {
        say("The drawer Data next to the program cannot be made");
    } else if (!have_file(progdir, ENGINE_NAME)) {
        say("The program " ENGINE_NAME " is missing next to this one (%s)", progdir);
    } else if (!identity()) {
        say("This device's identity could not be made; see identity.log in %s", datadir);
    } else {
        conf_allow_no_folder = 1;
        conf_init(&C); /* the defaults, for a first start without a file */
        if (have_file(datadir, "engine.conf") && conf_load(confpath, &C, why, sizeof why) != 0) {
            say("The settings file is not understood and was left alone: %s", why);
        } else {
            usable = 1;
            if (!C.name)
                C.name = dupstr("aros");
            set(namestr, MUIA_String_Contents, (IPTR)(C.name ? C.name : "aros"));
            set(idtext, MUIA_Text_Contents, (IPTR)device_id);
            nnset(autostart, MUIA_Selected, autostart_on());
            nnset(public_servers, MUIA_Selected, C.global_discovery && C.relays);
            fill_lists();
            say(C.ndevices ? "Synchronising continues when this window is closed" : "Start with Add device");
        }
    }
    if (!usable) {
        Object *off[] = { namestr, bcopy, badddev, beditdev, bremdev, baddfol, beditfol, bremfol, bpause, bscan, autostart, public_servers };
        for (size_t i = 0; i < sizeof off / sizeof *off; i++)
            set(off[i], MUIA_Disabled, TRUE);
    }

    struct MsgPort *tport = CreateMsgPort();
    struct timerequest *treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof *treq) : NULL;
    int timer_ok = treq && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) == 0;
    if (timer_ok) {
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 1;
        treq->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)treq);
    }

    ULONG sigs = 0;
    int stop_engine = 0;
    for (;;) {
        IPTR id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
        if (id == MUIV_Application_ReturnID_Quit)
            break;
        if (id == GID_STOPQUIT) {
            stop_engine = 1;
            break;
        }
        if (usable) {
            LONG a = -1;
            switch (id) {
            case GID_COPY: copy_id(); break;
            case GID_NAME: change_name(); break;
            case GID_ADDDEV: open_device(-1); break;
            case GID_EDITDEV:
                if ((a = selected(devlist, C.ndevices)) < 0)
                    say("Select a device first");
                else
                    open_device(a);
                break;
            case GID_EDITFOL:
                if ((a = selected(follist, C.nfolders)) < 0)
                    say("Select a folder first");
                else
                    open_folder(a);
                break;
            case GID_AUTOSTART: {
                IPTR on = 0;
                get(autostart, MUIA_Selected, &on);
                if (autostart_set(on != 0))
                    say(on ? "Synchronising starts with the system" : "Synchronising no longer starts with the system");
                else {
                    say("The start icon %s could not be %s", START_ICON, on ? "written" : "removed");
                    nnset(autostart, MUIA_Selected, autostart_on());
                }
                break;
            }
            case GID_PUBLIC: {
                IPTR on = 0;
                get(public_servers, MUIA_Selected, &on);
                C.global_discovery = C.relays = on != 0;
                apply();
                say(on ? "Devices are looked for through Syncthing's public servers, and relays are used"
                       : "Only the local network and typed addresses are used");
                break;
            }
            case GID_DEVOK: add_device(); break;
            case GID_DEVCANCEL: edit_dev = -1; set(devwin, MUIA_Window_Open, FALSE); break;
            case GID_REMDEV: remove_device(); break;
            case GID_ADDFOL: open_folder(-1); break;
            case GID_FOLOK: add_folder(); break;
            case GID_FOLCANCEL: edit_fol = -1; set(folwin, MUIA_Window_Open, FALSE); break;
            case GID_REMFOL: remove_folder(); break;
            case GID_PAUSE: folder_command(1); break;
            case GID_SCAN: folder_command(0); break;
            }
        }
        if (sigs) {
            ULONG tsig = timer_ok ? 1UL << tport->mp_SigBit : 0;
            ULONG got = Wait(sigs | tsig | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C)
                break;
            if (timer_ok && (got & tsig)) {
                WaitIO((struct IORequest *)treq);
                if (usable)
                    refresh();
                treq->tr_node.io_Command = TR_ADDREQUEST;
                treq->tr_time.tv_secs = TICK_SECONDS;
                treq->tr_time.tv_micro = 0;
                SendIO((struct IORequest *)treq);
            }
        }
    }
    set(win, MUIA_Window_Open, FALSE);
    if (usable && stop_engine)
        engine_stop();
    if (timer_ok) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq)
        DeleteIORequest((struct IORequest *)treq);
    if (tport)
        DeleteMsgPort(tport);
    rc = 0;
    MUI_DisposeObject(app);
    conf_free(&C);
out:
    if (RexxSysBase)
        CloseLibrary((struct Library *)RexxSysBase);
    if (IFFParseBase)
        CloseLibrary(IFFParseBase);
    if (IconBase)
        CloseLibrary(IconBase);
    CloseLibrary(MUIMasterBase);
    return rc;
}
