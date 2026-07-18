/*
*   n3ds-mcp fork addition: always-on file service. See include/fileservice.h.
*/

#include <3ds.h>
#include <arpa/inet.h>
#include <string.h>
#include "fileservice.h"
#include "menu.h"
#include "devmode.h"
#include "minisoc.h"
#include "sock_util.h"
#include "ifile.h"
#include "fmt.h"
#include "memory.h"

// NOTE: sock_util's server framework is deliberately NOT used here.
//
// It accepts on every bound listener itself, and ftplib opens the data
// connection BEFORE sending RETR/STOR -- so the data socket is consumed by an
// earlier poll iteration and a blocking accept in a data callback waits
// forever. Its accept callback also gets no port argument, so a control
// greeting would land in the data stream, and closing a data socket inline
// leaks the framework's client count permanently (after which every later
// transfer is accepted-then-closed, which ftplib reports as a SUCCESSFUL
// zero-byte download). There is no timer callback either, so idle reclamation
// is not expressible.
//
// This is one control client and one data socket, ever. A hand-rolled poll
// loop -- structurally the same as inputRedirectionThreadMain -- avoids all of
// that and gives every blocking call a bounded, cancellable wait.

static MyThread fileServiceThread;
static u8 CTR_ALIGN(8) fileServiceThreadStack[0x4000];

static bool  g_running = false;
static bool  g_shouldStop = false;
static bool  g_commitInFlight = false;
static bool  g_threadLive = false;   // a thread object exists (running or exiting)
static u32   g_nonce = 0;

// .bss, never the 0x4000 thread stack.
static u8   g_xferBuf[FS_XFER_BUF_SIZE];
static char g_line[FS_LINE_MAX];
static u32  g_lineLen = 0;

MyThread *fileServiceCreateThread(void)
{
    if(R_FAILED(MyThread_Create(&fileServiceThread, fileServiceThreadMain,
                                fileServiceThreadStack, 0x4000, 0x20, CORE_SYSTEM)))
        svcBreak(USERBREAK_PANIC);
    return &fileServiceThread;
}

bool FileService_IsRunning(void)      { return g_running; }
bool FileService_CommitInFlight(void) { return g_commitInFlight; }
u32  FileService_GetNonce(void)       { return g_nonce; }

// ---------------------------------------------------------------------------
// Logging -- the host reads this to learn the nonce and commit outcomes.
// ---------------------------------------------------------------------------

static void FS_Log(const char *fmt, ...)
{
    IFile file;
    char text[256];
    u64 written = 0, offset = 0;
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsprintf(text, fmt, args);
    va_end(args);
    if(len <= 0)
        return;

    if(R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                              fsMakePath(PATH_ASCII, FS_LOG_PATH),
                              FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        if(R_SUCCEEDED(IFile_GetSize(&file, &offset)))
            file.pos = offset;
        IFile_Write(&file, &written, text, (u32)len, 0);
        IFile_Close(&file);
    }
}

static void FS_NewNonce(void)
{
    // Not cryptographic, and not claimed to be: this stops replay and stops
    // forgery from a constant in a public repo. Anyone who can read the SD
    // card can read the nonce -- documented, not papered over.
    u64 t = svcGetSystemTick();
    g_nonce = (u32)(t ^ (t >> 32)) | 1u;
    FS_Log("nonce %08lX\n", (unsigned long)g_nonce);
}

// ---------------------------------------------------------------------------
// Bounded socket helpers. Every blocking operation is a poll with a deadline
// and a cancellation check, never a bare blocking call.
// ---------------------------------------------------------------------------

static bool FS_ShouldStop(void)
{
    return g_shouldStop || preTerminationRequested || !DevMode_IsEnabled();
}

static int FS_WaitReadable(int fd, u32 timeoutMs)
{
    u32 waited = 0;
    while(waited < timeoutMs)
    {
        struct pollfd pfd;
        int res;
        if(FS_ShouldStop())
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        res = socPoll(&pfd, 1, FS_POLL_MS);
        if(res > 0 && (pfd.revents & POLLIN))
            return 1;
        if(res < 0)
            return -1;
        waited += FS_POLL_MS;
    }
    return 0;
}

