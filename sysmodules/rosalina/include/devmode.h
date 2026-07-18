/*
*   n3ds-mcp fork addition: development mode.
*
*   One persistent master switch for every remote-control feature this fork
*   adds. ON means the console is a development target: input redirection and
*   the file service arm themselves at every boot, and it never sleeps. OFF
*   means it behaves like stock Luma3DS -- a gaming handheld -- across reboots,
*   with no background services and no battery cost.
*
*   Deliberately one switch rather than a flag per feature: the features are
*   only useful together, and a half-armed console (input but no file service,
*   or vice versa) is a confusing state that is easy to end up in by accident
*   and hard to diagnose remotely.
*/

#pragma once

#include <3ds/types.h>

#define DEVMODE_FLAG_PATH "/luma/n3ds-mcp-devmode.flag"

bool   DevMode_IsEnabled(void);
Result DevMode_SetEnabled(bool enable);

// Menu entry point: Rosalina root menu -> "n3ds-mcp development mode".
void RosalinaMenu_DevMode(void);
