/*
*   n3ds-mcp fork addition: development mode. See include/devmode.h.
*/

#include <3ds.h>
#include "devmode.h"
#include "fileservice.h"
#include "input_redirection.h"
#include "draw.h"
#include "menu.h"
#include "menus.h"
#include "ifile.h"
#include "fmt.h"
#include "utils.h"

static FS_ArchiveID DevMode_GetArchiveId(void)
{
    s64 out = 0;
    svcGetSystemInfo(&out, 0x10000, 0x203); // isSdMode
    return (bool)out ? ARCHIVE_SDMC : ARCHIVE_NAND_RW;
}

static s8 g_cached = -1;   // -1 unknown, 0 off, 1 on

bool DevMode_IsEnabled(void)
{
    IFile file;

    // Cached: this is polled from the 50ms menu tick and from the file
    // service's stop check, and an SD open per call is real I/O for a value
    // that only changes when someone uses the menu.
    if(g_cached >= 0)
        return g_cached == 1;
    if(R_FAILED(IFile_Open(&file, DevMode_GetArchiveId(), fsMakePath(PATH_EMPTY, ""),
                           fsMakePath(PATH_ASCII, DEVMODE_FLAG_PATH), FS_OPEN_READ)))
    {
        g_cached = 0;
        return false;
    }
    IFile_Close(&file);
    g_cached = 1;
    return true;
}

Result DevMode_SetEnabled(bool enable)
{
    Result res;
    g_cached = enable ? 1 : 0;
    if(enable)
    {
        IFile file;
        res = IFile_Open(&file, DevMode_GetArchiveId(), fsMakePath(PATH_EMPTY, ""),
                         fsMakePath(PATH_ASCII, DEVMODE_FLAG_PATH),
                         FS_OPEN_CREATE | FS_OPEN_WRITE);
        if(R_SUCCEEDED(res))
            IFile_Close(&file);
    }
    else
    {
        FS_Archive archive;
        res = FSUSER_OpenArchive(&archive, DevMode_GetArchiveId(),
                                 fsMakePath(PATH_EMPTY, ""));
        if(R_SUCCEEDED(res))
        {
            res = FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, DEVMODE_FLAG_PATH));
            FSUSER_CloseArchive(archive);
        }
    }
    return res;
}

void RosalinaMenu_DevMode(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        bool enabled = DevMode_IsEnabled();

        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "n3ds-mcp development mode");

        Draw_DrawString(10, 30, COLOR_WHITE, "Development mode:");
        Draw_DrawString(150, 30, enabled ? COLOR_GREEN : COLOR_RED,
                        enabled ? "ENABLED " : "DISABLED");

        Draw_DrawString(10, 50, COLOR_WHITE,
            enabled
                ? "Remote control is armed at every boot:\n"
                  "  input redirection, text readout, file\n"
                  "  service (FTP 4952), remote reboot.\n"
                  "  The console will NOT sleep."
                : "The console behaves like stock Luma3DS.\n"
                  "  No background services, no remote\n"
                  "  control, normal sleep and battery.\n"
                  "  This persists across reboots.");

        Draw_DrawString(10, 120, COLOR_WHITE, "Right now:");
        Draw_DrawString(10, 132, COLOR_WHITE, "  Input redirection:");
        Draw_DrawString(160, 132, inputRedirectionEnabled ? COLOR_GREEN : COLOR_RED,
                        inputRedirectionEnabled ? "running" : "stopped");
        Draw_DrawString(10, 144, COLOR_WHITE, "  File service:");
        Draw_DrawString(160, 144, FileService_IsRunning() ? COLOR_GREEN : COLOR_RED,
                        FileService_IsRunning() ? "running" : "stopped");

        if(FileService_IsRunning())
            Draw_DrawFormattedString(10, 164, COLOR_WHITE,
                                     "  Commit nonce: %08lX",
                                     (unsigned long)FileService_GetNonce());

        Draw_DrawString(10, 196, COLOR_WHITE, "Press A to toggle, B to go back.");
        Draw_FlushFramebuffer();
        Draw_Unlock();

        u32 pressed = waitInputWithTimeout(1000);

        if(pressed & KEY_A)
        {
            bool now = !enabled;
            DevMode_SetEnabled(now);

            // Apply immediately as well as persistently, so the menu does not
            // lie about the current state until the next reboot.
            if(now)
            {
                InputRedirection_TryStart();
                FileService_TryStart();
            }
            else
            {
                FileService_Disable(1000000000LL);
                InputRedirection_Disable(1000000000LL);
            }

            Draw_Lock();
            Draw_ClearFramebuffer();
            Draw_FlushFramebuffer();
            Draw_Unlock();
        }
        else if(pressed & KEY_B)
            return;
    }
    while(!menuShouldExit);
}