static bool FS_SendAll(int fd, const void *buf, u32 len)
{
    const u8 *p = (const u8 *)buf;
    u32 sent = 0;
    while(sent < len)
    {
        int n;
        if(FS_ShouldStop())
            return false;
        n = socSendto(fd, p + sent, len - sent, 0, NULL, 0);
        if(n <= 0)
            return false;
        sent += (u32)n;
    }
    return true;
}

static bool FS_Reply(int fd, const char *line)
{
    return FS_SendAll(fd, line, strlen(line));
}

// ---------------------------------------------------------------------------
// Path policy
// ---------------------------------------------------------------------------

static bool FS_PathIsSane(const char *path)
{
    const char *p;
    if(path == NULL || path[0] != '/')
        return false;
    if(strlen(path) >= 250)
        return false;
    // Reject any ".." segment outright rather than trying to normalise.
    for(p = path; *p != '\0'; p++)
    {
        if(p[0] == '.' && p[1] == '.' && (p == path || p[-1] == '/') &&
           (p[2] == '/' || p[2] == '\0'))
            return false;
    }
    return true;
}

static bool FS_WriteAllowed(const char *path)
{
    // Writes live in staging only. boot.firm is reachable solely through the
    // commit channel, which verifies the payload; letting a plain STOR near it
    // would make a truncated upload a brick.
    return FS_PathIsSane(path) &&
           strncmp(path, FS_STAGING_DIR, strlen(FS_STAGING_DIR)) == 0;
}

// ---------------------------------------------------------------------------
// FTP session state
// ---------------------------------------------------------------------------

typedef struct {
    int ctrl;
    int dataListen;
    int pendingData;   // accepted early: ftplib connects before it sends the verb
} FsSession;

static int FS_TakeDataSocket(FsSession *s)
{
    int fd;
    if(s->pendingData >= 0)
    {
        fd = s->pendingData;
        s->pendingData = -1;
        return fd;
    }
    if(FS_WaitReadable(s->dataListen, FS_BLOCKING_POLL_MS) != 1)
        return -1;
    fd = socAccept(s->dataListen, NULL, NULL);
    return fd;
}

// Fail a transfer without stranding the client's data connection. ftplib has
// already connected by the time the verb arrives; leaving that socket
// unconsumed desyncs the fixed data port for every later transfer.
static void FS_RejectTransfer(FsSession *s, const char *reply)
{
    int fd;
    FS_Reply(s->ctrl, reply);
    fd = FS_TakeDataSocket(s);
    if(fd >= 0)
        socClose(fd);
}

static void FS_DoRetr(FsSession *s, const char *path)
{
    IFile file;
    u64 size = 0, offset = 0;
    int data;

    // Resolve and open BEFORE the 150. A 150 followed by a failure hands the
    // client a silent empty file.
    if(!FS_PathIsSane(path) ||
       R_FAILED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                           fsMakePath(PATH_ASCII, path), FS_OPEN_READ)))
    {
        FS_RejectTransfer(s, "550 Not found\r\n");
        return;
    }
    IFile_GetSize(&file, &size);

    if(!FS_Reply(s->ctrl, "150 Opening data connection\r\n"))
    {
        IFile_Close(&file);
        return;
    }

    data = FS_TakeDataSocket(s);
    if(data < 0)
    {
        IFile_Close(&file);
        FS_Reply(s->ctrl, "425 No data connection\r\n");
        return;
    }

    while(offset < size)
    {
        u64 got = 0;
        u32 want = (u32)((size - offset) > FS_XFER_BUF_SIZE
                         ? FS_XFER_BUF_SIZE : (size - offset));
        file.pos = offset;
        if(R_FAILED(IFile_Read(&file, &got, g_xferBuf, want)) || got == 0)
            break;
        if(!FS_SendAll(data, g_xferBuf, (u32)got))
            break;
        offset += got;
    }

    socClose(data);
    IFile_Close(&file);
    FS_Reply(s->ctrl, offset == size ? "226 Transfer complete\r\n"
                                     : "426 Transfer aborted\r\n");
}

