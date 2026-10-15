"""board3 冒烟测试命令行入口。"""

from __future__ import annotations

import argparse
import json
import sys

from board3_testlib import Board3CliClient, Board3SmokeTestRunner, SerialTransport, SmokeTestConfig


def build_arg_parser() -> argparse.ArgumentParser:
    """构建命令行参数解析器。"""
    parser = argparse.ArgumentParser(description="执行 board3 CLI 冒烟测试。")
    parser.add_argument("--port", required=True, help="串口号，例如 COM7 或 /dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=115200, help="串口波特率")
    parser.add_argument("--allow-arm", action="store_true", help="允许测试过程中执行 sys arm on")
    parser.add_argument("--allow-hv", action="store_true", help="允许执行 hv set / hv freq / array 等高压相关命令")
    parser.add_argument(
        "--verify-heater-ttl",
        action="store_true",
        help="在加热链路检查前执行 sys verify-rs232 ttl",
    )
    parser.add_argument(
        "--allow-heater-write",
        action="store_true",
        help="在 TTL 确认后允许执行 heater set / heater stop",
    )
    parser.add_argument("--run-selftest", action="store_true", help="执行 sys selftest start/status")
    parser.add_argument(
        "--response-timeout",
        type=float,
        default=1.5,
        help="单条命令响应超时时间，单位秒",
    )
    return parser


def main() -> int:
    """脚本主入口。"""
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.allow_hv and not args.allow_arm:
        parser.error("--allow-hv requires --allow-arm")

    if args.allow_heater_write and not args.verify_heater_ttl:
        parser.error("--allow-heater-write requires --verify-heater-ttl")

    if args.allow_heater_write and not args.allow_arm:
        parser.error("--allow-heater-write requires --allow-arm")

    if args.response_timeout <= 0:
        parser.error("--response-timeout must be > 0")

    config = SmokeTestConfig(
        response_timeout_s=args.response_timeout,
        allow_arm=args.allow_arm,
        allow_hv=args.allow_hv,
        verify_heater_ttl=args.verify_heater_ttl,
        allow_heater_write=args.allow_heater_write,
        run_selftest=args.run_selftest,
    )

    with SerialTransport(args.port, args.baudrate) as transport:
        client = Board3CliClient(transport)
        runner = Board3SmokeTestRunner(client, config)
        report = runner.run()

    output = {
        "port": report.port,
        "baudrate": report.baudrate,
        "passed": report.passed,
        "steps": [
            {
                "name": step.name,
                "passed": step.passed,
                "detail": step.detail,
                "command": step.command,
                "payload": step.payload,
            }
            for step in report.steps
        ],
    }
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0 if report.passed else 1


if __name__ == "__main__":
    sys.exit(main())
