"""board3 + board2 联调检查命令行入口。"""

from __future__ import annotations

import argparse
import json
import sys

from board3_testlib import Board3Board2LinkRunner, Board3CliClient, SerialTransport


def build_arg_parser() -> argparse.ArgumentParser:
    """构建命令行参数解析器。"""
    parser = argparse.ArgumentParser(description="通过 board3 CLI 执行 board3 与 board2 的联调检查。")
    parser.add_argument("--port", required=True, help="串口号，例如 COM7 或 /dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=115200, help="串口波特率")
    parser.add_argument(
        "--confirm-heater-ttl",
        action="store_true",
        help="显式确认板3到板2的加热链路已经检查为 TTL 电平",
    )
    parser.add_argument("--allow-arm", action="store_true", help="允许在加热写命令前执行 sys arm on")
    parser.add_argument(
        "--allow-heater-write",
        action="store_true",
        help="在 arm 和 TTL 确认后允许执行 SET_HEATER / STOP_ALL 行为检查",
    )
    parser.add_argument("--write-channel", type=int, default=1, help="写命令测试时使用的加热通道")
    parser.add_argument("--write-duty", type=int, default=5, help="写命令测试时使用的占空比百分比")
    parser.add_argument("--response-timeout", type=float, default=1.5, help="单条命令响应超时时间，单位秒")
    parser.add_argument("--poll-timeout", type=float, default=5.0, help="状态轮询超时时间，单位秒")
    return parser


def main() -> int:
    """脚本主入口。"""
    parser = build_arg_parser()
    args = parser.parse_args()

    if args.allow_heater_write and not args.confirm_heater_ttl:
        parser.error("--allow-heater-write requires --confirm-heater-ttl")

    if args.allow_heater_write and not args.allow_arm:
        parser.error("--allow-heater-write requires --allow-arm")

    if args.write_channel not in (1, 2, 3):
        parser.error("--write-channel must be 1, 2, or 3")

    if not (0 <= args.write_duty <= 100):
        parser.error("--write-duty must be in [0, 100]")

    if args.response_timeout <= 0:
        parser.error("--response-timeout must be > 0")

    if args.poll_timeout <= 0:
        parser.error("--poll-timeout must be > 0")

    with SerialTransport(args.port, args.baudrate) as transport:
        client = Board3CliClient(transport)
        runner = Board3Board2LinkRunner(
            client=client,
            response_timeout_s=args.response_timeout,
            poll_timeout_s=args.poll_timeout,
            confirm_heater_ttl=args.confirm_heater_ttl,
            allow_arm=args.allow_arm,
            allow_heater_write=args.allow_heater_write,
            write_channel=args.write_channel,
            write_duty_pct=args.write_duty,
        )
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
