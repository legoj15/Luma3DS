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

#include <3ds.h>
#include <stdarg.h>
#include "fmt.h"
#include "draw.h"
#include "font.h"
#include "memory.h"
#include "menu.h"
#include "utils.h"
#include "csvc.h"

#define KERNPA2VA(a)            ((a) + (GET_VERSION_MINOR(osGetKernelVersion()) < 44 ? 0xD0000000 : 0xC0000000))

static u32 gpuSavedFramebufferAddr1, gpuSavedFramebufferAddr2, gpuSavedFramebufferFormat, gpuSavedFramebufferStride, gpuSavedFillColor;
static u32 framebufferCacheSize;
static void *framebufferCache;
static RecursiveLock lock;

void Draw_Init(void)
{
    RecursiveLock_Init(&lock);
}

void Draw_Lock(void)
{
    RecursiveLock_Lock(&lock);
}

void Draw_Unlock(void)
{
    RecursiveLock_Unlock(&lock);
}

// ---------------------------------------------------------------------------
// n3ds-mcp: glyph mirror
//
// Every pixel Rosalina puts on the bottom screen goes through
// Draw_DrawCharacter -- it is the only glyph write in the sysmodule -- so a
// shadow text grid maintained here captures the ENTIRE overlay UI losslessly,
// with zero edits at the ~218 call sites. That matters because the menu
// freezes gsp, so the video stream stalls and the overlay is otherwise
// invisible to a remote agent.
//
// Rows are keyed on the EXACT posY the firmware drew at, never quantised to a
// fixed pitch: menu.c steps 11px (SPACING_Y) while plugin/display.c steps
// 10px, and any fixed grid would silently merge two plugin menu items into one
// hybrid string -- corruption that reads as valid data.
// ---------------------------------------------------------------------------

#define MIRROR_COLS 53   // SCREEN_BOT_WIDTH / SPACING_X
#define MIRROR_ROWS 32

typedef struct {
    u8 posY;
    u8 used;                  // one past the highest column written
    u8 ch[MIRROR_COLS];
    u8 attr[MIRROR_COLS];     // bit7 = draw origin, bits0-2 = colour index
} MirrorRow;

static MirrorRow g_rows[MIRROR_ROWS];
static u32 g_nRows = 0;
static volatile u32 g_scrEpoch = 0;
static bool g_originPending = true;
static bool g_rowsOverflowed = false;

static u8 Draw_ColorIndex(u32 color)
{
    switch(color)
    {
        case COLOR_BLACK: return 0;
        case COLOR_WHITE: return 1;
        case COLOR_TITLE: return 2;
        case COLOR_RED:   return 3;
        case COLOR_GREEN: return 4;
        case COLOR_LIME:  return 5;
        default:          return 6;
    }
}

// Called by the framebuffer setup/restore/fill paths. Without this the grid
// would keep reporting stale menu text during the multi-second screenshot
// window and screen-brightness phase 2, when Rosalina owns input but has
// handed the framebuffer back and is drawing nothing.
void Draw_MirrorInvalidate(void)
{
    g_nRows = 0;
    g_rowsOverflowed = false;
    g_originPending = true;
    ++g_scrEpoch;
}

static void Draw_MirrorPut(u32 posX, u32 posY, u32 color, u8 character)
{
    u32 i, col;
    MirrorRow *row = NULL;

    if(posY >= SCREEN_BOT_HEIGHT || posX >= SCREEN_BOT_WIDTH)
        return;

    col = (posX + SPACING_X / 2) / SPACING_X;
    if(col >= MIRROR_COLS)
        return;

    for(i = 0; i < g_nRows; i++)
    {
        if(g_rows[i].posY == (u8)posY)
        {
            row = &g_rows[i];
            break;
        }
    }

    if(row == NULL)
    {
        if(g_nRows >= MIRROR_ROWS)
        {
            g_rowsOverflowed = true;
            return;
        }
        row = &g_rows[g_nRows++];
        row->posY = (u8)posY;
        row->used = 0;
        for(i = 0; i < MIRROR_COLS; i++)
        {
            row->ch[i] = ' ';
            row->attr[i] = 0;
        }
    }

    row->ch[col] = character;
    row->attr[col] = (u8)((g_originPending ? 0x80 : 0x00) | Draw_ColorIndex(color));
    if(col + 1 > row->used)
        row->used = (u8)(col + 1);

    g_originPending = false;
    ++g_scrEpoch;
}

