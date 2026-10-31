/*++
    Ft3680.h — FocalTech FT3680 SPI touch driver for spacewar (SM7325).

    KMDF function driver bound to ACPI\FTSX3680 (child of the QUP1 SE5 SPI
    controller \_SB.SP14), with mshidkmdf as upper filter presenting it as a
    HID Touch Screen. Protocol per winboot/touch_ft3680_spec.md (extracted
    from the LineageOS focaltech_touch V3.4 driver).
--*/
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <reshub.h>
#include <spb.h>
#include <gpio.h>
#include <hidport.h>

#define FT_POOL_TAG '0T3F'

/* Panel / chip constants (spec SS0, SS3; DESIGN.md for the ACPI side). */
#define FT_PANEL_MAX_X        1080
#define FT_PANEL_MAX_Y        2400
#define FT_MAX_POINTS         10
#define FT_REPORT_DATA_LEN    62        /* reg 0x01 frame: 10*6 + 2 */
#define FT_SPI_HEAD_LEN       7         /* addr + cmd + len16 + 3 dummy */
#define FT_SPI_MAX_FRAME      (FT_SPI_HEAD_LEN + FT_REPORT_DATA_LEN + 2)

/* FT3680 registers / commands (spec SS2.6). */
#define FT_REG_TOUCH_DATA     0x01
#define FT_REG_CHIP_ID        0xA3
#define FT_REG_POWER_MODE     0xA5
#define FT_POWER_MODE_SLEEP   0x03
#define FT_CMD_START1         0x55
#define FT_CMD_START2         0xAA
#define FT_CMD_READ_ID        0x90
#define FT_CMD_START_DELAY_MS 12

/* Boot IDs accepted by fts_get_chip_types (spec SS2.2): ROM or PB ID. */
#define FT_ID_ROM_H 0x56
#define FT_ID_ROM_L 0x62
#define FT_ID_PB_H  0x56
#define FT_ID_PB_L  0xE2

/* HID report layout: ReportId(1) + ContactCount(1) + 10 * 7-byte records. */
#define FT_HID_REPORT_ID      1
#define FT_HID_CONTACT_LEN    6
#define FT_HID_REPORT_LEN     (1 + 1 + FT_MAX_POINTS * FT_HID_CONTACT_LEN)

typedef struct _FT_TOUCH_POINT {
    UCHAR Id;
    UCHAR Flag;        /* 0 = down, 1 = up, 2 = contact */
    USHORT X;
    USHORT Y;
    UCHAR Pressure;
    UCHAR Area;
} FT_TOUCH_POINT;

typedef enum _FT_DEVICE_STATE {
    FtStateUninitialized = 0,
    FtStateReady,
    FtStateError
} FT_DEVICE_STATE;

