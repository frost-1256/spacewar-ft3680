/*++
    Driver.c — entry point and device-add for the FT3680 touch driver.
    The default queue routes the HID IOCTLs injected by mshidkmdf to Hid.c.
--*/
#include "Ft3680.h"

/* Load marker: proves the .sys image was loaded and DriverEntry ran,
   independent of any device-context machinery. Tries two paths. */
static VOID
FtWriteLoadMarker(VOID)
{
    static const PCWSTR paths[2] = { L"\\DosDevices\\C:\\ft3680.log",
                                     L"\\SystemRoot\\ft3680-driver-loaded.txt" };
    ULONG i;

    for (i = 0; i < 2; i++) {
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES oa;
        HANDLE handle = NULL;
        IO_STATUS_BLOCK iosb;
        LARGE_INTEGER offset;
        static const CHAR text[] = "FT3680: DriverEntry\r\n";

        RtlInitUnicodeString(&path, paths[i]);
        InitializeObjectAttributes(&oa, &path,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        if (NT_SUCCESS(ZwCreateFile(&handle,
                                    FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb,
                                    NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                    FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT,
                                    NULL, 0))) {
            offset.QuadPart = FILE_WRITE_TO_END_OF_FILE;
            ZwWriteFile(handle, NULL, NULL, NULL, &iosb, (PVOID)text,
                        sizeof(text) - 1, &offset, NULL);
            ZwFlushBuffersFile(handle, &iosb);
            ZwClose(handle);
        }
    }
}

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    FtWriteLoadMarker();
    WDF_DRIVER_CONFIG_INIT(&config, FtEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS
FtEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;
    PDEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = FtEvtPrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = FtEvtReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = FtEvtD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = FtEvtD0Exit;
    pnpPowerCallbacks.EvtDeviceSurpriseRemoval = FtEvtSurpriseRemoval;
    pnpPowerCallbacks.EvtDeviceQueryRemove = FtEvtQueryRemove;
    pnpPowerCallbacks.EvtDeviceQueryStop = FtEvtQueryStop;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    /* mshidkmdf fetches the static HID descriptors from its start
       path, before our power-managed default queue can dispatch.
       Answer those synchronously in the caller's context. */
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit,
                                              FtEvtIoInCallerContext);

    /* HIDCLASS/mshidkmdf is the power-policy owner of the HID stack.
       Relinquish ownership: claiming it duplicates the PPO and KMDF
       bugchecks 0x10D/0xD on the first D0->Dx SetPower (Restart). */
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = DeviceGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->GpioResetPin = 105;   /* from _CRS GpioIo in the ACPI node */
    ctx->State = FtStateUninitialized;
    KeInitializeMutex(&ctx->LogMutex, 0);
    KeInitializeMutex(&ctx->BusMutex, 0);
    FtLog(ctx, "DeviceAdd ok");

    /* Default queue: HID IOCTLs from the mshidkmdf shim land here. */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = FtEvtIoDeviceControl;
    queueConfig.EvtIoInternalDeviceControl = FtEvtIoInternalDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Manual queue where IOCTL_HID_READ_REPORT requests wait for a frame. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &ctx->HidReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Work items (PASSIVE_LEVEL: SPB synchronous IOCTLs + file logging). */
    {
        WDF_WORKITEM_CONFIG workConfig;
        WDF_OBJECT_ATTRIBUTES workAttrs;

        WDF_OBJECT_ATTRIBUTES_INIT(&workAttrs);
        workAttrs.ParentObject = device;

        WDF_WORKITEM_CONFIG_INIT(&workConfig, FtEvtBringUpWorkItem);
        status = WdfWorkItemCreate(&workConfig, &workAttrs, &ctx->BringUpWorkItem);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        WDF_WORKITEM_CONFIG_INIT(&workConfig, FtEvtTouchWorkItem);
        status = WdfWorkItemCreate(&workConfig, &workAttrs, &ctx->TouchWorkItem);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

VOID
FtEvtIoInCallerContext(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS status;
    WDF_REQUEST_PARAMETERS params;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (WdfRequestGetRequestorMode(Request) == KernelMode &&
        (params.Type == WdfRequestTypeDeviceControl ||
         params.Type == WdfRequestTypeDeviceControlInternal)) {
        ULONG code = params.Parameters.DeviceIoControl.IoControlCode;

        /* Static-data queries mshidkmdf issues while the stack is
           still starting: complete them here, the queues are not
           dispatching yet. HidHandleIoctl completes the request. */
        if (code == IOCTL_HID_GET_DEVICE_DESCRIPTOR ||
            code == IOCTL_HID_GET_REPORT_DESCRIPTOR ||
            code == IOCTL_HID_GET_DEVICE_ATTRIBUTES ||
            code == IOCTL_HID_GET_FEATURE) {
            FtLog(ctx, "hid early ioctl 0x%08X (caller context)", code);
            HidHandleIoctl(ctx, Request, code, 0, 0);
            return;
        }
    }

    status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}

VOID
FtEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                     _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength,
                     _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    /* Diagnostic: log every HID ioctl reaching the queue (descriptor
       queries are one-shot; READ_REPORT would repeat per frame). */
    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        FtLog(ctx, "queue ioctl 0x%08X in %Iu out %Iu",
              IoControlCode, InputBufferLength, OutputBufferLength);
    }
    /* HidHandleIoctl completes (or forwards) the request itself. */
    HidHandleIoctl(ctx, Request, IoControlCode, InputBufferLength, OutputBufferLength);
}

VOID
FtEvtIoInternalDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request,
                             _In_ size_t OutputBufferLength,
                             _In_ size_t InputBufferLength,
                             _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    /* hidclass/mshidkmdf speak IRP_MJ_INTERNAL_DEVICE_CONTROL with the
       IOCTL_HID_* codes: this is the channel that matters. */
    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        FtLog(ctx, "queue internal-ioctl 0x%08X in %Iu out %Iu",
              IoControlCode, InputBufferLength, OutputBufferLength);
    }
    HidHandleIoctl(ctx, Request, IoControlCode, InputBufferLength, OutputBufferLength);
}
