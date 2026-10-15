# Drop_Board 硬件设计

保留1–4号板的原始文件名与相对目录结构，使用 Altium Designer 打开对应 `.PrjPcb` 工程。

| 板卡目录 | 工程入口 | PDF原理图 |
|---|---|---|
| [板1PCB_V1](Drop_Board/板1PCB_V1/) | `Main_board1.PrjPcb` | [Main_board1.pdf](Drop_Board/板1PCB_V1/Main_board1.pdf) |
| [板2PCB_V1](Drop_Board/板2PCB_V1/) | `Main_board2.PrjPcb` | [Main_board2.pdf](Drop_Board/板2PCB_V1/Main_board2.pdf) |
| [板3PCB_V1](Drop_Board/板3PCB_V1/) | `Main_board3.PrjPcb` | [Main_board3.pdf](Drop_Board/板3PCB_V1/Main_board3.pdf) |
| [板4PCB_V1](Drop_Board/板4PCB_V1/) | `Main_board4.PrjPcb` | [Main_board4.pdf](Drop_Board/板4PCB_V1/Main_board4.pdf) |

## 包含内容

- 原理图 `.SchDoc`、PCB `.PcbDoc`、原理图库 `.SchLib`、封装库 `.PcbLib`。
- Altium项目和输出配置 `.PrjPcb` / `.OutJob`。
- 原目录现有PDF、加工要求、2–4号板BOM、CubeMX配置及设计检查报告。
- [功能信号表_260111.xlsx](Drop_Board/功能信号表_260111.xlsx)。原目录未提供1号板独立BOM，本次未另行生成。

历史日志、History压缩备份、`新建文件夹`中的四个ZIP和自动生成的项目结构索引未上传。未删除或修改原硬件文件。

## 本次核对

从设备资料中的Drop_Board复制76个文件，约119MB；逐文件SHA256与源文件一致。桌面与工作区的原理图、PCB、库、PDF、表格及加工文档也已核对一致。单文件均小于GitHub的100MiB限制。

四个工程引用的原理图、PCB、库与输出配置均存在。4号板工程还引用了源目录本就缺失的旧报告 `PCB_bioarray_V1.0.PcbDoc.htm`；该文件为HTML报告，不是PCB设计本体。保持工程原文，没有伪造报告或修改引用。1号板两个零字节备用库文件亦原样保留。

本次仅上传与校验文件，未重新运行Altium编译或DRC，现有报告和制造资料不代表本次重新验证。荧光与加热功能仍按当前项目状态标注为暂未启用。
