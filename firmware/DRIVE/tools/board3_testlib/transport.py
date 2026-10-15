"""board3 测试工具使用的串口传输层。"""

from __future__ import annotations

import time
from typing import Optional

try:
    import serial
except ImportError:  # pragma: no cover - optional dependency at runtime
    serial = None  # type: ignore[assignment]


class SerialTransportError(RuntimeError):
    """串口层无法完成操作时抛出的异常。"""


class SerialTransport:
    """简单的按行收发串口封装。"""

    def __init__(
        self,
        port: str,
        baudrate: int = 115200,
        timeout_s: float = 0.2,
        write_timeout_s: float = 1.0,
    ) -> None:
        self._port = port
        self._baudrate = baudrate
        self._timeout_s = timeout_s
        self._write_timeout_s = write_timeout_s
        self._serial: Optional["serial.Serial"] = None

    @property
    def port(self) -> str:
        return self._port

    @property
    def baudrate(self) -> int:
        return self._baudrate

    def open(self) -> None:
        """打开串口并清空已有接收缓存。"""

        if serial is None:
            raise SerialTransportError(
                "未安装 pyserial，请先执行：pip install pyserial"
            )

        if self._serial is not None:
            return

        self._serial = serial.Serial(
            port=self._port,
            baudrate=self._baudrate,
            timeout=self._timeout_s,
            write_timeout=self._write_timeout_s,
        )
        self.drain_input()

    def close(self) -> None:
        """关闭串口。"""

        if self._serial is not None:
            self._serial.close()
            self._serial = None

    def __enter__(self) -> "SerialTransport":
        self.open()
        return self

    def __exit__(self, exc_type, exc, exc_tb) -> None:
        self.close()

    def _require_open(self) -> "serial.Serial":
        if self._serial is None:
            raise SerialTransportError("serial port is not open")
        return self._serial

    def drain_input(self) -> None:
        """在发送新命令前清空遗留接收数据。"""

        port = self._require_open()
        port.reset_input_buffer()

    def write_line(self, line: str) -> None:
        """按 CRLF 结尾发送一行命令。"""

        port = self._require_open()
        payload = (line.strip() + "\r\n").encode("ascii")
        port.write(payload)
        port.flush()

    def read_line(self, timeout_s: float) -> str:
        """在超时前读取一行非空文本。"""

        port = self._require_open()
        deadline = time.monotonic() + timeout_s

        while time.monotonic() < deadline:
            raw = port.readline()
            if not raw:
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                return line

        raise SerialTransportError(f"串口 {self._port} 等待响应超时")
