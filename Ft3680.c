/*++
    Ft3680.c — FT3680 chip layer: bring-up (boot-ID flow), valid-poll,
    touch frame read/parse, sleep. Plus the file logger used everywhere.
    All sequencing follows winboot/touch_ft3680_spec.md (SS2, SS3, SS5).
--*/
#include "Ft3680.h"
#include <ntstrsafe.h>

VOID
FtDelayMs(_In_ ULONG Ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = (LONGLONG)Ms * -10000;   /* relative, 100ns units */
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

VOID
FtLog(_In_ PDEVICE_CONTEXT Ctx, _In_ PCSTR Format, ...)
{
    CHAR line[512];
    va_list args;
    NTSTATUS status;

    va_start(args, Format);
    status = RtlStringCchVPrintfA(line, sizeof(line), Format, args);
    va_end(args);
    if (!NT_SUCCESS(status)) {
        return;
    }
    KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "FT3680: %s\n", line));

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    KeWaitForSingleObject(&Ctx->LogMutex, Executive, KernelMode, FALSE, NULL);
    {
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES oa;
        HANDLE handle = NULL;
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER offset;

        RtlInitUnicodeString(&path, L"\\DosDevices\\C:\\ft3680.log");
        InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&handle, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb,
                                    NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                    FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT,
                                    NULL, 0))) {
            offset.QuadPart = FILE_WRITE_TO_END_OF_FILE;
            ZwWriteFile(handle, NULL, NULL, NULL, &iosb, line,
                        (ULONG)strnlen(line, sizeof(line)), &offset, NULL);
            ZwWriteFile(handle, NULL, NULL, NULL, &iosb, "\r\n", 2, &offset, NULL);
            ZwFlushBuffersFile(handle, &iosb);
            ZwClose(handle);
        }
    }
    KeReleaseMutex(&Ctx->LogMutex, FALSE);
}

/* fts_read_bootid (spec SS2.3): framed write {0x55,0xAA}, 12 ms, read 0x90. */
static NTSTATUS
FtReadBootId(_In_ PDEVICE_CONTEXT Ctx, _Out_writes_(2) PUCHAR Id)
{
    UCHAR startCmd[2] = { FT_CMD_START1, FT_CMD_START2 };
    NTSTATUS status;

    status = FtBusWrite(Ctx, startCmd, sizeof(startCmd));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    FtDelayMs(FT_CMD_START_DELAY_MS);
    status = FtBusRead(Ctx, FT_CMD_READ_ID, Id, 2);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (Id[0] == 0 || Id[1] == 0) {
        return STATUS_DEVICE_NOT_READY;   /* spec SS2.2: zero byte rejected */
    }
    return STATUS_SUCCESS;
}

/* fts_reset_proc(hdelay=0) + start-delay backoff + boot-ID match (SS2.1/2.2). */
/* Spec SS2.1 reset pulse: low, brief hold, high. The caller waits
 * fts_reset_proc's hdelay (200 ms on the probe path) before talking. */
VOID
FtChipReset(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS rsLo = GpioWritePin(Ctx, Ctx->GpioResetPin, FALSE);
    FtDelayMs(10);
    NTSTATUS rsHi = GpioWritePin(Ctx, Ctx->GpioResetPin, TRUE);
    FtLog(Ctx, "reset pulse: low -> %08X, high -> %08X", rsLo, rsHi);
}

/* Try every role permutation of pins {52,53,54,55}: reset + boot-ID
 * read per candidate. On a CRC-valid answer, keep that mapping.
 * Order note: the permutation that answers on spacewar is tried
 * first (MOSI 53 / MISO 52 / CLK 54 / CS 55 — the two data lines
 * are swapped vs the _CRS/DTS naming, found by this scan on
 * 2026-10-28 once the chip was powered). */
