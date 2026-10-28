/*++
    Device.c — PnP/power callbacks, resource parsing, interrupt + work items.

    Resource layout delivered by ACPI for \_SB.SP14.TSC1 (see DESIGN.md):
      _CRS order = SpiSerialBusV2, GpioInt, GpioIo.
    After translation that becomes: two CmResourceTypeConnection entries
    (SPB first, GPIO second — same order) and one CmResourceTypeInterrupt
    (the GpioInt line, routed through the TLMM GPIO controller).
--*/
#include "Ft3680.h"

NTSTATUS
FtEvtPrepareHardware(_In_ WDFDEVICE Device,
                     _In_ WDFCMRESLIST ResourcesRaw,
                     _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    ULONG count = WdfCmResourceListGetCount(ResourcesTranslated);
    FtLog(ctx, "PrepareHardware: %u resources", count);
    ULONG connSeen = 0;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR intTrans = NULL;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    /* Scratch for SPB_TRANSFER_LIST with two entries. */
    ctx->SpbListBufferSize = sizeof(SPB_TRANSFER_LIST) + sizeof(SPB_TRANSFER_LIST_ENTRY);
    ctx->SpbListBuffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 ctx->SpbListBufferSize, FT_POOL_TAG);
    if (ctx->SpbListBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        switch (desc->Type) {
        case CmResourceTypeConnection:
            FtLog(ctx, "conn %lu class %u type %u id %08X%08X", connSeen,
                  desc->u.Connection.Class, desc->u.Connection.Type,
                  desc->u.Connection.IdHighPart, desc->u.Connection.IdLowPart);
            if (connSeen == 0) {
                ctx->SpbDescriptor = *desc;
                ctx->HasSpbDescriptor = TRUE;
                status = SpbOpenTarget(Device, ctx, desc);
                if (!NT_SUCCESS(status)) {
                    FtLog(ctx, "SpbOpenTarget failed 0x%08X (diagnostic build: continuing)", status);
                    status = STATUS_SUCCESS;
                }
            } else if (connSeen == 1) {
                status = GpioOpenTarget(Device, ctx, desc);
                if (!NT_SUCCESS(status)) {
                    /* Reset control is best-effort: the chip may already
                       be powered and running from the Linux-side rails. */
                    FtLog(ctx, "GpioOpenTarget failed 0x%08X (continuing)", status);
                    status = STATUS_SUCCESS;
                }
            } else if (connSeen >= 2 && connSeen <= 5) {
                /* Bit-bang SPI pins (ACPI v9): MOSI, MISO, CLK, CS. */
                status = BbOpenTarget(Device, ctx, desc, connSeen - 2);
                if (!NT_SUCCESS(status)) {
                    FtLog(ctx, "BbOpenTarget %lu failed 0x%08X (continuing)",
                          connSeen - 2, status);
                    status = STATUS_SUCCESS;
                }
            }
            connSeen++;
            break;

        case CmResourceTypeInterrupt:
            intTrans = desc;
            break;

        default:
            break;
        }
    }

    if (ctx->SpbTarget == NULL) {
        FtLog(ctx, "SPB not open yet; will retry at D0");
    }

    {
        NTSTATUS tstatus = TlmmInit(ctx);
        ctx->UseBb = NT_SUCCESS(tstatus);
        FtLog(ctx, "tlmm init -> 0x%08X, UseBb %d", tstatus, ctx->UseBb);
    }

    /* Interrupt for the GpioInt line (GPIO81, edge / active-low per _CRS). */
    if (intTrans != NULL) {
        WDF_INTERRUPT_CONFIG intConfig;
        WDF_OBJECT_ATTRIBUTES intAttrs;

        WDF_INTERRUPT_CONFIG_INIT(&intConfig, FtEvtInterruptIsr, NULL);
        intConfig.InterruptTranslated = intTrans;
        intConfig.InterruptRaw = NULL;

        WDF_OBJECT_ATTRIBUTES_INIT(&intAttrs);
        intAttrs.ParentObject = Device;
        status = WdfInterruptCreate(Device, &intConfig, &intAttrs, &ctx->Interrupt);
        if (!NT_SUCCESS(status)) {
            /* Non-fatal for now: bring-up and SPB probing proceed without
               the IRQ (frames can be polled as a fallback). */
            FtLog(ctx, "WdfInterruptCreate failed 0x%08X (continuing)", status);
            ctx->Interrupt = NULL;
            status = STATUS_SUCCESS;
        }
    } else {
        FtLog(ctx, "WARNING: no interrupt resource; touch will not report");
    }

    /* Poll fallback for the touch INT line (GPIO81, active low):
       the GpioInt above does not connect on this platform, so
       watch the pad directly via TLMM at 10 ms cadence and feed
       the same work-item path the ISR uses. */
    {
        WDF_TIMER_CONFIG timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttrs;

        WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, FtEvtTouchPollTimer, 10);
        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttrs);
        timerAttrs.ParentObject = Device;
        status = WdfTimerCreate(&timerConfig, &timerAttrs, &ctx->TouchPollTimer);
        if (!NT_SUCCESS(status)) {
            FtLog(ctx, "WdfTimerCreate failed 0x%08X (continuing)", status);
            ctx->TouchPollTimer = NULL;
            status = STATUS_SUCCESS;
        }
    }

    InterlockedExchange(&ctx->HardwareStopping, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
FtEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    FtLog(ctx, "ReleaseHardware: entry (stopping + flushing work items)");
    InterlockedExchange(&ctx->HardwareStopping, 1);
    if (ctx->TouchPollTimer != NULL) {
        WdfTimerStop(ctx->TouchPollTimer, TRUE);
    }
    if (ctx->BringUpWorkItem != NULL) {
        WdfWorkItemFlush(ctx->BringUpWorkItem);
    }
    if (ctx->TouchWorkItem != NULL) {
        WdfWorkItemFlush(ctx->TouchWorkItem);
    }
    FtLog(ctx, "ReleaseHardware: teardown proceeds");

    if (ctx->SpbTarget != NULL) {
        WdfIoTargetClose(ctx->SpbTarget);
        ctx->SpbTarget = NULL;
    }
    if (ctx->GpioTarget != NULL) {
        WdfIoTargetClose(ctx->GpioTarget);
        ctx->GpioTarget = NULL;
        ctx->GpioTargetOpened = FALSE;
    }
    {
        ULONG b;
        for (b = 0; b < 4; b++) {
            if (ctx->BbHandles[b] != NULL) {
                ZwClose(ctx->BbHandles[b]);
                ctx->BbHandles[b] = NULL;
            }
            if (ctx->BbTargets[b] != NULL) {
                WdfIoTargetClose(ctx->BbTargets[b]);
                ctx->BbTargets[b] = NULL;
                ctx->BbOpened[b] = FALSE;
            }
        }
    }
    if (ctx->GpioHandle != NULL) {
        ZwClose(ctx->GpioHandle);
        ctx->GpioHandle = NULL;
    }
    if (ctx->TlmmBase != NULL) {
        MmUnmapIoSpace(ctx->TlmmBase, 0x00300000);
        ctx->TlmmBase = NULL;
    }
    if (ctx->SpbListBuffer != NULL) {
        ExFreePoolWithTag(ctx->SpbListBuffer, FT_POOL_TAG);
        ctx->SpbListBuffer = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
FtEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    /* Bring-up does blocking SPI + file I/O: run it in the work item. */
    FtLog(ctx, "D0Entry");
    if (!ctx->HardwareStopping) {
        WdfWorkItemEnqueue(ctx->BringUpWorkItem);
        if (ctx->TouchPollTimer != NULL) {
            WdfTimerStart(ctx->TouchPollTimer, WDF_REL_TIMEOUT_IN_MS(50));
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
FtEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    UNREFERENCED_PARAMETER(TargetState);
    FtLog(ctx, "D0Exit: entry (target state %d)", (ULONG)TargetState);
    if (ctx->TouchPollTimer != NULL) {
        WdfTimerStop(ctx->TouchPollTimer, FALSE);
    }
    if (ctx->State == FtStateReady) {
        (VOID)FtChipSleep(ctx);   /* reg 0xA5 = 0x03, spec SS5.3 */
        ctx->State = FtStateUninitialized;
    }
    return STATUS_SUCCESS;
}

VOID
FtEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    FtLog(ctx, "PnP: SURPRISE REMOVAL");
}

NTSTATUS
FtEvtQueryRemove(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    FtLog(ctx, "PnP: QUERY REMOVE (allowing)");
    return STATUS_SUCCESS;
}

NTSTATUS
FtEvtQueryStop(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    FtLog(ctx, "PnP: QUERY STOP (allowing)");
    return STATUS_SUCCESS;
}

BOOLEAN
FtEvtInterruptIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfInterruptGetDevice(Interrupt));
    UNREFERENCED_PARAMETER(MessageID);

    if (ctx->HardwareStopping || ctx->State != FtStateReady) {
        return TRUE;   /* claim it; nothing to read yet */
    }
    if (InterlockedCompareExchange(&ctx->TouchReadInFlight, 1, 0) == 0) {
        WdfWorkItemEnqueue(ctx->TouchWorkItem);
    } else {
        InterlockedExchange(&ctx->TouchReadAgain, 1);
    }
    return TRUE;
}

