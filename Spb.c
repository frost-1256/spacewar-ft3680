/*++
    Spb.c — SPB (SPI) transport and the FocalTech "SPI V2" framing
    (spec SS1). All bus traffic is one IOCTL_SPB_EXECUTE_SEQUENCE per frame,
    which keeps CS asserted across the entries of the sequence.

    Documented deviation from the Linux driver: Linux uses a single
    full-duplex spi_sync and checks status bits clocked in at RX byte 3
    during the header phase. SPB transfers are one-direction per entry, so
    those in-header status bits cannot be observed here. Writes are
    therefore unverified at the bus layer (the boot-ID read-back in Ft3680.c
    is the end-to-end check) and reads rely on the CRC-16 check.
--*/
#include "Ft3680.h"

#define SPB_RETRY_COUNT 3
#define FT_CS_GAP_US    150        /* spec SS1.2: CS high time between frames */

/* Build the RESOURCE_HUB path for a connection resource and open it. */
NTSTATUS
FtOpenConnectionTarget(_In_ WDFDEVICE Device, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn,
                       _Out_ WDFIOTARGET* Target)
{
    WCHAR pathBuffer[RESOURCE_HUB_PATH_SIZE];
    UNICODE_STRING path;
    WDFIOTARGET target;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    NTSTATUS status;

    LARGE_INTEGER connId;

    connId.LowPart  = Conn->u.Connection.IdLowPart;
    connId.HighPart = Conn->u.Connection.IdHighPart;

    RtlInitEmptyUnicodeString(&path, pathBuffer, sizeof(pathBuffer));
    status = RtlUnicodeStringPrintf(&path, L"%s%016I64x",
                                    RESOURCE_HUB_DEVICE_NAME_PREFIX,
                                    connId.QuadPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    FtLog(DeviceGetContext(Device), "conn path %S", pathBuffer);
    {
        /* Raw probe: separates a WDF-target quirk from a provider refusal. */
        OBJECT_ATTRIBUTES oa;
        HANDLE rawHandle = NULL;
        IO_STATUS_BLOCK rawIosb;
        NTSTATUS rawStatus;
        InitializeObjectAttributes(&oa, &path,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        rawStatus = ZwCreateFile(&rawHandle, GENERIC_READ | GENERIC_WRITE, &oa,
                                 &rawIosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                 0, NULL, 0);
        FtLog(DeviceGetContext(Device), "raw ZwCreateFile -> 0x%08X", rawStatus);
        if (NT_SUCCESS(rawStatus)) {
            ZwClose(rawHandle);
        }
    }
    status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, &target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &path,
                                                STANDARD_RIGHTS_ALL);
    openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
    status = WdfIoTargetOpen(target, &openParams);
    if (!NT_SUCCESS(status)) {
        NTSTATUS status2;
        WdfIoTargetClose(target);
        WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &path,
                                                    GENERIC_READ | GENERIC_WRITE);
        openParams.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
        status2 = WdfIoTargetOpen(target, &openParams);
        if (NT_SUCCESS(status2)) {
            *Target = target;
            return STATUS_SUCCESS;
        }
        WdfObjectDelete(target);
        return status;
    }
    *Target = target;
    return STATUS_SUCCESS;
}

/* Raw synchronous handle to a RESOURCE_HUB connection (same open that the
   probe in FtOpenConnectionTarget performs, but the handle is kept).
   Used for the GPIO pin IOCTLs, bypassing the WDF I/O-target layer. */