void Draw_DrawCharacter(u32 posX, u32 posY, u32 color, char character)
{
    u16 *const fb = (u16 *)FB_BOTTOM_VRAM_ADDR;

    Draw_MirrorPut(posX, posY, color, (u8)character);

    s32 y;
    for(y = 0; y < 10; y++)
    {
        char charPos = font[character * 10 + y];

        s32 x;
        for(x = 6; x >= 1; x--)
        {
            u32 screenPos = (posX * SCREEN_BOT_HEIGHT * 2 + (SCREEN_BOT_HEIGHT - y - posY - 1) * 2) + (5 - x) * 2 * SCREEN_BOT_HEIGHT;
            u32 pixelColor = ((charPos >> x) & 1) ? color : COLOR_BLACK;
            fb[screenPos / 2] = pixelColor;
        }
    }
}


u32 Draw_DrawString(u32 posX, u32 posY, u32 color, const char *string)
{
    // n3ds-mcp: mark the first glyph of this call as a draw origin, so the
    // client can tell a wrapped continuation from a genuinely new item. The
    // wrap below lands long cheat/plugin names on the next item's baseline,
    // and without this the client would count them as separate entries.
    g_originPending = true;

    for(u32 i = 0, line_i = 0; i < strlen(string); i++)
        switch(string[i])
        {
            case '\n':
                posY += SPACING_Y;
                line_i = 0;
                break;

            case '\t':
                line_i += 2;
                break;

            default:
                //Make sure we never get out of the screen
                if(line_i >= ((SCREEN_BOT_WIDTH) - posX) / SPACING_X)
                {
                    posY += SPACING_Y;
                    line_i = 1; //Little offset so we know the same string continues
                    if(string[i] == ' ') break; //Spaces at the start look weird
                }

                Draw_DrawCharacter(posX + line_i * SPACING_X, posY, color, string[i]);

                line_i++;
                break;
        }

    // Re-arm so a following raw Draw_DrawCharacter also counts as an origin.
    g_originPending = true;

    return posY;
}

u32 Draw_DrawFormattedString(u32 posX, u32 posY, u32 color, const char *fmt, ...)
{
    char buf[DRAW_MAX_FORMATTED_STRING_SIZE + 1];
    va_list args;
    va_start(args, fmt);
    vsprintf(buf, fmt, args);
    va_end(args);

    return Draw_DrawString(posX, posY, color, buf);
}

void Draw_FillFramebuffer(u32 value)
{
    Draw_MirrorInvalidate(); // n3ds-mcp
    memset(FB_BOTTOM_VRAM_ADDR, value, FB_BOTTOM_SIZE);
}

