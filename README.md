# R8C ID Check Clear By AlexDev-TH

โปรแกรม Windows native C++20 สำหรับเชื่อมต่อ UART โดยตรงกับ **R5F21548G (R8C/54G)** และ **R5F21347G (R8C/34G)** เพื่อขอ Forced Erase ตามมาตรฐานชิป ไม่ใช้ .NET, FDT, DLL ของ FDT หรือโปรแกรมภายนอกขณะทำงาน

รุ่นนี้เป็นชุดทดลองทางวิศวกรรม มีการตรวจโค้ดและทดสอบโปรโตคอลจำลองแล้ว แต่ยังไม่ได้ยืนยันการเชื่อมต่อหรือการล้างบนชิปจริง หน้าตาใช้ Windows common controls แบบเรียบ ๆ

## ใช้งาน

1. ตรวจตัวอักษรบนชิปแล้วเลือก **MCU** ให้ตรงรุ่น ห้ามเดารุ่นจาก boot version ใช้สาย USB-to-UART และวงจรโปรแกรมที่ตรวจสอบแล้ว ปิดโปรแกรมอื่นที่ใช้ COM เดียวกัน เครื่องมือไม่ใช้ AlexMapX-Module/K-Line และไม่ควรเลือก COM14 เพียงเพราะเป็นพอร์ตที่เคยใช้กับ AlexMapX
2. ต่อ TX/RX/GND และไฟตามวงจรที่ใช้ได้จริง ระดับแรงดันต้องตรงกับชิป ห้ามต่อสัญญาณ RS-232 โดยตรง โปรแกรมตั้ง DTR/RTS เป็น disabled; ขาเหล่านี้ต้องไม่ใช้ควบคุม RESET/MODE ในชุดทดสอบนี้
3. ตั้ง MODE และ RESET ด้วยตนเองให้เข้า standard serial I/O mode 2 เลือก COM แล้วกด **Connect** การเชื่อมต่ออ่าน boot version และสถานะเท่านั้น ไม่ส่ง ID หรือคำสั่งล้าง และล็อก profile ที่เลือกไว้ตลอด session
4. ตรวจไฟล์และวิธีเขียนกลับให้พร้อม กด **Clear ID** แล้วตอบการยืนยันการล้างของโปรแกรม คำเตือนแสดงรุ่นและจำนวนไบต์ที่จะล้าง
5. โปรแกรมขอ Forced Erase เพียงครั้งเดียว ถ้าชิปยอมรับ จะตรวจ ID ค่า erased และอ่าน blank check ตาม profile ดังนี้ จึงแสดงผลสำเร็จและปล่อย COM
   - R5F21548G: `0x7000–0x17FFF` รวม Program ROM 64 KiB และ Data Flash 4 KiB รวม **69,632 ไบต์ (272 หน้า)**
   - R5F21347G: `0x3000–0xFFFF` รวม Program ROM 48 KiB และ Data Flash 4 KiB รวม **53,248 ไบต์ (208 หน้า)**
6. รีเซ็ตกลับเข้าโหมดโปรแกรม แล้วใช้ FDT หรือเครื่องโปรแกรมเดิมเขียนเฟิร์มแวร์ที่ถูกต้องกลับเข้าไป ชิปที่ล้างแล้วไม่มีโปรแกรมสำหรับทำงาน หากไฟล์ที่เขียนกลับมี ID เดิม ระบบ ID จะกลับมาตามไฟล์นั้น

**Clear ID ในที่นี้คือการล้าง Program ROM และ Data Flash ทั้งหมด ไม่ใช่ลบเฉพาะรหัสโดยเก็บโปรแกรมเดิมไว้** หากต้องการเก็บโปรแกรมเดิม ต้องมีไฟล์สำรองที่ถูกต้องก่อนดำเนินการ

หน้าหลักขนาด 420×260 logical pixels มีช่องเลือก MCU, COM, Refresh, Connect/Disconnect, Clear ID และสถานะสั้น ๆ รายละเอียดเปิดเองจากปุ่ม **วิธีใช้** และ **บันทึก** โดยยืนยันก่อนล้างข้อมูลทุกครั้ง