BOOLEAN
FtBbPinScan(_In_ PDEVICE_CONTEXT Ctx)
{
    static const ULONG base[4] = { 53, 52, 54, 55 };
    ULONG a, b, c, d;
    int idx = 0;

    for (a = 0; a < 4; a++) for (b = 0; b < 4; b++) {
        if (b == a) continue;
        for (c = 0; c < 4; c++) {
            if (c == a || c == b) continue;
            d = 6 - a - b - c;
            Ctx->BbPins[BB_MOSI] = base[a];
            Ctx->BbPins[BB_MISO] = base[b];
            Ctx->BbPins[BB_CLK] = base[c];
            Ctx->BbPins[BB_CS] = base[d];
            Ctx->BbPinsConfigured = FALSE;
            FtChipReset(Ctx);
            FtDelayMs(250);
            if (Ctx->HardwareStopping) return FALSE;
            {
                UCHAR id[2] = { 0, 0 };
                NTSTATUS st = FtReadBootId(Ctx, id);
                FtLog(Ctx, "scan %d: MOSI %lu MISO %lu CLK %lu CS %lu -> %08X raw %02X %02X %02X %02X",
                      idx, base[a], base[b], base[c], base[d],
                      (ULONG)st, Ctx->RxData[0], Ctx->RxData[1],
                      Ctx->RxData[2], Ctx->RxData[3]);
                if (NT_SUCCESS(st)) {
                    FtLog(Ctx, "scan: FOUND mapping, boot id %02X %02X", id[0], id[1]);
                    return TRUE;
                }
            }
            idx++;
        }
    }
    /* restore the known-good mapping (see order note above) */
    Ctx->BbPins[BB_MOSI] = 53; Ctx->BbPins[BB_MISO] = 52;
    Ctx->BbPins[BB_CLK] = 54; Ctx->BbPins[BB_CS] = 55;
    Ctx->BbPinsConfigured = FALSE;
    FtLog(Ctx, "scan: no permutation answered");
    return FALSE;
}


