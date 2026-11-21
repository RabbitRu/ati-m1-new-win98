/*
 * w98agent - remote control agent for the Windows 98 test laptops.
 *
 * Listens on TCP 9898, one command per connection (see docs/STAND.md).
 * Broadcasts a UDP beacon so the host can find it without configuration.
 * "w98agent /install" registers autostart and the boot-time rollback hook.
 *
 * There is no authentication: use only on the isolated test network.
 *
 * Build: Open Watcom, see makefile.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <conio.h>

#define AGENT_PORT    9898
#define AGENT_VERSION 1
#define IO_TIMEOUT_S  30
#define CHUNK         16384

static char g_dir[MAX_PATH];        /* agent directory, short form, no trailing slash */
static char g_name[64];             /* computer name */
static volatile LONG g_deadman;     /* seconds left, 0 = disarmed */
static char g_buf[CHUNK];

/* ---------- small helpers ---------- */

static void agent_path(char *out, const char *leaf)
{
    wsprintf(out, "%s\\%s", g_dir, leaf);
}

static BOOL exists(const char *leaf)
{
    char p[MAX_PATH];
    agent_path(p, leaf);
    return GetFileAttributes(p) != 0xFFFFFFFFUL;
}

static void logf(const char *fmt, ...)
{
    char p[MAX_PATH], line[600];
    va_list ap;
    FILE *f;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 1, fmt, ap);
    line[sizeof line - 1] = 0;
    va_end(ap);
    agent_path(p, "AGENT.LOG");
    f = fopen(p, "a");
    if (f) {
        fprintf(f, "%lu %s\n", (unsigned long)GetTickCount(), line);
        fclose(f);
    }
}

static BOOL is_win9x(void)
{
    return (GetVersion() & 0x80000000UL) != 0;
}

/* ---------- reboot ---------- */

static void hard_reset(void)
{
    if (!is_win9x())
        return;                 /* port I/O faults outside Win9x */
    outp(0xCF9, 0x02);          /* chipset reset control */
    outp(0xCF9, 0x06);
    Sleep(200);
    outp(0x64, 0xFE);           /* keyboard controller reset line */
}

static DWORD WINAPI exitwin_thread(LPVOID p)
{
    (void)p;
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
    return 0;
}

/* Soft reboot; if the system is still alive after hard_after seconds, hard reset. */
static void do_reboot(int hard_after)
{
    DWORD tid;
    CloseHandle(CreateThread(NULL, 0, exitwin_thread, NULL, 0, &tid));
    if (hard_after > 0) {
        Sleep((DWORD)hard_after * 1000);
        logf("soft reboot did not finish, hard reset");
        hard_reset();
    }
}

/* ---------- beacon and dead-man timer ---------- */

static DWORD WINAPI ticker(LPVOID p)
{
    SOCKET u = INVALID_SOCKET;
    struct sockaddr_in to;
    char msg[128];
    BOOL on = TRUE;
    int n = 0;
    (void)p;

    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(AGENT_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    wsprintf(msg, "W98AGENT %d %s", AGENT_VERSION, g_name);

    for (;;) {
        Sleep(1000);
        if (g_deadman > 0 && InterlockedDecrement((LONG *)&g_deadman) == 0) {
            logf("dead-man timer expired, rebooting");
            do_reboot(30);
        }
        if (++n % 3)
            continue;
        if (u == INVALID_SOCKET) {
            u = socket(AF_INET, SOCK_DGRAM, 0);
            if (u != INVALID_SOCKET)
                setsockopt(u, SOL_SOCKET, SO_BROADCAST, (char *)&on, sizeof on);
        }
        if (u != INVALID_SOCKET &&
            sendto(u, msg, lstrlen(msg), 0, (struct sockaddr *)&to, sizeof to) < 0) {
            closesocket(u);     /* network not ready yet, retry later */
            u = INVALID_SOCKET;
        }
    }
}

/* ---------- socket I/O ---------- */

static BOOL wait_readable(SOCKET s)
{
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec = IO_TIMEOUT_S;
    tv.tv_usec = 0;
    return select(0, &fds, NULL, NULL, &tv) > 0;
}

static BOOL recv_line(SOCKET s, char *buf, int max)
{
    int n = 0;
    char c;
    while (n < max - 1) {
        if (!wait_readable(s) || recv(s, &c, 1, 0) != 1)
            return FALSE;
        if (c == '\n') {
            if (n && buf[n - 1] == '\r')
                n--;
            buf[n] = 0;
            return TRUE;
        }
        buf[n++] = c;
    }
    return FALSE;
}

static BOOL send_all(SOCKET s, const char *p, int len)
{
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0)
            return FALSE;
        p += n;
        len -= n;
    }
    return TRUE;
}