static void FS_DoStor(FsSession *s, const char *path)
{
    IFile file;
    int data;
    u64 total = 0;
    bool ok = true;

    if(!FS_WriteAllowed(path))
    {
        FS_RejectTransfer(s, "550 Writes are only permitted under /luma/staging/\r\n");
        return;
    }
    if(R_FAILED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                           fsMakePath(PATH_ASCII, path),
                           FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        FS_RejectTransfer(s, "550 Cannot create\r\n");
        return;
    }
    // CREATE|WRITE does not truncate; an existing longer file would keep its
    // tail. Every Rosalina call site that wants a fresh file does this.
    IFile_SetSize(&file, 0);

    if(!FS_Reply(s->ctrl, "150 Ready\r\n"))
    {
        IFile_Close(&file);
        return;
    }

    data = FS_TakeDataSocket(s);
    if(data < 0)
    {
        IFile_Close(&file);
        FS_Reply(s->ctrl, "425 No data connection\r\n");
        return;
    }

    for(;;)
    {
        int n;
        u64 written = 0;
        if(FS_WaitReadable(data, FS_BLOCKING_POLL_MS) != 1)
        {
            ok = false;
            break;
        }
        n = socRecvfrom(data, g_xferBuf, FS_XFER_BUF_SIZE, 0, NULL, 0);
        if(n == 0)
            break;               // clean EOF: the client closed
        if(n < 0)
        {
            ok = false;
            break;
        }
        if(total + (u64)n > FS_MAX_WRITE_BYTES)
        {
            ok = false;
            break;
        }
        file.pos = total;
        // IFile_Write can return SUCCESS with a short *total, so the byte
        // count must be checked too or we silently truncate.
        if(R_FAILED(IFile_Write(&file, &written, g_xferBuf, (u32)n, FS_WRITE_FLUSH))
           || written != (u64)n)
        {
            ok = false;
            break;
        }
        total += (u64)n;
    }

    socClose(data);
    IFile_Close(&file);
    FS_Reply(s->ctrl, ok ? "226 Transfer complete\r\n" : "552 Write failed\r\n");
}

static void FS_DoList(FsSession *s, const char *path, bool mlsd)
{
    Handle dir;
    FS_Archive archive;
    FS_DirectoryEntry entries[8];
    int data;
    char line[300];
    const char *dirPath = (path != NULL && path[0] != '\0') ? path : "/";

    if(!FS_PathIsSane(dirPath) ||
       R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
    {
        FS_RejectTransfer(s, "550 Not a directory\r\n");
        return;
    }
    if(R_FAILED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, dirPath))))
    {
        FSUSER_CloseArchive(archive);
        FS_RejectTransfer(s, "550 Not a directory\r\n");
        return;
    }

    if(!FS_Reply(s->ctrl, "150 Here it comes\r\n"))
    {
        FSDIR_Close(dir);
        FSUSER_CloseArchive(archive);
        return;
    }

    data = FS_TakeDataSocket(s);
    if(data < 0)
    {
        FSDIR_Close(dir);
        FSUSER_CloseArchive(archive);
        FS_Reply(s->ctrl, "425 No data connection\r\n");
        return;
    }

    for(;;)
    {
        u32 nRead = 0, i;
        if(R_FAILED(FSDIR_Read(dir, &nRead, 8, entries)) || nRead == 0)
            break;
        for(i = 0; i < nRead; i++)
        {
            char name[192];
            u32 j;
            bool printable = true;

            // FAT names are UTF-16. ftplib decodes the data channel as UTF-8
            // and an undecodable byte raises inside the client, so anything
            // outside ASCII is skipped rather than emitted as mojibake.
            for(j = 0; j < sizeof(name) - 1 && entries[i].name[j] != 0; j++)
            {
                u16 c = entries[i].name[j];
                if(c < 0x20 || c > 0x7E)
                {
                    printable = false;
                    break;
                }
                name[j] = (char)c;
            }
            if(!printable || j == 0)
                continue;
            name[j] = '\0';

            if(mlsd)
                sprintf(line, "type=%s; %s\r\n",
                        (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) ? "dir" : "file",
                        name);
            else
                sprintf(line, "%s\r\n", name);

            if(!FS_SendAll(data, line, strlen(line)))
                goto done;
        }
    }

