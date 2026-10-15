"""board3 串口 CLI 客户端封装。

职责：
- 统一发送命令
- 统一处理 JSON / 文本响应
- 对上层隐藏串口读写细节
"""

from __future__ import annotations

import json
from typing import Iterable, List

from .models import Board3CommandResult
from .transport import SerialTransport


class Board3CliClient:
    """对 board3 CLI 的高层封装。"""

    def __init__(self, transport: SerialTransport) -> None:
        self._transport = transport

    @property
    def port(self) -> str:
        return self._transport.port

    @property
    def baudrate(self) -> int:
        return self._transport.baudrate

    def _ensure_json_suffix(self, command: str) -> str:
        """确保命令带有 `--json` 后缀。"""
        if "--json" in command:
            return command
        return f"{command} --json"

    def query_json(self, command: str, timeout_s: float = 1.5) -> Board3CommandResult:
        """发送命令并要求返回 JSON。"""

        actual_command = self._ensure_json_suffix(command)
        self._transport.drain_input()
        self._transport.write_line(actual_command)
        response = self._transport.read_line(timeout_s)

        try:
            payload = json.loads(response)
        except json.JSONDecodeError:
            # 当前测试工具默认要求 JSON 可解析，
            # 如果固件返回纯文本，这里直接按失败记录，便于脚本统一处理。
            return Board3CommandResult(
                command=actual_command,
                raw_response=response,
                ok=False,
                payload=None,
                note="期望收到 JSON，但实际返回了纯文本",
            )

        ok = bool(payload.get("ok", False))
        return Board3CommandResult(
            command=actual_command,
            raw_response=response,
            ok=ok,
            payload=payload,
        )

    def query_text(self, command: str, timeout_s: float = 1.5) -> Board3CommandResult:
        """发送命令并返回原始文本响应。"""

        self._transport.drain_input()
        self._transport.write_line(command)
        response = self._transport.read_line(timeout_s)
        return Board3CommandResult(
            command=command,
            raw_response=response,
            ok=not response.lower().startswith("error:"),
            payload=None,
        )

    def sys_ping(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys ping", timeout_s)

    def sys_status(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys", timeout_s)

    def sys_arm_on(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys arm on", timeout_s)

    def sys_arm_off(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys arm off", timeout_s)

    def sys_verify_ttl(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys verify-rs232 ttl", timeout_s)

    def sys_verify_clear(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys verify-rs232 clear", timeout_s)

    def sys_clear_faults(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys clear-faults", timeout_s)

    def sys_selftest_start(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys selftest start", timeout_s)

    def sys_selftest_status(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("sys selftest", timeout_s)

    def gpio_read(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("gpio read", timeout_s)

    def adc_read(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("adc read", timeout_s)

    def heater_status(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("heater status", timeout_s)

    def heater_version(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("heater version", timeout_s)

    def heater_ping(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_text("heater ping", timeout_s)

    def heater_ntc(self, channel: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"heater ntc {channel}", timeout_s)

    def heater_cur(self, channel: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"heater cur {channel}", timeout_s)

    def heater_pwr(self, channel: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"heater pwr {channel}", timeout_s)

    def heater_set(self, channel: int, duty_pct: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_text(f"heater set {channel} {duty_pct}", timeout_s)

    def heater_stop(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_text("heater stop", timeout_s)

    def hv_read(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("hv read", timeout_s)

    def hv_set(self, volts: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"hv set {volts}", timeout_s)

    def hv_mode(self, mode: str, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"hv mode {mode}", timeout_s)

    def hv_freq(self, hz: int, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json(f"hv freq {hz}", timeout_s)

    def array_read(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("array read", timeout_s)

    def array_clear(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("array clear", timeout_s)

    def array_set(self, logic_bytes: Iterable[int], timeout_s: float = 1.5) -> Board3CommandResult:
        values: List[int] = list(logic_bytes)
        if len(values) != 16:
            raise ValueError("array_set 必须提供 16 个字节")
        if not all(isinstance(value, int) and (0 <= value <= 0xFF) for value in values):
            raise ValueError("array_set 的每个字节必须是 0~255 的整数")

        payload = " ".join(f"{value:02X}" for value in values)
        return self.query_json(f"array set {payload}", timeout_s)

    def aux_status(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("aux status", timeout_s)

    def aux_send(self, text: str, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_text(f"aux send {text}", timeout_s)

    def all_stop(self, timeout_s: float = 1.5) -> Board3CommandResult:
        return self.query_json("all_stop", timeout_s)