VOID
FtEvtTouchPollTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfTimerGetParentObject(Timer));
    BOOLEAN wantRead = FALSE;

    if (ctx->HardwareStopping || ctx->State != FtStateReady ||
        ctx->TlmmBase == NULL) {
        return;
    }
    if (!TlmmRead(ctx, 81)) {   /* INT is active-low: low = frame pending */
        wantRead = TRUE;
        if (InterlockedExchange(&ctx->IntLowLogged, 1) == 0) {
            FtLog(ctx, "poll: INT LOW observed (app asserts frames)");
        }
    }
    /* Fallback cadence: also read unconditionally every ~100 ms
       (touch-down latency bound while INT is unproven). The work
       item drains back-to-back frames once one lands, so a drag
       does not depend on this cadence. */
    if ((++ctx->PollTicks % 10) == 0) {
        wantRead = TRUE;
    }
    if (wantRead) {
        if (InterlockedCompareExchange(&ctx->TouchReadInFlight, 1, 0) == 0) {
            WdfWorkItemEnqueue(ctx->TouchWorkItem);
        } else {
            InterlockedExchange(&ctx->TouchReadAgain, 1);
        }
    }
}

VOID
FtEvtBringUpWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfWorkItemGetParentObject(WorkItem));
    NTSTATUS status;

    /* The controller may refuse connection opens during PrepareHardware;
       retry here (D0, work-item context) before giving up. */
    if (ctx->SpbTarget == NULL && ctx->HasSpbDescriptor && !ctx->UseBb) {
        WDFDEVICE device = WdfWorkItemGetParentObject(WorkItem);
        ULONG attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            status = SpbOpenTarget(device, ctx, &ctx->SpbDescriptor);
            if (NT_SUCCESS(status)) {
                FtLog(ctx, "SpbOpenTarget ok on D0 attempt %lu", attempt);
                break;
            }
            FtLog(ctx, "SpbOpenTarget D0 attempt %lu -> 0x%08X", attempt, status);
            {
                LARGE_INTEGER interval;
                interval.QuadPart = -2000000;   /* 200 ms */
                KeDelayExecutionThread(KernelMode, FALSE, &interval);
            }
        }
    }

    if (ctx->SpbTarget == NULL && !ctx->UseBb) {
        FtLog(ctx, "bring-up aborted: no bus (SPB closed, bit-bang unavailable)");
        ctx->State = FtStateError;
        return;
    }

    if (!ctx->BroughtUpOnce) {
        if (ctx->UseBb) {
            (VOID)FtBbPinScan(ctx);
        }
        status = FtChipBringUp(ctx);
        if (NT_SUCCESS(status)) {
            ctx->BroughtUpOnce = TRUE;
        }
    } else {
        /* Resume path for in-cell FT3680: no reset, just wait for the
           firmware to report a valid chip ID (spec SS5.3, SS2.5). */
        status = FtChipWaitValid(ctx);
        if (!NT_SUCCESS(status)) {
            status = FtChipBringUp(ctx);
        }
    }

    ctx->State = NT_SUCCESS(status) ? FtStateReady : FtStateError;
    ctx->ActiveMask = 0;   /* fresh contact tracking per power cycle */
    ctx->UpPendingMask = 0;
    ctx->UpPendingAged = 0;
    ctx->LastFrameValid = FALSE;   /* always report the first frame */
    FtLog(ctx, "bring-up done: status 0x%08X state %d bootid %02X %02X",
          status, ctx->State, ctx->BootId[0], ctx->BootId[1]);

    /* IRQ watch: if the FT3680 is powered and its firmware is running,
       touching the panel drives GPIO81 low. Sample for ~20 s and log
       every transition (user: touch/hold the screen during this). */
    if (ctx->TlmmBase != NULL && !ctx->HardwareStopping) {
        BOOLEAN prev = TlmmRead(ctx, 81);
        int i;
        FtLog(ctx, "irqwatch: start, GPIO81 in = %lu (touch the screen now)",
              (ULONG)prev);
        for (i = 0; i < 200; i++) {
            BOOLEAN v;
            if (ctx->HardwareStopping) break;
            FtDelayMs(100);
            v = TlmmRead(ctx, 81);
            if (v != prev) {
                FtLog(ctx, "irqwatch: GPIO81 -> %lu at ~%d ms", (ULONG)v, (i + 1) * 100);
                prev = v;
            }
        }
        FtLog(ctx, "irqwatch: end, GPIO81 in = %lu", (ULONG)prev);
    }
}