// n3ds-mcp: serialise the shadow grid into `out`, starting at the epoch field.
// Writes: epoch(4) cols(1) nRows(1) payloadLen(2) then row records, each
//   posY(1) textLen(1) text[textLen]  [+colour nibbles]  [+origin bits]
// Rows are emitted ascending by posY. Every write is bounded during generation
// rather than checked afterwards, so a dense screen truncates cleanly instead
// of overrunning. Returns bytes written; sets flag bits 3/4/5.
u32 Draw_SerializeScreenText(u8 *out, u32 outSize, u32 wantPlanes, u32 *flags)
{
    u32 order[MIRROR_ROWS];
    u32 n, i, j, pos, payloadStart;
    u32 epochBefore, epochAfter;
    u32 emitted = 0;

    if(outSize < 8)
        return 0;

    epochBefore = g_scrEpoch;

    n = g_nRows;
    if(n > MIRROR_ROWS)
        n = MIRROR_ROWS;

    // Insertion sort by posY. At most 32 entries, drawn in arbitrary order.
    for(i = 0; i < n; i++)
    {
        u32 k = i;
        order[i] = i;
        while(k > 0 && g_rows[order[k - 1]].posY > g_rows[i].posY)
        {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = i;
    }

    pos = 8;                 // epoch(4) cols(1) nRows(1) payloadLen(2)
    payloadStart = pos;

    for(i = 0; i < n; i++)
    {
        const MirrorRow *row = &g_rows[order[i]];
        u32 len = row->used;
        u32 need;

        while(len > 0 && row->ch[len - 1] == ' ')
            len--;                              // trim trailing spaces

        need = 2 + len;
        if(wantPlanes & 1) need += (len + 1) / 2;
        if(wantPlanes & 2) need += (len + 7) / 8;

        if(pos + need > outSize)
        {
            *flags |= (1u << 5);                // truncated
            break;
        }

        out[pos++] = row->posY;
        out[pos++] = (u8)len;
        for(j = 0; j < len; j++)
            out[pos++] = row->ch[j];

        if(wantPlanes & 1)
        {
            for(j = 0; j < len; j += 2)
            {
                u8 lo = (u8)(row->attr[j] & 0x07);
                u8 hi = (j + 1 < len) ? (u8)(row->attr[j + 1] & 0x07) : 0;
                out[pos++] = (u8)(lo | (hi << 4));
            }
        }

        if(wantPlanes & 2)
        {
            for(j = 0; j < len; j += 8)
            {
                u8 bits = 0, b;
                for(b = 0; b < 8 && j + b < len; b++)
                    if(row->attr[j + b] & 0x80)
                        bits |= (u8)(1u << b);
                out[pos++] = bits;
            }
        }

        emitted++;
    }

    epochAfter = g_scrEpoch;
    if(epochAfter != epochBefore)
        *flags |= (1u << 3);                    // torn read
    if(g_rowsOverflowed)
        *flags |= (1u << 4);
    if(wantPlanes & 1) *flags |= (1u << 1);
    if(wantPlanes & 2) *flags |= (1u << 2);

    out[0] = (u8)(epochBefore);
    out[1] = (u8)(epochBefore >> 8);
    out[2] = (u8)(epochBefore >> 16);
    out[3] = (u8)(epochBefore >> 24);
    out[4] = MIRROR_COLS;
    out[5] = (u8)emitted;
    out[6] = (u8)((pos - payloadStart) & 0xFF);
    out[7] = (u8)(((pos - payloadStart) >> 8) & 0xFF);

    return pos;
}

void Draw_ClearFramebuffer(void)
{
    Draw_FillFramebuffer(0);
}

Result Draw_AllocateFramebufferCache(u32 size)
{
    // Can't use fbs in FCRAM when HOME Menu is active (AXI config related maybe?)
    u32 addr = 0x0D000000;
    u32 tmp;

    size = (size + 0xFFF) >> 12 << 12; // round-up

    if (framebufferCache != NULL)
        __builtin_trap();

    Result res = svcControlMemoryEx(&tmp, addr, 0, size, MEMOP_ALLOC | MEMOP_REGION_SYSTEM, MEMPERM_READWRITE, true);
    if (R_FAILED(res))
    {
        framebufferCache = NULL;
        framebufferCacheSize = 0;
    }
    else
    {
        framebufferCache = (u32 *)addr;
        framebufferCacheSize = size;
    }

    return res;
}

Result Draw_AllocateFramebufferCacheForScreenshot(u32 size)
{
    u32 remaining = (u32)osGetMemRegionFree(MEMREGION_SYSTEM);
    u32 sz = remaining < size ? remaining : size;
    return Draw_AllocateFramebufferCache(sz);
}

void Draw_FreeFramebufferCache(void)
{
    u32 tmp;
    if (framebufferCache != NULL)
        svcControlMemory(&tmp, (u32)framebufferCache, 0, framebufferCacheSize, MEMOP_FREE, 0);
    framebufferCacheSize = 0;
    framebufferCache = NULL;
}

void *Draw_GetFramebufferCache(void)
{
    return framebufferCache;
}

u32 Draw_GetFramebufferCacheSize(void)
{
    return framebufferCacheSize;
}

u32 Draw_SetupFramebuffer(void)
{
    while((GPU_PSC0_CNT | GPU_PSC1_CNT | GPU_TRANSFER_CNT | GPU_CMDLIST_CNT) & 1);

    Draw_FlushFramebuffer();
    memcpy(framebufferCache, FB_BOTTOM_VRAM_ADDR, FB_BOTTOM_SIZE);
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();

    u32 format = GPU_FB_BOTTOM_FMT;

    gpuSavedFramebufferAddr1 = GPU_FB_BOTTOM_ADDR_1;
    gpuSavedFramebufferAddr2 = GPU_FB_BOTTOM_ADDR_2;
    gpuSavedFramebufferFormat = format;
    gpuSavedFramebufferStride = GPU_FB_BOTTOM_STRIDE;

    format = (format & ~7) | GSP_RGB565_OES;
    format |= 3 << 8; // set VRAM bits

    GPU_FB_BOTTOM_ADDR_1 = GPU_FB_BOTTOM_ADDR_2 = FB_BOTTOM_VRAM_PA;
    GPU_FB_BOTTOM_FMT = format;
    GPU_FB_BOTTOM_STRIDE = 240 * 2;

    gpuSavedFillColor = LCD_BOT_FILLCOLOR;
    LCD_BOT_FILLCOLOR = 0;

    return framebufferCacheSize;
}

void Draw_RestoreFramebuffer(void)
{
    // n3ds-mcp: the app's frame is going back on screen. Anything the mirror
    // still holds is no longer what a person would see -- this is the
    // screenshot window and brightness phase 2, where Rosalina keeps input
    // but stops drawing. Reporting stale menu text there would be a lie.
    Draw_MirrorInvalidate();
    memcpy(FB_BOTTOM_VRAM_ADDR, framebufferCache, FB_BOTTOM_SIZE);
    Draw_FlushFramebuffer();

    LCD_BOT_FILLCOLOR = gpuSavedFillColor;
    GPU_FB_BOTTOM_ADDR_1 = gpuSavedFramebufferAddr1;
    GPU_FB_BOTTOM_ADDR_2 = gpuSavedFramebufferAddr2;
    GPU_FB_BOTTOM_FMT = gpuSavedFramebufferFormat;
    GPU_FB_BOTTOM_STRIDE = gpuSavedFramebufferStride;
}

void Draw_FlushFramebuffer(void)
{
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)FB_BOTTOM_VRAM_ADDR, FB_BOTTOM_SIZE);
}

