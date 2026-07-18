/*
*   n3ds-mcp fork addition: an always-on, toggleable file service.
*
*   Replaces the dependency on ftpd, which is a homebrew APP and therefore does
*   not survive a reboot -- so every firmware iteration needed a human to
*   relaunch it. This lives in Rosalina, starts itself when development mode is
*   on, and comes back after a remote reboot like InputRedirection does.
*
*   It speaks a small FTP subset on purpose. The client is Python's ftplib, so
*   ftp_get/ftp_list/fetch_crash_dumps keep working unchanged -- and a human can
*   point FileZilla at the console when the agent path is dark, which is exactly
*   the hole that cost us a physical reboot once already.
*
*   Writes are confined to /luma/staging/. /boot.firm is NOT writable over FTP;
*   it can only be replaced through the nonce-authenticated commit channel,
*   which verifies before, during and after the copy. A bad boot.firm means
*   recovering with a card reader, so the file channel is never allowed near it.
*/

#pragma once

#include <3ds/types.h>
#include "MyThread.h"

// Ports. Deliberately NOT 4950 (HID input) or 4951 (command channel): a fault
// here must never be able to take down input injection or the text readout.
#define FS_CTRL_PORT    4952
#define FS_DATA_PORT    4953
#define FS_COMMIT_PORT  4954

#define FS_STAGING_DIR   "/luma/staging/"
#define FS_STAGING_FIRM  "/luma/staging/boot.firm.new"
#define FS_BOOT_FIRM     "/boot.firm"
#define FS_LOG_PATH      "/luma/n3ds-mcp-fs.txt"

#define FS_XFER_BUF_SIZE    (16 * 1024)
#define FS_MAX_WRITE_BYTES  (2 * 1024 * 1024)
#define FS_LINE_MAX         512
#define FS_POLL_MS          50
#define FS_BLOCKING_POLL_MS 2000
#define FS_XFER_TIMEOUT_MS  60000

// Commit packet: 'FCMT', nonce, length, crc32, flags, reserved.
#define FS_COMMIT_MAGIC     "FCMT"
#define FS_COMMIT_REPLY     "FCMR"
#define FS_COMMIT_LEN       24

MyThread *fileServiceCreateThread(void);
void      fileServiceThreadMain(void);

Result FileService_TryStart(void);
Result FileService_Disable(s64 timeout);
bool   FileService_IsRunning(void);
bool   FileService_CommitInFlight(void);

// The console-generated commit nonce. Regenerated at boot and consumed by
// every commit attempt, successful or not, so a captured packet cannot be
// replayed. Published in the log file the host already reads.
u32    FileService_GetNonce(void);