NTSTATUS
FtOpenConnectionHandle(_In_ PDEVICE_CONTEXT Ctx, _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn,
                       _Out_ PHANDLE Handle)
{
    WCHAR pathBuffer[RESOURCE_HUB_PATH_SIZE];
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER connId;
    NTSTATUS status;

    *Handle = NULL;
    connId.LowPart  = Conn->u.Connection.IdLowPart;
    connId.HighPart = Conn->u.Connection.IdHighPart;

    RtlInitEmptyUnicodeString(&path, pathBuffer, sizeof(pathBuffer));
    status = RtlUnicodeStringPrintf(&path, L"%s%016I64x",
                                    RESOURCE_HUB_DEVICE_NAME_PREFIX,
                                    connId.QuadPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ZwCreateFile(Handle, GENERIC_READ | GENERIC_WRITE, &oa,
                          &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                          0, NULL, 0);
    FtLog(Ctx, "raw handle open -> 0x%08X", status);
    return status;
}

NTSTATUS
SpbOpenTarget(_In_ WDFDEVICE Device, _In_ PDEVICE_CONTEXT Ctx,
              _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn)
{
    return FtOpenConnectionTarget(Device, Conn, &Ctx->SpbTarget);
}

USHORT
FtCrcKermit(_In_reads_(Len) PUCHAR Data, _In_ ULONG Len)
{
    /* CRC-16, reflected poly 0x8408, init 0xFFFF, LSB-first (spec SS1.4). */
    USHORT crc = 0xFFFF;
    ULONG i;
    for (i = 0; i < Len; i++) {
        int bit;
        crc = (USHORT)(crc ^ Data[i]);
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (USHORT)((crc >> 1) ^ 0x8408) : (USHORT)(crc >> 1);
        }
    }
    return crc;
}

/* One SPB sequence: entries described by (Dir[i], Buf[i], Len[i]). */
static NTSTATUS
SpbExecute(_In_ PDEVICE_CONTEXT Ctx,
           _In_ ULONG EntryCount,
           _In_reads_(EntryCount) const SPB_TRANSFER_DIRECTION* Dirs,
           _In_reads_(EntryCount) PUCHAR* Bufs,
           _In_reads_(EntryCount) const ULONG* Lens)
{
    PSPB_TRANSFER_LIST list = (PSPB_TRANSFER_LIST)Ctx->SpbListBuffer;
    WDF_MEMORY_DESCRIPTOR inDesc;
    ULONG i;
    NTSTATUS status;

    if (Ctx->UseBb) {
        return BbExecute(Ctx, EntryCount, Dirs, Bufs, Lens);
    }

    RtlZeroMemory(list, Ctx->SpbListBufferSize);
    SPB_TRANSFER_LIST_INIT(list, EntryCount);

    for (i = 0; i < EntryCount; i++) {
        list->Transfers[i] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
            Dirs[i], 0, Bufs[i], Lens[i]);
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, list, Ctx->SpbListBufferSize);
    status = WdfIoTargetSendIoctlSynchronously(Ctx->SpbTarget, NULL,
                                               IOCTL_SPB_EXECUTE_SEQUENCE,
                                               &inDesc, NULL, NULL, NULL);
    return status;
}

/* Framed register write (spec SS1.3). WriteBuf[0] = register address. */
NTSTATUS
FtBusWrite(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(WriteLen) PUCHAR WriteBuf, _In_ ULONG WriteLen)
{
    UCHAR frame[FT_SPI_MAX_FRAME];
    ULONG dataLen = WriteLen - 1;
    ULONG frameLen = FT_SPI_HEAD_LEN + dataLen;
    SPB_TRANSFER_DIRECTION dir = SpbTransferDirectionToDevice;
    PUCHAR bufs[1];
    ULONG lens[1];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    int attempt;

    if (WriteLen < 1 || frameLen > sizeof(frame)) {
        return STATUS_INVALID_PARAMETER;
    }

    frame[0] = WriteBuf[0];
    frame[1] = 0x00;                              /* WRITE_CMD */
    frame[2] = (UCHAR)((dataLen >> 8) & 0xFF);
    frame[3] = (UCHAR)(dataLen & 0xFF);
    frame[4] = frame[5] = frame[6] = 0;           /* dummy bytes */
    if (dataLen > 0) {
        RtlCopyMemory(&frame[FT_SPI_HEAD_LEN], &WriteBuf[1], dataLen);
    }

    bufs[0] = frame;
    lens[0] = frameLen;

    KeWaitForSingleObject(&Ctx->BusMutex, Executive, KernelMode, FALSE, NULL);
    for (attempt = 0; attempt < SPB_RETRY_COUNT; attempt++) {
        status = SpbExecute(Ctx, 1, &dir, bufs, lens);
        if (NT_SUCCESS(status)) {
            break;
        }
        KeStallExecutionProcessor(FT_CS_GAP_US);
    }
    KeReleaseMutex(&Ctx->BusMutex, FALSE);
    return status;
}

/* Framed write for payloads too large for FtBusWrite's fixed frame
   buffer (the FW download's 32 KiB PRAM packets). Same framing,
   but the head and the caller's data go out as two entries of one
   sequence, so CS stays asserted and nothing is copied. */
