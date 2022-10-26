#   effects.py: simple sound effect players -- part of IDJC.
#   Copyright 2012-2022 Stephen Fairchild (s-fairchild@users.sourceforge.net)
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program in the file entitled COPYING.
#   If not, see <http://www.gnu.org/licenses/>.

import os
import time
import gettext
import json
import uuid
import itertools
import urllib

import gi
from gi.repository import Gtk
from gi.repository import Gdk
from gi.repository import GdkPixbuf
from gi.repository import GObject
from gi.repository import Pango

from idjc import *
from .playergui import *
from .prelims import *
from .gtkstuff import LEDDict
from .gtkstuff import WindowSizeTracker
from .gtkstuff import DefaultEntry
from .gtkstuff import timeout_add, source_remove
from .tooltips import set_tip
from .utils import LinkUUIDRegistry

_ = gettext.translation(FGlobs.package_name, FGlobs.localedir,
                                                        fallback=True).gettext

PM = ProfileManager()
link_uuid_reg = LinkUUIDRegistry()

# Pixbufs for LED's of the specified size.
LED = LEDDict(9)


class Effect(Gtk.Grid):
    """A trigger button for an audio effect or jingle.

    Takes a numeric parameter for identification. Also includes numeric I.D.,
    L.E.D., stop, and config button.
    """

    CONFIG_TARGET = Gdk.Atom.intern('application/x-idjc-effect', False)

    def __init__(self, num, others, parent):
        self.num = num
        self.others = others
        self.approot = parent
        self.pathname = None
        self.uuid = str(uuid.uuid4())
        self._repeat_works = False

        Gtk.Grid.__init__(self, column_homogeneous=True, row_homogeneous=True)
        self.set_border_width(2)
        self.set_column_spacing(3)

        self.label = Gtk.Label.new("{:02d}".format(num + 1))
        pad = Gtk.Box()
        sizegroup = Gtk.SizeGroup.new(Gtk.SizeGroupMode.HORIZONTAL)
        sizegroup.add_widget(self.label)
        sizegroup.add_widget(pad)

        self.highlight = 0

        image = Gtk.Image.new_from_icon_name("media-playback-stop-symbolic", Gtk.IconSize.BUTTON)
        self.stop = Gtk.Button()
        self.stop.add(image)
        self.attach(self.stop, 0, 0, 3, 1)
        self.stop.connect("clicked", self._on_stop)
        set_tip(self.stop, _('Stop'))

        self.trigger = Gtk.Button()
        self.attach_next_to(self.trigger, self.stop, Gtk.PositionType.RIGHT, 12, 1)

        self.progress = Gtk.ProgressBar()
        self.progress.set_size_request(20, 0)
        self.progress.set_orientation(Gtk.Orientation.HORIZONTAL)

        self.trigger_label = Gtk.Label()
        self.trigger_label.set_ellipsize(Pango.EllipsizeMode.END)

        trigger_label_box = Gtk.HBox()
        trigger_label_box.set_spacing(3)
        trigger_label_box.pack_start(self.label, False)
        trigger_label_box.pack_start(self.trigger_label, True)
        trigger_label_box.pack_end(pad, False)

        vbox = Gtk.VBox()
        vbox.pack_start(trigger_label_box, True)
        vbox.pack_start(self.progress, True)
        self.trigger.add(vbox)

        self.trigger.connect("clicked", self._on_trigger)
        set_tip(self.trigger, _('Play'))

        self.repeat = Gtk.ToggleButton()
        image = Gtk.Image.new_from_icon_name("media-playlist-repeat-symbolic",
                                             Gtk.IconSize.BUTTON)
        self.repeat.add(image)
        image.show()
        self.attach_next_to(self.repeat, self.trigger, Gtk.PositionType.RIGHT, 2, 1)
        set_tip(self.repeat, _('Repeat'))

        self.config_image = Gtk.Image.new_from_icon_name("media-tape-symbolic",
                                             Gtk.IconSize.BUTTON)
        self.config = Gtk.Button()
        self.config.add(self.config_image)
        self.attach_next_to(self.config, self.repeat, Gtk.PositionType.RIGHT, 2, 1)
        self.config.connect("clicked", self._on_config)
        self.config.drag_source_set(Gdk.ModifierType.BUTTON1_MASK, None, Gdk.DragAction.COPY)
        target_list = Gtk.TargetList()
        target_list.add(self.CONFIG_TARGET, Gtk.TargetFlags.SAME_APP, 1)
        self.config.drag_source_set_target_list(target_list)

        self.config.connect("drag-begin", self._drag_begin)
        self.config.connect("drag-data-get", self._drag_get_data)
        set_tip(self.config, _('Configure'))
        self.config.drag_dest_set(Gtk.DestDefaults.ALL, None, Gdk.DragAction.COPY)
        target_list = Gtk.TargetList()
        target_list.add_uri_targets(0)
        target_list.add_text_targets(0)
        target_list.add(self.CONFIG_TARGET, Gtk.TargetFlags.SAME_APP, 1)
        self.config.drag_dest_set_target_list(target_list)
        self.config.connect("drag-data-received", self._drag_data_received)

        self.dialog = EffectConfigDialog(self, parent.window)
        self.dialog.connect("response", self._on_dialog_response)
        self.dialog.emit("response", Gtk.ResponseType.NO)
        self.timeout_source_id = None
        self.interlude = IDJC_Media_Player(None, None, parent)
        self.effect_length = 0.0
        # Create the widget that will be used in the tab
        self.tabwidget = Gtk.HBox()
        self.tabwidget.set_spacing(3)
        sep = Gtk.VSeparator()
        self.tabwidget.pack_start(sep)
        vb = Gtk.VBox()
        self.tabwidget.pack_start(vb)
        hb = Gtk.HBox()
        hb.set_spacing(3)
        self.tabeffectname = Gtk.Label()
        self.tabeffecttime = Gtk.Label()
        hb.pack_start(self.tabeffectname)
        hb.pack_start(self.tabeffecttime)
        vb.pack_start(hb)
        self.tabeffectprog = Gtk.ProgressBar()
        self.tabeffectprog.set_size_request(0, 5)
        vb.pack_start(self.tabeffectprog)
        self.tabwidget.show_all()

    def _drag_begin(self, widget, context):
        widget.drag_source_set_icon_name("media-tape-symbolic")

    def _drag_get_data(self, widget, context, selection, target_id, etime):
        selection.set(selection.get_target(), 8, (self.num,))
        return True

    def _drag_data_received(self, widget, context, x, y, dragged, info, etime):
        if info == 1:
            other = self.others[dragged.get_data()[0]]
            if other != self:
                self.stop.clicked()
                other.stop.clicked()
                self._swap(other)
                return True
        else:
            data = dragged.get_data().splitlines()
            if len(data) == 1 and data[0].startswith(b"file://"):
                pathname = urllib.request.unquote(data[0][7:].decode('utf-8'))
                title = self.interlude.get_media_metadata(pathname).title
                if title:
                    self.stop.clicked()
                    self._set(pathname, title, 0.0)
                    return True
        return False

    def _swap(self, other):
        new_pathname = other.pathname
        new_text = other.trigger_label.get_text() or ""
        new_level = other.level
        new_sensitive = other.trigger.get_sensitive()

        other._set(self.pathname, self.trigger_label.get_text() or "",
                   self.level, self.trigger.get_sensitive())
        self._set(new_pathname, new_text, new_level, new_sensitive)

    def _set(self, pathname, button_text, level, sensitive=True):
        if pathname is None:
            self.dialog.set_current_folder(os.path.expanduser("~"))
        else:
            try:
                self.dialog.set_filename(pathname)
            except:
                self.dialog.set_current_folder(os.path.expanduser("~"))

        self.dialog.button_entry.set_text(button_text)
        self.dialog.gain_adj.set_value(level)
        self._on_dialog_response(self.dialog,
                                 Gtk.ResponseType.ACCEPT
                                 if sensitive else
                                 Gtk.ResponseType.NO,
                                 pathname)

    def _on_config(self, widget):
        self.stop.clicked()
        if self.pathname and os.path.isfile(self.pathname):
            self.dialog.select_filename(self.pathname)
        self.dialog.button_entry.set_text(self.trigger_label.get_text() or "")
        self.dialog.gain_adj.set_value(self.level)
        self.dialog.show()

    def _on_trigger(self, widget):
        if self.trigger.get_sensitive():
            self._repeat_works = True
            if self.pathname:
                if not self.timeout_source_id:
                    if self.effect_length == 0.0:
                        self.effect_length = self.interlude.get_media_metadata(self.pathname, True)
                    self.effect_start = time.time()
                    self.timeout_source_id = timeout_add(playergui.PROGRESS_TIMEOUT,
                                    self._progress_timeout)
                    self.tabeffectname.set_text(self.trigger_label.get_text())
                    self.tabeffecttime.set_text('0.0')
                    self.tabeffectprog.set_fraction(0.0)
                    self.approot.jingles.nb_effects_box.add(self.tabwidget)
                    self.approot.effect_started(self.trigger_label.get_text(),
                                                self.pathname, self.num)
                else: # Restarted the effect
                    self.effect_start = time.time()
                self.approot.mixer_write(
                    "EFCT={}\nPLRP={}\n"
                    "RGDB={}\nACTN=playeffect\nend\n".format(self.num, self.pathname, self.level))

    def _on_stop(self, widget):
        self._repeat_works = False
        self.approot.mixer_write("EFCT={}\nACTN=stopeffect\nend\n".format(self.num))

    def _progress_timeout(self):
        now = time.time()
        played = now - self.effect_start
        try:
            ratio = min(played / self.effect_length, 1.0)
        except ZeroDivisionError:
            pass
        else:
            self.progress.set_fraction(ratio)
            self.tabeffectprog.set_fraction(ratio)
            self.tabeffecttime.set_text("{:4.1f}".format(self.effect_length - played))
        return True

    def _stop_progress(self):
        if self.timeout_source_id:
            source_remove(self.timeout_source_id)
            self.timeout_source_id = None
            self.progress.set_fraction(0.0)
            self.approot.jingles.nb_effects_box.remove(self.tabwidget)
            self.approot.effect_stopped(self.num)

    def _on_dialog_response(self, dialog, response_id, pathname=None):
        if response_id in (Gtk.ResponseType.ACCEPT, Gtk.ResponseType.NO):
            self.pathname = pathname or dialog.get_filename()
            text = dialog.button_entry.get_text() if self.pathname and \
                                        os.path.isfile(self.pathname) else ""
            if response_id == Gtk.ResponseType.NO:
                dialog.unselect_all()
                dialog.set_filename("")
                dialog.set_current_folder(os.path.expanduser("~"))
                dialog.button_entry.set_text("")
                dialog.gain_adj.set_value(0.0)
                dialog._stored_filename = None
                self.pathname = ""
                text = ""
            text = text.strip()
            sens = bool(text)
            for each in self.trigger, self.stop, self.repeat:
                each.set_sensitive(sens)
            self.config_image.set_visible(sens)

            self.trigger_label.set_use_markup(True)
            self.trigger_label.set_label(GLib.markup_escape_text(text))
            self.level = dialog.gain_adj.get_value()

            sens = self.pathname is not None and os.path.isfile(self.pathname)
            if response_id == Gtk.ResponseType.ACCEPT and pathname is not None:
                self.uuid = str(uuid.uuid4())
            self.effect_length = 0.0 # Force effect length to be read again.

    def marshall(self):
        link = link_uuid_reg.get_link_filename(self.uuid)
        if link is not None:
            # Replace orig file abspath with alternate path to a hard link
            # except when link is None as happens when a hard link fails.
            link = PathStr("links") / link
            self.pathname = PM.basedir / link
            if not self.dialog.get_visible():
                self.dialog.set_filename(self.pathname)
        return json.dumps([self.trigger_label.get_text(),
                          (link or self.pathname), self.level, self.uuid])

    def unmarshall(self, data):
        try:
            label, pathname, level, self.uuid = json.loads(data)
        except ValueError:
            label = ""
            pathname = None
            level = 0.0

        if pathname is not None and not pathname.startswith(os.path.sep):
            pathname = PM.basedir / pathname
        if pathname is None or not os.path.isfile(pathname):
            self.dialog.unselect_all()
            label = ""
        else:
            self.dialog.set_filename(pathname)
        self.dialog.button_entry.set_text(label)
        self.dialog.gain_adj.set_value(level)
        self._on_dialog_response(self.dialog, Gtk.ResponseType.ACCEPT, pathname)
        self.pathname = pathname

    def update_highlight(self, highlight):
        if highlight != self.highlight:
            self.highlight = highlight

            if not highlight and self._repeat_works and self.repeat.get_active():
                self.trigger.clicked()
            elif not highlight:
                self._stop_progress()

            if self.trigger_label.get_use_markup():
                if highlight:
                    self.trigger_label.set_label("<span foreground='red' font_weight='bold'>{}</span>".format(self.trigger_label.get_label()))
                else:
                    self.trigger_label.set_label(GLib.markup_escape_text(self.trigger_label.get_text()))


