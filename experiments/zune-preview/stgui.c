/* Zune preview of a front end for stpoc (not a product: one window, one
 * folder, no settings). It shows what a session reports while it runs.
 *
 *   stgui STPOC IDDIR IP PORT PEER FOLDER DEST [TIMEOUT=seconds] [RECEIVEONLY]
 *
 * By default a session runs `stpoc twoway` (two-way, first version:
 * deletions and conflicts are held and reported); RECEIVEONLY runs the
 * receive-only `stpoc sync` instead.
 *
 * "Synchronise now" starts a helper process that runs STPOC with
 * --status T:stgui-<id>.txt and waits for it; a timer (every half second)
 * appends the new lines of that file to the list. The session view ends
 * when the helper reports that STPOC has exited, whether or not a
 * "Result:" line was written (a crash or a failed status write is shown
 * with STPOC's return code). <id> is this task's address, so two
 * instances do not share files. Arguments are quoted for the shell; a
 * double quote or a line break in an argument is refused. stpoc closes the
 * status file after each line, so it is never held open while read. */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <libraries/mui.h>
#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

#include <stdio.h>
#include <string.h>

#define DEFAULT_TIMEOUT 3600

struct Library *MUIMasterBase;

enum { ID_SYNC = 1 };

static Object *app, *win, *list, *status, *bsync;
static LONG status_pos;
static int running, seen_result;
static char pending[1024];
static char status_path[64], log_path[64], cmd[2400];

/* Set by the helper process before it signals the main task. */
static struct Task *main_task;
static BYTE done_sig = -1;
static volatile LONG child_rc;

static void helper(void)
{
    BPTR in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE), out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    LONG rc = -1;
    if (in && out)
        rc = SystemTags((CONST_STRPTR)cmd, SYS_Input, (IPTR)in, SYS_Output, (IPTR)out, NP_StackSize, 262144,
                        TAG_DONE);
    if (in)
        Close(in);
    if (out)
        Close(out);
    child_rc = rc;
    Forbid(); /* the main task may not run before this process has gone */
    Signal(main_task, 1UL << done_sig);
}

/* Appends ARG to the command, in double quotes when it holds a space or a
 * character the shell or ReadArgs would interpret; 0 if it cannot be
 * quoted (a double quote, '*' which escapes inside quotes, a line break). */
static int add_arg(const char *arg)
{
    if (strpbrk(arg, "\"\n\r*"))
        return 0;
    int quote = !*arg || strpbrk(arg, " \t;<>|=`") != NULL;
    size_t n = strlen(cmd);
    int w = snprintf(cmd + n, sizeof cmd - n, quote ? "%s\"%s\"" : "%s%s", n ? " " : "", arg);
    return w > 0 && (size_t)w < sizeof cmd - n;
}

/* Reads what was appended to the status file since the last call. */
static void poll_status(void)
{
    BPTR fh = Open((CONST_STRPTR)status_path, MODE_OLDFILE);
    if (!fh)
        return;
    char buf[512];
    Seek(fh, status_pos, OFFSET_BEGINNING);
    LONG got;
    while ((got = Read(fh, buf, sizeof buf - 1)) > 0) {
        status_pos += got;
        buf[got] = 0;
        char *p = buf;
        for (char *nl; (nl = strchr(p, '\n')); p = nl + 1) {
            *nl = 0;
            strncat(pending, p, sizeof pending - strlen(pending) - 1);
            DoMethod(list, MUIM_List_InsertSingle, (IPTR)pending, MUIV_List_Insert_Bottom);
            set(list, MUIA_List_Active, MUIV_List_Active_Bottom);
            set(status, MUIA_Text_Contents, (IPTR)pending);
            if (strncmp(pending, "Result:", 7) == 0)
                seen_result = 1;
            pending[0] = 0;
        }
        strncat(pending, p, sizeof pending - strlen(pending) - 1);
    }
    Close(fh);
}

