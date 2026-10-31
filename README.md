# spacewar-ft3680

A Windows KMDF touchscreen driver for the **FocalTech FT3680** controller in the
**Nothing Phone (1)** (codename `spacewar`, SM7325), for Windows 11 ARM64 running
via the [Mu-Silicium](https://github.com/frost-1256/Mu-Silicium) UEFI port.

Tap, release and long-press (press-and-hold) work. Verified on real hardware
2026-10-28.

## Why this driver looks the way it does

The FT3680 hangs off a QUP SPI slave (SE5 / SPI13) that Windows' SPB stack
refuses to open on this platform, so the driver **bit-bangs SPI through the
TLMM GPIO registers** (phys 0x0F100000; MOSI 53 / MISO 52 / CLK 54 / CS 55).
Highlights:

- **Framed SPI V2 protocol** with CRC-16/Kermit, as documented from the
  LineageOS `focaltech_touch` driver (see References).
- **Host firmware download on every boot**: the FT3680 boots into ROM and its
  application firmware is volatile, so the driver loads `C:\ft3680_fw.bin`
  into PRAM at D0 entry (ECC-verified), then starts the app firmware.
- **HID over `mshidkmdf`** (UpperFilters). Implements the IOCTL_HID_* surface
  including `IOCTL_HID_GET_STRING` — without it, hidclass silently refuses to
  adopt the device. The report descriptor is the Microsoft minimal 10-finger
  form (contact ID / tip / in-range / X / Y, 62-byte reports).
  *Do not add Tip Pressure*: a descriptor carrying it crashed the Windows
  shell (combase.dll fail-fast, E_INVALIDARG) on this stack.
- **Polling, not interrupts**: the panel IRQ line (GPIO81) arrives configured
  as an output and never signals, so a 10 ms timer polls it and drains frames.
- **Ghost-contact filter**: with the ADSP subsystem running, the panel emits
  stuck records with raw pressure = 0 and raw area = 0; a record is dropped
  only after 4 consecutive all-zero frames for the same contact ID.
- **Rescan-proof (auto pin restore)**: a PnP rescan (`pnputil /scan-devices`,
  Device Manager rescan) re-muxes the bit-bang SPI pins GPIO52-55 and
  silently kills communication with the chip. Before every bus transaction
  the driver checks the four pin CTLs (`PinPolicy.h`); if they match the one
  observed poisoned pattern, it restores them in place (rate-limited to
  8 attempts per 10 s, logged to `C:\ft3680.log`). Healthy pins are only
  read, and unknown configurations are never written.
- **Power is not the driver's job**: the touch rails (pm8350c ldo3 2.8 V /
  ldo8 1.8 V) are voted on by the UEFI before Windows starts. Some subsystem
  starts (e.g. Qualcomm Subsystem Manager) still kill touch mid-session for
  reasons unrelated to the pin re-mux above. See the Mu-Silicium fork's
  Status/issues for the current state of that problem.

## Requirements

- Windows 11 ARM64 (23H2) on spacewar with the `ACPI\FTSX3680` device present
  (the spacewar DSDT in [Silicium-ACPI](https://github.com/frost-1256/Silicium-ACPI)
  provides it), **testsigning enabled**.
- `C:\ft3680_fw.bin` — the FT3680 panel firmware (V27). **Not included in this
  repo** (it is FocalTech firmware, distributed with the device); extract it
  from a LineageOS build for spacewar, where it is embedded in the
  `focaltech_touch` kernel driver (`fts_fw_recovery`).

## Build

Visual Studio 2022 + WDK (developed against the EWDK/WDK 28000 headers),
ARM64 configuration:

```powershell
.\build_and_sign.ps1
```

This builds `FT3680Touch.sys` and signs it with a self-signed test
certificate (created on first run). Install with `pnputil /add-driver
FT3680Touch.inf /install` or stage it offline with DISM.

## Files

| File | Role |
|---|---|
| `Driver.c` | DriverEntry, EvtDeviceAdd, queues |
| `Device.c` | PrepareHardware, D0 entry/exit, poll timer, touch work item |
| `Ft3680.c` | Chip layer: reset, boot-ID, bring-up retries, frame parse, file logger |
| `FtFw.c` | PRAM firmware download (ROM boot flow, ECC check) |
| `Gpio.c` | TLMM register access and the SPI bit-bang engine |
| `Spb.c` | SPB target handling (kept for diagnosis; the bit-bang path is authoritative) |
| `Hid.c` | HID descriptor, IOCTL_HID_* handlers, report building |
| `Ft3680.h` | Shared types and prototypes |
| `FT3680Touch.inf` | Installs for `ACPI\FTSX3680`, UpperFilters = mshidkmdf |

The driver writes a diagnostic log to `C:\ft3680.log`.

## References

- LineageOS `focaltech_touch` V3.4 (FT3680 protocol and firmware recovery flow)
- Microsoft KMDF HID miniport samples (minimal descriptor form)
- [Mu-Silicium fork](https://github.com/frost-1256/Mu-Silicium) — UEFI + ACPI
  side of the spacewar Windows port

## License

MIT — see [LICENSE](LICENSE). The panel firmware is not covered by this
license and is not distributed here.
