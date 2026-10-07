# PCB & Context Switching Inspector — C / Linux

## ผู้จัดทำ

- พัชรพล กองแก้ว 673380415-5 sec4
- ศุภกิตติ์ ฟันเฟือย 673380427-8 sec4
- กฤษฎา นามมนต์เทียน 673380388-2 sec4

เดโมฉบับเต็มตามแนวทางใน `PCB_Context_Switching_Inspector_Demo_Guide.pdf`
สร้าง Parent 1 ตัว และ Child 3 ตัวจริงด้วย `fork()` ให้ Parent เลือก P1 → P2 → P3
แบบ Round Robin ใช้ `SIGCONT` ให้ทำงาน และ `SIGSTOP` ให้หยุด ก่อนอ่าน `/proc/<PID>/status`

## 1. คอมไพล์และรันทันที

ใช้ Linux หรือ Ubuntu บน WSL2 โดยพิมพ์คำสั่งต่อไปนี้ใน **Ubuntu terminal**
โปรแกรมนี้ใช้ Linux API จึงไม่ใช่โปรแกรม Windows `.exe`

```bash
# เฉพาะกรณียังไม่มีเครื่องมือ
sudo apt update
sudo apt install build-essential python3

# กรณีใช้ WSL กับโฟลเดอร์งานนี้
cd /mnt/c/Users/PC/Downloads/OS_PCB

gcc -std=c11 -Wall -Wextra -Wpedantic -O2 pcb_inspector.c -o pcb_inspector
./pcb_inspector
```

หากใช้ Linux เครื่องอื่น ให้คัดลอกโฟลเดอร์นี้แล้ว `cd` เข้าไปแทน path ของ WSL
เมื่อเห็น `Press ENTER` ให้กด Enter เพื่อเริ่ม ระบบจะรันจนทั้งสาม process ทำงานครบ
และ Parent เก็บสถานะจบด้วย `waitpid()` ค่าเริ่มต้นคือ quantum 1 วินาที งาน P1=8, P2=4, P3=12 หน่วย
และ sleep 250 ms ต่อหน่วย กด Ctrl+C เพื่อยุติและเก็บกวาด children ได้ทุกช่วง

ใช้ Makefile ได้เช่นกัน:

```bash
make
make run
make demo
make test
```

## 2. รูปแบบเดโม

```bash
# ทำงานต่อเนื่องโดยไม่รอ Enter ตอนเริ่ม
./pcb_inspector --auto

# เหมาะสำหรับนำเสนอ: งาน CPU จริง และกด Enter ก่อนแต่ละ quantum
./pcb_inspector --step --mode cpu

# ทดสอบเร็ว พร้อมเห็นการหยุดหลายรอบ
./pcb_inspector --auto --mode cpu --quantum 100 --bursts 8,4,12 --unit-ms 50

# บันทึก output ลงไฟล์ พร้อมดูหน้าจอ
./pcb_inspector --auto | tee demo.log

./pcb_inspector --help
```

| ตัวเลือก | ค่าเริ่มต้น | ความหมาย |
|---|---:|---|
| `--quantum MS` | 1000 | เวลาต่อรอบตามนาฬิกา monotonic ช่วง 10–60000 ms |
| `--bursts A,B,C` | 8,4,12 | จำนวนหน่วยงานของ P1,P2,P3 แต่ละค่าช่วง 1–10000 |
| `--units N` | — | กำหนดจำนวนหน่วยเท่ากันทั้งสาม child ช่วง 1–10000 |
| `--unit-ms MS` | 250 | เวลาต่อหน่วย ช่วง 1–60000 ms |
| `--mode sleep` | sleep | ใช้ `nanosleep()` เหมือนแนวเดโมในคู่มือ |
| `--mode cpu` | — | คำนวณจริง วัดเวลาทำงานด้วย `CLOCK_PROCESS_CPUTIME_ID` |
| `--auto` | ปิด | ข้ามการรอ Enter ตอนเริ่ม |
| `--step` | ปิด | รอ Enter ก่อนเลือก process แต่ละรอบ |

