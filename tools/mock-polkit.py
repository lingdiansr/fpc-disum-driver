#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Test-only PolicyKit authority that authorizes every request.

This is NOT a PolicyKit replacement. It is used only by
tools/fprintd-nofinger-smoke.sh on a private D-Bus bus, so fprintd can be
tested without a system polkit daemon. Never run it on a real system bus.
"""
import os

from gi.repository import Gio, GLib

XML = """<node>
<interface name="org.freedesktop.PolicyKit1.Authority">
  <method name="CheckAuthorization">
    <arg type="(sa{sv})" direction="in"/>
    <arg type="s" direction="in"/>
    <arg type="a{ss}" direction="in"/>
    <arg type="u" direction="in"/>
    <arg type="s" direction="in"/>
    <arg type="(bba{ss})" direction="out"/>
  </method>
  <method name="CancelCheckAuthorization">
    <arg type="s" direction="in"/>
  </method>
</interface>
</node>"""

address = os.environ.get("DBUS_SYSTEM_BUS_ADDRESS")
if not address:
    raise SystemExit("DBUS_SYSTEM_BUS_ADDRESS is not set")

connection = Gio.DBusConnection.new_for_address_sync(
    address,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
    | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None,
    None,
)
node = Gio.DBusNodeInfo.new_for_xml(XML)


def on_call(connection, sender, path, interface, method, params, invocation):
    if method == "CheckAuthorization":
        invocation.return_value(GLib.Variant("((bba{ss}))", ((True, False, {}),)))
    else:
        invocation.return_value(None)


connection.register_object(
    "/org/freedesktop/PolicyKit1/Authority", node.interfaces[0], on_call, None, None
)
Gio.bus_own_name_on_connection(
    connection,
    "org.freedesktop.PolicyKit1",
    Gio.BusNameOwnerFlags.NONE,
    None,
    None,
)
GLib.MainLoop().run()
