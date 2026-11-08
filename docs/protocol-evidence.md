# Direct UART protocol evidence

Engineering evidence, original review 2026-10-16; R5F21347G profile added 2026-11-07. Scope: R5F21548G and R5F21347G, standard serial I/O mode 2. No target was connected, reset, read, erased, or programmed during this review. The application has no FDT runtime dependency; installed FDT files were inspected statically for interoperability evidence.

## Manufacturer constraints

[R8C/54E, 54F, 54G, 54H hardware manual, R01UH0189EJ0210](https://www.renesas.com/en/document/mah/r8c54e-group-54f-group-54g-group-54h-group-users-manual): sections 5.3.2-5.3.4, pages 60-62, define ID matching, forced erase, and serial prohibition. Sending the reserved forced-erase ID is conditional. A normal stored ID with ROMCP1:ROMCR = 01 prohibits forced erase. The pinned original K03 image has that protection combination; this tool cannot promise recovery of a chip carrying it with an unknown ID. Section 27.2, pages 675-676, defines user ROM as both program ROM and data flash: this device has 64 KiB at 0x8000-0x17FFF and 4 KiB at 0x7000-0x7FFF. Forced erase therefore must not be described as preserving data flash. Section 27.6, pages 706-707, supports asynchronous mode 2, MODE low, TXD/RXD and reset. The user's pad labels do not prove electrical levels or the MODE strap.

[R8C/34E-H shortsheet, REJ03B0246-0010](https://www.renesas.com/en/document/dst/r8c34e-group-r8c34f-group-r8c34g-group-r8c34h-group-shortsheet): section 3.3 and Figure 3.3, page 25, define R5F21347G program ROM as 48 KiB at 0x4000-0xFFFF and data flash as 4 KiB at 0x3000-0x3FFF. The contiguous user-memory verification range is therefore 0x3000-0xFFFF inclusive, 53,248 bytes or 208 pages. This corrects the initial assumption that 34G had no data flash; that assumption was not implemented.

[R8C/1x, 2x serial protocol specification, R01AN0334EJ0110](https://www.renesas.com/en/document/apn/r8c1x-2x-series-standard-serial-io-mode-protocol-specification): sections 2.2, 4.1, 4.6, 4.8, 4.9 document 9600 8N1, sixteen zero bytes separated by at least 20 ms after reset settling, B0 echo, page read, two-byte status, ID check, and eight-byte version output. Its stated product scope does not include 34G or 54G. Exact-device FDT evidence below corroborates the implemented subset; this is not blanket applicability of every command in that application note.

## Exact installed-device evidence

Files inspected without execution:

- `C:/Program Files (x86)/Renesas/FDT4.09/FDTApiExt.dll`, version 4.09.02.000, x86 image base 0x10000000; SHA-256 `982cae3defd6eec017e8123a2d9602a4cbfd0cdda713eb3b26e1820ef3b9be3d`.
- `C:/Program Files (x86)/Renesas/FDT4.09/kernels/ProtD/R5F21548G/Renesas/1_0_00/R5F21548G.fcf`; SHA-256 `e5a88abec4d5cadc3afcc8ef066842ab88f5bc187d73c9cdd5f068caa61bf4f5`.
- `C:/Program Files (x86)/Renesas/FDT4.09/kernels/ProtD/R5F21347G/Renesas/1_0_00/R5F21347G.fcf`; SHA-256 `63f61932875b4a9842e9fa1649711057ca0ea44fa3cd331e0bf2d325cb211e89`.

Both profiles specify architecture R8C, Protocol 4/D, Device Code 00FFDF, MCU ID 0002, COM support, and 4096 data bytes. The 54G profile specifies 65536 program bytes at 0x8000 and data flash at 0x7000. The 34G profile specifies 49152 program bytes at 0x4000 and data flash at 0x3000, matching the shortsheet. Both explicitly disable the generic Erase All Blocks feature. Configuration parsing stores Protocol at object offset 0x88 (DLL VA 0x100176A8-0x10017700) and hexadecimal MCU ID at offset 0xD0 (0x1001798B-0x10017A09). This distinguishes the MCU-2 path from alternate 9/0F/10 command formats.

| Operation | Static code evidence in the hashed DLL |
| --- | --- |
| Synchronization | `ZeroTransmission` at 0x10085D85 checks configured Protocol 4 at 0x10085F5F and 0x10085FDB, selects B0, and sends zero/B0 synchronization. Related implementation 0x10087745 explicitly sends sixteen separate zero bytes with 20 ms waits, then B0, requiring exactly one B0 response at 0x1008788B-0x100878A0. The latter is virtual slot +0xEC in tables beginning 0x10107754/0x10107B54, selected by 0x10085E4E for another communication backend; it is corroboration, not a claim that every COM backend takes that exact function. |
| Version | 0x1002F688 sends FB and reads exactly eight bytes at 0x1002F6E5-0x1002F700 for MCU 2. Handshake caller: 0x10087950. The stricter `VER.x.xx` digit check in this application is a conservative admission rule; an actual 54G reply has not been captured. |
| ID check | `SendR8CidCode`, 0x100A9D1B, constructs F5 plus three little-endian ID-base bytes, length 07, and seven ID bytes at 0x100A9DDB-0x100A9E59. Caller 0x100A971A supplies the configured Device Code. It requests status, then requires packed status mask 0x0C = 0x0C. |
| Status | `RequestBootProgramStatusProtocolD`, 0x100B62EC, sends 70 and reads two bytes. At 0x100B658A-0x100B65DA it rejects first-byte mask 38, second-byte mask 02, and second bit 80 without bit 10. First-byte bit 80 means ready (0x100B668C-0x100B66AA). ID state is second-byte bits 2-3. FF/FF is rejected. |
| Page read | `ReadData_ProtocolD`, 0x100B33EC, sets page length 256 at 0x100B34C4. MCU 2 follows the three-byte packet at 0x100B36B5-0x100B36E1: FF, address bits 8-15, address bits 16-23. Read call is 0x100B380F. |

Observed with Microsoft dumpbin 14.51.36256.0. REA/Ghidra string analysis timed out; no conclusion relies on its missing output. The analysis session was closed. No FDT API function or target-facing application was executed.

## Reviewed application behavior and remaining proof

`src/protocol.cpp` uses 25 ms spacing, one forced-ID request, bounded status polling, and no ordinary A7 erase or automatic destructive retry. It then authenticates the default erased ID and rechecks ready/matched status. The selected device profile requires either all 272 pages at 0x7000-0x17FFF for 54G or all 208 pages at 0x3000-0xFFFF for 34G to contain FF. An error, partial response, timeout, or nonblank byte cannot produce success. Page contents are not exported. The additional FDT status rejection masks are present. `Session` stores the selected profile only after a successful connection and uses that same profile for erasure; the UI disables profile changes while connected.

The 60-second erase deadline and per-read timeouts are application limits, not measured 34G/54G erase timings. No live version reply, erase completion, blank proof, or FDT reconnection is verified for either device yet. Handshake/version strings do not uniquely identify a chip: the operator must confirm the exact part and direct UART connection. Manual reset/MODE selection remains required; do not infer DTR/RTS wiring. Unknown protection state is not permission to claim bypass. The known protected K03 condition remains unsupported for unknown-ID forced erase.