VOID
FtEvtTouchWorkItem(_In_ WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfWorkItemGetParentObject(WorkItem));
    ULONG drain = 0;

    for (;;) {
        FT_TOUCH_POINT points[FT_MAX_POINTS];
        ULONG pointCount = 0;
        UCHAR fingerNum = 0;
        NTSTATUS status;

        InterlockedExchange(&ctx->TouchReadAgain, 0);
        RtlZeroMemory(points, sizeof(points)); /* stable padding for the
                                                  duplicate comparison */
        status = FtReadTouchFrame(ctx, points, &pointCount, &fingerNum);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(ctx->LastPoints, points,
                          pointCount * sizeof(FT_TOUCH_POINT));
            ctx->LastPointCount = pointCount;
            ctx->LastFrameValid = TRUE;
            /* Report the frame exactly as parsed (simple
               path: no debounce / dedupe - see git note
               in FtEvtTouchWorkItem). */
            HidBuildReport(ctx, points, pointCount);
            HidCompleteReadWithReport(ctx);
            if (ctx->FrameLogCount < 200 || (ctx->FrameLogCount % 100) == 0) {
                ULONG i;
                FtLog(ctx, "frame %lu fingers %u parsed %lu",
                      ctx->FrameLogCount, fingerNum, pointCount);
                for (i = 0; i < pointCount; i++) {
                    FtLog(ctx, "  pt id %u flag %u x %u y %u p %u a %u",
                          points[i].Id, points[i].Flag, points[i].X,
                          points[i].Y, points[i].Pressure, points[i].Area);
                }
            }
            ctx->FrameLogCount++;
        } else {
            /* No new frame latched: end of burst. */
            /* Diagnostic: show WHY a read produced no frame (status +
               the first raw payload bytes), capped to stay readable. */
            static LONG failLogCount = 0;
            LONG n = InterlockedIncrement(&failLogCount);
            if (n <= 40 || (n % 50) == 0) {
                FtLog(ctx, "frame read failed 0x%08X raw %02X %02X %02X %02X %02X %02X int %lu",
                      status, ctx->RxData[0], ctx->RxData[1], ctx->RxData[2],
                      ctx->RxData[3], ctx->RxData[4], ctx->RxData[5],
                      (ULONG)(ctx->TlmmBase ? (TlmmRead(ctx, 81) ? 1 : 0) : 9));
            }
            if ((n % 50) == 0) {
                /* Liveness probe on failure streaks: is the app
                   still answering register reads? */
                UCHAR a3 = 0, a6 = 0;
                (VOID)FtBusRead(ctx, 0xA3, &a3, 1);
                (VOID)FtBusRead(ctx, 0xA6, &a6, 1);
                FtLog(ctx, "chip alive check: A3 %02X A6 %02X", a3, a6);
            }
        }

        if (InterlockedCompareExchange(&ctx->TouchReadAgain, 0, 0) == 0) {
            /* Nobody re-asked. But if the last read produced a
               frame, the chip latches a fresh one every scan
               cycle while the finger is down: keep draining so a
               drag/scroll becomes a continuous report stream
               instead of one frame per poll tick. A failed read
               (no new frame latched) ends the burst. */
            if (NT_SUCCESS(status) && ++drain < 512 &&
                !ctx->HardwareStopping) {
                FtDelayMs(4);
                continue;
            }
            break;
        }
        drain = 0;
    }
    InterlockedExchange(&ctx->TouchReadInFlight, 0);
}