NTSTATUS
FtBusWriteLarge(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(WriteLen) PUCHAR WriteBuf,
                _In_ ULONG WriteLen)
{
    UCHAR head[FT_SPI_HEAD_LEN];
    ULONG dataLen = WriteLen - 1;
    SPB_TRANSFER_DIRECTION dirs[2] = { SpbTransferDirectionToDevice,
                                       SpbTransferDirectionToDevice };
    PUCHAR bufs[2];
    ULONG lens[2];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    int attempt;

    if (WriteLen < 1 || dataLen > 0xFFFF) {
        return STATUS_INVALID_PARAMETER;
    }

    head[0] = WriteBuf[0];
    head[1] = 0x00;                               /* WRITE_CMD */
    head[2] = (UCHAR)((dataLen >> 8) & 0xFF);
    head[3] = (UCHAR)(dataLen & 0xFF);
    head[4] = head[5] = head[6] = 0;              /* dummy bytes */

    bufs[0] = head;         lens[0] = FT_SPI_HEAD_LEN;
    bufs[1] = &WriteBuf[1]; lens[1] = dataLen;

    KeWaitForSingleObject(&Ctx->BusMutex, Executive, KernelMode, FALSE, NULL);
    for (attempt = 0; attempt < SPB_RETRY_COUNT; attempt++) {
        status = SpbExecute(Ctx, 2, dirs, bufs, lens);
        if (NT_SUCCESS(status)) {
            break;
        }
        KeStallExecutionProcessor(FT_CS_GAP_US);
    }
    KeReleaseMutex(&Ctx->BusMutex, FALSE);
    return status;
}

/* Framed register read (spec SS1.4), CRC-verified, retried. */
NTSTATUS
FtBusRead(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Address,
          _Out_writes_(DataLen) PUCHAR Data, _In_ ULONG DataLen)
{
    SPB_TRANSFER_DIRECTION dirs[2] = { SpbTransferDirectionToDevice,
                                       SpbTransferDirectionFromDevice };
    PUCHAR bufs[2];
    ULONG lens[2];
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    int attempt;

    Ctx->TxHead[0] = Address;
    Ctx->TxHead[1] = 0xA0;                        /* READ_CMD = 0x80 | CRC_EN */
    Ctx->TxHead[2] = (UCHAR)((DataLen >> 8) & 0xFF);
    Ctx->TxHead[3] = (UCHAR)(DataLen & 0xFF);
    Ctx->TxHead[4] = Ctx->TxHead[5] = Ctx->TxHead[6] = 0;

    bufs[0] = Ctx->TxHead;       lens[0] = FT_SPI_HEAD_LEN;
    bufs[1] = Ctx->RxData;       lens[1] = DataLen + 2;   /* data + CRC16 */

    KeWaitForSingleObject(&Ctx->BusMutex, Executive, KernelMode, FALSE, NULL);
    for (attempt = 0; attempt < SPB_RETRY_COUNT; attempt++) {
        RtlZeroMemory(Ctx->RxData, DataLen + 2);
        status = SpbExecute(Ctx, 2, dirs, bufs, lens);
        if (NT_SUCCESS(status)) {
            USHORT crcCalc = FtCrcKermit(Ctx->RxData, DataLen);
            USHORT crcRead = (USHORT)((Ctx->RxData[DataLen + 1] << 8) |
                                      Ctx->RxData[DataLen]);
            if (crcCalc == crcRead) {
                RtlCopyMemory(Data, Ctx->RxData, DataLen);
                break;
            }
            FtLog(Ctx, "crc fail: raw %02X %02X %02X %02X %02X %02X calc %04X read %04X",
                  Ctx->RxData[0], Ctx->RxData[1], Ctx->RxData[2], Ctx->RxData[3],
                  Ctx->RxData[4], Ctx->RxData[5], crcCalc, crcRead);
            status = STATUS_CRC_ERROR;
        }
        KeStallExecutionProcessor(FT_CS_GAP_US);
    }
    KeReleaseMutex(&Ctx->BusMutex, FALSE);
    return status;
}

NTSTATUS
SpbReadRegister(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR Address,
                _Out_writes_(DataLen + 2) PUCHAR Out, _In_ ULONG DataLen)
{
    return FtBusRead(Ctx, Address, Out, DataLen);
}