static void reply(SOCKET s, const char *fmt, ...)
{
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 2, fmt, ap);
    line[sizeof line - 2] = 0;
    va_end(ap);
    lstrcat(line, "\n");
    send_all(s, line, lstrlen(line));
}

static void reply_winerr(SOCKET s, const char *what)
{
    reply(s, "ERR %s failed, error %lu", what, (unsigned long)GetLastError());
}

/* Send "OK <prefix><size>" followed by the file contents. */
static void send_file(SOCKET s, const char *path, const char *prefix)
{
    HANDLE h;
    DWORD size, got;
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                   OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        reply_winerr(s, "open");
        return;
    }
    size = GetFileSize(h, NULL);
    reply(s, "OK %s%lu", prefix, (unsigned long)size);
    while (ReadFile(h, g_buf, CHUNK, &got, NULL) && got)
        if (!send_all(s, g_buf, (int)got))
            break;
    CloseHandle(h);
}

/* Create every missing directory on the way to a file path. */
static void make_parent_dirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    lstrcpyn(tmp, path, sizeof tmp);
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = 0;
            CreateDirectory(tmp, NULL);
            *p = '\\';
        }
    }
}

/* ---------- commands ---------- */

static void cmd_ping(SOCKET s)
{
    reply(s, "OK w98agent %d name=%s win9x=%d pending=%d trial=%d rolled=%d "
             "snapshot=%d deadman=%ld dir=%s",
          AGENT_VERSION, g_name, is_win9x() ? 1 : 0,
          exists("PENDING.FLG"), exists("TRIAL.FLG"), exists("ROLLED.FLG"),
          exists("GOOD\\SYSTEM.DAT"), (long)g_deadman, g_dir);
}

static void cmd_put(SOCKET s, char *arg)
{
    unsigned long size, left;
    char *path;
    HANDLE h;
    DWORD put;
    BOOL ok = TRUE;

    size = strtoul(arg, &path, 10);
    while (*path == ' ')
        path++;
    if (!*path) {
        reply(s, "ERR usage: PUT <size> <path>");
        return;
    }
    make_parent_dirs(path);
    h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        reply_winerr(s, "create");
        return;
    }
    for (left = size; left && ok; ) {
        int want = left > CHUNK ? CHUNK : (int)left;
        int n = wait_readable(s) ? recv(s, g_buf, want, 0) : 0;
        if (n <= 0)
            ok = FALSE;
        else if (!WriteFile(h, g_buf, (DWORD)n, &put, NULL) || put != (DWORD)n)
            ok = FALSE;
        else
            left -= (unsigned long)n;
    }
    CloseHandle(h);
    if (ok) {
        reply(s, "OK %lu", size);
    } else {
        DeleteFile(path);
        reply(s, "ERR transfer or write failed");
    }
}