ถ้าใช้ `--auto --step` จะข้ามเฉพาะ prompt แรก แต่ยังหยุดก่อนแต่ละรอบ
ถ้า stdin เป็น EOF โปรแกรมจะเดินต่อโดยอัตโนมัติ จึงใช้ในสคริปต์ได้
ถ้าใช้ทั้ง `--units` และ `--bursts` ค่าตัวเลือกที่อยู่ท้ายสุดมีผล

โหมด sleep แสดงการบล็อกรอเวลาได้ แต่ไม่ได้จำลอง CPU burst จริง:
เวลารอ sleep อาจครบระหว่างที่ process ถูก STOP และกลับมาจบ sleep เมื่อ CONT
ส่วนโหมด cpu จะสะสมเฉพาะ CPU time ของ child จึงเหมาะสำหรับสาธิตงานที่ยังเหลือหลังหยุด/ทำงานต่อ
เวลาจริงของแต่ละ quantum อาจคลาดเคลื่อนจากภาระระบบและเวลาที่ parent ได้รับ CPU
โปรแกรมนี้ไม่ใช่ระบบ real-time และไม่ได้ตั้ง policy เป็น Linux `SCHED_RR`

## 3. สิ่งที่ต้องสังเกต

1. `[P1] created`, `[P2] created`, `[P3] created` มี PID ต่างกัน และ PPID เป็น Parent เดียวกัน
2. ทั้งสามตัว `raise(SIGSTOP)` จากนั้น Parent รอด้วย `waitpid(..., WUNTRACED)`
3. Inspector แสดง `State: T (stopped)` จริงจาก `/proc` หลังยืนยันการหยุดแล้ว
4. `[Scheduler] Select P1` ตามด้วย P2 และ P3 แบบวนรอบ ข้ามตัวที่จบแล้ว
5. เมื่อหมด quantum จะส่ง SIGSTOP และรอยืนยันก่อนเริ่มตัวถัดไป
6. เมื่อ child จบจะเห็น `reaped: exit=0` และท้ายสุดมี dispatches/confirmed_stops,
   ตาราง Response/Waiting/Turnaround และ Gantt chart จากลำดับ dispatch จริง

### ตารางเวลาและ Gantt chart

ใช้ `CLOCK_MONOTONIC` วัดเวลา และแสดงหน่วย **มิลลิวินาที**
ถือว่า child ทั้งสามเข้า ready queue ของการทดลองพร้อมกัน (arrival=0)
หลังสร้าง/ตรวจสถานะครบและผ่าน Enter แรก จึงไม่รวมเวลาตั้งต้นหรือเวลารอ Enter แรก

| คอลัมน์ | ความหมาย |
|---|---|
| Units | จำนวนหน่วยงานที่กำหนดให้แต่ละ process |
| Target(ms) | units × unit-ms; เป็น CPU time เป้าหมายในโหมด cpu หรือผลรวมเวลาที่ขอ sleep ในโหมด sleep |
| Response | เวลาก่อนส่ง SIGCONT ครั้งแรก − arrival |
| Active | ผลรวมช่วงตั้งแต่ก่อนส่ง SIGCONT จน Parent ยืนยัน stopped หรือสังเกตว่าจบ |
| Turnaround | เวลาที่ Parent สังเกตว่าจบผ่าน waitpid − arrival |
| Waiting | Turnaround − Active คือเวลาที่รอการเลือกจากตัวควบคุม RR |

มีค่าเฉลี่ย Response, Waiting และ Turnaround ของทั้งสาม process ให้ด้วย
Active เป็นเวลาตามนาฬิกา รวม sleep และเวลารอ kernel จัด CPU ระหว่างเปิดให้รัน
จึงไม่ใช่ CPU burst ที่วัดจริง และ Waiting ไม่ใช่ค่า Linux run-queue waiting time
ไม่ใช้ `turnaround − Target` เพราะ Target ไม่ใช่เวลาที่ใช้จริงทั้งหมด
การตรวจจบใช้ polling ช่วงไม่เกิน 10 ms ตามที่ขอ sleep แต่ scheduler อาจทำให้สังเกตช้ากว่านั้น

