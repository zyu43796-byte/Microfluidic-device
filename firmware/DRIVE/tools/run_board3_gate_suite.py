"""按 Gate 执行 board3 一键回归（G3/G5/G8/G10）。"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List

from board3_testlib import (
    Board3Board2LinkRunner,
    Board3CliClient,
    Board3SmokeTestRunner,
    SerialTransport,
    SerialTransportError,
    SmokeTestConfig,
    SmokeTestReport,
    SmokeTestStepResult,
)

SUPPORTED_GATES = ("G3", "G5", "G8", "G10")


def parse_gates(gates_text: str) -> List[str]:
    """解析 gate 列表。"""
    gates: List[str] = []
    for raw in gates_text.split(","):
        gate = raw.strip().upper()
        if not gate:
            continue
        if gate not in SUPPORTED_GATES:
            raise ValueError(f"unsupported gate: {gate}")
        if gate not in gates:
            gates.append(gate)
    if not gates:
        raise ValueError("no valid gates specified")
    return gates


def build_blocked_report(port: str, baudrate: int, gate: str, reason: str) -> SmokeTestReport:
    """构造一个“前置条件不满足”的失败报告。"""
    report = SmokeTestReport(port=port, baudrate=baudrate)
    report.steps.append(
        SmokeTestStepResult(
            name=f"{gate}_precheck",
            passed=False,
            detail=reason,
            command="",
            payload=None,
        )
    )
    return report


def smoke_config(
    response_timeout_s: float,
    poll_timeout_s: float,
    allow_arm: bool,
    allow_hv: bool,
    verify_heater_ttl: bool,
    allow_heater_write: bool,
    run_selftest: bool,
) -> SmokeTestConfig:
    """构造 SmokeTestConfig。"""
    return SmokeTestConfig(
        response_timeout_s=response_timeout_s,
        status_poll_timeout_s=poll_timeout_s,
        status_poll_interval_s=0.25,
        allow_arm=allow_arm,
        allow_hv=allow_hv,
        verify_heater_ttl=verify_heater_ttl,
        allow_heater_write=allow_heater_write,
        run_selftest=run_selftest,
    )


def report_to_dict(report: SmokeTestReport) -> Dict[str, object]:
    """将报告结构转成 JSON 可序列化对象。"""
    return {
        "port": report.port,
        "baudrate": report.baudrate,
        "passed": report.passed,
        "steps": [asdict(step) for step in report.steps],
    }


def run_gate_g3(
    client: Board3CliClient,
    response_timeout_s: float,
    poll_timeout_s: float,
) -> SmokeTestReport:
    """G3：L4 + L3 主通信链路。"""
    runner = Board3SmokeTestRunner(
        client=client,
        config=smoke_config(
            response_timeout_s=response_timeout_s,
            poll_timeout_s=poll_timeout_s,
            allow_arm=False,
            allow_hv=False,
            verify_heater_ttl=False,
            allow_heater_write=False,
            run_selftest=False,
        ),
    )
    return runner.run()


def run_gate_g5(
    client: Board3CliClient,
    response_timeout_s: float,
    poll_timeout_s: float,
    confirm_heater_ttl: bool,
    allow_arm: bool,
    allow_heater_write: bool,
    write_channel: int,
    write_duty: int,
) -> SmokeTestReport:
    """G5：L3 + L2 温控链路。"""
    runner = Board3Board2LinkRunner(
        client=client,
        response_timeout_s=response_timeout_s,
        poll_timeout_s=poll_timeout_s,
        confirm_heater_ttl=confirm_heater_ttl,
        allow_arm=allow_arm,
        allow_heater_write=allow_heater_write,
        write_channel=write_channel,
        write_duty_pct=write_duty,
    )
    return runner.run()


def run_gate_g8(
    client: Board3CliClient,
    response_timeout_s: float,
    poll_timeout_s: float,
) -> SmokeTestReport:
    """G8：L3 + L1 高压链路。"""
    runner = Board3SmokeTestRunner(
        client=client,
        config=smoke_config(
            response_timeout_s=response_timeout_s,
            poll_timeout_s=poll_timeout_s,
            allow_arm=True,
            allow_hv=True,
            verify_heater_ttl=False,
            allow_heater_write=False,
            run_selftest=False,
        ),
    )
    return runner.run()


def run_gate_g10(
    client: Board3CliClient,
    response_timeout_s: float,
    poll_timeout_s: float,
    confirm_heater_ttl: bool,
    run_selftest: bool,
) -> SmokeTestReport:
    """G10：四板假负载整机（温控+高压综合）。"""
    runner = Board3SmokeTestRunner(
        client=client,
        config=smoke_config(
            response_timeout_s=response_timeout_s,
            poll_timeout_s=poll_timeout_s,
            allow_arm=True,
            allow_hv=True,
            verify_heater_ttl=confirm_heater_ttl,
            allow_heater_write=True,
            run_selftest=run_selftest,
        ),
    )
    return runner.run()


def build_arg_parser() -> argparse.ArgumentParser:
    """构建命令行参数解析器。"""
    parser = argparse.ArgumentParser(description="按 Gate 执行 board3 一键回归（G3/G5/G8/G10）。")
    parser.add_argument("--port", required=True, help="串口号，例如 COM7 或 /dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=115200, help="串口波特率")
    parser.add_argument(
        "--gates",
        default="G3,G5,G8,G10",
        help="要执行的 Gate 列表，逗号分隔，例如 G3,G5 或 G8,G10",
    )
    parser.add_argument("--response-timeout", type=float, default=1.5, help="单条命令超时时间（秒）")
    parser.add_argument("--poll-timeout", type=float, default=5.0, help="轮询超时时间（秒）")

    parser.add_argument(
        "--confirm-heater-ttl",
        dest="confirm_heater_ttl",
        action="store_true",
        default=True,
        help="执行前自动确认 board3->board2 链路为 TTL（默认开启）",
    )
    parser.add_argument(
        "--no-confirm-heater-ttl",
        dest="confirm_heater_ttl",
        action="store_false",
        help="不自动执行 TTL 确认（可能导致 G5/G10 失败）",
    )

    parser.add_argument("--allow-arm", action="store_true", help="允许执行 sys arm on（G8/G10 必需）")
    parser.add_argument("--allow-hv", action="store_true", help="允许执行高压命令（G8/G10 必需）")
    parser.add_argument(
        "--allow-heater-write",
        action="store_true",
        help="允许执行 heater set/stop（G10 必需；G5 可选）",
    )
    parser.add_argument("--write-channel", type=int, default=1, help="G5 写命令测试通道（1..3）")
    parser.add_argument("--write-duty", type=int, default=5, help="G5 写命令测试占空比（0..100）")
    parser.add_argument("--run-selftest", action="store_true", help="在 G10 中附带执行 sys selftest")
    parser.add_argument("--save", help="将 JSON 报告保存到指定路径")
    return parser


def main() -> int:
    """脚本主入口。"""
    parser = build_arg_parser()
    args = parser.parse_args()

    try:
        gates = parse_gates(args.gates)
    except ValueError as exc:
        parser.error(str(exc))

    if args.response_timeout <= 0:
        parser.error("--response-timeout must be > 0")
    if args.poll_timeout <= 0:
        parser.error("--poll-timeout must be > 0")
    if args.write_channel not in (1, 2, 3):
        parser.error("--write-channel must be 1, 2, or 3")
    if not (0 <= args.write_duty <= 100):
        parser.error("--write-duty must be in [0, 100]")
    if args.allow_heater_write and not args.allow_arm:
        parser.error("--allow-heater-write requires --allow-arm")

    started_at = datetime.now(timezone.utc).isoformat()
    gate_reports: Dict[str, SmokeTestReport] = {}

    try:
        with SerialTransport(args.port, args.baudrate) as transport:
            client = Board3CliClient(transport)

            for gate in gates:
                if gate == "G3":
                    gate_reports[gate] = run_gate_g3(client, args.response_timeout, args.poll_timeout)
                    continue

                if gate == "G5":
                    gate_reports[gate] = run_gate_g5(
                        client=client,
                        response_timeout_s=args.response_timeout,
                        poll_timeout_s=args.poll_timeout,
                        confirm_heater_ttl=args.confirm_heater_ttl,
                        allow_arm=args.allow_arm,
                        allow_heater_write=args.allow_heater_write,
                        write_channel=args.write_channel,
                        write_duty=args.write_duty,
                    )
                    continue

                if gate == "G8":
                    if not args.allow_arm:
                        gate_reports[gate] = build_blocked_report(
                            args.port, args.baudrate, gate, "G8 requires --allow-arm",
                        )
                        continue
                    if not args.allow_hv:
                        gate_reports[gate] = build_blocked_report(
                            args.port, args.baudrate, gate, "G8 requires --allow-hv",
                        )
                        continue
                    gate_reports[gate] = run_gate_g8(client, args.response_timeout, args.poll_timeout)
                    continue

                if gate == "G10":
                    if not args.allow_arm:
                        gate_reports[gate] = build_blocked_report(
                            args.port, args.baudrate, gate, "G10 requires --allow-arm",
                        )
                        continue
                    if not args.allow_hv:
                        gate_reports[gate] = build_blocked_report(
                            args.port, args.baudrate, gate, "G10 requires --allow-hv",
                        )
                        continue
                    if not args.allow_heater_write:
                        gate_reports[gate] = build_blocked_report(
                            args.port, args.baudrate, gate, "G10 requires --allow-heater-write",
                        )
                        continue
                    gate_reports[gate] = run_gate_g10(
                        client=client,
                        response_timeout_s=args.response_timeout,
                        poll_timeout_s=args.poll_timeout,
                        confirm_heater_ttl=args.confirm_heater_ttl,
                        run_selftest=args.run_selftest,
                    )
                    continue
    except SerialTransportError as exc:
        output = {
            "started_at_utc": started_at,
            "port": args.port,
            "baudrate": args.baudrate,
            "requested_gates": list(gates),
            "overall_passed": False,
            "summary": {},
            "gates": {},
            "error": str(exc),
        }
        if args.save:
            save_path = Path(args.save)
            save_path.parent.mkdir(parents=True, exist_ok=True)
            save_path.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(output, ensure_ascii=False, indent=2))
        return 1

    overall_passed = all(gate_reports[gate].passed for gate in gates)
    summary = {gate: gate_reports[gate].passed for gate in gates}

    output = {
        "started_at_utc": started_at,
        "port": args.port,
        "baudrate": args.baudrate,
        "requested_gates": list(gates),
        "overall_passed": overall_passed,
        "summary": summary,
        "gates": {gate: report_to_dict(gate_reports[gate]) for gate in gates},
    }

    if args.save:
        save_path = Path(args.save)
        save_path.parent.mkdir(parents=True, exist_ok=True)
        save_path.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")

    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0 if overall_passed else 1


if __name__ == "__main__":
    sys.exit(main())
