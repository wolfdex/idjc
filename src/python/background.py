#   background.py: background audio player -- part of IDJC.
#   Copyright 2022 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

import gettext

import gi
from gi.repository import Gtk
from gi.repository import Gdk
from gi.repository import GdkPixbuf

from idjc import *
from .playergui import *
from .tooltips import set_tip
#from .prelims import *

_ = gettext.translation(FGlobs.package_name, FGlobs.localedir,
                                                        fallback=True).gettext

class Background(Gtk.HBox):
    def __init__(self, parent):
        Gtk.HBox.__init__(self)
        self.approot = parent
        self.nb_label = Gtk.Label.new(_("Background"))
        self.nb_label.show()

        self.ivol_adj = Gtk.Adjustment(value=64.0, lower=0.0, upper=127.0, step_increment=1.0, page_increment=10.0)
        self.ivol_adj.connect("value-changed", lambda w: parent.send_new_mixer_stats())
        parent.label_subst.add_widget(self.nb_label, "bgplayername", _('Background'))
        vbox = Gtk.VBox()
        self.pack_start(vbox, True, True)
        self.player = IDJC_Media_Player(vbox, "interlude", parent)

        ilevel_vbox = Gtk.VBox()
        self.pack_start(ilevel_vbox, False, padding=3)

        pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(PGlobs.themedir / "volume16.svg",
                                                         16, 16, True)
        ivol_image = Gtk.Image.new_from_pixbuf(pixbuf)
        ilevel_vbox.pack_start(ivol_image, False, padding=2)
        ivol = Gtk.Scale(adjustment=self.ivol_adj, orientation=Gtk.Orientation.VERTICAL)
        ivol.set_inverted(True)
        ivol.set_draw_value(False)
        ilevel_vbox.pack_start(ivol, padding=2)
        set_tip(ivol, _('Background Tracks volume.'))

        self.viewlevels = (5,)
        ilevel_vbox.show_all()
        vbox.show()
        self.show()

    @property
    def flush(self):
        return 0

    @flush.setter
    def flush(self, value):
        pass