เวลารอ Enter ของ `--step` หลังเริ่มทดลอง รวมถึงเวลาพิมพ์/inspect ระหว่างรอบ **รวมอยู่ในเวลา**
เมื่อต้องการเปรียบเทียบงานสั้น/ยาว ให้ใช้ `--auto --mode cpu` เพื่อไม่มีเวลาหยุดนำเสนอเข้ามาปน
โหมดเริ่มต้นยังเป็น sleep ตามเดโมเดิม

Gantt chart พิมพ์ท้ายโปรแกรม เช่น `|P1|P2|P3|P1|P3|...`
ตัวอย่างนี้แสดงรูปแบบเท่านั้น ลำดับจริงขึ้นอยู่กับเวลารัน แต่ละช่องหมายถึงหนึ่ง dispatch
ความกว้างช่องไม่ใช่สัดส่วนเวลา ไม่มีช่องสำหรับเวลารอ Enter หรือ overhead
หากเหลือ P3 ตัวเดียว อาจเห็น `|P3|P3|` เพราะเป็นการเลือกสองรอบจริง

หลังเริ่มโปรแกรมจะมีคำสั่ง `ps` ที่ใส่ PID จริงให้คัดลอกไปใช้อีก terminal
โดยเฉพาะช่วงรอ Enter ที่ children ทุกตัวหยุดอยู่:

```bash
# แทน 1234 ด้วย PID ของ child ที่แสดงบนหน้าจอ
ps -o pid,ppid,stat,comm -p 1234
cat /proc/1234/status
```

State ที่เกี่ยวข้อง: `R` คือ running/runnable, `S` คือ interruptible sleep,
`T` คือ stopped และ `Z` คือจบแล้วแต่ยังไม่ถูก reap
เดโมนี้จงใจอ่านสถานะหลังหยุด จึงคาดว่าจะเห็น T และไม่ได้สร้างช่วง zombie เพื่อโชว์
หลัง reap แล้ว `/proc/PID` จะหายไปตามปกติ

## 4. ประเด็นที่ต้องอธิบายให้ถูกต้อง

- `ProcessInfo` เป็นข้อมูลติดตาม child ของโปรแกรมนี้ ไม่ใช่ PCB จริง
  แนวคิด PCB ใน Linux เชื่อมโยงกับ `task_struct` และโครงสร้างอื่นใน kernel
- Parent เป็นตัวควบคุมใน user space; Linux kernel ยังเลือกและจัดสรร CPU จริง
- `SIGSTOP`/`SIGCONT` เป็นเครื่องมือเปลี่ยนสถานะ process ไม่ใช่ context switch โดยตรง
  การ save/restore execution context เป็นหน้าที่ kernel โค้ดนี้ไม่อ่าน register context
- `confirmed_stops` นับเฉพาะการหยุดที่ Parent ยืนยันหลัง quantum ไม่รวมการหยุดตอนเริ่ม
  ส่วน `dispatches` คือจำนวนครั้งที่สั่ง CONT ทั้งสองค่าไม่ใช่จำนวน context switch ของ kernel
- `voluntary_ctxt_switches` และ `nonvoluntary_ctxt_switches` อ่านจาก kernel จริง
  อาจเพิ่มจาก sleep, I/O, การแย่ง CPU และเหตุอื่น จึงไม่เท่ากับตัวนับเดโมแบบ 1:1
  `/proc/PID/status` เป็นข้อมูลของ task หลัก; เดโมนี้แต่ละ child มี thread เดียว

## 5. ลำดับนำเสนอประมาณ 5–10 นาที

| ช่วง | สิ่งที่ทำและอธิบาย |
|---|---|
| 0–1 นาที | เปิด source อธิบาย Parent และ children 3 ตัว รวมถึง PCB/task_struct |
| 1–2 นาที | รัน `./pcb_inspector --step --mode cpu` ชี้ PID/PPID และ State T |
| 2–3 นาที | เปิด terminal ที่สอง ใช้คำสั่ง ps ที่โปรแกรมแสดง |
| 3–5 นาที | กด Enter ผ่าน prompt เริ่มต้นและก่อนแต่ละรอบ ชี้ P1 → P2 → P3 |
| 5–6 นาที | ชี้งานที่ทำต่อจากเดิมและค่า voluntary/nonvoluntary ใน snapshot |
| 6–8 นาที | อธิบาย controller กับ kernel scheduler และข้อจำกัดของตัวนับ |
| ช่วงท้าย | กด Enter ต่อจนจบ หรือ Ctrl+C เพื่อแสดงการ cleanup |