static void cmd_dir(SOCKET s, const char *pattern)
{
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        reply(s, "OK 0");
        return;
    }
    reply(s, "OK 1");
    do {
        reply(s, "%c %lu %s", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : 'f',
              (unsigned long)fd.nFileSizeLow, fd.cFileName);
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

/* Run a program with output captured to a file. timeout_ms 0 = do not wait. */
static void cmd_exec(SOCKET s, char *arg, BOOL visible)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    HANDLE hout, hin;
    char outpath[MAX_PATH], prefix[64];
    unsigned long timeout;
    char *cmdline;
    DWORD code = 0;

    timeout = strtoul(arg, &cmdline, 10);
    while (*cmdline == ' ')
        cmdline++;
    if (!*cmdline) {
        reply(s, "ERR usage: EXEC <timeout_ms> <command line>");
        return;
    }

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    agent_path(outpath, "EXEC.OUT");
    hout = CreateFile(outpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    hin = CreateFile("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                     OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = (WORD)(visible ? SW_SHOWNORMAL : SW_HIDE);
    si.hStdInput = hin;
    si.hStdOutput = hout;
    si.hStdError = hout;

    if (!CreateProcess(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        reply_winerr(s, "CreateProcess");
        CloseHandle(hout);
        CloseHandle(hin);
        return;
    }
    CloseHandle(hout);
    CloseHandle(hin);
    CloseHandle(pi.hThread);

    if (timeout == 0) {
        CloseHandle(pi.hProcess);
        reply(s, "OK started %lu", (unsigned long)pi.dwProcessId);
        return;
    }
    if (WaitForSingleObject(pi.hProcess, timeout) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        lstrcpy(prefix, "timeout ");
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
        wsprintf(prefix, "exit=%lu ", (unsigned long)code);
    }
    CloseHandle(pi.hProcess);
    send_file(s, outpath, prefix);
}

/* Capture the screen as a 24-bit BMP and send it. */
static void cmd_shot(SOCKET s)
{
    HDC scr, mem;
    HBITMAP bmp, old;
    BOOL ok;
    BITMAPFILEHEADER fh;
    BITMAPINFO bi;
    int w, h;
    DWORD stride, total, y;
    char *row;

    scr = GetDC(NULL);
    if (!scr) {
        reply(s, "ERR no screen DC");
        return;
    }
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    mem = CreateCompatibleDC(scr);
    bmp = CreateCompatibleBitmap(scr, w, h);
    stride = ((DWORD)w * 3 + 3) & ~3UL;
    row = (char *)malloc(stride);
    if (!mem || !bmp || !row) {
        reply(s, "ERR cannot allocate capture bitmap");
        goto done;
    }
    old = (HBITMAP)SelectObject(mem, bmp);
    ok = BitBlt(mem, 0, 0, w, h, scr, 0, 0, SRCCOPY);
    SelectObject(mem, old);     /* GetDIBits needs the bitmap deselected */
    if (!ok) {
        reply_winerr(s, "BitBlt");
        goto done;
    }

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    total = stride * (DWORD)h;
    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + total;

    reply(s, "OK %lu", (unsigned long)fh.bfSize);
    send_all(s, (char *)&fh, sizeof fh);
    send_all(s, (char *)&bi.bmiHeader, sizeof bi.bmiHeader);
    for (y = 0; y < (DWORD)h; y++) {
        memset(row, 0, stride);
        GetDIBits(scr, bmp, y, 1, row, &bi, DIB_RGB_COLORS);
        if (!send_all(s, row, (int)stride))
            break;
    }
done:
    free(row);
    if (bmp)
        DeleteObject(bmp);
    if (mem)
        DeleteDC(mem);
    ReleaseDC(NULL, scr);
}

static void handle(SOCKET s)
{
    char line[1024];
    char *arg;

    if (!recv_line(s, line, sizeof line))
        return;
    arg = strchr(line, ' ');
    if (arg)
        *arg++ = 0;
    else
        arg = line + lstrlen(line);

    if (lstrcmp(line, "PING") && lstrcmp(line, "PUT"))
        logf("%s %s", line, arg);

    if (!lstrcmp(line, "PING")) {
        cmd_ping(s);
    } else if (!lstrcmp(line, "PUT")) {
        cmd_put(s, arg);
    } else if (!lstrcmp(line, "GET")) {
        send_file(s, arg, "");
    } else if (!lstrcmp(line, "DEL")) {
        if (DeleteFile(arg) || GetLastError() == ERROR_FILE_NOT_FOUND)
            reply(s, "OK");
        else
            reply_winerr(s, "delete");
    } else if (!lstrcmp(line, "DIR")) {
        cmd_dir(s, arg);
    } else if (!lstrcmp(line, "EXEC")) {
        cmd_exec(s, arg, FALSE);
    } else if (!lstrcmp(line, "SPAWN")) {
        cmd_exec(s, arg, TRUE);
    } else if (!lstrcmp(line, "SHOT")) {
        cmd_shot(s);
    } else if (!lstrcmp(line, "ARM")) {
        long secs = atol(arg);
        if (secs < 5) {
            reply(s, "ERR usage: ARM <seconds, at least 5>");
        } else {
            InterlockedExchange((LONG *)&g_deadman, secs);
            reply(s, "OK %ld", secs);
        }
    } else if (!lstrcmp(line, "DISARM")) {
        InterlockedExchange((LONG *)&g_deadman, 0);
        reply(s, "OK");
    } else if (!lstrcmp(line, "REBOOT")) {
        int hard_after = atoi(arg);
        reply(s, "OK");
        closesocket(s);
        do_reboot(hard_after);
    } else if (!lstrcmp(line, "RESET")) {
        reply(s, "OK");
        closesocket(s);
        Sleep(300);
        hard_reset();
    } else {
        reply(s, "ERR unknown command %s", line);
    }
}

/* ---------- install ---------- */

static const char boot_bat_body[] =
    "if exist %W98A%\\SNAP.FLG goto snap\r\n"
    "if exist %W98A%\\PENDING.FLG goto roll\r\n"
    "if exist %W98A%\\TRIAL.FLG goto arm\r\n"
    "goto end\r\n"
    "\r\n"
    ":arm\r\n"
    "rem first boot of a trial driver: the next unconfirmed boot rolls back\r\n"
    "ren %W98A%\\TRIAL.FLG PENDING.FLG\r\n"
    "goto end\r\n"
    "\r\n"
    ":snap\r\n"
    "echo w98agent: saving known-good configuration\r\n"
    "if not exist %W98A%\\GOOD\\NUL md %W98A%\\GOOD\r\n"
    "%winbootdir%\\command\\attrib -r -h -s %winbootdir%\\system.dat\r\n"
    "%winbootdir%\\command\\attrib -r -h -s %winbootdir%\\user.dat\r\n"
    "copy /y %winbootdir%\\system.dat %W98A%\\GOOD > nul\r\n"
    "copy /y %winbootdir%\\user.dat %W98A%\\GOOD > nul\r\n"
    "copy /y %winbootdir%\\system.ini %W98A%\\GOOD > nul\r\n"
    "copy /y %winbootdir%\\win.ini %W98A%\\GOOD > nul\r\n"
    "%winbootdir%\\command\\attrib +r +h +s %winbootdir%\\system.dat\r\n"
    "%winbootdir%\\command\\attrib +r +h +s %winbootdir%\\user.dat\r\n"
    "del %W98A%\\SNAP.FLG\r\n"
    "goto end\r\n"
    "\r\n"
    ":roll\r\n"
    "del %W98A%\\PENDING.FLG\r\n"
    "if not exist %W98A%\\GOOD\\SYSTEM.DAT goto end\r\n"
    "echo w98agent: trial driver was not confirmed, restoring known-good configuration\r\n"
    "%winbootdir%\\command\\attrib -r -h -s %winbootdir%\\system.dat\r\n"
    "%winbootdir%\\command\\attrib -r -h -s %winbootdir%\\user.dat\r\n"
    "copy /y %W98A%\\GOOD\\system.dat %winbootdir% > nul\r\n"
    "copy /y %W98A%\\GOOD\\user.dat %winbootdir% > nul\r\n"
    "copy /y %W98A%\\GOOD\\system.ini %winbootdir% > nul\r\n"
    "copy /y %W98A%\\GOOD\\win.ini %winbootdir% > nul\r\n"
    "%winbootdir%\\command\\attrib +r +h +s %winbootdir%\\system.dat\r\n"
    "%winbootdir%\\command\\attrib +r +h +s %winbootdir%\\user.dat\r\n"
    "if exist %winbootdir%\\wnbootng.sts del %winbootdir%\\wnbootng.sts\r\n"
    "echo rolled back > %W98A%\\ROLLED.FLG\r\n"
    "\r\n"
    ":end\r\n"
    "set W98A=\r\n";

static BOOL write_boot_bat(void)
{
    char p[MAX_PATH];
    FILE *f;
    agent_path(p, "W98BOOT.BAT");
    f = fopen(p, "wb");
    if (!f)
        return FALSE;
    fprintf(f, "@echo off\r\nrem generated by w98agent /install, do not edit\r\n"
               "set W98A=%s\r\n", g_dir);
    fputs(boot_bat_body, f);
    fclose(f);
    return TRUE;
}

/* Put "call <dir>\W98BOOT.BAT" at the top of AUTOEXEC.BAT. */
static BOOL hook_autoexec(const char *autoexec)
{
    FILE *f;
    char *old = NULL;
    long len = 0;
    char bak[MAX_PATH];

    f = fopen(autoexec, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = (char *)malloc((size_t)len + 1);
        if (!old) {
            fclose(f);
            return FALSE;
        }
        len = (long)fread(old, 1, (size_t)len, f);
        old[len] = 0;
        fclose(f);
        if (strstr(old, "W98BOOT.BAT")) {
            free(old);
            return TRUE;        /* already hooked */
        }
        agent_path(bak, "AUTOEXEC.OLD");
        CopyFile(autoexec, bak, FALSE);
    }
    SetFileAttributes(autoexec, FILE_ATTRIBUTE_NORMAL);
    f = fopen(autoexec, "wb");
    if (!f) {
        free(old);
        return FALSE;
    }
    fprintf(f, "call %s\\W98BOOT.BAT\r\n", g_dir);
    if (old)
        fwrite(old, 1, (size_t)len, f);
    fclose(f);
    free(old);
    return TRUE;
}

/* Make a failed boot go straight to a normal boot, so the rollback hook runs. */
static void tune_msdos_sys(const char *path)
{
    DWORD attrs = GetFileAttributes(path);
    if (attrs == 0xFFFFFFFFUL)
        return;
    SetFileAttributes(path, FILE_ATTRIBUTE_NORMAL);
    WritePrivateProfileString("Options", "BootMenu", "0", path);
    WritePrivateProfileString("Options", "BootWarn", "0", path);
    WritePrivateProfileString("Options", "AutoScan", "0", path);
    WritePrivateProfileString(NULL, NULL, NULL, path);
    SetFileAttributes(path, attrs);
}

static int do_install(BOOL quiet)
{
    char exe[MAX_PATH], msg[400];
    HKEY key;
    BOOL ok = TRUE;

    GetModuleFileName(NULL, exe, sizeof exe);
    if (RegCreateKey(HKEY_LOCAL_MACHINE,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\RunServices",
                     &key) == ERROR_SUCCESS) {
        RegSetValueEx(key, "W98Agent", 0, REG_SZ, (BYTE *)exe, lstrlen(exe) + 1);
        RegCloseKey(key);
    } else {
        ok = FALSE;
    }
    if (!write_boot_bat())
        ok = FALSE;
    if (!hook_autoexec("C:\\AUTOEXEC.BAT"))
        ok = FALSE;
    tune_msdos_sys("C:\\MSDOS.SYS");

    wsprintf(msg, ok ? "w98agent installed in %s.\nReboot to start it."
                     : "w98agent install in %s FAILED.", g_dir);
    logf("%s", msg);
    if (!quiet)
        MessageBox(NULL, msg, "w98agent", ok ? MB_OK : MB_OK | MB_ICONERROR);
    return ok ? 0 : 1;
}

/* ---------- main ---------- */

static void keep_running_after_logoff(void)
{
    typedef DWORD (WINAPI *RSP)(DWORD, DWORD);
    RSP rsp = (RSP)GetProcAddress(GetModuleHandle("KERNEL32"), "RegisterServiceProcess");
    if (rsp)
        rsp(0, 1);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    char exe[MAX_PATH];
    char *slash;
    DWORD n, tid;
    WSADATA wsa;
    SOCKET srv = INVALID_SOCKET;
    (void)inst; (void)prev; (void)show;

    GetModuleFileName(NULL, exe, sizeof exe);
    if (!GetShortPathName(exe, g_dir, sizeof g_dir))
        lstrcpy(g_dir, exe);
    slash = strrchr(g_dir, '\\');
    if (slash)
        *slash = 0;
    n = sizeof g_name;
    if (!GetComputerName(g_name, &n))
        lstrcpy(g_name, "unknown");

    if (strstr(cmdline, "/install"))
        return do_install(strstr(cmdline, "/quiet") != NULL);

    CreateMutex(NULL, TRUE, "W98AgentRunning");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    keep_running_after_logoff();
    if (WSAStartup(0x0101, &wsa))
        return 1;
    logf("start version %d pending=%d rolled=%d", AGENT_VERSION,
         exists("PENDING.FLG"), exists("ROLLED.FLG"));
    CloseHandle(CreateThread(NULL, 0, ticker, NULL, 0, &tid));

    for (;;) {
        struct sockaddr_in a;
        SOCKET c;
        if (srv == INVALID_SOCKET) {
            BOOL on = TRUE;
            srv = socket(AF_INET, SOCK_STREAM, 0);
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_port = htons(AGENT_PORT);
            a.sin_addr.s_addr = INADDR_ANY;
            if (srv != INVALID_SOCKET)
                setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on);
            if (srv == INVALID_SOCKET || bind(srv, (struct sockaddr *)&a, sizeof a) ||
                listen(srv, 4)) {
                if (srv != INVALID_SOCKET)
                    closesocket(srv);
                srv = INVALID_SOCKET;
                Sleep(5000);    /* network stack not ready yet */
                continue;
            }
        }
        c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) {
            closesocket(srv);
            srv = INVALID_SOCKET;
            Sleep(1000);
            continue;
        }
        handle(c);
        closesocket(c);
    }
}