typedef struct _DEVICE_CONTEXT {
    /* Resources / targets */
    WDFIOTARGET SpbTarget;          /* SPI controller (RESOURCE_HUB) */
    CM_PARTIAL_RESOURCE_DESCRIPTOR SpbDescriptor;  /* stashed for D0 retries */
    BOOLEAN HasSpbDescriptor;
    WDFIOTARGET GpioTarget;         /* GPIO controller (RESOURCE_HUB) */
    BOOLEAN     GpioTargetOpened;
    ULONG       GpioResetPin;       /* 105 from _CRS */
    HANDLE      GpioHandle;         /* raw handle for pin IOCTLs (bypasses
                                       the WDF target layer: WDF_VIOLATION
                                       suspect since the first sync send) */
    HANDLE      IoEvent;            /* shared completion event for raw IOCTLs */
    PVOID       TlmmBase;           /* TLMM MMIO mapping (direct pin control) */

    /* Bit-banged SPI over GPIO (fallback when the SPB controller refuses
       client opens). _CRS order: MOSI(52), MISO(53), CLK(54), CS(55). */
    WDFIOTARGET BbTargets[4];
    BOOLEAN     BbOpened[4];
    BOOLEAN     UseBb;
    HANDLE      BbHandles[4];
    WDFINTERRUPT Interrupt;

    /* SPB transfer scratch (PASSIVE_LEVEL use only) */
    PUCHAR      SpbListBuffer;      /* SPB_TRANSFER_LIST + 2 entries */
    ULONG       SpbListBufferSize;
    UCHAR       TxHead[FT_SPI_HEAD_LEN];
    UCHAR       RxData[FT_REPORT_DATA_LEN + 2];   /* data + CRC16 */

    /* HID pending reads (manual queue) */
    WDFQUEUE    HidReadQueue;

    /* Work items */
    WDFWORKITEM BringUpWorkItem;
    WDFWORKITEM TouchWorkItem;
    WDFTIMER    TouchPollTimer;     /* GPIO81 poll fallback: the ACPI
                                       GpioInt does not connect on this
                                       platform (WdfInterruptCreate
                                       fails), so poll the INT line. */
    ULONG       PollTicks;          /* diagnostic cadence counter */
    LONG        IntLowLogged;       /* one-shot: INT fall observed */
    volatile LONG TouchReadInFlight;
    volatile LONG TouchReadAgain;
    volatile LONG HardwareStopping;
    ULONG BbPins[4];            /* role -> TLMM pin (scan-mutable) */
    BOOLEAN BbPinsConfigured;   /* ReleaseHardware racing guard */

    /* Chip state */
    FT_DEVICE_STATE State;
    BOOLEAN BroughtUpOnce;
    UCHAR   BootId[2];

    /* Logging */
    KMUTEX LogMutex;
    KMUTEX BusMutex;
    ULONG  FrameLogCount;

    /* Auto pin-restore: rate-limit window + counters (Gpio.c) */
    ULONGLONG AutoWindowStart;
    ULONG  AutoWindowCount;
    ULONG  AutoAttempts;
    ULONG  AutoRepairs;
    BOOLEAN AutoUnknownLogged;
    BOOLEAN AutoLimitLogged;

    /* Last built HID input report (ReportId excluded; stored from byte 1) */
    UCHAR   LastReport[FT_HID_REPORT_LEN];
    BOOLEAN LastReportValid;
    USHORT  ActiveMask;             /* contact IDs currently reported down */
    USHORT  LastX[FT_MAX_POINTS];   /* last position per contact ID (for */
    USHORT  LastY[FT_MAX_POINTS];   /* synthesized release records) */
    FT_TOUCH_POINT LastPoints[FT_MAX_POINTS]; /* last reported frame, */
    ULONG   LastPointCount;         /* for duplicate suppression */
    BOOLEAN LastFrameValid;
    USHORT  UpPendingMask;          /* releases held by the debounce */
    USHORT  UpPendingAged;          /* pending releases that already
                                       survived one no-frame read */
    FT_TOUCH_POINT UpPendingRec[FT_MAX_POINTS]; /* release record to
                                       emit per pending contact ID */
    UCHAR   ZeroStreak[FT_MAX_POINTS]; /* consecutive frames in which
                                       this ID reported raw pressure
                                       and area both zero (ghost
                                       filter in FtReadTouchFrame) */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

/* Driver.c */
/* Bit-bang role indices into Ctx->BbPins[]. */
#define BB_MOSI 0
#define BB_MISO 1
#define BB_CLK  2
#define BB_CS   3

VOID FtChipReset(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN FtBbPinScan(_In_ PDEVICE_CONTEXT Ctx);
VOID TlmmWrite(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin, _In_ BOOLEAN Value);
BOOLEAN TlmmRead(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD FtEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL FtEvtIoDeviceControl;
VOID FtEvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                                  _In_ size_t OutputBufferLength,
                                  _In_ size_t InputBufferLength,
                                  _In_ ULONG IoControlCode);
EVT_WDF_IO_IN_CALLER_CONTEXT FtEvtIoInCallerContext;

/* Device.c */
EVT_WDF_DEVICE_PREPARE_HARDWARE FtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE FtEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY FtEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT FtEvtD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL FtEvtSurpriseRemoval;
EVT_WDF_DEVICE_QUERY_REMOVE FtEvtQueryRemove;
EVT_WDF_DEVICE_QUERY_STOP FtEvtQueryStop;
EVT_WDF_INTERRUPT_ISR FtEvtInterruptIsr;
EVT_WDF_WORKITEM FtEvtBringUpWorkItem;
EVT_WDF_WORKITEM FtEvtTouchWorkItem;
EVT_WDF_TIMER FtEvtTouchPollTimer;

/* Spb.c */
NTSTATUS FtOpenConnectionTarget(_In_ WDFDEVICE Device, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn, _Out_ WDFIOTARGET* Target);
NTSTATUS FtOpenConnectionHandle(_In_ PDEVICE_CONTEXT Ctx, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn, _Out_ PHANDLE Handle);
NTSTATUS SpbOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn);
NTSTATUS SpbReadRegister(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Address, _Out_writes_(DataLen + 2) PUCHAR Out, _In_ ULONG DataLen);
NTSTATUS FtBusWrite(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(WriteLen) PUCHAR WriteBuf, _In_ ULONG WriteLen);
NTSTATUS FtBusWriteLarge(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(WriteLen) PUCHAR WriteBuf, _In_ ULONG WriteLen);
NTSTATUS FtFwDownload(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS FtBusRead(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Address, _Out_writes_(DataLen) PUCHAR Data, _In_ ULONG DataLen);
USHORT FtCrcKermit(_In_reads_(Len) PUCHAR Data, _In_ ULONG Len);

/* Gpio.c */
NTSTATUS GpioOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn);
NTSTATUS GpioWritePin(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Pin, _In_ BOOLEAN Value);
NTSTATUS BbOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn, _In_ ULONG Slot);
NTSTATUS BbExecute(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG EntryCount, _In_ const SPB_TRANSFER_DIRECTION* Dirs, _In_ PUCHAR* Bufs, _In_ const ULONG* Lens);
NTSTATUS TlmmInit(_In_ PDEVICE_CONTEXT Ctx);

