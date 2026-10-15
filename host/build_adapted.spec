# Run: .venv/Scripts/python.exe -m PyInstaller --noconfirm build_adapted.spec
from pathlib import Path
root = Path(SPECPATH)
a = Analysis([str(root / 'adapted_main.py')], pathex=[str(root)],
             datas=[(str(root / 'chip/chip_point.csv'), 'chip'),
                    (str(root / 'icon'), 'icon'),
                    (str(root / 'assets/startup/xiuyun.mp4'), 'assets/startup'),
                    (str(root / 'assets/ui'), 'assets/ui')],
             hiddenimports=['serial.tools.list_ports_windows'],
             excludes=['PyQt6', 'PySide6', 'PySide2', 'matplotlib', 'scipy', 'pandas', 'IPython'],
             noarchive=False)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name='DMF_Host_Adapted',
          debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
          console=False, disable_windowed_traceback=False)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name='DMF_Host_Adapted')