## 6. โครงสร้างไฟล์และการทดสอบ

- `pcb_inspector.c` — source C ไฟล์เดียว ไม่ต้องใช้ไลบรารีภายนอก
- `Makefile` — build/run/demo/test/clean
- `tests/test_demo.py` — integration tests บน Linux ใช้ Python standard library
- `TEST_RESULTS.md` — รายงานการทดสอบจากสภาพแวดล้อมที่ใช้พัฒนา
- `demo_output.txt` — output จริงจากการรันเดโมเต็มด้วยค่าเริ่มต้น
- `demo_cpu_output.txt` — output จริงโหมด CPU พร้อมตารางเวลาและ Gantt ของงาน 8,4,12 units

รันทดสอบโดยไม่ใช้ make ได้:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 pcb_inspector.c -o pcb_inspector
python3 tests/test_demo.py
```

Tests ตรวจลำดับ Round Robin, Gantt ที่ตรงกับ dispatch จริง, สมการของตารางเวลา,
burst ต่างกัน/การ override, ขอบเขตการนับเวลารอ Enter, ข้อมูล `/proc`, การจบก่อน/ใกล้ quantum,
การหยุดรอ Enter, การ interrupt ตอน stopped/ตอนทำงาน, child ที่ถูกฆ่าจากภายนอก,
การตรวจ argument และการไม่เหลือ child หลัง Parent จบตามเส้นทางที่ทดสอบ
ไม่ตรวจเลข PID หรือค่าตัวนับแบบตายตัว เพราะขึ้นอยู่กับสภาพเครื่องขณะรัน

เมื่อพบข้อผิดพลาดหรือ SIGINT/SIGTERM/SIGHUP Parent จะ kill และ reap children ที่ยังเหลือ
ตั้ง `PR_SET_PDEATHSIG` ให้ child จบหาก Parent ตายกะทันหันด้วย
กรณี Parent ถูก SIGKILL จะไม่สามารถเรียก cleanup ของ Parent ได้ และการ reap จะเป็นหน้าที่
process ที่รับเลี้ยง orphan ตามระบบ ไม่รับประกันการหายของ zombie ทันทีในกรณีนั้น

Exit code: `0` สำเร็จ, `1` runtime/child error, `2` argument ไม่ถูกต้อง,
`130` SIGINT (Ctrl+C), `143` SIGTERM

## 7. แก้ปัญหาเบื้องต้น

- `gcc: command not found`: ติดตั้ง `build-essential` ภายใน Ubuntu/Linux
- `make: command not found`: ใช้คำสั่ง gcc ด้านบนได้ หรือติดตั้ง `build-essential`
- ดูเหมือนค้าง: ตรวจว่ากำลังรอ Enter โดยเฉพาะ `--step`
- เปิด `/proc/PID/status` ไม่ได้: ตรวจว่า PID ยังไม่จบ และรันบน Linux ที่ mount `/proc`
- Output สลับบรรทัดตอนสร้าง child: ลำดับสร้างที่พิมพ์อาจต่างกันเพราะเป็น process จริง
  ส่วนลำดับ dispatch ถูกควบคุมโดย Parent
- ต้องการให้เห็นหลายรอบ: ใช้ `--mode cpu` และให้ `units × unit-ms` มากกว่า quantum

อ้างอิงขอบเขตเดโมจาก PDF ที่แนบ ส่วนคำแนะนำเรื่องสไลด์/วิดีโอใน PDF
เป็นบริบทประกอบ โฟลเดอร์นี้ส่งมอบเฉพาะโค้ดเดโม คู่มือ และการทดสอบตามคำขอ