int main(void)
{
    static const char tmpl[] = "STPOC/A,IDDIR/A,IP/A,PORT/A,PEER/A,FOLDER/A,DEST/A,TIMEOUT/K/N,RECEIVEONLY/S";
    IPTR a[9] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)tmpl, (SIPTR *)a, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)"stgui");
        return 20;
    }
    if (!(MUIMasterBase = OpenLibrary((CONST_STRPTR)"muimaster.library", 0))) {
        FreeArgs(rda);
        return 20;
    }
    main_task = FindTask(NULL);
    snprintf(status_path, sizeof status_path, "T:stgui-%lx.txt", (unsigned long)(IPTR)main_task);
    snprintf(log_path, sizeof log_path, "T:stgui-%lx.log", (unsigned long)(IPTR)main_task);
    char limit[16];
    snprintf(limit, sizeof limit, "%ld", a[7] ? (long)*(LONG *)a[7] : (long)DEFAULT_TIMEOUT);
    cmd[0] = 0;
    int args_ok = add_arg((char *)a[0]);
    args_ok = args_ok && add_arg("--status") && add_arg(status_path) && add_arg(log_path) && add_arg(a[8] ? "sync" : "twoway");
    for (int i = 1; i <= 6; i++)
        args_ok = args_ok && add_arg((char *)a[i]);
    args_ok = args_ok && add_arg(limit);
    done_sig = AllocSignal(-1);
    char peer[200], folder[200], dest[300];
    snprintf(peer, sizeof peer, "\33b%.7s\33n-...  at %s:%s", (char *)a[4], (char *)a[2], (char *)a[3]);
    snprintf(folder, sizeof folder, "\33b%s\33n", (char *)a[5]);
    snprintf(dest, sizeof dest, "%s", (char *)a[6]);

    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"Syncthing for AROS",
        MUIA_Application_Version, (IPTR)"$VER: stgui 0.0.2 (10.11.2026) preview",
        MUIA_Application_Description, (IPTR)"Preview of a front end for stpoc",
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (IPTR)"Syncthing for AROS - preview",
            MUIA_Window_ID, MAKE_ID('S', 'T', 'G', 'P'),
            WindowContents, VGroup,
                Child, ColGroup(2), GroupFrameT("Folder"),
                    Child, Label2("Folder"), Child, TextObject, TextFrame, MUIA_Text_Contents, (IPTR)folder, End,
                    Child, Label2("Peer"), Child, TextObject, TextFrame, MUIA_Text_Contents, (IPTR)peer, End,
                    Child, Label2("Into"), Child, TextObject, TextFrame, MUIA_Text_Contents, (IPTR)dest, End,
                End,
                Child, VGroup, GroupFrameT("Session"),
                    Child, ListviewObject,
                        MUIA_Listview_Input, FALSE,
                        MUIA_Listview_List, list = ListObject,
                            ReadListFrame,
                            MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                            MUIA_List_DestructHook, MUIV_List_DestructHook_String,
                        End,
                    End,
                    Child, status = TextObject, TextFrame,
                        MUIA_Text_Contents, (IPTR)"Not synchronised in this session yet",
                    End,
                End,
                Child, HGroup,
                    Child, bsync = SimpleButton("_Synchronise now"),
                    Child, HSpace(0),
                End,
            End,
        End,
    End;
    if (!app) {
        CloseLibrary(MUIMasterBase);
        FreeArgs(rda);
        return 20;
    }
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);
    DoMethod(bsync, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_SYNC);
    set(win, MUIA_Window_Open, TRUE);

    struct MsgPort *tport = CreateMsgPort();
    struct timerequest *treq = tport ? (struct timerequest *)CreateIORequest(tport, sizeof *treq) : NULL;
    int timer_ok = treq && OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)treq, 0) == 0;
    if (timer_ok) {
        treq->tr_node.io_Command = TR_ADDREQUEST;
        treq->tr_time.tv_secs = 0;
        treq->tr_time.tv_micro = 500000;
        SendIO((struct IORequest *)treq);
    }

    if (!args_ok || done_sig < 0) {
        set(status, MUIA_Text_Contents,
            (IPTR)(args_ok ? "No free signal" : "An argument contains a quote, * or a line break; not started"));
        set(bsync, MUIA_Disabled, TRUE);
    }

    ULONG sigs = 0;
    int quit = 0;
    for (;;) {
        IPTR id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
        if (id == MUIV_Application_ReturnID_Quit)
            quit = 1;
        if (quit && !running)
            break;
        if (quit)
            set(status, MUIA_Text_Contents, (IPTR)"Quitting when the session has ended...");
        if (id == ID_SYNC && !running && !quit) {
            DoMethod(list, MUIM_List_Clear);
            DeleteFile((CONST_STRPTR)status_path);
            status_pos = 0;
            pending[0] = 0;
            seen_result = 0;
            SetSignal(0, 1UL << done_sig);
            if (!CreateNewProcTags(NP_Entry, (IPTR)helper, NP_Name, (IPTR)"stgui session", NP_StackSize, 65536,
                                   TAG_DONE)) {
                set(status, MUIA_Text_Contents, (IPTR)"Could not start the session");
            } else {
                running = 1;
                set(bsync, MUIA_Disabled, TRUE);
                set(status, MUIA_Text_Contents, (IPTR)"Connecting...");
            }
        }
        if (sigs || (quit && running)) {
            ULONG tsig = timer_ok ? 1UL << tport->mp_SigBit : 0;
            ULONG dsig = done_sig >= 0 ? 1UL << done_sig : 0;
            ULONG got = Wait(sigs | tsig | dsig | SIGBREAKF_CTRL_C);
            if ((got & SIGBREAKF_CTRL_C) && !running)
                break;
            if (got & dsig) {
                poll_status();
                running = 0;
                set(bsync, MUIA_Disabled, FALSE);
                if (!seen_result) {
                    static char msg[200];
                    snprintf(msg, sizeof msg, "stpoc ended (return code %ld) without a result; log: %s",
                             (long)child_rc, log_path);
                    DoMethod(list, MUIM_List_InsertSingle, (IPTR)msg, MUIV_List_Insert_Bottom);
                    set(status, MUIA_Text_Contents, (IPTR)msg);
                }
                if (quit)
                    break;
            }
            if (timer_ok && (got & tsig)) {
                WaitIO((struct IORequest *)treq);
                if (running)
                    poll_status();
                treq->tr_node.io_Command = TR_ADDREQUEST;
                treq->tr_time.tv_secs = 0;
                treq->tr_time.tv_micro = 500000;
                SendIO((struct IORequest *)treq);
            }
        }
    }
    if (timer_ok) {
        AbortIO((struct IORequest *)treq);
        WaitIO((struct IORequest *)treq);
        CloseDevice((struct IORequest *)treq);
    }
    if (treq)
        DeleteIORequest((struct IORequest *)treq);
    if (done_sig >= 0)
        FreeSignal(done_sig);
    if (tport)
        DeleteMsgPort(tport);
    MUI_DisposeObject(app);
    CloseLibrary(MUIMasterBase);
    FreeArgs(rda);
    return 0;
}
