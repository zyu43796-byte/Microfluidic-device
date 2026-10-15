# board3 测试工具说明

这个目录保留 board3 历史维护/联调工具，不参与 MCU 编译。

## 2026-10-15 清理版兼容性提醒

当前固件已删除主动 `sys selftest start/status`、阵列底层实验命令、`hv pot/acburst`、`magnet pulse`。灯带静态命令的保持时间只接受省略或0，不再支持正数阻塞等待。

**不要对当前清理版运行 `--run-selftest`。** 下文含该选项的示例仅供旧维护固件参考，当前固件会拒绝相应命令。其余测试也没有在本次连接真实硬件执行，不能把保留脚本当成已通过的板卡验证。带 `--allow-hv`、`--allow-heater-write` 的脚本会驱动真实输出，只能在设备处于安全测试状态并明确授权后使用。

本次已执行的是工程外的源码静态保护检查，以及上位机模拟/单元测试；详情见工程根目录《固件清理结果.md》。旧工具源码原样保留，供维护使用。

## 目录结构

- `board3_testlib/transport.py`
  - 串口传输层
- `board3_testlib/client.py`
  - board3 CLI 高层命令封装
- `board3_testlib/runner.py`
  - board3 单板冒烟测试执行器
- `board3_testlib/board2_link_runner.py`
  - board3 到 board2 的链路联调执行器
- `run_board3_smoketest.py`
  - board3 单板测试入口
- `run_board3_board2_linktest.py`
  - board3 + board2 联调测试入口
- `run_board3_gate_suite.py`
  - 一键 Gate 回归入口（串联 G3/G5/G8/G10）

## 依赖安装

先安装 `pyserial`：

```bash
pip install pyserial
```

## 使用示例

只读冒烟测试：

```bash
python tools/run_board3_smoketest.py --port COM7
```

带 TTL 确认和 selftest 的测试：

```bash
python tools/run_board3_smoketest.py --port COM7 --verify-heater-ttl --run-selftest
```

带加热写命令的冒烟测试（需要 arm + TTL 确认）：

```bash
python tools/run_board3_smoketest.py --port COM7 --allow-arm --verify-heater-ttl --allow-heater-write
```

带高压相关命令的测试：

```bash
python tools/run_board3_smoketest.py --port COM7 --allow-arm --allow-hv
```

board3 + board2 链路验证：

```bash
python tools/run_board3_board2_linktest.py --port COM7 --confirm-heater-ttl
```

board3 + board2 链路验证，并附带一轮加热写命令：

```bash
python tools/run_board3_board2_linktest.py --port COM7 --confirm-heater-ttl --allow-arm --allow-heater-write --write-channel 1 --write-duty 5
```

一键 Gate 回归（默认 G3,G5,G8,G10）：

```bash
python tools/run_board3_gate_suite.py --port COM7 --allow-arm --allow-hv --allow-heater-write
```

仅跑基础链路（G3 + G5）：

```bash
python tools/run_board3_gate_suite.py --port COM7 --gates G3,G5 --confirm-heater-ttl
```

一键回归并保存报告：

```bash
python tools/run_board3_gate_suite.py --port COM7 --allow-arm --allow-hv --allow-heater-write --run-selftest --save reports/gate_suite_latest.json
```

## 安全说明

- `--allow-arm` 会执行 `sys arm on`
- `--allow-hv` 会执行高压相关命令
- `--verify-heater-ttl` 会执行 `sys verify-rs232 ttl`
- `--allow-heater-write` 会执行 `heater set` 和 `heater stop`

`--allow-heater-write` 额外要求：

- `run_board3_smoketest.py`：同时给出 `--allow-arm` 和 `--verify-heater-ttl`
- `run_board3_board2_linktest.py`：同时给出 `--allow-arm` 和 `--confirm-heater-ttl`

如果硬件还没有处于安全测试状态，不要启用：

- `--allow-hv`
- `--allow-heater-write`

说明：

- 分步脚本 `run_board3_smoketest.py` 与 `run_board3_board2_linktest.py` 继续保留，不会被一键脚本替代。
- 一键脚本适合回归复测；定位问题时建议回到分步脚本逐项下钻。
