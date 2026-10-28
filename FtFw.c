/*++
    FtFw.c — FT3680 application firmware download (host-load model).

    The FT3680 on spacewar has no self-starting app firmware: after
    reset it sits in ROM boot mode (boot ID answers, reg 0xD0 reads
    the boot-state value 0x01) and reg 0x01 returns the
    "FW not running" pattern (0xEF...) forever. The Lineage driver
    downloads the app into PRAM at probe time; this file ports
    that flow (focaltech_flash.c: fts_fw_recovery with
    need_reset=false -> fts_pram_write_ecc + fts_pram_start) for
    the FT3680 upgrade settings {rom 56 62, app2_offset 128 KiB,
    ecclen_max 128 KiB, eccok 0xA5, upgsts_boot 0x01,
    length_coefficient 4, spi_pe 0, ecc_delay 5}:

      1. The FW image is read from C:\ft3680_fw.bin (the raw
         FT3680_WXN_M146_V27 app image, 119284 bytes).
      2. The PRAM app size is parsed from the image itself
         (offset 0x100: BE16 code_len + BE16 complement;
         size = code_len * 4).
      3. PRAM write: per 32752-byte packet, ROMBOOT
         SET_PRAM_ADDR (0xAD + addr24) then WRITE (0xAE + data).
      4. ECC check: the chip computes an AL2 FCS over the PRAM
         range (cmd 0xCC, read back with 0xCD) and it must equal
         the host-computed value.
      5. Start: ROMBOOT START_APP (0x08), then the caller polls
         reg 0xA3 for the running app's chip ID.

    All bus traffic reuses the standard framed transfers
    (FtBusWrite / FtBusWriteLarge / FtBusRead).
--*/
#include "Ft3680.h"

#define FT_ROMBOOT_SET_PRAM_ADDR  0xAD
#define FT_ROMBOOT_WRITE          0xAE
#define FT_ROMBOOT_START_APP      0x08
#define FT_ROMBOOT_ECC            0xCC
#define FT_ROMBOOT_ECC_READ       0xCD

#define FT_FW_PACKET_SIZE         (32 * 1024 - 16)
#define FT_FW_MAX_SIZE            (128 * 1024)
#define FT_FW_MIN_SIZE            0x120
#define FT_FW_APP_INFO_OFFSET     0x100
#define FT_FW_LENGTH_COEFFICIENT  4
#define FT_FW_ECC_DELAY_MS        5

static NTSTATUS
FtFwLoadImage(_Out_ PUCHAR* Image, _Out_ ULONG* ImageLen)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    HANDLE handle = NULL;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    PUCHAR buf = NULL;
    NTSTATUS status;
    LARGE_INTEGER offset;
    ULONG len;

    *Image = NULL;
    *ImageLen = 0;

    RtlInitUnicodeString(&path, L"\\DosDevices\\C:\\ft3680_fw.bin");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    status = ZwCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &oa, &iosb,
                          NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwQueryInformationFile(handle, &iosb, &info, sizeof(info),
                                    FileStandardInformation);
    if (!NT_SUCCESS(status)) {
        goto done;
    }
    if (info.EndOfFile.QuadPart < FT_FW_MIN_SIZE ||
        info.EndOfFile.QuadPart > FT_FW_MAX_SIZE) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }
    len = (ULONG)info.EndOfFile.QuadPart;

    buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, len, 'wfTF');
    if (buf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    offset.QuadPart = 0;
    status = ZwReadFile(handle, NULL, NULL, NULL, &iosb, buf, len, &offset, NULL);
    if (NT_SUCCESS(status) && iosb.Information != len) {
        status = STATUS_END_OF_FILE;
    }
    if (NT_SUCCESS(status)) {
        *Image = buf;
        *ImageLen = len;
        buf = NULL;
    }

done:
    if (buf != NULL) {
        ExFreePoolWithTag(buf, 'wfTF');
    }
    if (handle != NULL) {
        ZwClose(handle);
    }
    return status;
}

/* AL2 FCS over 16-bit big-endian words, init 0 (fts_ecc_cal_host). */
static USHORT
FtFwEccHost(_In_reads_(Len) const UCHAR* Data, _In_ ULONG Len)
{
    USHORT ecc = 0;
    ULONG i;
    int j;

    for (i = 0; i + 1 < Len; i += 2) {
        ecc = (USHORT)(ecc ^ ((Data[i] << 8) | Data[i + 1]));
        for (j = 0; j < 16; j++) {
            ecc = (ecc & 1) ? (USHORT)((ecc >> 1) ^ 0x8408) : (USHORT)(ecc >> 1);
        }
    }
    return ecc;
}

static NTSTATUS
FtFwEccTp(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Saddr, _In_ ULONG Len,
          _Out_ PUSHORT Ecc)
{
    UCHAR cmd[7];
    UCHAR value[2] = { 0, 0 };
    NTSTATUS status;

    cmd[0] = FT_ROMBOOT_ECC;
    cmd[1] = (UCHAR)((Saddr >> 16) & 0xFF);
    cmd[2] = (UCHAR)((Saddr >> 8) & 0xFF);
    cmd[3] = (UCHAR)(Saddr & 0xFF);
    cmd[4] = (UCHAR)((Len >> 16) & 0xFF);
    cmd[5] = (UCHAR)((Len >> 8) & 0xFF);
    cmd[6] = (UCHAR)(Len & 0xFF);
    status = FtBusWrite(Ctx, cmd, sizeof(cmd));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* mdelay(2) + setting ecc_delay(5) in the Linux flow */
    FtDelayMs(2 + FT_FW_ECC_DELAY_MS);

    status = FtBusRead(Ctx, FT_ROMBOOT_ECC_READ, value, 2);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Ecc = (USHORT)((value[0] << 8) | value[1]);
    return STATUS_SUCCESS;
}

