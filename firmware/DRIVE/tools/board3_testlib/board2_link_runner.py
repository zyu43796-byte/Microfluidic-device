"""通过 board3 CLI 做 board2 链路联调检查。"""

from __future__ import annotations

import time
from typing import Callable, Dict, Optional, Tuple

from .client import Board3CliClient
from .models import Board3CommandResult, SmokeTestReport, SmokeTestStepResult


class Board3Board2LinkRunner:
    """按固定步骤执行 board3 与 board2 的链路验证。"""

    def __init__(
        self,
        client: Board3CliClient,
        response_timeout_s: float = 1.5,
        poll_timeout_s: float = 5.0,
        poll_interval_s: float = 0.25,
        confirm_heater_ttl: bool = False,
        allow_arm: bool = False,
        allow_heater_write: bool = False,
        write_channel: int = 1,
        write_duty_pct: int = 5,
    ) -> None:
        if write_channel not in (1, 2, 3):
            raise ValueError("write_channel 必须是 1/2/3")
        if (write_duty_pct < 0) or (write_duty_pct > 100):
            raise ValueError("write_duty_pct 必须在 0~100")
        if response_timeout_s <= 0:
            raise ValueError("response_timeout_s 必须 > 0")
        if poll_timeout_s <= 0:
            raise ValueError("poll_timeout_s 必须 > 0")
        if poll_interval_s <= 0:
            raise ValueError("poll_interval_s 必须 > 0")

        self._client = client
        self._response_timeout_s = response_timeout_s
        self._poll_timeout_s = poll_timeout_s
        self._poll_interval_s = poll_interval_s
        self._confirm_heater_ttl = confirm_heater_ttl
        self._allow_arm = allow_arm
        self._allow_heater_write = allow_heater_write
        self._write_channel = write_channel
        self._write_duty_pct = write_duty_pct

    def _record_json_step(
        self,
        report: SmokeTestReport,
        name: str,
        command_fn: Callable[[], Board3CommandResult],
        predicate: Callable[[Dict[str, object]], bool],
        detail_builder: Callable[[Dict[str, object]], str],
    ) -> Optional[Dict[str, object]]:
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
            return None

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
        return payload

    def _record_text_step(
        self,
        report: SmokeTestReport,
        name: str,
        command_fn: Callable[[], Board3CommandResult],
        predicate: Callable[[str], bool],
    ) -> str:
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
            return ""

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
        return result.raw_response

    def _poll_heater_status(
        self,
        predicate: Callable[[Dict[str, object]], bool],
    ) -> Tuple[Dict[str, object], str]:
        """轮询 heater status，直到满足条件或超时。"""
        deadline = time.monotonic() + self._poll_timeout_s
        last_payload: Dict[str, object] = {}

        while time.monotonic() < deadline:
            try:
                result = self._client.heater_status(self._response_timeout_s)
            except Exception as exc:  # pragma: no cover - runtime I/O failure path
                return last_payload, f"exception: {exc}"
            last_payload = result.payload or {}
            if result.ok and predicate(last_payload):
                return last_payload, ""
            time.sleep(self._poll_interval_s)

        return last_payload, ""

    def _record_polled_status(
        self,
        report: SmokeTestReport,
        name: str,
        predicate: Callable[[Dict[str, object]], bool],
        detail_builder: Callable[[Dict[str, object]], str],
    ) -> Dict[str, object]:
        payload, poll_error = self._poll_heater_status(predicate)
        passed = (poll_error == "") and predicate(payload)
        detail = poll_error if poll_error else detail_builder(payload)
        report.steps.append(
            SmokeTestStepResult(
                name=name,
                passed=passed,
                detail=detail,
                command="heater status --json",
                payload=payload,
            )
        )
        return payload

    @staticmethod
    def _heater_payload_has_three_channels(payload: Dict[str, object]) -> bool:
        ntc = payload.get("ntc")
        current = payload.get("current")
        power = payload.get("power")
        duty = payload.get("duty")
        return (
            isinstance(ntc, list)
            and len(ntc) == 3
            and isinstance(current, list)
            and len(current) == 3
            and isinstance(power, list)
            and len(power) == 3
            and isinstance(duty, list)
            and len(duty) == 3
        )

    def run(self) -> SmokeTestReport:
        """执行 board3 与 board2 的联调检查。"""
        report = SmokeTestReport(port=self._client.port, baudrate=self._client.baudrate)

        self._record_json_step(
            report,
            "sys_status_before_link",
            lambda: self._client.sys_status(self._response_timeout_s),
            lambda payload: "rs232_verified" in payload,
            lambda payload: (
                f"rs232_verified={payload.get('rs232_verified')} "
                f"heater_online={payload.get('heater_online')}"
            ),
        )

        if self._confirm_heater_ttl:
            self._record_json_step(
                report,
                "sys_verify_ttl",
                lambda: self._client.sys_verify_ttl(self._response_timeout_s),
                lambda payload: payload.get("rs232_verified", False) is True,
                lambda payload: f"rs232_verified={payload.get('rs232_verified')}",
            )
        else:
            report.steps.append(
                SmokeTestStepResult(
                    name="sys_verify_ttl",
                    passed=False,
                    detail="联调前必须先显式确认板3到板2链路是 TTL 电平",
                    command="sys verify-rs232 ttl --json",
                    payload=None,
                )
            )
            return report

        self._record_text_step(
            report,
            "heater_ping_command",
            lambda: self._client.heater_ping(self._response_timeout_s),
            lambda response: "sent" in response.lower(),
        )

        self._record_polled_status(
            report,
            "heater_online_poll",
            lambda payload: bool(payload.get("online", False)),
            lambda payload: (
                f"online={payload.get('online')} "
                f"version={payload.get('version', '')} "
                f"last_response={payload.get('last_response', '')}"
            ),
        )

        self._record_json_step(
            report,
            "heater_version_command",
            lambda: self._client.heater_version(self._response_timeout_s),
            lambda payload: "version" in payload,
            lambda payload: f"version={payload.get('version', '')}",
        )

        self._record_polled_status(
            report,
            "heater_version_poll",
            lambda payload: isinstance(payload.get("version"), str) and len(payload.get("version", "")) > 0,
            lambda payload: f"version={payload.get('version', '')}",
        )

        self._record_json_step(
            report,
            "heater_status_shape",
            lambda: self._client.heater_status(self._response_timeout_s),
            self._heater_payload_has_three_channels,
            lambda payload: (
                f"ntc={payload.get('ntc')} current={payload.get('current')} "
                f"power={payload.get('power')} duty={payload.get('duty')}"
            ),
        )

        for channel in (1, 2, 3):
            self._record_json_step(
                report,
                f"heater_ntc_ch{channel}",
                lambda ch=channel: self._client.heater_ntc(ch, self._response_timeout_s),
                self._heater_payload_has_three_channels,
                lambda payload, ch=channel: f"ntc{ch}={payload.get('ntc', [None, None, None])[ch - 1]}",
            )

            self._record_json_step(
                report,
                f"heater_cur_ch{channel}",
                lambda ch=channel: self._client.heater_cur(ch, self._response_timeout_s),
                self._heater_payload_has_three_channels,
                lambda payload, ch=channel: f"cur{ch}={payload.get('current', [None, None, None])[ch - 1]}",
            )

            self._record_json_step(
                report,
                f"heater_pwr_ch{channel}",
                lambda ch=channel: self._client.heater_pwr(ch, self._response_timeout_s),
                self._heater_payload_has_three_channels,
                lambda payload, ch=channel: f"pwr{ch}={payload.get('power', [None, None, None])[ch - 1]}",
            )

        if self._allow_arm:
            self._record_json_step(
                report,
                "sys_arm_on_for_heater_write",
                lambda: self._client.sys_arm_on(self._response_timeout_s),
                lambda payload: payload.get("armed", False) is True,
                lambda payload: f"armed={payload.get('armed')}",
            )

        if self._allow_heater_write:
            self._record_text_step(
                report,
                "heater_set_command",
                lambda: self._client.heater_set(self._write_channel, self._write_duty_pct, self._response_timeout_s),
                lambda response: "sent" in response.lower(),
            )

            self._record_polled_status(
                report,
                "heater_set_effect_poll",
                lambda payload: (
                    isinstance(payload.get("duty"), list)
                    and len(payload["duty"]) >= self._write_channel
                    and int(payload["duty"][self._write_channel - 1]) == self._write_duty_pct
                ),
                lambda payload: f"duty={payload.get('duty')}",
            )

            self._record_text_step(
                report,
                "heater_stop_command",
                lambda: self._client.heater_stop(self._response_timeout_s),
                lambda response: "sent" in response.lower(),
            )

            self._record_polled_status(
                report,
                "heater_stop_effect_poll",
                lambda payload: isinstance(payload.get("duty"), list) and sum(int(value) for value in payload["duty"]) == 0,
                lambda payload: f"duty={payload.get('duty')}",
            )

        self._record_json_step(
            report,
            "all_stop_after_link_test",
            lambda: self._client.all_stop(self._response_timeout_s),
            lambda payload: payload.get("ok", False) is True,
            lambda payload: "all_stop executed",
        )

        return report
