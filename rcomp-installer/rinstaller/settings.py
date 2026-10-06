"""Persistent settings (build/settings.json next to the tool; never inside the R-comp checkout)."""
import json
import os
import pathlib
import threading

TOOL_ROOT = pathlib.Path(__file__).resolve().parent.parent
HOME = pathlib.Path.home()


def default_layout(tool_root=TOOL_ROOT, home=HOME):
    """(R-comp checkout, folder for the work, app and disc folders) for the folder this tool sits in.

    In the repository the installer is rcomp-installer/ of the R-comp checkout, whose parent is therefore the checkout.
    The pipeline refuses a work or output folder inside the checkout, so those go next to it
    (<checkout>/../rcomp-installer-data). A separate copy of the tool (the developer's) keeps the earlier defaults:
    ~/rcomp, and build/ next to the tool."""
    parent = tool_root.parent
    if (parent / 'tools' / 'm6_inventory.py').is_file():
        return parent, parent.parent / 'rcomp-installer-data'
    return home / 'rcomp', tool_root / 'build'


RCOMP_ROOT, DATA_ROOT = default_layout()
IN_CHECKOUT = RCOMP_ROOT == TOOL_ROOT.parent

DEFAULTS = {
    'ps5_host': '',             # the console's address; set it in Settings
    'ftp_port': 2120,
    'loader_port': 9021,
    'ctl_port': 9111,
    'klog_port': 3232,
    'rcomp_root': str(RCOMP_ROOT),
    'deps_root': str(RCOMP_ROOT.parent / 'deps'),
    'cygwin_bash': '',          # empty: R-comp's build/vulkan-gta-radv-20260928/host-cygwin-full/bin/bash.exe
    'payload_sdk': '',          # ps5-payload-sdk root (holds bin/prospero-clang or win/prospero-clang.cmd)
    'output_dir': str(DATA_ROOT / 'apps'),
    'work_dir': str(DATA_ROOT / 'work'),
    'registration_wait': 90,
    'ftp_connections': 8,       # parallel FTP connections for upload / read-back
    'isos_dir': str(DATA_ROOT / 'isos' if IN_CHECKOUT else HOME / 'rcomp' / 'isos'),  # the discs 'Check all' goes through
    'watch_seconds': 180,       # how long 'Try it on my PS5' watches a game after starting it
}


class Settings:
    def __init__(self, path=None):
        self.path = pathlib.Path(path or TOOL_ROOT / 'build' / 'settings.json')
        self.lock = threading.Lock()
        self.values = dict(DEFAULTS)
        if self.path.is_file():
            try:
                self.values.update(json.loads(self.path.read_text(encoding='utf-8')))
            except ValueError:
                pass

    def get(self, key):
        with self.lock:
            return self.values.get(key, DEFAULTS.get(key))

    def all(self):
        with self.lock:
            return dict(self.values)

    def update(self, new):
        with self.lock:
            for k, v in new.items():
                if k not in DEFAULTS:
                    continue
                if isinstance(DEFAULTS[k], int):
                    v = int(v)
                self.values[k] = v
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix('.tmp')
            tmp.write_text(json.dumps(self.values, indent=2) + '\n', encoding='utf-8')
            os.replace(tmp, self.path)
            return dict(self.values)