static NTSTATUS
FtFwWritePramOnce(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(Len) PUCHAR Image,
                  _In_ ULONG Len, _In_ PUCHAR Scratch)
{
    ULONG packets = (Len + FT_FW_PACKET_SIZE - 1) / FT_FW_PACKET_SIZE;
    ULONG i;
    NTSTATUS status;

    for (i = 0; i < packets; i++) {
        ULONG offset = i * FT_FW_PACKET_SIZE;
        ULONG plen = Len - offset;
        UCHAR setAddr[4];

        if (plen > FT_FW_PACKET_SIZE) {
            plen = FT_FW_PACKET_SIZE;
        }
        if (Ctx->HardwareStopping) {
            return STATUS_DEVICE_NOT_READY;
        }

        setAddr[0] = FT_ROMBOOT_SET_PRAM_ADDR;
        setAddr[1] = (UCHAR)((offset >> 16) & 0xFF);
        setAddr[2] = (UCHAR)((offset >> 8) & 0xFF);
        setAddr[3] = (UCHAR)(offset & 0xFF);
        status = FtBusWrite(Ctx, setAddr, sizeof(setAddr));
        if (!NT_SUCCESS(status)) {
            FtLog(Ctx, "fw: set pram addr pkt %lu -> 0x%08X", i, status);
            return status;
        }

        Scratch[0] = FT_ROMBOOT_WRITE;
        RtlCopyMemory(&Scratch[1], &Image[offset], plen);
        status = FtBusWriteLarge(Ctx, Scratch, 1 + plen);
        if (!NT_SUCCESS(status)) {
            FtLog(Ctx, "fw: write pram pkt %lu (%lu bytes) -> 0x%08X",
                  i, plen, status);
            return status;
        }
        FtLog(Ctx, "fw: pram pkt %lu/%lu written (%lu bytes)",
              i + 1, packets, plen);
    }

    /* ECC over the whole PRAM range (ecclen_max 128 KiB >= Len). */
    {
        USHORT host = FtFwEccHost(Image, Len);
        USHORT tp = 0;

        status = FtFwEccTp(Ctx, 0, Len, &tp);
        if (!NT_SUCCESS(status)) {
            FtLog(Ctx, "fw: ecc tp read -> 0x%08X", status);
            return status;
        }
        FtLog(Ctx, "fw: ecc host %04X tp %04X", host, tp);
        if (host != tp) {
            return STATUS_CRC_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS
FtFwDownload(_In_ PDEVICE_CONTEXT Ctx)
{
    PUCHAR image = NULL;
    PUCHAR scratch = NULL;
    ULONG imageLen = 0;
    ULONG appSize;
    USHORT codeLen, codeLenN;
    NTSTATUS status;
    int attempt;

    status = FtFwLoadImage(&image, &imageLen);
    if (!NT_SUCCESS(status)) {
        FtLog(Ctx, "fw: cannot load C:\\ft3680_fw.bin -> 0x%08X", status);
        return status;
    }
    FtLog(Ctx, "fw: image loaded, %lu bytes", imageLen);

    codeLen = (USHORT)((image[FT_FW_APP_INFO_OFFSET] << 8) |
                       image[FT_FW_APP_INFO_OFFSET + 1]);
    codeLenN = (USHORT)((image[FT_FW_APP_INFO_OFFSET + 2] << 8) |
                        image[FT_FW_APP_INFO_OFFSET + 3]);
    if ((USHORT)(codeLen + codeLenN) != 0xFFFF) {
        FtLog(Ctx, "fw: bad app info len pair %04X %04X", codeLen, codeLenN);
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }
    appSize = (ULONG)codeLen * FT_FW_LENGTH_COEFFICIENT;
    FtLog(Ctx, "fw: pram app size %lu", appSize);
    if (appSize < FT_FW_MIN_SIZE || appSize > imageLen) {
        status = STATUS_INVALID_IMAGE_FORMAT;
        goto done;
    }

    scratch = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                      1 + FT_FW_PACKET_SIZE, 'wfTF');
    if (scratch == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }

    status = STATUS_DEVICE_NOT_READY;
    for (attempt = 0; attempt < 3; attempt++) {
        UCHAR startCmd = FT_ROMBOOT_START_APP;

        FtLog(Ctx, "fw: download attempt %d", attempt);
        status = FtFwWritePramOnce(Ctx, image, appSize, scratch);
        if (!NT_SUCCESS(status)) {
            FtLog(Ctx, "fw: write/ecc failed -> 0x%08X", status);
            continue;
        }
        status = FtBusWrite(Ctx, &startCmd, 1);
        if (!NT_SUCCESS(status)) {
            FtLog(Ctx, "fw: start app -> 0x%08X", status);
            continue;
        }
        FtDelayMs(10);
        FtLog(Ctx, "fw: app started");
        status = STATUS_SUCCESS;
        break;
    }

done:
    if (scratch != NULL) {
        ExFreePoolWithTag(scratch, 'wfTF');
    }
    if (image != NULL) {
        ExFreePoolWithTag(image, 'wfTF');
    }
    return status;
}
