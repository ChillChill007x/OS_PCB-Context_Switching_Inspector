#!/usr/bin/env python3
"""Linux integration tests: real processes, signals, procfs and cleanup."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = str(ROOT / "pcb_inspector")


def child_pids(text):
    return [int(pid) for pid in re.findall(r"created: PID=(\d+)", text)]


def read_log(output):
    # pread leaves the file offset used by the child writer untouched.
    return os.pread(output.fileno(), 1024 * 1024, 0).decode()


def timing_rows(text):
    rows = re.findall(
        r"(?m)^P([123])\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+"
        r"([\d.]+)\s+([\d.]+)\s+([\d.]+)$", text)
    return {int(p): dict(pid=int(pid), units=int(units), target=float(target),
                         response=float(response), waiting=float(waiting),
                         turnaround=float(turnaround), active=float(active))
            for p, pid, units, target, response, waiting, turnaround, active in rows}


class DemoTests(unittest.TestCase):
    def assert_gone(self, pids):
        self.assertEqual(len(pids), 3)
        for pid in pids:
            self.assertFalse(Path(f"/proc/{pid}").exists(), f"child {pid} still exists")

    def run_demo(self, *args):
        result = subprocess.run([EXE, *args], input="", text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("All child processes finished and reaped.", result.stdout)
        self.assertEqual(result.stdout.count("reaped: exit=0"), 3)
        self.assert_gone(child_pids(result.stdout))
        self.assert_metrics_and_chart(result.stdout)
        return result.stdout

    def assert_metrics_and_chart(self, text):
        rows = timing_rows(text)
        self.assertEqual(set(rows), {1, 2, 3})
        for row in rows.values():
            self.assertGreaterEqual(row["response"], 0)
            self.assertGreaterEqual(row["waiting"] + 0.001, row["response"])
            self.assertGreater(row["active"], 0)
            self.assertAlmostEqual(row["turnaround"], row["waiting"] + row["active"], delta=0.002)
        chart = re.search(r"(?m)^\|(?:P[123]\|)+$", text)
        self.assertIsNotNone(chart)
        sequence = re.findall(r"P([123])", chart.group())
        self.assertEqual(sequence, re.findall(r"\[Scheduler\] Select P([123])", text))
        for i in range(1, 4):
            summary = re.search(rf"P{i} PID=\d+ dispatches=(\d+)", text)
            self.assertEqual(sequence.count(str(i)), int(summary.group(1)))
            after_exit = text.split(f"[Parent] P{i} reaped:", 1)[1]
            self.assertNotIn(f"[Scheduler] Select P{i}", after_exit)

    def test_default_unequal_bursts(self):
        out = self.run_demo("--auto", "--mode", "cpu", "--quantum", "30", "--unit-ms", "20")
        rows = timing_rows(out)
        self.assertEqual([rows[i]["units"] for i in (1, 2, 3)], [8, 4, 12])
        self.assertEqual([rows[i]["target"] for i in (1, 2, 3)], [160, 80, 240])
        for i, units in enumerate([8, 4, 12], 1):
            self.assertRegex(out, rf"\[P{i} \| PID=\d+\] work unit {units:02}/{units:02} completed")

    def test_custom_bursts_and_uniform_override(self):
        for args, expected in [(("--bursts", "3,1,5"), [3, 1, 5]),
                               (("--bursts", "3,1,5", "--units", "2"), [2, 2, 2]),
                               (("--units", "2", "--bursts", "3,1,5"), [3, 1, 5])]:
            out = self.run_demo("--auto", "--unit-ms", "1", *args)
            rows = timing_rows(out)
            self.assertEqual([rows[i]["units"] for i in (1, 2, 3)], expected)

    def test_initial_pause_excluded_and_step_pause_included(self):
        process, output, text = self.start_paused("--units", "1", "--unit-ms", "1")
        time.sleep(0.3)
        released = time.monotonic()
        process.stdin.close()  # EOF releases initial pause.
        self.assertEqual(process.wait(timeout=5), 0)
        elapsed = (time.monotonic() - released) * 1000
        rows = timing_rows(read_log(output))
        self.assertEqual(len(rows), 3)
        self.assertLessEqual(max(row["turnaround"] for row in rows.values()), elapsed + 5)

        process, output, text = self.start_paused("--auto", "--step", "--units", "1", "--unit-ms", "1")
        time.sleep(0.2)  # First step pause occurs after arrival=0.
        process.stdin.close()
        self.assertEqual(process.wait(timeout=5), 0)
        text = read_log(output)
        self.assert_metrics_and_chart(text)
        self.assertGreaterEqual(timing_rows(text)[1]["response"], 190)

    def test_round_robin_and_real_proc_fields(self):
        out = self.run_demo("--auto", "--mode", "cpu", "--quantum", "30",
                            "--units", "2", "--unit-ms", "70")
        order = re.findall(r"\[Scheduler\] Select P(\d)", out)
        self.assertEqual(order[:6], ["1", "2", "3", "1", "2", "3"])
        self.assertIn("[Stop confirmed]", out)
        self.assertRegex(out, r"State:\s+T")
        self.assertRegex(out, r"(?m)^voluntary_ctxt_switches:\s+\d+$")
        self.assertRegex(out, r"(?m)^nonvoluntary_ctxt_switches:\s+\d+$")
        self.assertEqual(len(set(child_pids(out))), 3)
        for i in range(1, 4):
            self.assertIn(f"[P{i}] FINISHED", out)
            self.assertRegex(out, rf"\[P{i} \| PID=\d+\] work unit 02/02 completed")

    def test_sleep_and_eof_prompt(self):
        self.run_demo("--quantum", "20", "--units", "3", "--unit-ms", "25")

    def test_finishes_before_quantum(self):
        out = self.run_demo("--auto", "--quantum", "1000", "--units", "1", "--unit-ms", "1")
        self.assertNotIn("[Stop confirmed]", out)

    def test_exit_near_quantum_boundary(self):
        for _ in range(8):
            self.run_demo("--auto", "--quantum", "10", "--units", "1", "--unit-ms", "10")

    def start_paused(self, *args):
        # A temporary regular file avoids pipe buffering/deadlocks during signal tests.
        output = tempfile.TemporaryFile(mode="w+")
        process = subprocess.Popen([EXE, *args], stdin=subprocess.PIPE,
                                   stdout=output, stderr=output, text=True)
        self.addCleanup(output.close)
        self.addCleanup(self.stop_process, process)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            text = read_log(output)
            if "Press ENTER to continue" in text:
                return process, output, text
            if process.poll() is not None:
                self.fail(text)
            time.sleep(0.01)
        self.fail("did not reach initial prompt")

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process.stdin:
            process.stdin.close()

    def test_interrupt_while_all_children_stopped(self):
        process, output, text = self.start_paused()
        pids = child_pids(text)
        for pid in pids:
            status = Path(f"/proc/{pid}/status").read_text()
            self.assertRegex(status, r"State:\s+T")
        process.send_signal(signal.SIGINT)
        self.assertEqual(process.wait(timeout=5), 130)
        self.assert_gone(pids)

    def test_interrupt_during_cpu_work(self):
        process, output, text = self.start_paused("--mode", "cpu", "--units", "100")
        process.stdin.write("\n")
        process.stdin.flush()
        time.sleep(0.1)
        process.send_signal(signal.SIGTERM)
        self.assertEqual(process.wait(timeout=5), 143)
        self.assert_gone(child_pids(text))

    def test_unexpected_child_death(self):
        process, output, text = self.start_paused()
        pids = child_pids(text)
        os.kill(pids[0], signal.SIGKILL)
        process.stdin.write("\n")
        process.stdin.flush()
        self.assertEqual(process.wait(timeout=5), 1)
        self.assert_gone(pids)

    def test_step_mode_holds_next_dispatch(self):
        process, output, text = self.start_paused("--step", "--mode", "cpu")
        process.stdin.write("\n")  # Initial prompt, then first step prompt.
        process.stdin.flush()
        time.sleep(0.1)
        text = read_log(output)
        self.assertEqual(text.count("Press ENTER"), 2)
        self.assertNotIn("[Scheduler] Select", text)
        process.stdin.write("\n")
        process.stdin.flush()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            text = read_log(output)
            if text.count("Press ENTER") == 3:
                break
            time.sleep(0.02)
        self.assertIn("[Scheduler] Select P1", text)
        self.assertNotIn("[Scheduler] Select P2", text)
        process.send_signal(signal.SIGINT)
        self.assertEqual(process.wait(timeout=5), 130)
        self.assert_gone(child_pids(text))

    def test_bad_arguments(self):
        for args in [("--quantum", "0"), ("--units", "-1"), ("--mode", "bad"),
                     ("--units", "999999999999999999999"), ("--unit-ms", "1x"),
                     ("--bursts", "1,2"), ("--bursts", "1,2,3,4"),
                     ("--bursts", "1,0,3"), ("--bursts", "1,,3"),
                     ("--bursts", "1,2,3x"), ("--bursts", "1,2,10001"),
                     ("--bursts", "1,2,999999999999999999999"), ("--bursts",),
                     ("--quantum",), ("--unknown",)]:
            result = subprocess.run([EXE, *args], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("created:", result.stdout)

    def test_help(self):
        result = subprocess.run([EXE, "--help"], capture_output=True, text=True, timeout=2)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage:", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