/* Ft3680.c (chip layer + logging) */
VOID FtLog(_In_ PDEVICE_CONTEXT Ctx, _In_ PCSTR Format, ...);
NTSTATUS FtChipBringUp(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS FtChipWaitValid(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS FtReadTouchFrame(_In_ PDEVICE_CONTEXT Ctx, _Out_ FT_TOUCH_POINT* Points, _Out_ ULONG* PointCount, _Out_ UCHAR* FingerNum);
NTSTATUS FtChipSleep(_In_ PDEVICE_CONTEXT Ctx);
VOID FtDelayMs(_In_ ULONG Ms);

/* Hid.c */
extern const UCHAR g_FtHidReportDescriptor[];
extern const ULONG g_FtHidReportDescriptorSize;
VOID HidHandleIoctl(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _In_ ULONG IoControlCode, _In_ size_t InLen, _In_ size_t OutLen);
VOID HidCompleteReadWithReport(_In_ PDEVICE_CONTEXT Ctx);
VOID HidBuildReport(_In_ PDEVICE_CONTEXT Ctx, _In_ const FT_TOUCH_POINT* Points, _In_ ULONG PointCount);

/* ntddk.h in this kit does not declare the flush API; ntoskrnl exports it. */
extern NTSTATUS ZwFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);
extern NTSTATUS NTAPI ZwCreateEvent(_Out_ PHANDLE EventHandle, _In_ ACCESS_MASK DesiredAccess, _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes, _In_ EVENT_TYPE EventType, _In_ BOOLEAN InitialState);
extern NTSTATUS NTAPI ZwWaitForSingleObject(_In_ HANDLE Handle, _In_ BOOLEAN Alertable, _In_opt_ PLARGE_INTEGER Timeout);
