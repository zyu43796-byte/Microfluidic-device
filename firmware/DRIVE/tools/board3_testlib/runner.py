"""基于 board3 CLI 的冒烟测试执行器。"""

from __future__ import annotations

import time
from typing import Callable, Dict

from .client import Board3CliClient
from .models import Board3CommandResult, SmokeTestConfig, SmokeTestReport, SmokeTestStepResult


class Board3SmokeTestRunner:
    """按固定步骤执行 board3 冒烟测试。"""

    def __init__(self, client: Board3CliClient, config: SmokeTestConfig) -> None:
        self._client = client
        self._config = config

    def _record_json_step(
        self,
        report: SmokeTestReport,
        name: str,
        command_fn: Callable[[], Board3CommandResult],
        predicate: Callable[[Dict[str, object]], bool],
        detail_builder: Callable[[Dict[str, object]], str],
    ) -> None:
        try:
            result = command_fn()
        except Exception as exc:  # pragma: no cover - runtime I/O failure path
            report.steps.append(
                SmokeTestStepResult(
                    name=name,
                    passed=False,
                    detail=f"exception: {exc}",
                    command="",
                    payload=None,
                )
            )
            return

        payload = result.payload or {}
        passed = result.ok and predicate(payload)
        if result.ok:
            detail = detail_builder(payload)
        else:
            detail = result.note or result.raw_response or "command failed"
        report.steps.append(
            SmokeTestStepResult(
                name=name,
                passed=passed,
                detail=detail,
                command=result.command,
                payload=payload,
            )
        )

    def _record_text_step(
        self,
        report: SmokeTestReport,
        name: str,
        command_fn: Callable[[], Board3CommandResult],
        predicate: Callable[[str], bool],
    ) -> None:
        try:
            result = command_fn()
        except Exception as exc:  # pragma: no cover - runtime I/O failure path
            report.steps.append(
                SmokeTestStepResult(
                    name=name,
                    passed=False,
                    detail=f"exception: {exc}",
                    command="",
                    payload=None,
                )
            )
            return

        passed = result.ok and predicate(result.raw_response)
        detail = result.raw_response if result.ok else (result.note or result.raw_response or "command failed")
        report.steps.append(
            SmokeTestStepResult(
                name=name,
                passed=passed,
                detail=detail,
                command=result.command,
                payload=None,
            )
        )

    def _poll_selftest_until_done(self, report: SmokeTestReport) -> None:
        """轮询 `sys selftest`，直到结束或超时。"""
        deadline = time.monotonic() + self._config.status_poll_timeout_s
        last_payload: Dict[str, object] = {}

        while time.monotonic() < deadline:
            try:
                result = self._client.sys_selftest_status(self._config.response_timeout_s)
            except Exception as exc:  # pragma: no cover - runtime I/O failure path
                report.steps.append(
                    SmokeTestStepResult(
                        name="sys_selftest",
                        passed=False,
                        detail=f"exception: {exc}",
                        command="sys selftest --json",
                        payload=None,
                    )
                )
                return
            if not result.ok:
                report.steps.append(
                    SmokeTestStepResult(
                        name="sys_selftest",
                        passed=False,
                        detail=result.note or result.raw_response or "command failed",
                        command=result.command,
                        payload=result.payload,
                    )
                )
                return
            last_payload = result.payload or {}
            if not bool(last_payload.get("running", False)):
                break
            time.sleep(self._config.status_poll_interval_s)

        results = last_payload.get("results", [])
        passed = (
            isinstance(results, list)
            and len(results) > 0
            and all(int(item.get("state", 0)) in (2, 4) for item in results if isinstance(item, dict))
        )
        detail = "selftest 已完成" if not bool(last_payload.get("running", False)) else "selftest 超时"
        report.steps.append(
            SmokeTestStepResult(
                name="sys_selftest",
                passed=passed and not bool(last_payload.get("running", False)),
                detail=detail,
                command="sys selftest",
                payload=last_payload,
            )
        )

    def run(self) -> SmokeTestReport:
        """执行当前配置下的 board3 冒烟测试序列。"""

        report = SmokeTestReport(
            port=self._client.port,
            baudrate=self._client.baudrate,
        )

        self._record_json_step(
            report,
            "sys_ping",
            lambda: self._client.sys_ping(self._config.response_timeout_s),
            lambda payload: payload.get("pong", False) is True,
            lambda payload: f"pong={payload.get('pong')}",
        )

        self._record_json_step(
            report,
            "sys_status",
            lambda: self._client.sys_status(self._config.response_timeout_s),
            lambda payload: "hv_mode" in payload and "faults" in payload,
            lambda payload: (
                f"armed={payload.get('armed')} "
                f"hv_mode={payload.get('hv_mode')} "
                f"faults={payload.get('faults')}"
            ),
        )

        self._record_json_step(
            report,
            "gpio_read",
            lambda: self._client.gpio_read(self._config.response_timeout_s),
            lambda payload: "interlock_closed" in payload,
            lambda payload: (
                f"biochip={payload.get('biochip_check')} "
                f"pos_sw1={payload.get('pos_sw1')} "
                f"bioarray={payload.get('bioarray_check')} "
                f"interlock={payload.get('interlock_closed')}"
            ),
        )

        self._record_json_step(
            report,
            "adc_read",
            lambda: self._client.adc_read(self._config.response_timeout_s),
            lambda payload: "array_raw" in payload and "voltage_raw" in payload,
            lambda payload: (
                f"array_raw={payload.get('array_raw')} "
                f"voltage_raw={payload.get('voltage_raw')} "
                f"sync_active={payload.get('sync_active')}"
            ),
        )

        self._record_json_step(
            report,
            "heater_status_cached",
            lambda: self._client.heater_status(self._config.response_timeout_s),
            lambda payload: "online" in payload and "duty" in payload,
            lambda payload: (
                f"online={payload.get('online')} "
                f"version={payload.get('version', '')}"
            ),
        )

        if self._config.verify_heater_ttl:
            self._record_json_step(
                report,
                "sys_verify_ttl",
                lambda: self._client.sys_verify_ttl(self._config.response_timeout_s),
                lambda payload: payload.get("rs232_verified", False) is True,
                lambda payload: f"rs232_verified={payload.get('rs232_verified')}",
            )

        if self._config.allow_arm:
            self._record_json_step(
                report,
                "sys_arm_on",
                lambda: self._client.sys_arm_on(self._config.response_timeout_s),
                lambda payload: payload.get("armed", False) is True,
                lambda payload: f"armed={payload.get('armed')}",
            )

        if self._config.allow_hv:
            # 高压相关命令默认放在显式开关后，避免脚本误触发危险操作。
            self._record_json_step(
                report,
                "hv_set_80",
                lambda: self._client.hv_set(80, self._config.response_timeout_s),
                lambda payload: int(payload.get("setpoint_v", 0)) == 80,
                lambda payload: f"setpoint_v={payload.get('setpoint_v')}",
            )

            self._record_json_step(
                report,
                "hv_freq_1000",
                lambda: self._client.hv_freq(1000, self._config.response_timeout_s),
                lambda payload: int(payload.get("ac_freq_hz", 0)) == 1000,
                lambda payload: f"ac_freq_hz={payload.get('ac_freq_hz')}",
            )

            self._record_json_step(
                report,
                "array_clear",
                lambda: self._client.array_clear(self._config.response_timeout_s),
                lambda payload: payload.get("logic") == [0] * 16,
                lambda payload: f"logic={payload.get('logic')}",
            )

            pattern = [0x01] + [0x00] * 15
            self._record_json_step(
                report,
                "array_set_pattern",
                lambda: self._client.array_set(pattern, self._config.response_timeout_s),
                lambda payload: isinstance(payload.get("logic"), list) and payload["logic"][0] == 1,
                lambda payload: f"logic={payload.get('logic')}",
            )

            self._record_json_step(
                report,
                "hv_mode_off",
                lambda: self._client.hv_mode("off", self._config.response_timeout_s),
                lambda payload: payload.get("mode") == "off",
                lambda payload: f"mode={payload.get('mode')}",
            )

        if self._config.verify_heater_ttl:
            self._record_text_step(
                report,
                "heater_ping",
                lambda: self._client.heater_ping(self._config.response_timeout_s),
                lambda response: "sent" in response.lower(),
            )

        if self._config.allow_heater_write:
            # 加热写命令也要求显式授权，便于后续接手者安全复用。
            self._record_text_step(
                report,
                "heater_set_ch1_5pct",
                lambda: self._client.heater_set(1, 5, self._config.response_timeout_s),
                lambda response: "sent" in response.lower(),
            )
            self._record_text_step(
                report,
                "heater_stop",
                lambda: self._client.heater_stop(self._config.response_timeout_s),
                lambda response: "sent" in response.lower(),
            )

        if self._config.run_selftest:
            self._record_json_step(
                report,
                "sys_selftest_start",
                lambda: self._client.sys_selftest_start(self._config.response_timeout_s),
                lambda payload: "results" in payload,
                lambda payload: f"running={payload.get('running')} step={payload.get('step')}",
            )
            self._poll_selftest_until_done(report)

        self._record_json_step(
            report,
            "all_stop",
            lambda: self._client.all_stop(self._config.response_timeout_s),
            lambda payload: payload.get("ok", False) is True,
            lambda payload: "all_stop executed",
        )

        return report
