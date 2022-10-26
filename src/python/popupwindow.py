#   popupwindow.py: for when standard gtk tooltips just don't cut it
#   Copyright (C) 2007-2020 Stephen Fairchild (s-fairchild@users.sourceforge.net)
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

__all__ = ['PopupWindow']

import gi
from gi.repository import GObject, Gtk
from .gtkstuff import timeout_add, source_remove


class PopupWindowCancelled(Exception):
    pass


class PopupWindow:
    def __init__(self, widget, popuptime, popdowntime, timeout, \
                 winpopulate_callback, inhibit_callback=lambda: False):
        self.widget = widget
        self.popuptime = popuptime
        self.popdowntime = popdowntime
        self.timeout = timeout
        self.winpopulate_callback = winpopulate_callback
        self.inhibit_callback = inhibit_callback
        self.popup_window = None
        self.inside_widget = False
        self.widget.connect("motion_notify_event", self.handle_mouse, "move")
        self.widget.connect("enter_notify_event", self.handle_mouse, "enter")
        self.widget.connect("leave_notify_event", self.handle_mouse, "leave")
        self.widget.connect("button_press_event", self.handle_mouse, "button")
        self.widget.connect("button_release_event", self.handle_mouse, "button")
        self.widget.connect("scroll_event", self.handle_mouse, "scroll")

    def timeout_callback(self):
        try:
            self.timer_count += 1
            self.total_timer_count += 1
            if self.timer_count == self.popuptime:
                if not self.timeout or self.total_timer_count < \
                                                self.popuptime + self.timeout:
                    self.popup_window = Gtk.Window(type=Gtk.WindowType.POPUP)
                    self.popup_window.set_decorated(False)
                    if self.winpopulate_callback(self.popup_window, \
                                            self.widget, self.x, self.y) != -1:
                        self.popup_window.realize()
                        # Calculate the popup window positioning.
                        w_popup = self.popup_window.get_size()[0]
                        # Get root window width.
                        w_root = self.popup_window.get_screen(
                                        ).get_root_window().get_geometry()[2]
                        offset = w_root - int(self.x_root) - w_popup - 4
                        if offset > 0:  # Right justify if needed.
                            offset = 0
                        x_pos = int(self.x_root) + 4 + offset
                        # No right justification for popups that won't fit.
                        if x_pos < 0:
                            # Display against left window edge.
                            x_pos = 0
                        self.popup_window.move(x_pos, int(self.y_root) + 4)
                        self.popup_window.show()
                    else:
                        raise PopupWindowCancelled("window populate callback returned"
                                                    " -1 -- window cancelled")
                else:
                    raise PopupWindowCancelled("timeout exceeded")
            if self.timer_count > self.popdowntime:
                raise PopupWindowCancelled("popdown time reached")
        except PopupWindowCancelled:
            if self.popup_window is not None:
                self.popup_window.destroy()
                self.popup_window = None
            return False
        else:
            return True

    def handle_mouse(self, widget, event, data):
        self.timer_count = 0
        # Store absolute mouse x and y coordiates.
        self.x_root = event.x_root
        self.y_root = event.y_root
        # This information could be useful too.
        self.x = event.x
        self.y = event.y
        # Any event triggers destruction of popup windows currently open.
        if self.popup_window is not None:
            self.popup_window.destroy()
            source_remove(self.timeout_id)
            self.popup_window = None
            if data == "leave": return False
        if data == "enter" and self.inside_widget == False:
            self.timeout_id = timeout_add(100, self.timeout_callback)
            self.inside_widget = True
            self.total_timer_count = 0
        if data == "leave":
            if self.inside_widget:
                source_remove(self.timeout_id)
                self.inside_widget = False
        if data == "button" or data == "scroll" or self.inhibit_callback() \
                                                        and self.inside_widget:
            source_remove(self.timeout_id)