done:
    socClose(data);
    FSDIR_Close(dir);
    FSUSER_CloseArchive(archive);
    FS_Reply(s->ctrl, "226 Transfer complete\r\n");
}

static void FS_DoDele(FsSession *s, const char *path)
{
    FS_Archive archive;
    if(!FS_WriteAllowed(path))
    {
        FS_Reply(s->ctrl, "550 Deletes are only permitted under /luma/staging/\r\n");
        return;
    }
    if(R_FAILED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
    {
        FS_Reply(s->ctrl, "550 Cannot open archive\r\n");
        return;
    }
    if(R_SUCCEEDED(FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, path))))
        FS_Reply(s->ctrl, "250 Deleted\r\n");
    else
        FS_Reply(s->ctrl, "550 Not found\r\n");
    FSUSER_CloseArchive(archive);
}

// Returns false when the session should end.
static bool FS_Dispatch(FsSession *s, char *line)
{
    char *arg;
    char verb[8];
    u32 i;

    for(i = 0; i < sizeof(verb) - 1 && line[i] != ' ' && line[i] != '\0'; i++)
        verb[i] = (line[i] >= 'a' && line[i] <= 'z') ? (char)(line[i] - 32) : line[i];
    verb[i] = '\0';

    arg = strchr(line, ' ');
    if(arg != NULL)
    {
        arg++;
        while(*arg == ' ')
            arg++;
    }
    else
        arg = (char *)"";

    if(strcmp(verb, "USER") == 0 || strcmp(verb, "PASS") == 0)
        return FS_Reply(s->ctrl, "230 Logged in\r\n");
    if(strcmp(verb, "TYPE") == 0)
    {
        // BOTH I and A must be 2xx. retrlines sends TYPE A, and ftplib raises
        // on any 5xx -- which would break directory listing and its NLST
        // fallback together.
        return FS_Reply(s->ctrl, "200 Type set\r\n");
    }
    if(strcmp(verb, "PASV") == 0)
    {
        char reply[64];
        sprintf(reply, "227 Entering Passive Mode (0,0,0,0,%u,%u)\r\n",
                (unsigned)(FS_DATA_PORT >> 8), (unsigned)(FS_DATA_PORT & 0xFF));
        // ftplib discards the advertised address and reuses the control host,
        // so the zeros are fine and avoid guessing our own IP.
        return FS_Reply(s->ctrl, reply);
    }
    if(strcmp(verb, "SYST") == 0)
        return FS_Reply(s->ctrl, "215 UNIX Type: L8\r\n");
    if(strcmp(verb, "PWD") == 0)
        return FS_Reply(s->ctrl, "257 \"/\"\r\n");
    if(strcmp(verb, "CWD") == 0)
        return FS_Reply(s->ctrl, "250 OK\r\n");
    if(strcmp(verb, "NOOP") == 0)
        return FS_Reply(s->ctrl, "200 OK\r\n");
    if(strcmp(verb, "SIZE") == 0)
    {
        IFile file;
        u64 size = 0;
        char reply[64];
        if(!FS_PathIsSane(arg) ||
           R_FAILED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                               fsMakePath(PATH_ASCII, arg), FS_OPEN_READ)))
            return FS_Reply(s->ctrl, "550 Not found\r\n");
        IFile_GetSize(&file, &size);
        IFile_Close(&file);
        sprintf(reply, "213 %lu\r\n", (unsigned long)size);
        return FS_Reply(s->ctrl, reply);
    }
    if(strcmp(verb, "RETR") == 0) { FS_DoRetr(s, arg); return true; }
    if(strcmp(verb, "STOR") == 0) { FS_DoStor(s, arg); return true; }
    if(strcmp(verb, "MLSD") == 0) { FS_DoList(s, arg, true);  return true; }
    if(strcmp(verb, "NLST") == 0 || strcmp(verb, "LIST") == 0)
    {
        FS_DoList(s, arg, false);
        return true;
    }
    if(strcmp(verb, "DELE") == 0) { FS_DoDele(s, arg); return true; }
    if(strcmp(verb, "QUIT") == 0)
    {
        FS_Reply(s->ctrl, "221 Bye\r\n");
        return false;
    }
    return FS_Reply(s->ctrl, "502 Not implemented\r\n");
}