NTSTATUS
FtChipBringUp(_In_ PDEVICE_CONTEXT Ctx)
{
    int attempt;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    BOOLEAN fwFailed = FALSE;

    FtLog(Ctx, "bring-up: entry (reset pulse via handle next)");

    for (attempt = 0; attempt < 6; attempt++) {
        UCHAR id[2] = { 0, 0 };

        if (Ctx->HardwareStopping) {
            FtLog(Ctx, "bring-up aborted: hardware stopping");
            return STATUS_DEVICE_NOT_READY;
        }
        if (fwFailed) {
            /* A previous FW download died mid-transfer (the chip went
               silent during the long PRAM write). On images where the
               subsystem drivers come alive (ACPI v11+), their start
               activity disturbs the touch chip during that window.
               Wait for the storm to pass, then retry from a clean
               reset instead of giving up for the whole boot. */
            FtLog(Ctx, "bring-up: waiting 4 s before retry %d", attempt);
            FtDelayMs(4000);
            fwFailed = FALSE;
            if (Ctx->HardwareStopping) {
                FtLog(Ctx, "bring-up aborted: hardware stopping");
                return STATUS_DEVICE_NOT_READY;
            }
        }
        FtChipReset(Ctx);
        /* fts_reset_proc(ts, 200): the ROM needs ~200 ms after release. */
        FtDelayMs(200 + attempt * 50);

        status = FtReadBootId(Ctx, id);
        FtLog(Ctx, "bootid attempt %d: status 0x%08X id %02X %02X",
              attempt, status, id[0], id[1]);
        if (NT_SUCCESS(status) &&
            ((id[0] == FT_ID_ROM_H && id[1] == FT_ID_ROM_L) ||
             (id[0] == FT_ID_PB_H && id[1] == FT_ID_PB_L))) {
            UCHAR d0 = 0, a3 = 0, a6 = 0;
            Ctx->BootId[0] = id[0];
            Ctx->BootId[1] = id[1];
            /* Diagnostic: is the app FW running or is the chip sitting
               in boot mode (LOS downloads FW in the latter case)?
               D0 = boot state (0x01 = boot mode for FT3680). */
            FtBusRead(Ctx, 0xD0, &d0, 1);
            FtBusRead(Ctx, FT_REG_CHIP_ID, &a3, 1);
            FtBusRead(Ctx, 0xA6, &a6, 1);
            FtLog(Ctx, "regs: D0(boot state) %02X A3(chip id) %02X A6(fw ver) %02X",
                  d0, a3, a6);
            if (d0 == 0x01) {
                /* Boot mode: the app firmware is host-loaded
                   (FtFw.c) — without it reg 0x01 stays at the
                   0xEF "FW not running" pattern forever. */
                FtLog(Ctx, "chip in boot mode: downloading fw");
                status = FtFwDownload(Ctx);
                if (!NT_SUCCESS(status)) {
                    FtLog(Ctx, "fw download failed -> 0x%08X (will retry)",
                          status);
                    fwFailed = TRUE;
                    continue;
                }
                status = FtChipWaitValid(Ctx);
                if (!NT_SUCCESS(status)) {
                    FtLog(Ctx, "fw started but chip not valid -> 0x%08X"
                          " (will retry)", status);
                    fwFailed = TRUE;
                    continue;
                }
                d0 = 0; a3 = 0; a6 = 0;
                FtBusRead(Ctx, 0xD0, &d0, 1);
                FtBusRead(Ctx, FT_REG_CHIP_ID, &a3, 1);
                FtBusRead(Ctx, 0xA6, &a6, 1);
                FtLog(Ctx, "regs after fw: D0 %02X A3 %02X A6 %02X",
                      d0, a3, a6);
                /* Work mode = normal (reg 0x00 = 0x00), then let
                   the app settle into scanning before the first
                   frame reads (the first FW boot's reads all came
                   back empty; a settle delay fixed the flakiness
                   pattern seen across ft3680-22 vs -23). */
                {
                    UCHAR workMode[2] = { 0x00, 0x00 };
                    (VOID)FtBusWrite(Ctx, workMode, sizeof(workMode));
                }
                FtDelayMs(300);
            }
            return STATUS_SUCCESS;
        }
        /* keep FtReadBootId's real status for the caller's log */
    }
    return status;
}

/* fts_wait_tp_to_valid (spec SS2.5): poll reg 0xA3 for chip_idh 0x56. */
NTSTATUS
FtChipWaitValid(_In_ PDEVICE_CONTEXT Ctx)
{
    int i;
    for (i = 0; i < 5; i++) {
        UCHAR v = 0;
        NTSTATUS status;
        if (Ctx->HardwareStopping) {
            return STATUS_DEVICE_NOT_READY;
        }
        status = FtBusRead(Ctx, FT_REG_CHIP_ID, &v, 1);
        if (NT_SUCCESS(status) && v == FT_ID_ROM_H) {
            return STATUS_SUCCESS;
        }
        FtDelayMs(200);
    }
    return STATUS_DEVICE_NOT_READY;
}