u32 Draw_GetCurrentFramebufferAddress(bool top, bool left)
{
    if(GPU_FB_BOTTOM_SEL & 1)
    {
        if(left)
            return top ? GPU_FB_TOP_LEFT_ADDR_2 : GPU_FB_BOTTOM_ADDR_2;
        else
            return top ? GPU_FB_TOP_RIGHT_ADDR_2 : GPU_FB_BOTTOM_ADDR_2;
    }
    else
    {
        if(left)
            return top ? GPU_FB_TOP_LEFT_ADDR_1 : GPU_FB_BOTTOM_ADDR_1;
        else
            return top ? GPU_FB_TOP_RIGHT_ADDR_1 : GPU_FB_BOTTOM_ADDR_1;
    }
}

void Draw_GetCurrentScreenInfo(u32 *width, bool *is3d, bool top)
{
    if (top)
    {
        bool isNormal2d = (GPU_FB_TOP_FMT & BIT(6)) != 0;
        *is3d = (GPU_FB_TOP_FMT & BIT(5)) != 0;
        *width = !(*is3d) && !isNormal2d ? 800 : 400;
    }
    else
    {
        *is3d = false;
        *width = 320;
    }
}

static inline void Draw_WriteUnaligned(u8 *dst, u32 tmp, u32 size)
{
    memcpy(dst, &tmp, size);
}

