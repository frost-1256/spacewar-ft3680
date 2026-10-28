/*++
    Gpio.c — reset-pin control through the GPIO controller (TLMM), via the
    standard controller-relative pins IOCTL. The GpioIo connection resource
    from _CRS opens the GPIO controller through the resource hub; pins are
    then addressed by their _CRS pin number (reset = GPIO105).
--*/
#include "Ft3680.h"

NTSTATUS
GpioOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx,
               _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn)
{
    /* Canonical KMDF path only: WDF target, WDF sends. (Raw-handle opens
       succeeded but GpioClx answered WRITE_PINS with DEVICE_NOT_READY;
       the earlier WDF-send crash build also had double opens in play.) */
    NTSTATUS status = FtOpenConnectionTarget(Device, Conn, &Ctx->GpioTarget);
    if (NT_SUCCESS(status)) {
        Ctx->GpioTargetOpened = TRUE;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* TLMM direct MMIO (bypasses qcgpio/GpioClx entirely: on this platform */
/* the provider answers pin IOCTLs for the QUP SE pins with             */
/* DEVICE_NOT_READY, while the pins themselves are plain TLMM).        */
/* Layout from LineageOS pinctrl-yupik.c: pin regs at TLMM+0x100000,    */
/* stride 0x1000; CTL: pull[1:0] mux[5:2] drv[8:6] oe[9]; IO: in[0]     */
/* out[1]. Function 0 = GPIO.                                          */
/* ------------------------------------------------------------------ */

#define TLMM_PHYS_BASE   0x0F100000   /* ACPI GIO0 window = kernel TLMM base (0xF000000) + REG_BASE (0x100000) */
#define TLMM_MAP_SIZE    0x00300000
#define TLMM_PIN_BASE    0x00000000   /* pin regs start AT the ACPI window base */
#define TLMM_PIN_STRIDE  0x1000
#define TLMM_CTL_OE      (1u << 9)
#define TLMM_CTL_MUX_MSK (0xFu << 2)
#define TLMM_IO_IN       (1u << 0)
#define TLMM_IO_OUT      (1u << 1)

static volatile ULONG*
TlmmPinRegs(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin)
{
    return (volatile ULONG*)((PUCHAR)Ctx->TlmmBase + TLMM_PIN_BASE +
                             Pin * TLMM_PIN_STRIDE);
}

NTSTATUS
TlmmInit(_In_ PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = TLMM_PHYS_BASE;
    Ctx->TlmmBase = MmMapIoSpace(pa, TLMM_MAP_SIZE, MmNonCached);
    if (Ctx->TlmmBase == NULL) {
        FtLog(Ctx, "tlmm: map failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    FtLog(Ctx, "tlmm: mapped at %p (ctx %p fieldaddr %p)", Ctx->TlmmBase, Ctx, &Ctx->TlmmBase);
    return STATUS_SUCCESS;
}

/* Configure a pin as GPIO (function 0); Oe=TRUE for outputs. */
static VOID
TlmmConfigPin(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin, _In_ BOOLEAN Oe)
{
    volatile ULONG* ctl = TlmmPinRegs(Ctx, Pin);
    ULONG v = *ctl;
    v &= ~TLMM_CTL_MUX_MSK;              /* function 0 = GPIO */
    if (Oe) { v |= TLMM_CTL_OE; } else { v &= ~TLMM_CTL_OE; }
    *ctl = v;
}

VOID
TlmmWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin, _In_ BOOLEAN Value)
{
    volatile ULONG* io = TlmmPinRegs(Ctx, Pin) + 1;   /* IO reg = CTL + 4 */
    ULONG v = *io;
    if (Value) { v |= TLMM_IO_OUT; } else { v &= ~TLMM_IO_OUT; }
    *io = v;
}

BOOLEAN
TlmmRead(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin)
{
    volatile ULONG* io = TlmmPinRegs(Ctx, Pin) + 1;
    return (*io & TLMM_IO_IN) != 0;
}

NTSTATUS
GpioWritePin(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin, _In_ BOOLEAN Value)
{
    static LONG logged = 0;
    if (Ctx == NULL || Ctx->TlmmBase == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (InterlockedExchange(&logged, 1) == 0) {
        FtLog(Ctx, "gpio: first pin write via TLMM, irql %lu",
              (ULONG)KeGetCurrentIrql());
    }
    TlmmConfigPin(Ctx, Pin, TRUE);
    TlmmWrite(Ctx, Pin, Value);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Bit-banged SPI over GPIO (fallback transport).                      */
/* Pin slots (ACPI v9 _CRS order): 0=MOSI(52) 1=MISO(53) 2=CLK(54)     */
/* 3=CS(55). SPI mode 0, MSB first.                                    */
/* ------------------------------------------------------------------ */

#define BB_HALF_US  3
#define BB_CS_GAP_US 150

NTSTATUS
BbOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx,
             _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn, _In_ ULONG Slot)
{
    NTSTATUS status = FtOpenConnectionTarget(Device, Conn, &Ctx->BbTargets[Slot]);
    if (NT_SUCCESS(status)) {
        Ctx->BbOpened[Slot] = TRUE;
        FtLog(Ctx, "bb: pin slot %lu open ok", Slot);
    }
    return status;
}

/* TLMM pin numbers live in Ctx->BbPins (role-indexed), default
 * {52, 53, 54, 55} = MOSI MISO CLK CS; the pin scan may permute them. */
#define BbPinNo(Role) (Ctx->BbPins[(Role)])

static UCHAR
BbShiftByte(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Out)
{
    UCHAR in = 0;
    int bit;
    for (bit = 7; bit >= 0; bit--) {
        TlmmWrite(Ctx, BbPinNo(BB_MOSI), (BOOLEAN)((Out >> bit) & 1));
        KeStallExecutionProcessor(BB_HALF_US);
        TlmmWrite(Ctx, BbPinNo(BB_CLK), TRUE);
        KeStallExecutionProcessor(BB_HALF_US);
        if (TlmmRead(Ctx, BbPinNo(BB_MISO))) {
            in |= (UCHAR)(1 << bit);
        }
        TlmmWrite(Ctx, BbPinNo(BB_CLK), FALSE);
        KeStallExecutionProcessor(BB_HALF_US);
    }
    return in;
}

/* Same contract as SpbExecute: CS held across all entries. */
NTSTATUS
BbExecute(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG EntryCount,
          _In_ const SPB_TRANSFER_DIRECTION* Dirs,
          _In_ PUCHAR* Bufs, _In_ const ULONG* Lens)
{
    static LONG logged = 0;
    ULONG e, i;

    if (Ctx->TlmmBase == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (!Ctx->BbPinsConfigured) {
        if (InterlockedExchange(&logged, 1) == 0) {
            FtLog(Ctx, "bb: first execute (TLMM), irql %lu",
                  (ULONG)KeGetCurrentIrql());
        }
        TlmmConfigPin(Ctx, BbPinNo(BB_MOSI), TRUE);
        TlmmConfigPin(Ctx, BbPinNo(BB_MISO), FALSE);
        TlmmConfigPin(Ctx, BbPinNo(BB_CLK), TRUE);
        TlmmConfigPin(Ctx, BbPinNo(BB_CS), TRUE);
        /* Touch INT (GPIO81): mux it to GPIO input with a pull-up
           ourselves. Nothing else configures this pin (the WDF
           interrupt create failed with 0xC020020F), and if a
           previous stage left it muxed to another function, its
           IN bit reads a constant level and INT never asserts --
           the suspected reason frames only ever arrived via the
           slow unconditional poll. */
        {
            volatile ULONG* ctl81 = TlmmPinRegs(Ctx, 81);
            ULONG before = *ctl81;
            ULONG v = before;
            v &= ~TLMM_CTL_MUX_MSK;      /* function 0 = GPIO */
            v &= ~TLMM_CTL_OE;           /* input */
            v = (v & ~0x3u) | 0x3u;      /* bias pull-up */
            *ctl81 = v;
            FtLog(Ctx, "bb: int pin81 ctl %08X -> %08X in %lu",
                  before, *ctl81, (ULONG)TlmmRead(Ctx, 81));
        }
        TlmmWrite(Ctx, BbPinNo(BB_CLK), FALSE);
        TlmmWrite(Ctx, BbPinNo(BB_CS), TRUE);
        Ctx->BbPinsConfigured = TRUE;
        FtLog(Ctx, "bb: pins configured MOSI %lu MISO %lu CLK %lu CS %lu",
              Ctx->BbPins[BB_MOSI], Ctx->BbPins[BB_MISO], Ctx->BbPins[BB_CLK],
              Ctx->BbPins[BB_CS]);
        FtLog(Ctx, "bb: ctl MOSI %08X MISO %08X CLK %08X CS %08X | in MOSI %lu MISO %lu CLK %lu CS %lu",
              *TlmmPinRegs(Ctx, Ctx->BbPins[BB_MOSI]),
              *TlmmPinRegs(Ctx, Ctx->BbPins[BB_MISO]),
              *TlmmPinRegs(Ctx, Ctx->BbPins[BB_CLK]),
              *TlmmPinRegs(Ctx, Ctx->BbPins[BB_CS]),
              (ULONG)TlmmRead(Ctx, Ctx->BbPins[BB_MOSI]),
              (ULONG)TlmmRead(Ctx, Ctx->BbPins[BB_MISO]),
              (ULONG)TlmmRead(Ctx, Ctx->BbPins[BB_CLK]),
              (ULONG)TlmmRead(Ctx, Ctx->BbPins[BB_CS]));
    }

    TlmmWrite(Ctx, BbPinNo(BB_CS), FALSE);   /* assert (low) */
    KeStallExecutionProcessor(BB_HALF_US);

    for (e = 0; e < EntryCount; e++) {
        for (i = 0; i < Lens[e]; i++) {
            if (Dirs[e] == SpbTransferDirectionToDevice) {
                (VOID)BbShiftByte(Ctx, Bufs[e][i]);
            } else if (Dirs[e] == SpbTransferDirectionFromDevice) {
                Bufs[e][i] = BbShiftByte(Ctx, 0x00);
            }
        }
    }

    TlmmWrite(Ctx, BbPinNo(BB_CLK), FALSE);
    TlmmWrite(Ctx, BbPinNo(BB_CS), TRUE);              /* release */
    KeStallExecutionProcessor(BB_CS_GAP_US);
    return STATUS_SUCCESS;
}
