# idjcmonitor.py (C) 2013-2018 Stephen Fairchild
# Released under the GNU Lesser General Public License version 2.0 (or
# at your option, any later version).

"""A middleware facilitator for IDJC aimed towards station admin tasks.

Requires IDJC 0.8.9 or higher.

Example usage: http://idjc.sourceforge.net/code_idjcmon.html
"""

import os
import sys
import time

from gi.repository import GObject
import dbus
from dbus.mainloop.glib import DBusGMainLoop


__all__ = ["IDJCMonitor"]


BUS_BASENAME = "net.sf.idjc"
OBJ_BASENAME = "/net/sf/idjc"


def pid_exists(pid):
    """Check whether pid exists in the current process table."""

    if pid < 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError as e:
        return e.errno == os.errno.EPERM
    else:
        return True


class IDJCMonitor(GObject.Object):
    """Monitor IDJC internals relating to a specific profile or session.
    
    Can yield information about streams, music metadata, health.
    example usage: http://idjc.sourceforge.net/code_idjcmon.html
    """
    
    __gsignals__ = {
        'launch': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                    (GObject.TYPE_STRING, GObject.TYPE_UINT)),
        'quit': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                  (GObject.TYPE_STRING, GObject.TYPE_UINT)),
        'streamstate-changed': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                                 (GObject.TYPE_INT, GObject.TYPE_BOOLEAN,
                                  GObject.TYPE_STRING)),
        'recordstate-changed': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                                 (GObject.TYPE_INT, GObject.TYPE_BOOLEAN,
                                  GObject.TYPE_STRING)),
        'channelstate-changed': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                                  (GObject.TYPE_UINT, GObject.TYPE_BOOLEAN)),
        'voip-mode-changed': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                               (GObject.TYPE_UINT,)),
        'metadata-changed': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                              (GObject.TYPE_STRING,) * 5),
        'effect-started': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                           (GObject.TYPE_STRING,) * 2 + (GObject.TYPE_UINT,)),
        'effect-stopped': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                           (GObject.TYPE_UINT,)),
        'player-started': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                           (GObject.TYPE_STRING,)),
        'player-stopped': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                           (GObject.TYPE_STRING,)),
        'tracks-finishing': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                             ()),
        'announcement': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                          (GObject.TYPE_STRING,) * 3),
        'frozen': (GObject.SIGNAL_RUN_LAST, GObject.TYPE_NONE,
                    (GObject.TYPE_STRING, GObject.TYPE_UINT,
                     GObject.TYPE_BOOLEAN))
    }
    
    __gproperties__ = {
        'artist': (GObject.TYPE_STRING, 'artist', 'artist from track metadata',
                   "", GObject.PARAM_READABLE),
        'title': (GObject.TYPE_STRING, 'title', 'title from track metadata',
                  "", GObject.PARAM_READABLE),
        'album': (GObject.TYPE_STRING, 'album', 'album from track metadata',
                  "", GObject.PARAM_READABLE),
        'songname': (GObject.TYPE_STRING, 'songname',
                     'the song name from metadata tags when available'
                     ' and from the filenmame when not',
                     "", GObject.PARAM_READABLE),
        'music-filename': (GObject.TYPE_STRING, 'music_filename',
                           'the audio file pathname of the track',
                           "", GObject.PARAM_READABLE),
        'streaminfo': (GObject.TYPE_PYOBJECT, 'streaminfo',
                       'information about the streams',
                       GObject.PARAM_READABLE),
        'recordinfo': (GObject.TYPE_PYOBJECT, 'recordinfo',
                       'information about the recorders',
                       GObject.PARAM_READABLE),
        'channelinfo': (GObject.TYPE_PYOBJECT, 'channelinfo',
                        'toggle state of the audio channels',
                        GObject.PARAM_READABLE),
        'voip-mode': (GObject.TYPE_UINT, 'voip-mode',
                      'voice over ip mixer mode', 0, 2, 0,
                      GObject.PARAM_READABLE)
    }
    
    def __init__(self, profile):
        """Takes the profile parameter e.g. "default".
        
        Can also handle sessions with "session.sessionname"
        """
        
        GObject.GObject.__init__(self)
        self.__profile = profile
        self.__bus = dbus.SessionBus(mainloop=DBusGMainLoop())
        self.__bus_address = ".".join((BUS_BASENAME, profile))
        self.__base_objpath = OBJ_BASENAME
        self.__base_interface = BUS_BASENAME
        self.__artist = self.__title = self.__album = ""
        self.__songname = self.__music_filename = ""
        self.__shutdown = False
        self._start_probing()

    @property
    def main(self):
        """A DBus interface to the main object.
        
        Code that uses this should catch any AttributeError exceptions.
        """
        return dbus.Interface(self.__main, self.__base_interface)
        
    @property
    def output(self):
        """A DBus interface to the output object.
        
        Code that uses this should catch any AttributeError exceptions.
        """
        return dbus.Interface(self.__output, self.__base_interface)

    @property
    def controls(self):
        """A DBus interface to the controls object.
        
        Code that uses this should catch any AttributeError exceptions.
        """
        return dbus.Interface(self.__controls, self.__base_interface)
        
    def shutdown(self):
        """Block both signal emission and property reads."""
        
        self.__shutdown = True

    def _start_probing(self):
        self.__watchdog_id = None
        self.__probe_id = None
        self.__watchdog_notice = False
        self.__pid = 0
        self.__frozen = False
        self.__main = self.__output = self.__controls = None
        if not self.__shutdown:
            self.__probe_id = GObject.timeout_add_seconds(
                                                2, self._idjc_started_probe)

    def _idjc_started_probe(self):
        # Check for a newly started IDJC instance of the correct profile.
        
        def bgo(tail):
            return self.__bus.get_object(self.__bus_address,
                                         self.__base_objpath + tail)
        try:
            self.__main = bgo("/main")
            self.__output = bgo("/output")
            self.__controls = bgo("/controls")
            self.__player_left = bgo("/player/left")
            self.__player_right = bgo("/player/right")
            self.__player_interlude = bgo("/player/interlude")
            self.__players = (self.__player_left, self.__player_right,
                              self.__player_interlude)

            main_iface = dbus.Interface(self.__main, self.__base_interface)
            main_iface.pid(reply_handler=self._pid_reply_handler,
                           error_handler=self._pid_error_handler)

        except dbus.exceptions.DBusException:
            # Keep searching periodically.
            return not self.__shutdown
        else:
            return False

    def _pid_reply_handler(self, value):
        self.__pid = value
        
        def cts(obj, signal, handler):
            obj.connect_to_signal(signal, handler)

        try:
            cts(self.__main, "track_metadata_changed", self._metadata_handler)
            cts(self.__main, "effect_started", self._effect_started_handler)
            cts(self.__main, "effect_stopped", self._effect_stopped_handler)
            cts(self.__main, "quitting", self._quit_handler)
            cts(self.__main, "heartbeat", self._heartbeat_handler)
            cts(self.__main, "channelstate_changed", self._channelstate_handler)
            cts(self.__main, "voip_mode_changed", self._voip_mode_handler)
            cts(self.__main, "tracks_finishing", self._tracks_finishing_handler)
            cts(self.__output, "streamstate_changed", self._streamstate_handler)
            cts(self.__output, "recordstate_changed", self._recordstate_handler)
            for each in self.__players:
                cts(each, "playing", self._playing_handler)
                cts(each, "announcement", self._announcement_handler)

            # Start watchdog thread.
            self.__watchdog_id = GObject.timeout_add_seconds(3, self._watchdog)

            self.__streams = {n: (False, "unknown") for n in range(10)}
            self.__recorders = {n: (False, "unknown") for n in range(4)}
            self.__channels = [False]*12
            self.__voip_mode = 0
            main_iface = dbus.Interface(self.__main, self.__base_interface)
            output_iface = dbus.Interface(self.__output, self.__base_interface)
            
            self.emit("launch", self.__profile, self.__pid)
            
            # Tell IDJC to initialize as empty its cache of sent data.
            # This yields a dump of server related info.
            main_iface.new_plugin_started()
            output_iface.new_plugin_started()
        except dbus.exceptions.DBusException:
            self._start_probing()

    def _pid_error_handler(self, error):
        self._start_probing()

    def _watchdog(self):
        if self.__watchdog_notice:
            if pid_exists(int(self.__pid)):
                if not self.__frozen:
                    self.__frozen = True
                    self.emit("frozen", self.__profile, self.__pid, True)
                return True
            else:
                for id_, (conn, where) in self.__streams.iteritems():
                    if conn:
                        self._streamstate_handler(id_, 0, where)

                for id_, (rec, where) in self.__recorders.iteritems():
                    if rec:
                        self._recordstate_handler(id_, 0, where)
                        
                for index, open_ in enumerate(self.__channels):
                    if open_:
                        self._channelstate_handler(index, 0)
                
                self._quit_handler()
                return False
        elif self.__frozen:
            self.__frozen = False
            self.emit("frozen", self.__profile, self.__pid, False)

        self.__watchdog_notice = True
        return not self.__shutdown

    def _heartbeat_handler(self):
        self.__watchdog_notice = False

    def _quit_handler(self):
        """Start scanning for a new bus object."""

        if self.__watchdog_id is not None:
            GObject.source_remove(self.__watchdog_id)
            self.emit("quit", self.__profile, self.__pid)
        self._start_probing()
        
    def _streamstate_handler(self, numeric_id, connected, where):
        numeric_id = int(numeric_id)
        connected = bool(connected)
        self.__streams[numeric_id] = (connected, where)
        self.notify("streaminfo")
        self.emit("streamstate-changed", numeric_id, connected, where)

    def _recordstate_handler(self, numeric_id, recording, where):
        numeric_id = int(numeric_id)
        recording = bool(recording)
        self.__recorders[numeric_id] = (recording, where)
        self.notify("recordinfo")
        self.emit("recordstate-changed", numeric_id, recording, where)

    def _channelstate_handler(self, numeric_id, open_):
        numeric_id = int(numeric_id)
        open_ = bool(open_)
        self.__channels[numeric_id] = open_
        self.notify("channelinfo")
        self.emit("channelstate-changed", numeric_id, open_)

    def _voip_mode_handler(self, mode):
        mode = int(mode)
        self.__voip_mode = mode
        self.notify("voip-mode")
        self.emit("voip-mode-changed", mode)

    def _tracks_finishing_handler(self):
        self.emit("tracks-finishing")

    def _metadata_handler(self, *args):
        # see definition of names below for 'args' specification.

        def update_property(name, value):
            oldvalue = getattr(self, f"_IDJCMonitor__{name}")
            if value != oldvalue:
                setattr(self, f"_IDJCMonitor__{name}", value)
                self.notify(name)

        names = "artist title album songname music_filename".split()
        for name, arg in zip(names, args):
            update_property(name, arg)

        self.emit("metadata-changed", self.__artist, self.__title,
                  self.__album, self.__songname, self.__music_filename)

    def _effect_started_handler(self, title, pathname, player):
        self.emit("effect-started", title, pathname, player)

    def _effect_stopped_handler(self, player):
        self.emit("effect-stopped", player)

    def _playing_handler(self, name, state):
        if state:
            self.emit("player-started", name)
        else:
            self.emit("player-stopped", name)

    def _announcement_handler(self, player, state, message):
        self.emit("announcement", player, state, message)

    def do_get_property(self, prop):
        if self.__shutdown:
            raise AttributeError(
                        "Attempt to read property after shutdown was called.")
        
        name = prop.name
        
        if name in ("artist", "title", "album", "songname", "music_filename",
                    "effect_pathname"):
            return getattr(self, "_IDJCMonitor__" + name)
        if name == "streaminfo":
            return tuple(self.__streams[n] for n in range(10))
        elif name == "recordinfo":
            return tuple(self.__recorders[n] for n in range(4))
        elif name == "channelinfo":
            return tuple(self.__channels[n] for n in range(12))
        elif name == "voip-mode":
            return self.__voip_mode
        else:
            raise AttributeError("Unknown property {} in {!r}".format(name, self))

    def notify(self, property_name):
        if not self.__shutdown:
            GObject.Object.notify(self, property_name)
            
    def emit(self, *args, **kwargs):
        if not self.__shutdown:
            GObject.Object.emit(self, *args, **kwargs)
