"""Keep the bundled Qt and Shiboken DLL folders in Windows' loader search path."""
import os
import sys

_dll_directory_handles = []
if sys.platform == "win32" and getattr(sys, "frozen", False):
    for _folder in ("PySide6", "shiboken6"):
        _path = os.path.join(sys._MEIPASS, _folder)
        if os.path.isdir(_path):
            _dll_directory_handles.append(os.add_dll_directory(_path))
            os.environ["PATH"] = _path + os.pathsep + os.environ.get("PATH", "")