class EffectConfigDialog(Gtk.FileChooserDialog):
    """Configuration dialog for an Effect."""

    file_filter = Gtk.FileFilter()
    file_filter.set_name(_('Supported media'))
    for each in supported.media:
        if each not in (".cue", ".txt"):
            file_filter.add_pattern("*" + each)
            file_filter.add_pattern("*" + each.upper())

    def __init__(self, effect, window):
        Gtk.FileChooserDialog.__init__(self, title=_('Effect {} Config'.format(effect.num + 1)),
                                       transient_for=window)
        self.add_buttons(_("Clear"), Gtk.ResponseType.NO,
                         _("Cancel"), Gtk.ResponseType.REJECT,
                         _("OK"), Gtk.ResponseType.ACCEPT)
        self.set_modal(True)

        ca = self.get_content_area()
        ca.set_spacing(5)
        vbox = Gtk.VBox()
        ca.pack_start(vbox, False, True, 0)
        vbox.set_border_width(5)

        hbox = Gtk.HBox()
        hbox.set_spacing(3)
        label = Gtk.Label.new(_('Trigger text'))
        self.button_entry = DefaultEntry(_('No Name'))
        hbox.pack_start(label, False)
        hbox.pack_start(self.button_entry, False)

        spc = Gtk.HBox()
        hbox.pack_start(spc, False, padding=3)

        label = Gtk.Label.new(_('Level adjustment (dB)'))
        self.gain_adj = Gtk.Adjustment(value=0.0, lower=-10.0, upper=10.0, step_increment=0.5)
        gain = Gtk.SpinButton.new(self.gain_adj, 1.0, 1)
        hbox.pack_start(label, False)
        hbox.pack_start(gain, False)

        vbox.pack_start(hbox, False)

        ca.show_all()
        self.connect("notify::visible", self._cb_notify_visible)
        self.connect("delete-event", lambda w, e: w.hide() or True)
        self.connect("response", self._cb_response)
        self.add_filter(self.file_filter)

    def set_filename(self, filename):
        self._stored_filename = filename
        Gtk.FileChooserDialog.set_filename(self, filename)

    def _cb_notify_visible(self, *args):
        # Make sure filename is shown in the location box.

        if self.get_visible():
            filename = self.get_filename()
            if filename is None:
                try:
                    if self._stored_filename is not None:
                        self.set_filename(self._stored_filename)
                except AttributeError:
                    pass
        else:
            self._stored_filename = self.get_filename()


    def _cb_response(self, dialog, response_id):
        dialog.hide()
        if response_id == Gtk.ResponseType.NO:
            dialog.unselect_all()
            dialog.set_current_folder(os.path.expanduser("~"))
            self.button_entry.set_text("")
            self.gain_adj.set_value(0.0)