void Draw_CreateBitmapHeader(u8 *dst, u32 width, u32 heigth)
{
    static const u8 bmpHeaderTemplate[54] = {
        // BITMAPFILEHEADER
        0x42, 0x4D, 0xCC, 0xCC, 0xCC, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x40 /* data offset */, 0x00, 0x00, 0x00,

        // BITMAPINFOHEADER
        0x28, 0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x01, 0x00, 0x18, 0x00, 0x00,
        0x00, 0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x12, 0x0B, 0x00, 0x00, 0x12, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    memcpy(dst, bmpHeaderTemplate, 54);
    memset(dst + 54, 0, 64 - 54);
    Draw_WriteUnaligned(dst + 2, 64 + 3 * width * heigth, 4);
    Draw_WriteUnaligned(dst + 0x12, width, 4);
    Draw_WriteUnaligned(dst + 0x16, heigth, 4);
    Draw_WriteUnaligned(dst + 0x22, 3 * width * heigth, 4);
}

static inline void Draw_ConvertPixelToBGR8(u8 *dst, const u8 *src, GSPGPU_FramebufferFormat srcFormat)
{
    u8 red, green, blue;
    switch(srcFormat)
    {
        case GSP_RGBA8_OES:
        {
            u32 px = *(u32 *)src;
            dst[0] = (px >>  8) & 0xFF;
            dst[1] = (px >> 16) & 0xFF;
            dst[2] = (px >> 24) & 0xFF;
            break;
        }
        case GSP_BGR8_OES:
        {
            dst[2] = src[2];
            dst[1] = src[1];
            dst[0] = src[0];
            break;
        }
        case GSP_RGB565_OES:
        {
            // thanks neobrain
            u16 px = *(u16 *)src;
            blue = px & 0x1F;
            green = (px >> 5) & 0x3F;
            red = (px >> 11) & 0x1F;

            dst[0] = (blue  << 3) | (blue  >> 2);
            dst[1] = (green << 2) | (green >> 4);
            dst[2] = (red   << 3) | (red   >> 2);

            break;
        }
        case GSP_RGB5_A1_OES:
        {
            u16 px = *(u16 *)src;
            blue = (px >> 1) & 0x1F;
            green = (px >> 6) & 0x1F;
            red = (px >> 11) & 0x1F;

            dst[0] = (blue  << 3) | (blue  >> 2);
            dst[1] = (green << 3) | (green >> 2);
            dst[2] = (red   << 3) | (red   >> 2);

            break;
        }
        case GSP_RGBA4_OES:
        {
            u16 px = *(u32 *)src;
            blue = (px >> 4) & 0xF;
            green = (px >> 8) & 0xF;
            red = (px >> 12) & 0xF;

            dst[0] = (blue  << 4) | (blue  >> 0);
            dst[1] = (green << 4) | (green >> 0);
            dst[2] = (red   << 4) | (red   >> 0);

            break;
        }
        default: break;
    }
}

typedef struct FrameBufferConvertArgs {
    u8 *buf;
    u32 width;
    u8 startingLine;
    u8 numLines;
    u8 scaleFactorY;
    bool top;
    bool left;
} FrameBufferConvertArgs;

static void Draw_ConvertFrameBufferLinesKernel(const FrameBufferConvertArgs *args)
{
    static const u8 formatSizes[] = { 4, 3, 2, 2, 2 };

    GSPGPU_FramebufferFormat fmt = args->top ? (GSPGPU_FramebufferFormat)(GPU_FB_TOP_FMT & 7) : (GSPGPU_FramebufferFormat)(GPU_FB_BOTTOM_FMT & 7);
    u32 width = args->width;
    u32 stride = args->top ? GPU_FB_TOP_STRIDE : GPU_FB_BOTTOM_STRIDE;

    u32 pa = Draw_GetCurrentFramebufferAddress(args->top, args->left);
    u8 *addr = (u8 *)KERNPA2VA(pa);

    for (u32 y = args->startingLine; y < args->startingLine + args->numLines; y++)
    {
        for (u8 i = 0; i < args->scaleFactorY; i++)
        {
            for(u32 x = 0; x < width; x++)
            {
                __builtin_prefetch(addr + x * stride + y * formatSizes[fmt], 0, 3);
                Draw_ConvertPixelToBGR8(args->buf + (x + width * (args->scaleFactorY * y + i)) * 3 , addr + x * stride + y * formatSizes[fmt], fmt);
            }
        }
    }
}

void Draw_ConvertFrameBufferLines(u8 *buf, u32 width, u32 startingLine, u32 numLines, u32 scaleFactorY, bool top, bool left)
{
    FrameBufferConvertArgs args = { buf, width, (u8)startingLine, (u8)numLines, (u8)scaleFactorY, top, left };
    svcCustomBackdoor(Draw_ConvertFrameBufferLinesKernel, &args);
}