static void FS_RunSession(FsSession *s)
{
    u32 idleMs = 0;

    if(!FS_Reply(s->ctrl, "220 n3ds-mcp file service\r\n"))
        return;
    g_lineLen = 0;

    while(!FS_ShouldStop())
    {
        struct pollfd pfd[2];
        int res;

        pfd[0].fd = s->ctrl;       pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = s->dataListen; pfd[1].events = POLLIN; pfd[1].revents = 0;

        res = socPoll(pfd, 2, FS_POLL_MS);
        if(res < 0)
            return;
        if(res == 0)
        {
            idleMs += FS_POLL_MS;
            if(idleMs > FS_XFER_TIMEOUT_MS)
                return;            // reclaim an abandoned session
            continue;
        }
        idleMs = 0;

        // Accept the data connection eagerly: ftplib opens it BEFORE sending
        // the transfer verb, so it is already waiting here.
        if((pfd[1].revents & POLLIN) && s->pendingData < 0)
            s->pendingData = socAccept(s->dataListen, NULL, NULL);

        if(pfd[0].revents & POLLIN)
        {
            int n = socRecvfrom(s->ctrl, g_line + g_lineLen,
                                FS_LINE_MAX - 1 - g_lineLen, 0, NULL, 0);
            if(n <= 0)
                return;
            g_lineLen += (u32)n;
            g_line[g_lineLen] = '\0';

            // Frame on CRLF: a verb can be split across TCP segments, and a
            // recv boundary is not a message boundary.
            for(;;)
            {
                char *eol = strstr(g_line, "\r\n");
                u32 rest;
                if(eol == NULL)
                {
                    if(g_lineLen >= FS_LINE_MAX - 1)
                    {
                        FS_Reply(s->ctrl, "500 Line too long\r\n");
                        g_lineLen = 0;
                    }
                    break;
                }
                *eol = '\0';
                if(!FS_Dispatch(s, g_line))
                    return;
                rest = g_lineLen - (u32)(eol + 2 - g_line);
                memmove(g_line, eol + 2, rest);
                g_lineLen = rest;
                g_line[g_lineLen] = '\0';
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Commit channel -- the only path that may touch /boot.firm.
// ---------------------------------------------------------------------------

static u32 FS_Crc32(u32 crc, const u8 *buf, u32 len)
{
    u32 i, j;
    crc = ~crc;
    for(i = 0; i < len; i++)
    {
        crc ^= buf[i];
        for(j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(s32)(crc & 1)));
    }
    return ~crc;
}

static bool FS_CommitFirmware(u32 expectLen, u32 expectCrc, const char **why)
{
    IFile src, dst;
    u64 srcSize = 0, offset = 0;
    u32 crc = 0;
    bool ok = true;

    if(preTerminationRequested)
    {
        *why = "system is shutting down";
        return false;
    }

    // ARCHIVE_SDMC hardcoded, never the config archive helper: on a
    // NAND-installed Luma that resolves to nand:/rw, so the write would land
    // somewhere harmless-looking, report success, and change nothing.
    if(R_FAILED(IFile_Open(&src, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                           fsMakePath(PATH_ASCII, FS_STAGING_FIRM), FS_OPEN_READ)))
    {
        *why = "no staged firmware";
        return false;
    }
    IFile_GetSize(&src, &srcSize);

    if(srcSize != (u64)expectLen)
    {
        IFile_Close(&src);
        *why = "staged length does not match the commit request";
        return false;
    }

    {   // A CRC match cannot catch "right size, wrong file entirely".
        u64 got = 0;
        src.pos = 0;
        if(R_FAILED(IFile_Read(&src, &got, g_xferBuf, 4)) || got != 4 ||
           memcmp(g_xferBuf, "FIRM", 4) != 0)
        {
            IFile_Close(&src);
            *why = "staged file is not a FIRM";
            return false;
        }
    }

    if(R_FAILED(IFile_Open(&dst, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                           fsMakePath(PATH_ASCII, FS_BOOT_FIRM),
                           FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        IFile_Close(&src);
        *why = "cannot open /boot.firm for writing";
        return false;
    }
    IFile_SetSize(&dst, srcSize);

    // Single pass: CRC the bytes AS WRITTEN. Verifying staging and then
    // re-reading it to copy leaves a window where a concurrent STOR could
    // change what actually lands.
    while(offset < srcSize)
    {
        u64 got = 0, written = 0;
        u32 want = (u32)((srcSize - offset) > FS_XFER_BUF_SIZE
                         ? FS_XFER_BUF_SIZE : (srcSize - offset));
        src.pos = offset;
        if(R_FAILED(IFile_Read(&src, &got, g_xferBuf, want)) || got != (u64)want)
        {
            ok = false;
            *why = "read from staging failed";
            break;
        }
        dst.pos = offset;
        if(R_FAILED(IFile_Write(&dst, &written, g_xferBuf, want, FS_WRITE_FLUSH))
           || written != (u64)want)
        {
            ok = false;
            *why = "short write to /boot.firm";
            break;
        }
        crc = FS_Crc32(crc, g_xferBuf, want);
        offset += want;
    }

    IFile_Close(&src);
    IFile_Close(&dst);        // close is what commits the directory entry

    if(!ok)
        return false;
    if(crc != expectCrc)
    {
        *why = "crc mismatch while writing";
        return false;
    }

    {   // Reopen fresh and re-read: the durability check.
        u64 checkSize = 0, off = 0;
        u32 rcrc = 0;
        if(R_FAILED(IFile_Open(&dst, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                               fsMakePath(PATH_ASCII, FS_BOOT_FIRM), FS_OPEN_READ)))
        {
            *why = "cannot reopen /boot.firm to verify";
            return false;
        }
        IFile_GetSize(&dst, &checkSize);
        if(checkSize != srcSize)
        {
            IFile_Close(&dst);
            *why = "written size does not match";
            return false;
        }
        while(off < checkSize)
        {
            u64 got = 0;
            u32 want = (u32)((checkSize - off) > FS_XFER_BUF_SIZE
                             ? FS_XFER_BUF_SIZE : (checkSize - off));
            dst.pos = off;
            if(R_FAILED(IFile_Read(&dst, &got, g_xferBuf, want)) || got != (u64)want)
            {
                IFile_Close(&dst);
                *why = "readback failed";
                return false;
            }
            rcrc = FS_Crc32(rcrc, g_xferBuf, want);
            off += want;
        }
        IFile_Close(&dst);
        if(rcrc != expectCrc)
        {
            *why = "readback crc mismatch -- /boot.firm may be damaged";
            return false;
        }
    }

    return true;
}

static void FS_HandleCommit(int sock)
{
    u8 pkt[64];
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    u32 nonce, length, crc;
    const char *why = "unknown";
    bool ok;
    u8 reply[12];
    int n = socRecvfrom(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&src, &srclen);

    if(n != FS_COMMIT_LEN || memcmp(pkt, FS_COMMIT_MAGIC, 4) != 0)
        return;

    memcpy(&nonce,  pkt + 4,  4);
    memcpy(&length, pkt + 8,  4);
    memcpy(&crc,    pkt + 12, 4);

    if(nonce != g_nonce)
    {
        FS_Log("commit refused: bad nonce\n");
        FS_NewNonce();
        ok = false;
    }
    else if(length == 0 || length > FS_MAX_WRITE_BYTES)
    {
        FS_Log("commit refused: implausible length %lu\n", (unsigned long)length);
        FS_NewNonce();
        ok = false;
    }
    else
    {
        g_commitInFlight = true;
        ok = FS_CommitFirmware(length, crc, &why);
        g_commitInFlight = false;
        FS_Log(ok ? "commit ok: %lu bytes\n" : "commit FAILED: %lu bytes\n",
               (unsigned long)length);
        if(!ok)
            FS_Log("  reason: %s\n", why);
        // Consume the nonce either way, so a captured packet is never replayable.
        FS_NewNonce();
    }

    memcpy(reply, FS_COMMIT_REPLY, 4);
    reply[4] = ok ? 1 : 0;
    reply[5] = reply[6] = reply[7] = 0;
    memcpy(reply + 8, &g_nonce, 4);
    if(srclen >= (socklen_t)sizeof(struct sockaddr_in))
        socSendto(sock, reply, sizeof(reply), 0, (struct sockaddr *)&src, srclen);
}

// ---------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------

void fileServiceThreadMain(void)
{
    int ctrlListen = -1, dataListen = -1, commitSock = -1;
    struct sockaddr_in addr;

    if(R_FAILED(miniSocInit()))
        return;

    ctrlListen = socSocket(AF_INET, SOCK_STREAM, 0);
    dataListen = socSocket(AF_INET, SOCK_STREAM, 0);
    commitSock = socSocket(AF_INET, SOCK_DGRAM, 0);
    if(ctrlListen < 0 || dataListen < 0 || commitSock < 0)
        goto cleanup;

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = socGethostid();

    addr.sin_port = htons(FS_CTRL_PORT);
    if(socBind(ctrlListen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
       socListen(ctrlListen, 2) != 0)
        goto cleanup;

    addr.sin_port = htons(FS_DATA_PORT);
    if(socBind(dataListen, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
       socListen(dataListen, 2) != 0)
        goto cleanup;

    addr.sin_port = htons(FS_COMMIT_PORT);
    if(socBind(commitSock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto cleanup;

    FS_NewNonce();
    FS_Log("file service listening: ftp %u, commit %u\n",
           (unsigned)FS_CTRL_PORT, (unsigned)FS_COMMIT_PORT);
    g_running = true;

    while(!FS_ShouldStop())
    {
        struct pollfd pfd[2];
        int res;

        pfd[0].fd = ctrlListen; pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = commitSock; pfd[1].events = POLLIN; pfd[1].revents = 0;

        res = socPoll(pfd, 2, FS_POLL_MS);
        if(res < 0)
            break;
        if(res == 0)
            continue;

        if(pfd[1].revents & POLLIN)
            FS_HandleCommit(commitSock);

        if(pfd[0].revents & POLLIN)
        {
            FsSession s;
            s.ctrl = socAccept(ctrlListen, NULL, NULL);
            if(s.ctrl < 0)
                continue;
            s.dataListen = dataListen;
            s.pendingData = -1;
            FS_RunSession(&s);
            if(s.pendingData >= 0)
                socClose(s.pendingData);
            socClose(s.ctrl);
        }
    }

cleanup:
    g_running = false;
    g_threadLive = false;
    if(ctrlListen >= 0) socClose(ctrlListen);
    if(dataListen >= 0) socClose(dataListen);
    if(commitSock >= 0) socClose(commitSock);
    miniSocExit();
}

Result FileService_TryStart(void)
{
    // Bounded, and only ever one live thread. The menu tick calls this every
    // 50ms while the service is not running, and MyThread_Create panics on
    // failure -- so an unguarded retry would spawn threads until the console
    // dies. A bind failure (WiFi not up yet) must cost a few retries, not the
    // system.
    static u32 attempts = 0;
    static u64 lastAttemptTick = 0;
    u64 now = svcGetSystemTick();

    if(g_running || g_threadLive)
        return 0;
    if(attempts >= 10)
        return -1;
    // Space attempts ~2s apart. The caller ticks every 50ms, so unspaced
    // retries are all consumed before the network can possibly be ready.
    if(lastAttemptTick != 0 && (now - lastAttemptTick) < (SYSCLOCK_ARM11 * 2))
        return -1;

    lastAttemptTick = now;
    attempts++;
    g_shouldStop = false;
    g_threadLive = true;
    fileServiceCreateThread();
    return 0;
}

Result FileService_Disable(s64 timeout)
{
    if(!g_running)
        return 0;
    // A commit is mid-copy of boot.firm; interrupting it is the brick case.
    // Wait specifically on that before signalling shutdown.
    {
        s64 waited = 0;
        while(g_commitInFlight && waited < 3000000000LL)
        {
            svcSleepThread(50 * 1000 * 1000LL);
            waited += 50 * 1000 * 1000LL;
        }
    }
    g_shouldStop = true;
    return MyThread_Join(&fileServiceThread, timeout);
}