class EffectBank(Gtk.Frame):
    """A vertical stack of effects with level controls."""

    def __init__(self, qty, base, filename, parent, all_effects, vol_adj, mute_adj):
        Gtk.Frame.__init__(self)
        self.base = base
        self.session_filename = filename

        hbox = Gtk.HBox()
        hbox.set_spacing(1)
        self.add(hbox)
        vbox = Gtk.VBox()
        hbox.pack_start(vbox)

        self.effects = []
        self.all_effects = all_effects

        count = 0

        for row in range(qty):
            effect = Effect(base + row, self.all_effects, parent)
            self.effects.append(effect)
            self.all_effects.append(effect)
            vbox.pack_start(effect)
            count += 1

        level_vbox = Gtk.VBox()
        hbox.pack_start(level_vbox, False, padding=3)

        pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(PGlobs.themedir / "volume16.svg",
                                                         16, 16, True)
        vol_image = Gtk.Image.new_from_pixbuf(pixbuf)
        vol_image.set_margin_top(2)

        # vol_image = Gtk.Image.new_from_file(PGlobs.themedir / "volume2.png")
        vol = Gtk.Scale(adjustment=vol_adj, orientation=Gtk.Orientation.VERTICAL)
        vol.set_inverted(True)
        vol.set_draw_value(False)
        set_tip(vol, _('Effects volume.'))


        pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(PGlobs.themedir / "headroom16.svg",
                                                         16, 16, True)
        headroom_image = Gtk.Image.new_from_pixbuf(pixbuf)
        headroom_image.set_margin_top(2)
        #pb = GdkPixbuf.Pixbuf.new_from_file(PGlobs.themedir / "headroom.png")
        #mute_image = Gtk.Image.new_from_pixbuf(pb)
        headroom = Gtk.Scale(adjustment=mute_adj, orientation=Gtk.Orientation.VERTICAL)
        headroom.set_inverted(True)
        headroom.set_draw_value(False)
        set_tip(headroom, _('Player headroom that is applied when an effect is playing.'))

        spc = Gtk.VBox()

        for widget, expand in zip((vol_image, vol, spc, headroom_image, headroom),
                                    (False, True, False, False, True)):
            level_vbox.pack_start(widget, expand, padding=2)

    def marshall(self):
        return json.dumps([x.marshall() for x in self.effects])

    def unmarshall(self, data):
        for per_widget_data, widget in zip(json.loads(data), self.effects):
            widget.unmarshall(per_widget_data)

    def restore_session(self):
        try:
            with open(PM.basedir / self.session_filename, "r") as f:
                self.unmarshall(f.read())
        except IOError:
            print("failed to read effects session file")

    def save_session(self, where):
        try:
            with open((where or PM.basedir) / self.session_filename, "w") as f:
                f.write(self.marshall())
        except IOError:
            print("failed to write effects session file")

    def update_highlights(self, bits):
        for bit, each in enumerate(self.effects):
            each.update_highlight((1 << bit + self.base) & bits)

    def stop(self):
        for each in self.effects:
            each.stop.clicked()

    def uuids(self):
        return (x.uuid for x in self.widgets)

    def pathnames(self):
        return (x.pathname for x in self.widgets)


