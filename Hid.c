/*++
    Hid.c -- HID minidriver surface for the mshidkmdf shim: descriptors,
    attribute/feature IOCTLs, and the pended READ_REPORT path.

    Report descriptor: flat layout (one Application collection, Contact
    Count + 10 fixed contact records + a Contact Count Maximum feature).
    See README.md for why this layout was chosen and its acceptance risk.
--*/
#include "Ft3680.h"

const UCHAR g_FtHidReportDescriptor[] = {
    0x05, 0x0D,                                     /* Usage Page (Digitizer) */
    0x09, 0x04,                                     /* Usage (Touch Screen) */
    0xA1, 0x01,                                     /* Collection (Application) */
        0x85, 0x01,                                 /*   Report ID (1) */
        0x09, 0x54,                                 /*   Usage (Contact Count) */
        0x15, 0x00, 0x25, 0x0A,                     /*   Logical Min 0, Max 10 */
        0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
        /* --- contact records x10, each in its own Finger logical
               collection (the form Microsoft's pointer stack
               expects; record: 6 bytes. Tip Pressure is deliberately ABSENT: with it, the WinRT pointer path fail-fasts in combase.dll (E_INVALIDARG) and XAML hosts (Explorer/Settings/Start) crash on touch - device-proven 2026-10-28) --- */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        0x09, 0x22,                                     /* Usage (Finger) */
        0xA1, 0x02,                                     /* Collection (Logical) */
            0x09, 0x51,                                 /*   Usage (Contact Identifier) */
            0x15, 0x00, 0x25, 0x09,                     /*   Logical Min 0, Max 9 */
            0x75, 0x08, 0x95, 0x01, 0x81, 0x02,         /*   Input (Data,Var,Abs) */
            0x09, 0x42,                                 /*   Usage (Tip Switch) */
            0x09, 0x32,                                 /*   Usage (In Range) */
            0x15, 0x00, 0x25, 0x01,                     /*   Logical Min 0, Max 1 */
            0x75, 0x01, 0x95, 0x02, 0x81, 0x02,         /*   Input */
            0x75, 0x06, 0x95, 0x01, 0x81, 0x03,         /*   Input (Cnst) padding */
            0x05, 0x01,                                 /*   Usage Page (Generic Desktop) */
            0x09, 0x30,                                 /*   Usage (X) */
            0x16, 0x00, 0x00, 0x26, 0x38, 0x04,         /*   Logical 0..1080 */
            0x36, 0x00, 0x00, 0x46, 0x38, 0x04,         /*   Physical 0..1080 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
            0x09, 0x31,                                 /*   Usage (Y) */
            0x16, 0x00, 0x00, 0x26, 0x60, 0x09,         /*   Logical 0..2400 */
            0x36, 0x00, 0x00, 0x46, 0x60, 0x09,         /*   Physical 0..2400 */
            0x75, 0x10, 0x95, 0x01, 0x81, 0x02,         /*   Input */
        0xC0,                                           /* End Collection */
        /* --- end of contact records --- */
        0x05, 0x0D,                                 /*   Usage Page (Digitizer) */
        0x09, 0x55,                                 /*   Usage (Contact Count Maximum) */
        0x15, 0x00, 0x25, 0x0A,                     /*   Logical Min 0, Max 10 */
        0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,         /*   Feature (Data,Var,Abs) */
    0xC0                                            /* End Collection */
};

const ULONG g_FtHidReportDescriptorSize = sizeof(g_FtHidReportDescriptor);

static const HID_DESCRIPTOR g_FtHidDescriptor = {
    sizeof(HID_DESCRIPTOR),
    0x21,                       /* bDescriptorType: HID */
    0x0101,                     /* bcdHID 1.01 */
    0x00,                       /* bCountryCode */
    1,                          /* bNumDescriptors */
    { { 0x22, sizeof(g_FtHidReportDescriptor) } }
};

/* Build the current input report (incl. Report ID byte) into the context. */
VOID
HidBuildReport(_In_ PDEVICE_CONTEXT Ctx, _In_ const FT_TOUCH_POINT* Points,
               _In_ ULONG PointCount)
{
    ULONG i;
    ULONG out = 0;
    USHORT presentDown = 0;
    USHORT presentAny = 0;

    RtlZeroMemory(Ctx->LastReport, sizeof(Ctx->LastReport));
    Ctx->LastReport[0] = FT_HID_REPORT_ID;
    for (i = 0; i < PointCount && i < FT_MAX_POINTS; i++) {
        PUCHAR rec = &Ctx->LastReport[2 + out * FT_HID_CONTACT_LEN];
        BOOLEAN tip = (Points[i].Flag == 0 || Points[i].Flag == 2);
        rec[0] = Points[i].Id;
        rec[1] = tip ? 0x03 : 0x00;             /* Tip Switch + In Range */
        rec[2] = (UCHAR)(Points[i].X & 0xFF);
        rec[3] = (UCHAR)(Points[i].X >> 8);
        rec[4] = (UCHAR)(Points[i].Y & 0xFF);
        rec[5] = (UCHAR)(Points[i].Y >> 8);
        out++;
        if (Points[i].Id < FT_MAX_POINTS) {
            presentAny = (USHORT)(presentAny | (1u << Points[i].Id));
            if (tip) {
                presentDown = (USHORT)(presentDown | (1u << Points[i].Id));
                Ctx->LastX[Points[i].Id] = Points[i].X;
                Ctx->LastY[Points[i].Id] = Points[i].Y;
            }
        }
    }
    /* Contacts that were down last frame but are absent now must
       still get an explicit release record: if a frame is dropped
       between the chip and us, the contact otherwise vanishes
       without an UP, and the shell's pointer state machine is
       left holding a stuck finger (a suspected cause of Explorer
       / Start instability). Synthesize the UP at the last known
       position. Total records stay <= 10 (IDs are 0..9). */
    {
        USHORT vanished = (USHORT)(Ctx->ActiveMask & ~presentAny);
        ULONG id;
        for (id = 0; id < FT_MAX_POINTS; id++) {
            if (vanished & (1u << id)) {
                PUCHAR rec = &Ctx->LastReport[2 + out * FT_HID_CONTACT_LEN];
                rec[0] = (UCHAR)id;
                rec[1] = 0x00;                  /* tip up, out of range */
                rec[2] = (UCHAR)(Ctx->LastX[id] & 0xFF);
                rec[3] = (UCHAR)(Ctx->LastX[id] >> 8);
                rec[4] = (UCHAR)(Ctx->LastY[id] & 0xFF);
                rec[5] = (UCHAR)(Ctx->LastY[id] >> 8);
                out++;
            }
        }
    }
    Ctx->ActiveMask = presentDown;
    Ctx->LastReport[1] = (UCHAR)out;            /* Contact Count: records
                                                   in this report */
    Ctx->LastReportValid = TRUE;
}

