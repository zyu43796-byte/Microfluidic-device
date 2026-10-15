"""board3 测试工具公共数据模型。"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional


@dataclass
class Board3CommandResult:
    """表示一次 CLI 命令执行结果。"""

    command: str
    raw_response: str
    ok: bool
    payload: Optional[Dict[str, Any]] = None
    note: str = ""


@dataclass
class SmokeTestStepResult:
    """表示一条冒烟测试步骤结果。"""

    name: str
    passed: bool
    detail: str
    command: str = ""
    payload: Optional[Dict[str, Any]] = None


@dataclass
class SmokeTestConfig:
    """冒烟测试执行器使用的配置项。"""

    response_timeout_s: float = 1.5
    status_poll_timeout_s: float = 5.0
    status_poll_interval_s: float = 0.25
    allow_arm: bool = False
    allow_hv: bool = False
    verify_heater_ttl: bool = False
    allow_heater_write: bool = False
    run_selftest: bool = False


@dataclass
class SmokeTestReport:
    """完整的冒烟测试报告。"""

    port: str
    baudrate: int
    steps: List[SmokeTestStepResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        """当所有步骤都通过时返回 True。"""

        return all(step.passed for step in self.steps)