class EffectsPlayers(Gtk.HBox):
    def __init__(self, parent):
        self.approot = parent

        sg = Gtk.SizeGroup.new(Gtk.SizeGroupMode.VERTICAL)
        self.nb_label = Gtk.HBox(False, 10)
        vb = Gtk.VBox()
        lbl = Gtk.Label.new(_('Effects'))
        sg.add_widget(lbl)
        lbl.set_margin_top(2)
        lbl.set_margin_bottom(2)
        vb.pack_start(lbl)
        vb.show()
        self.nb_label.pack_start(vb)
        self.nb_effects_box = Gtk.HBox(False, 5)
        self.nb_effects_box.connect("add", self._on_nb_add, parent.player_nb)
        self.nb_effects_box.connect("remove", self._on_nb_remove)
        self.nb_label.pack_start(self.nb_effects_box)
        self.nb_label.show_all()
        self.nb_effects_box.hide()
        Gtk.HBox.__init__(self)
        self.set_border_width(4)
        self.set_spacing(10)
        self.viewlevels = (5,)

        self.jvol_adj = (Gtk.Adjustment(value=127.0, lower=0.0, upper=127.0, step_increment=1.0, page_increment=10.0),
                         Gtk.Adjustment(value=127.0, lower=0.0, upper=127.0, step_increment=1.0, page_increment=10.0))
        self.jmute_adj = (Gtk.Adjustment(value=100.0, lower=0.0, upper=127.0, step_increment=1.0, page_increment=10.0),
                          Gtk.Adjustment(value=100.0, lower=0.0, upper=127.0, step_increment=1.0, page_increment=10.0))
        for each in (self.jvol_adj[0], self.jvol_adj[1],
                                        self.jmute_adj[0], self.jmute_adj[1]):
            each.connect("value-changed",
                                lambda w: parent.send_new_mixer_stats())

        effects_hbox = Gtk.HBox()
        effects_hbox.set_homogeneous(True)
        effects_hbox.set_spacing(6)
        effects = PGlobs.num_effects
        base = 0
        max_rows = 12
        effect_cols = (effects + max_rows - 1) // max_rows
        self.all_effects = []
        self.effect_banks = []
        for col in range(effect_cols):
            bank = EffectBank(min(effects - base, max_rows), base,
            "effects{}_session".format(col + 1), parent, self.all_effects,
            self.jvol_adj[col], self.jmute_adj[col])
            parent.label_subst.add_widget(bank,
                            "effectbank{}".format(col), _('Effects {}'.format(col + 1)))
            self.effect_banks.append(bank)
            effects_hbox.pack_start(bank)
            base += max_rows
        self.pack_start(effects_hbox)
        sg.add_widget(self.all_effects[0].tabwidget)
        self.approot.player_nb.connect('switch-page',
                                       self._on_nb_switch_page,
                                       self.nb_effects_box)
        self.show_all()

    def _on_nb_add(self, container, child, notebook):
        page_widget = notebook.get_nth_page(notebook.get_current_page())
        if not isinstance(page_widget, EffectsPlayers):
            container.show()

    def _on_nb_remove(self, container, child):
        if not container.get_children():
            container.hide()

    def _on_nb_switch_page(self, notebook, page, page_num, box):
        page_widget = notebook.get_nth_page(page_num)
        if isinstance(page_widget, EffectsPlayers):
            box.hide()
        elif box.get_children():
            box.show()

    def restore_session(self):
        for each in self.effect_banks:
            each.restore_session()

    def save_session(self, where):
        for each in self.effect_banks:
            each.save_session(where)

    def update_effect_highlights(self, ep):
        for each in self.effect_banks:
            each.update_highlights(ep)

    def clear_indicators(self):
        """Set all LED indicators to off."""

        pass

    def cleanup(self):
        pass

    @property
    def playing(self):
        return False

    @property
    def flush(self):
        return 0

    @flush.setter
    def flush(self, value):
        pass