/* Read + parse one touch frame from reg 0x01 (spec SS3). */
NTSTATUS
FtReadTouchFrame(_In_ PDEVICE_CONTEXT Ctx, _Out_ FT_TOUCH_POINT* Points,
                 _Out_ ULONG* PointCount, _Out_ UCHAR* FingerNum)
{
    UCHAR buf[FT_REPORT_DATA_LEN];
    UCHAR etype, fingers;
    ULONG base, i, count = 0;
    NTSTATUS status;

    *PointCount = 0;
    *FingerNum = 0;

    RtlFillMemory(buf, sizeof(buf), 0xFF);
    status = FtBusRead(Ctx, FT_REG_TOUCH_DATA, buf, sizeof(buf));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Firmware-state patterns (spec SS3.1): treat as "no frame". */
    if (buf[1] == 0xEF && buf[2] == 0xEF && buf[3] == 0xEF) {
        FtLog(Ctx, "FW not running pattern (0xEF)");
        return STATUS_DEVICE_NOT_READY;
    }
    if (buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0xFF) {
        return STATUS_DEVICE_NOT_READY;   /* FW still initialising */
    }

    etype = (UCHAR)((buf[1] >> 4) & 0x0F);
    fingers = (UCHAR)(buf[1] & 0x0F);
    *FingerNum = fingers;

    switch (etype) {
    case 0x00:   /* TOUCH_DEFAULT */
    case 0x02:   /* TOUCH_EVENT_NUM */
        base = 2;
        break;
    case 0x08:   /* TOUCH_EXTRA_MSG: records start at base 4 */
        base = 4;
        break;
    default:
        return STATUS_DEVICE_NOT_READY;   /* gestures/pen/etc.: drop (SS3.4) */
    }

    /* Iterate all records like the Linux parser (spec SS3.3): trailing
       0xFF-filled records terminate via id >= 0x0A. */
    for (i = 0; i < FT_MAX_POINTS; i++) {
        ULONG off = base + 6 * i;
        UCHAR id, flag;
        if (off + 5 >= FT_REPORT_DATA_LEN) {
            break;
        }
        id = (UCHAR)(buf[off + 2] >> 4);
        if (id >= FT_MAX_POINTS) {
            break;
        }
        flag = (UCHAR)(buf[off + 0] >> 6);
        if ((flag == 0 || flag == 2) && fingers == 0) {
            return STATUS_DEVICE_NOT_READY;   /* abnormal frame (SS3.3) */
        }
        if (flag != 1 && buf[off + 4] == 0 && buf[off + 5] == 0) {
            /* Suspicious record: with the DSP subsystems active the
               panel leaves stuck slots in the frame (frozen edge
               coordinates, raw pressure AND area both zero) that
               never release and wedge the shell's pointer state.
               But a real finger can also flash 0/0 briefly (the
               Linux driver substitutes defaults for exactly that
               case), and dropping those frames breaks press-and-
               hold. Compromise: tolerate up to 3 consecutive zero
               frames per ID (values substituted below); from the
               4th on, treat the slot as a ghost and drop it - the
               release debounce then lifts the contact cleanly. */
            Ctx->ZeroStreak[id]++;
            if (Ctx->ZeroStreak[id] >= 4) {
                static ULONG ghostDrops = 0;
                ghostDrops++;
                if (ghostDrops == 1 || (ghostDrops % 64) == 0) {
                    FtLog(Ctx, "ghost filter: dropped %lu zero p/a records",
                          ghostDrops);
                }
                continue;
            }
        } else if (buf[off + 4] != 0 || buf[off + 5] != 0) {
            Ctx->ZeroStreak[id] = 0;
        }
        Points[count].Id = id;
        Points[count].Flag = flag;
        Points[count].X = (USHORT)(((buf[off + 0] & 0x0F) << 8) | buf[off + 1]);
        Points[count].Y = (USHORT)(((buf[off + 2] & 0x0F) << 8) | buf[off + 3]);
        Points[count].Pressure = buf[off + 4] ? buf[off + 4] : 0x3F;
        Points[count].Area = buf[off + 5] ? buf[off + 5] : 0x09;
        count++;
    }

    if (count == 0) {
        return STATUS_DEVICE_NOT_READY;
    }
    *PointCount = count;
    return STATUS_SUCCESS;
}

/* Suspend write: reg 0xA5 = 0x03 sleep (spec SS5.3). */
NTSTATUS
FtChipSleep(_In_ PDEVICE_CONTEXT Ctx)
{
    UCHAR cmd[2] = { FT_REG_POWER_MODE, FT_POWER_MODE_SLEEP };
    return FtBusWrite(Ctx, cmd, sizeof(cmd));
}
