/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2020 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#pragma once

#include <3ds/types.h>
#include "MyThread.h"

extern bool inputRedirectionEnabled;
extern Handle inputRedirectionThreadStartedEvent;

extern int inputRedirectionStartResult;

MyThread *inputRedirectionCreateThread(void);
void inputRedirectionThreadMain(void);
Result InputRedirection_Disable(s64 timeout);
Result InputRedirection_DoOrUndoPatches(void);

// n3ds-mcp fork additions
#define IR_AUTOSTART_FLAG_PATH "/luma/inputredirection_autostart.flag"

// n3ds-mcp TRANSPORT PROBE (temporary): where the socSendto diagnosis lands.
// Plain integer args so this header keeps its (deliberately tiny) include set.
#define IR_PROBE_LOG_PATH "/luma/n3ds-mcp-probe.txt"
void InputRedirection_WriteProbeLog(int sent, int sent2, int tmpsock,
                                    u32 srclen, u32 family, u32 addrBE, u32 portHost);

// Lifecycle trace. Input redirection going down is otherwise invisible from
// the PC -- the port just goes quiet, which looks like every other failure.
#define IR_LIFECYCLE_LOG_PATH "/luma/n3ds-mcp-ir.txt"
void InputRedirection_WriteLifecycleLog(const char *event, int cmdSock, int a, int b);

// The command channel lives on its own socket/port so that a fault in it can
// never take down HID injection. 4950 stays exactly as upstream.
#define REMOTE_CMD_PORT 4951
void InputRedirection_HandleCommand(int cmdSock, u32 *errorCount);

// One-shot "close the Rosalina menu" request, set by the IR UDP thread on a
// bit3 rising edge and consumed (cleared, together with menuShouldExit) by
// the menu thread's poll loop.
extern bool remoteMenuCloseRequested;

// One-shot "reboot the console" request, set by the IR UDP thread on the
// RRBT command and consumed by the menu thread. Deliberately actioned from
// the menu thread rather than the IR thread: that is the context
// RosalinaMenu_PowerOffOrReboot already calls APT_HardwareResetAsync from.
extern bool remoteRebootRequested;

// Command magics. All are 8 bytes -- shorter than the 12-byte minimum of an
// input packet -- so stock Luma discards them at the `n < 12` guard and this
// protocol is backward-compatible by construction.
#define REMOTE_CMD_LEN      8
#define REMOTE_MAGIC_QUERY  "RSCR"  // read the Rosalina overlay as text
#define REMOTE_MAGIC_REBOOT "RRBT"  // reboot; bytes 4..7 must match below
#define REMOTE_REBOOT_KEY   0x3D5A1C7EU

// Set once the user touches the InputRedirection menu item; permanently
// disables autostart for this boot so manual intent is never overridden.
extern bool inputRedirectionManuallyControlled;

Result InputRedirection_TryStart(void);
bool InputRedirection_IsAutostartEnabled(void);
Result InputRedirection_SetAutostartEnabled(bool enable);
void InputRedirection_HandleAutostart(void);
bool InputRedirection_AutostartPending(void);

