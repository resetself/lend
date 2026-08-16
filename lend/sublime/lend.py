# Lend window watcher for Sublime Text.
#
# Sublime Text's `subl -w` flag only waits for *files*, so a bare
# `subl -w <dir>` returns immediately while the directory window stays open.
# That leaves the Lend daemon (lendd) with no reliable "window closed" signal.
#
# This plugin reports the folders currently open across all windows to a marker
# file (~/.lend/sublime_windows.json) that lendd polls, so a directory editing
# session can end the moment its window is closed. It is a tiny EventListener
# plus a periodic heartbeat (to keep the marker fresh so lendd knows the plugin
# is active). It is safe to keep installed even when Lend is not in use.
import json
import os
import time

import sublime
import sublime_plugin

MARKER = os.path.expanduser("~/.lend/sublime_windows.json")
HEARTBEAT_MS = 10000


def _folders():
    out = []
    for w in sublime.windows():
        out.extend(w.folders())
    return out


def _dump():
    try:
        data = {"ts": time.time(), "folders": _folders()}
        tmp = MARKER + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f)
        os.replace(tmp, MARKER)
    except Exception:
        pass


def _heartbeat():
    _dump()
    sublime.set_timeout(_heartbeat, HEARTBEAT_MS)


def plugin_loaded():
    _dump()
    sublime.set_timeout(_heartbeat, HEARTBEAT_MS)


class LendWindowListener(sublime_plugin.EventListener):
    def on_activated(self, view):
        _dump()

    def on_new_window(self, window):
        # The new window's folders are not set yet; dump slightly later.
        sublime.set_timeout(_dump, 100)

    def on_pre_close_window(self, window):
        # The window is not removed from sublime.windows() until just after
        # this callback, so dump after a short delay to reflect the removal.
        sublime.set_timeout(_dump, 200)

    def on_exit(self):
        # App is quitting: clear the marker synchronously so lendd ends any
        # directory session immediately (no delayed callback can be relied on
        # here since the plugin host is tearing down).
        try:
            data = {"ts": time.time(), "folders": []}
            tmp = MARKER + ".tmp"
            with open(tmp, "w") as f:
                json.dump(data, f)
            os.replace(tmp, MARKER)
        except Exception:
            pass
