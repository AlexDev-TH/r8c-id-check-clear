# การร่วมพัฒนา

ยินดีรับ issue และ pull request เอกสารนี้สรุปแนวทางเพื่อให้การแก้ไขเข้ากันกับโครงสร้างเดิมและปลอดภัยกับฮาร์ดแวร์จริง

## ก่อนเริ่ม

- อ่านส่วนโครงสร้างโปรเจกต์และวิธีพัฒนาต่อใน [README](README.md) ให้เข้าใจสถาปัตยกรรมสามชั้น คือ ชั้นโปรโตคอล ชั้นงาน และชั้นหน้าตา
- ตั้งสภาพแวดล้อม Windows 10 หรือ 11 พร้อม Visual Studio C++ Build Tools ที่มี MSVC x64 และ Windows SDK

## วงจรการแก้ไข

1. แยก branch จาก `main`
2. แก้โค้ดโดยคงการแยกชั้น ชั้นโปรโตคอลต้องไม่ผูกกับ Win32 ตรรกะที่ทดสอบได้ควรอยู่ในชั้นโปรโตคอลหรือชั้นงาน
3. build และรันชุดทดสอบให้ผ่านทั้งหมด

```powershell
./build.ps1
./run-tests.ps1
```

4. เปิด pull request อธิบายสิ่งที่แก้และเหตุผล ถ้าแตะส่วนที่เกี่ยวกับพฤติกรรมของชิป ให้แนบที่มาอ้างอิงจากคู่มือ

## แนวทางโค้ด

- ใช้ C++20 ให้ build ผ่านสะอาดที่ระดับ `/W4` และ `/permissive-` ไม่พึ่ง dependency ภายนอกนอกจาก Win32 library
- ข้อความ UI เป็นภาษาไทยในชั้น `src/main.cpp` ชั้นโปรโตคอลและชั้นงานใช้ข้อความอังกฤษตรงไปตรงมา
- ตั้งชื่อให้สื่อความหมาย หลีกเลี่ยงตัวย่อที่เดายาก และคงสไตล์เดิมของไฟล์ที่แก้

## การเพิ่ม MCU profile ใหม่

ทำตามขั้นตอนในส่วนวิธีพัฒนาต่อของ README คือ เพิ่มค่าใน enum `Device` เพิ่ม `DeviceProfile` ใน `src/protocol.cpp` จับคู่ใน `device_profile` เพิ่มตัวเลือกใน UI ตรวจช่วงที่อยู่กับคู่มือฮาร์ดแวร์ บันทึกหลักฐานใน `docs/protocol-evidence.md` แล้วรันชุดทดสอบให้ผ่าน

## ความปลอดภัยที่ต้องรักษาไว้

- ห้ามเพิ่มพฤติกรรมที่ลองล้างซ้ำอัตโนมัติ
- ห้ามเขียน option bytes เพื่อฝืนปลดการป้องกันของชิป
- ถือว่าทุกเงื่อนไขที่อ่านหรือยืนยันไม่ครบคือไม่สำเร็จ และต้องแจ้งผู้ใช้ตามจริง
- ยืนยันการล้างข้อมูลก่อนดำเนินการทุกครั้ง

## English

Contributions are welcome through issues and pull requests. Read the project structure and development sections in the [README](README.md) first. Keep the three layer split intact, where the protocol layer stays free of Win32 so it can be tested without hardware. Branch from `main`, keep the build clean at `/W4` and `/permissive-`, and make both `./build.ps1` and `./run-tests.ps1` pass before opening a pull request. When you change anything that describes chip behavior, cite the relevant manual section and record evidence in `docs/protocol-evidence.md`. Never add automatic erase retries, never write option bytes to force protection open, treat every incomplete read or verification as a failure, and always confirm before erasing.
