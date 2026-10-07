# ผลทดสอบ

ทดสอบเวอร์ชันตารางเวลา/Gantt/งานต่างขนาด วันที่ 6 ตุลาคม 2026 บน Ubuntu / WSL2
ใช้ gcc และ Python 3 ภายใน Ubuntu

## Build

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 pcb_inspector.c -o pcb_inspector
```

ผ่าน Exit code 0 ไม่มี warning/error

## Integration tests

```bash
python3 tests/test_demo.py
```

ผลการรันล่าสุด: **13 tests ผ่านทั้งหมด** (`Ran 13 tests in 4.681s`, `OK`)

| การทดสอบ | ผล |
|---|---|
| ปฏิเสธ arguments ผิด โดยไม่สร้าง child | ผ่าน |
| Child จบใกล้ quantum 10 ms จำนวน 8 รอบทดสอบ | ผ่าน |
| Child จบก่อน quantum | ผ่าน |
| แสดง help | ผ่าน |
| SIGTERM ระหว่างงาน CPU: exit 143 และไม่เหลือ child | ผ่าน |
| SIGINT ระหว่าง children หยุดรอ: exit 130 และไม่เหลือ child | ผ่าน |
| Round Robin, PID ต่างกัน, State T และ context-switch fields | ผ่าน |
| โหมด sleep และ stdin EOF | ผ่าน |
| โหมด step รอ Enter ก่อน dispatch ถัดไป | ผ่าน |
| Child ถูก SIGKILL จากภายนอก: Parent ล้มเหลวและ cleanup ที่เหลือ | ผ่าน |
| ค่าเริ่มต้น P1=8, P2=4, P3=12 units และงานเสร็จครบตามจำนวน | ผ่าน |
| --bursts และ --units override ตามลำดับ arguments | ผ่าน |
| ไม่นับ prompt แรก แต่รวมเวลารอใน --step | ผ่าน |

ในการรันทดสอบที่จบปกติ ตรวจเพิ่มทุกครั้งว่า:

- ตารางมีข้อมูลทั้งสาม process และ waiting ≥ response ≥ 0
- turnaround = waiting + active (ยอมรับการปัดเศษ 0.002 ms)
- Gantt ตรงกับ dispatch log ทุกช่อง และจำนวนช่องตรงกับตัวนับ dispatches
- ไม่มีการเลือก process ซ้ำหลัง reap แล้ว

## รันเดโมเต็มด้วยค่าเริ่มต้น

```bash
./pcb_inspector --auto > demo_output.txt
```

ผ่าน Exit code 0 ทั้งสาม child ทำงานครบ 8, 4, 12 หน่วยตามลำดับและถูก reap แล้ว
ดู output ทั้งหมดใน `demo_output.txt`

## รันโหมด CPU เพื่อเปรียบเทียบงานสั้น/ยาว

```bash
./pcb_inspector --auto --mode cpu --quantum 100 --bursts 8,4,12 --unit-ms 50 > demo_cpu_output.txt
```

ผ่าน Exit code 0 ตารางเวลาที่ได้ (ms):

| Process | Units | Target | Response | Waiting | Turnaround | Active |
|---|---:|---:|---:|---:|---:|---:|
| P1 | 8 | 400 | 1.736 | 716.548 | 1131.286 | 414.738 |
| P2 | 4 | 200 | 108.303 | 565.186 | 780.353 | 215.167 |
| P3 | 12 | 600 | 222.566 | 740.300 | 1360.955 | 620.655 |

P2 จบก่อน P1 และ P3 ในการรันนี้ Gantt ที่บันทึกจริง:

```text
|P1|P2|P3|P1|P2|P3|P1|P2|P3|P1|P3|P1|P3|P3|P3|
```

ดู output จริงทั้งหมดใน `demo_cpu_output.txt` เลข PID เวลา จำนวนรอบ และ kernel counters
อาจเปลี่ยนเมื่อรันใหม่ตามจังหวะการทำงานของระบบ

ทดสอบผ่านคำสั่ง gcc/Python โดยตรง ยังไม่ได้เรียกเป้าหมาย Makefile จริง
และไม่ได้ทดสอบบน Linux distribution อื่น
หรือทดสอบจำลอง resource exhaustion เพื่อบังคับให้ fork ล้มเหลว