## ข้อจำกัดที่ตรงกับชิป

- เมื่อ ROMCP1/ROMCR เป็น `01` และ ID ที่เก็บไม่ใช่ reserved forced-erase ID ชิปจะตรวจ ID ตามปกติและไม่ล้างโดยไม่รู้รหัส โปรแกรมนี้ไม่ข้ามข้อจำกัดดังกล่าว
- Reserved ID ที่ปิด standard serial interface อาจทำให้เชื่อมต่อไม่ได้
- Boot version เป็นข้อมูลโปรแกรมบูต ไม่ใช่รหัสยืนยันรุ่นชิป ผู้ใช้ต้องเลือกรุ่นตรงตัวอักษรบนชิป โปรแกรมรับรองเฉพาะสอง profile ข้างต้น
- Timeout, ID mismatch, ข้อมูลอ่านไม่ครบ, flash error หรืออ่านพบไบต์ที่ยังไม่ว่าง ไม่ถือว่าสำเร็จ ไม่ลองล้างซ้ำอัตโนมัติ และไม่เขียน option bytes เพื่อฝืนเปิดการป้องกัน
- ระหว่างงาน UI ยังตอบสนอง แต่ไม่อนุญาตปิดโปรแกรมกลางงาน หลังล้มเหลวให้ดู log และตรวจสถานะจริงก่อนรีเซ็ตหรือเขียนต่อ

## Build และทดสอบ

ต้องใช้ Windows และ Visual Studio C++ Build Tools พร้อม MSVC x64 และ Windows SDK

```powershell
./build.ps1
./build/run-tests.ps1
```

ไฟล์โปรแกรมจะอยู่ที่ `R8C ID Check Clear By AlexDev-TH.exe` ซอร์สอยู่ใน `src/` การทดสอบแบบไม่ต่ออุปกรณ์อยู่ใน `tests/` และหลักฐานโปรโตคอลอยู่ใน `docs/` เมื่อใช้งาน โปรแกรมสร้าง session log ใน `logs/` โดยไม่บันทึกเนื้อหา ROM

## License

เผยแพร่ภายใต้ [MIT License](LICENSE) ไม่มีระบบลงทะเบียนหรือ License Key

## แหล่งอ้างอิง

[R8C/54E–H Hardware Manual](https://www.renesas.com/en/document/mah/r8c54e-group-54f-group-54g-group-54h-group-users-manual), Rev.2.10: §3.3 memory map; §5.3.3/Table 5.3 forced erase; §5.3.4 serial protection; §27.2 user ROM รวม program/data flash และ §27.6 standard serial I/O

[R8C/34E–H Shortsheet](https://www.renesas.com/en/document/dst/r8c34e-group-r8c34f-group-r8c34g-group-r8c34h-group-shortsheet), REJ03B0246-0010: §3.3/Figure 3.3 ระบุ R5F21347G มี Program ROM 48 KiB ที่ `0x4000–0xFFFF` และ Data Flash 4 KiB ที่ `0x3000–0x3FFF`

[R8C Standard Serial Protocol Specification](https://www.renesas.com/en/document/apn/r8c1x-2x-series-standard-serial-io-mode-protocol-specification?language=en), R01AN0334EJ0110: initial handshake, ID packet, status, version และ page read เอกสารนี้ระบุ 1x/2x จึงใช้คู่กับ FDT Protocol D profile ที่ตรงรุ่น ไม่ได้อ้างเอกสารนี้ลำพังว่ารับรอง 34G/54G หรือแทนผลทดสอบจริง

FDT 4.09 profiles `R5F21548G.fcf` และ `R5F21347G.fcf` ยืนยัน Protocol 4/D, Device Code `00FFDF`, MCU ID `0002`, ขนาดและที่อยู่ Program/Data Flash ที่ตรงกับข้างต้น รายละเอียดและ hash อยู่ใน `docs/protocol-evidence.md`