class EffectsPlayersWindow(Gtk.Window):
    def __init__(self, notebook, notebook_page):
        Gtk.Window.__init__(self)
        self._notebook = notebook
        self._notebook_page = notebook_page
        self.set_title(_('IDJC Effects') + PM.title_extra)
        self.connect("delete-event", lambda w, e: self.hide() or True)
        self.connect("notify::visible", self.cb_visible)

    def cb_visible(self, window, *args):
        if window.props.visible:
            if not window.get_children():
                page = self._notebook.get_current_page()
                pane = self._notebook.get_nth_page(self._notebook_page)
                self._notebook.remove_page(self._notebook_page)
                table = Gtk.Grid()
                table.set_row_homogeneous(True)
                table.set_column_homogeneous(True)
                button = Gtk.Button.new_with_label(_('Restore'))
                button.connect("clicked", lambda w: window.hide())
                table.attach(button, 0, 0, 1, 1)
                table.attach_next_to(Gtk.Box(), button, Gtk.PositionType.LEFT, 1, 1)
                table.attach_next_to(Gtk.Box(), button, Gtk.PositionType.RIGHT, 1, 1)
                table.attach_next_to(Gtk.Box(), button, Gtk.PositionType.TOP, 1, 4)
                table.attach_next_to(Gtk.Box(), button, Gtk.PositionType.BOTTOM, 1, 4)
                table.show_all()
                self._notebook.insert_page(table, pane.nb_label,
                                           self._notebook_page)
                self._notebook.set_current_page(page)
                window.add(pane)
        else:
            try:
                pane = window.get_children()[0]
            except IndexError:
                pass
            else:
                window.remove(pane)
                page = self._notebook.get_current_page()
                self._notebook.remove_page(self._notebook_page)
                self._notebook.insert_page(pane, pane.nb_label,
                                           self._notebook_page)
                self._notebook.set_current_page(page)