VOID
HidCompleteReadWithReport(_In_ PDEVICE_CONTEXT Ctx)
{
    WDFREQUEST request;
    NTSTATUS status;

    if (!Ctx->LastReportValid) {
        return;
    }
    status = WdfIoQueueRetrieveNextRequest(Ctx->HidReadQueue, &request);
    if (!NT_SUCCESS(status)) {
        return;   /* nobody is waiting; hidport will ask again */
    }
    {
        PVOID buffer;
        size_t length = 0;
        status = WdfRequestRetrieveOutputBuffer(request, FT_HID_REPORT_LEN,
                                                &buffer, &length);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(buffer, Ctx->LastReport, FT_HID_REPORT_LEN);
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS,
                                              FT_HID_REPORT_LEN);
        } else {
            WdfRequestComplete(request, status);
        }
    }
}

VOID
HidHandleIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request,
               _In_ ULONG IoControlCode, _In_ size_t InLen, _In_ size_t OutLen)
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytesReturned = 0;
    PVOID buffer;
    size_t length = 0;

    UNREFERENCED_PARAMETER(InLen);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DESCRIPTOR),
                                                &buffer, &length);
        if (NT_SUCCESS(status)) {
            RtlCopyMemory(buffer, &g_FtHidDescriptor, sizeof(HID_DESCRIPTOR));
            bytesReturned = sizeof(HID_DESCRIPTOR);
        }
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = WdfRequestRetrieveOutputBuffer(Request, 1, &buffer, &length);
        if (NT_SUCCESS(status)) {
            size_t copy = min(length, (size_t)g_FtHidReportDescriptorSize);
            RtlCopyMemory(buffer, g_FtHidReportDescriptor, copy);
            bytesReturned = copy;
        }
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES),
                                                &buffer, &length);
        if (NT_SUCCESS(status)) {
            PHID_DEVICE_ATTRIBUTES attr = (PHID_DEVICE_ATTRIBUTES)buffer;
            RtlZeroMemory(attr, sizeof(*attr));
            attr->Size = sizeof(HID_DEVICE_ATTRIBUTES);
            attr->VendorID = 0x2808;            /* FocalTech */
            attr->ProductID = 0x3680;           /* FT3680 */
            attr->VersionNumber = 0x0100;
            bytesReturned = sizeof(HID_DEVICE_ATTRIBUTES);
        }
        break;

    case IOCTL_HID_GET_STRING:
        /* Input: ULONG whose low word is the string index
           (1 = manufacturer, 2 = product, 3 = serial).
           Output: NUL-terminated wide string. */
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG),
                                               &buffer, &length);
        if (NT_SUCCESS(status)) {
            ULONG index = (*(PULONG)buffer) & 0xFFFF;
            const WCHAR* text;
            size_t textBytes;

            switch (index) {
            case 1:  text = L"FocalTech"; textBytes = sizeof(L"FocalTech"); break;
            case 3:  text = L"0001"; textBytes = sizeof(L"0001"); break;
            default: text = L"FT3680 Touch Controller";
                     textBytes = sizeof(L"FT3680 Touch Controller"); break;
            }
            status = WdfRequestRetrieveOutputBuffer(Request, textBytes,
                                                    &buffer, &length);
            if (NT_SUCCESS(status)) {
                size_t copy = min(length, textBytes);
                RtlCopyMemory(buffer, text, copy);
                bytesReturned = copy;
            }
        }
        break;

    case IOCTL_HID_READ_REPORT:
        /* Pend it; FtEvtTouchWorkItem completes it with the next frame. */
        status = WdfRequestForwardToIoQueue(Request, Ctx->HidReadQueue);
        if (NT_SUCCESS(status)) {
            return;   /* completed later */
        }
        break;

    case IOCTL_HID_GET_FEATURE:
        status = WdfRequestRetrieveOutputBuffer(Request, 2, &buffer, &length);
        if (NT_SUCCESS(status)) {
            PUCHAR feat = (PUCHAR)buffer;
            feat[0] = FT_HID_REPORT_ID;
            feat[1] = FT_MAX_POINTS;            /* Contact Count Maximum */
            bytesReturned = 2;
        }
        break;

    case IOCTL_HID_SET_FEATURE:
        bytesReturned = 0;
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        break;

    default:
        FtLog(Ctx, "unhandled HID ioctl 0x%08X", IoControlCode);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
    UNREFERENCED_PARAMETER(OutLen);
}
