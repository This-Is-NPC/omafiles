#!/usr/bin/env python3
"""Exercise the device helper against UDisks2 on two private D-Bus buses."""

import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

from gi.repository import Gio, GLib

PREFIX = 'org.freedesktop.UDisks2'
ROOT = '/org/freedesktop/UDisks2'
BLOCK = ROOT + '/block_devices/omafiles_test'
DRIVE = ROOT + '/drives/omafiles_test'
FS = PREFIX + '.Filesystem'

# GVfs combines UDisks properties with the kernel mount table. Only its private
# test process receives this synthetic table and stat; no device is created.
MOUNT_FIXTURE = r'''#define _LARGEFILE64_SOURCE
#include <gio/gunixmounts.h>
GList *g_unix_mount_entries_get(guint64 *time_read) {
    size_t count = 0;
    GUnixMountEntry **entries = g_unix_mount_entries_get_from_file(g_getenv("OMAFILES_TEST_MOUNTS"), time_read, &count);
    GList *result = NULL;
    for (size_t i = 0; i < count; ++i) result = g_list_prepend(result, entries[i]);
    g_free(entries);
    return g_list_reverse(result);
}
#include <sys/stat.h>
#include <string.h>
#include <dlfcn.h>
int stat(const char *path, struct stat *result) {
    if (strcmp(path, "/dev/omafiles_test") == 0) {
        memset(result, 0, sizeof(*result));
        result->st_mode = S_IFBLK | 0600;
        result->st_rdev = 23041;
        return 0;
    }
    int (*real_stat)(const char *, struct stat *) = dlsym(RTLD_NEXT, "stat");
    return real_stat(path, result);
}

int stat64(const char *path, struct stat64 *result) {
    _Static_assert(sizeof(struct stat) == sizeof(struct stat64), "The fixture requires matching Linux stat layouts");
    struct stat value;
    int status = stat(path, &value);
    if (status == 0) memcpy(result, &value, sizeof(value));
    return status;
}
'''


def mount_fixture(base, env):
    source, library = base / 'mounts.c', base / 'mounts.so'
    source.write_text(MOUNT_FIXTURE)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'gio-unix-2.0'], text=True, env=env).split()
    subprocess.run(['cc', '-shared', '-fPIC', str(source), '-o', str(library), *flags, '-ldl'], check=True, env=env)
    return library


def wait_until(predicate, description):
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.01)
    raise AssertionError('Did not complete: ' + description)


class FakeDisks:
    def __init__(self, address, mount_path):
        self.connection = Gio.DBusConnection.new_for_address_sync(
            address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        self.connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'RequestName', GLib.Variant('(su)', (PREFIX, 0)),
            None, Gio.DBusCallFlags.NONE, -1, None)
        self.mount_path = mount_path
        self.mounts = mount_path.parent / 'mounts'
        self.mounts.touch()
        self.calls = []
        self.error = ''
        self.pending = None
        self.delay = False
        self.objects = {}
        self.registrations = []
        self.registered = set()
        xml = ET.parse('/usr/share/dbus-1/interfaces/org.freedesktop.UDisks2.xml')
        self.infos = {}
        for element in xml.getroot().findall('interface'):
            name = element.get('name')
            if name not in (PREFIX + '.Manager', PREFIX + '.Drive', PREFIX + '.Block', FS):
                continue
            self.infos[name] = Gio.DBusNodeInfo.new_for_xml('<node>' + ET.tostring(element, encoding='unicode') + '</node>').interfaces[0]
        manager_xml = '''<node><interface name="org.freedesktop.DBus.ObjectManager">
          <method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
          <signal name="InterfacesAdded"><arg type="o"/><arg type="a{sa{sv}}"/></signal>
          <signal name="InterfacesRemoved"><arg type="o"/><arg type="as"/></signal>
        </interface></node>'''
        manager = Gio.DBusNodeInfo.new_for_xml(manager_xml).interfaces[0]
        self.registrations.append(self.connection.register_object_with_closures2(ROOT, manager, self.call, None, None))
        self.add(ROOT + '/Manager', PREFIX + '.Manager', {'Version': '2.10.1'})

    def properties(self, interface, overrides):
        result = {}
        for prop in self.infos[interface].properties:
            kind = prop.signature
            if kind in ('s',): value = ''
            elif kind == 'o': value = '/'
            elif kind == 'b': value = False
            elif kind in ('t', 'u', 'i', 'x'): value = 0
            elif kind.startswith('a{'): value = {}
            elif kind.startswith('a'): value = []
            else: raise AssertionError('Unhandled property type: ' + kind)
            result[prop.name] = GLib.Variant(kind, overrides.get(prop.name, value))
        return result

    def add(self, path, interface, values):
        self.objects.setdefault(path, {})[interface] = self.properties(interface, values)
        if (path, interface) not in self.registered:
            self.registrations.append(self.connection.register_object_with_closures2(path, self.infos[interface], self.call,
                lambda connection, sender, object_path, name, prop: self.objects[object_path][name][prop], None))
            self.registered.add((path, interface))

    def connect(self):
        self.add(DRIVE, PREFIX + '.Drive', {'Vendor': 'Omafiles', 'Model': 'Private test USB',
            'Id': 'test-drive', 'ConnectionBus': 'usb', 'Removable': True, 'MediaRemovable': False,
            'MediaAvailable': True, 'CanPowerOff': True, 'Ejectable': False, 'Size': 1048576})
        self.add(BLOCK, PREFIX + '.Block', {'Device': list(b'/dev/omafiles_test\0'),
            'PreferredDevice': list(b'/dev/omafiles_test\0'), 'Drive': DRIVE,
            'DeviceNumber': 23041, 'IdUsage': 'filesystem', 'IdType': 'ext4', 'IdUUID': 'omafiles-test-uuid',
            'IdLabel': 'Private removable', 'HintSystem': False, 'Size': 1048576})
        self.add(BLOCK, FS, {})

    def hotplug(self):
        self.connect()
        for path in (DRIVE, BLOCK):
            self.connection.emit_signal(None, ROOT, 'org.freedesktop.DBus.ObjectManager', 'InterfacesAdded',
                GLib.Variant('(oa{sa{sv}})', (path, self.objects[path])))

    def emit(self, path, interface, changed):
        self.objects[path][interface].update(changed)
        self.connection.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            GLib.Variant('(sa{sv}as)', (interface, changed, [])))

    def disconnect(self):
        for path in (BLOCK, DRIVE):
            names = list(self.objects.pop(path))
            self.connection.emit_signal(None, ROOT, 'org.freedesktop.DBus.ObjectManager', 'InterfacesRemoved',
                GLib.Variant('(oas)', (path, names)))

    def call(self, connection, sender, path, interface, method, parameters, invocation):
        if method == 'GetManagedObjects':
            invocation.return_value(GLib.Variant('(a{oa{sa{sv}}})', (self.objects,)))
            return
        args = parameters.unpack()
        if os.environ.get('OMAFILES_DEVICE_TEST_DEBUG'): print('udisks:', method, args, flush=True)
        self.calls.append((method, args))
        if self.error:
            invocation.return_dbus_error(PREFIX + '.Error.DeviceBusy', self.error)
        elif self.delay:
            self.pending = invocation
        elif method == 'Mount':
            self.mounts.write_text(f'/dev/omafiles_test {self.mount_path} ext4 rw,nosuid,nodev 0 0\n')
            self.emit(BLOCK, FS, {'MountPoints': GLib.Variant('aay', [list(os.fsencode(self.mount_path) + b'\0')])})
            invocation.return_value(GLib.Variant('(s)', (str(self.mount_path),)))
        elif method == 'Unmount':
            self.mounts.write_text('')
            self.emit(BLOCK, FS, {'MountPoints': GLib.Variant('aay', [])})
            invocation.return_value(None)
        elif method in ('PowerOff', 'Eject'):
            self.disconnect()
            invocation.return_value(None)
        else:
            invocation.return_dbus_error('org.freedesktop.DBus.Error.UnknownMethod', method)


class FakePhone:
    name = 'org.gtk.vfs.MTPVolumeMonitor'
    path = '/org/gtk/Private/RemoteVolumeMonitor'
    interface = 'org.gtk.Private.RemoteVolumeMonitor'
    volume_type = '(ssssssbbssa{ss}sa{sv})'
    mount_type = '(ssssssbsassa{sv})'
    drive_type = '(ssssbbbbbbbbuasa{ss}sa{sv})'

    def __init__(self, connection, xml, local_path):
        self.connection = connection
        self.local_path = local_path
        self.mounted = False
        self.local = True
        self.uri = 'mtp://omafiles-test/'
        self.calls = []
        info = next(info for info in Gio.DBusNodeInfo.new_for_xml(xml).interfaces if info.name == self.interface)
        self.registration = connection.register_object_with_closures2(self.path, info, self.call, None, None)
        connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'RequestName', GLib.Variant('(su)', (self.name, 0)), None, Gio.DBusCallFlags.NONE, -1, None)

    def volume(self):
        return ('phone-test', 'Z private phone', '', '', 'phone-uuid', self.uri,
            not self.mounted, False, '', 'phone-mount' if self.mounted else '', {}, '', {'always-call-mount': GLib.Variant('b', True)})

    def mount(self):
        root = self.local_path.as_uri() if self.local else self.uri
        return ('phone-mount', 'Z private phone', '', '', 'phone-uuid', root, True, 'phone-test', [], '', {})

    def emit(self, signal, kind, id, value):
        self.connection.emit_signal(None, self.path, self.interface, signal,
            GLib.Variant('(ss' + kind + ')', (self.name, id, value)))

    def call(self, connection, sender, path, interface, method, parameters, invocation):
        if method == 'IsSupported': invocation.return_value(GLib.Variant('(b)', (True,)))
        elif method == 'List':
            invocation.return_value(GLib.Variant('(a' + self.drive_type + 'a' + self.volume_type + 'a' + self.mount_type + ')',
                ([], [self.volume(), ('phone-alias', *self.volume()[1:])], [self.mount()] if self.mounted else [])))
        elif method == 'VolumeMount':
            self.calls.append(method)
            self.emit('VolumeRemoved', self.volume_type, 'phone-alias', ('phone-alias', *self.volume()[1:]))
            self.mounted = True
            self.emit('MountAdded', self.mount_type, 'phone-mount', self.mount())
            self.emit('VolumeChanged', self.volume_type, 'phone-test', self.volume())
            invocation.return_value(None)
        elif method == 'MountUnmount':
            self.calls.append(method)
            mount = self.mount()
            self.mounted = False
            self.emit('VolumeChanged', self.volume_type, 'phone-test', self.volume())
            self.emit('MountRemoved', self.mount_type, 'phone-mount', mount)
            invocation.return_value(None)
        else:
            invocation.return_dbus_error('org.freedesktop.DBus.Error.UnknownMethod', method)


class Helper:
    def __init__(self, binary, env):
        self.process = subprocess.Popen([str(binary)], env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.messages = queue.Queue()
        self.rows = []
        self.finished = []
        def reader():
            for line in self.process.stdout:
                self.messages.put(json.loads(line))
                if os.environ.get('OMAFILES_DEVICE_TEST_DEBUG'): print('helper:', line.strip(), flush=True)
        threading.Thread(target=reader, daemon=True).start()

    def collect(self):
        while not self.messages.empty():
            message = self.messages.get_nowait()
            if 'devices' in message: self.rows = message['devices']
            if message.get('finished'): self.finished.append(message)

    def wait(self, predicate, description):
        def ready():
            self.collect()
            if self.process.poll() is not None:
                raise AssertionError('Helper stopped: ' + self.process.stderr.read())
            return predicate()
        return wait_until(ready, description)

    def act(self, action, id='/dev/omafiles_test'):
        self.process.stdin.write(json.dumps({'id': id, 'action': action}) + '\n')
        self.process.stdin.flush()

    def result(self, action):
        def ready():
            return next((message for message in self.finished if message['action'] == action), None)
        result = self.wait(ready, action + ' result')
        self.finished.remove(result)
        return result


def main():
    binary = Path(sys.argv[1]).resolve()
    processes = []
    loop = None
    thread = None
    with tempfile.TemporaryDirectory(prefix='omafiles-device-bus-', dir=os.environ.get('TMPDIR', '/tmp')) as temporary:
        base = Path(temporary)
        env = os.environ.copy()
        for key in ('HOME', 'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME', 'XDG_RUNTIME_DIR', 'TMPDIR'):
            path = base / key
            path.mkdir(mode=0o700)
            env[key] = str(path)
        env.update(GVFS_DISABLE_FUSE='1', QT_QPA_PLATFORM='offscreen',
            QT_QPA_PLATFORMTHEME='', GSETTINGS_SCHEMA_DIR='/usr/share/glib-2.0/schemas',
            GSETTINGS_BACKEND='memory', PULSE_SERVER='unix:' + str(base / 'no-audio'),
            PIPEWIRE_REMOTE='disconnected')
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'LD_PRELOAD', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_SYSTEM_BUS_ADDRESS'):
            env.pop(key, None)
        for key in ('XDG_DATA_DIRS', 'XDG_CONFIG_DIRS'):
            directory = base / key
            directory.mkdir()
            env[key] = str(directory)
        monitors = Path(env['XDG_DATA_DIRS']) / 'gvfs/remote-volume-monitors'
        monitors.mkdir(parents=True)
        services = Path(env['XDG_DATA_DIRS']) / 'dbus-1/services'
        services.mkdir(parents=True)
        shutil.copyfile('/usr/share/dbus-1/services/org.gtk.vfs.Daemon.service', services / 'org.gtk.vfs.Daemon.service')
        for name in ('udisks2', 'mtp'):
            shutil.copyfile('/usr/share/gvfs/remote-volume-monitors/' + name + '.monitor', monitors / (name + '.monitor'))
        env.pop('GIO_USE_VOLUME_MONITOR', None)
        env.pop('GIO_USE_VFS', None)
        env.pop('GVFS_REMOTE_VOLUME_MONITOR_IGNORE', None)
        try:
            for variable in ('DBUS_SYSTEM_BUS_ADDRESS', 'DBUS_SESSION_BUS_ADDRESS'):
                bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                processes.append(bus)
                env[variable] = bus.stdout.readline().strip()
                assert env[variable].startswith('unix:')
            disk = FakeDisks(env['DBUS_SYSTEM_BUS_ADDRESS'], Path(env['HOME']) / 'mounted')
            disk.mount_path.mkdir()
            loop = GLib.MainLoop()
            thread = threading.Thread(target=loop.run, daemon=True)
            thread.start()
            monitor_env = {**env, 'LD_PRELOAD': str(mount_fixture(base, env)), 'OMAFILES_TEST_MOUNTS': str(disk.mounts)}
            monitor = subprocess.Popen(['/usr/lib/gvfs-udisks2-volume-monitor'], env=monitor_env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(monitor)
            session = Gio.DBusConnection.new_for_address_sync(env['DBUS_SESSION_BUS_ADDRESS'],
                Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
            wait_until(lambda: session.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
                'org.freedesktop.DBus', 'NameHasOwner', GLib.Variant('(s)', ('org.gtk.vfs.UDisks2VolumeMonitor',)),
                None, Gio.DBusCallFlags.NONE, -1, None).unpack()[0], 'GVfs monitor registration')
            info = session.call_sync('org.gtk.vfs.UDisks2VolumeMonitor', '/org/gtk/Private/RemoteVolumeMonitor',
                'org.freedesktop.DBus.Introspectable', 'Introspect', None, None, Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
            phone = FakePhone(session, info, base / 'phone')
            phone.local_path.mkdir()
            helper = Helper(binary, env)
            processes.append(helper.process)
            helper.wait(lambda: len(helper.rows) == 1 and helper.rows[0]['phone'], 'discover private phone')
            disk.hotplug()
            helper.wait(lambda: len(helper.rows) == 2, 'discover hot-plugged private UDisks volume')
            assert {row['name'] for row in helper.rows} == {'Private removable', 'Z private phone'}, helper.rows
            assert not any(row['mounted'] for row in helper.rows)
            assert next(row for row in helper.rows if row['phone'])['id'] == 'mtp://omafiles-test/'
            assert disk.calls == [], disk.calls
            assert [row['name'] for row in helper.rows] == ['Private removable', 'Z private phone']
            disk.emit(DRIVE, PREFIX + '.Drive', {'Removable': GLib.Variant('b', False)})
            helper.wait(lambda: len(helper.rows) == 1, 'exclude internal drive')
            disk.emit(DRIVE, PREFIX + '.Drive', {'Removable': GLib.Variant('b', True)})
            helper.wait(lambda: len(helper.rows) == 2, 'USB drive with nonremovable media')
            helper.act('open')
            result = helper.result('open')
            assert result['ok'] and result['path'] == str(disk.mount_path), result
            helper.wait(lambda: next(row for row in helper.rows if not row['phone'])['mounted'], 'mounted state')
            helper.act('open')
            assert helper.result('open')['ok']
            assert sum(method == 'Mount' for method, _ in disk.calls) == 1, disk.calls
            disk.error = 'The private device is busy with an open file.'
            helper.act('unmount')
            result = helper.result('unmount')
            assert not result['ok'] and 'busy' in result['reason'].lower(), result
            assert next(row for row in helper.rows if not row['phone'])['mounted']
            disk.error = ''
            helper.act('unmount')
            assert helper.result('unmount')['ok']
            helper.wait(lambda: not next(row for row in helper.rows if not row['phone'])['mounted'], 'unmounted state')
            disk.error = 'The private device is busy with an open file.'
            helper.act('mount')
            result = helper.result('mount')
            assert not result['ok'] and disk.error in result['reason'], result
            disk.error = ''
            disk.delay = True
            helper.act('mount')
            wait_until(lambda: disk.pending, 'delayed mount request')
            helper.act('cancel')
            result = helper.result('mount')
            assert not result['ok'] and 'cancel' in result['reason'].lower(), result
            disk.pending.return_dbus_error(PREFIX + '.Error.Cancelled', 'Canceled test operation')
            disk.pending = None
            disk.delay = False
            helper.act('eject')
            assert helper.result('eject')['ok']
            helper.wait(lambda: len(helper.rows) == 1 and helper.rows[0]['phone'], 'hot unplug')
            helper.act('open')
            result = helper.result('open')
            assert not result['ok'] and 'no longer connected' in result['reason'], result
            disk.hotplug()
            helper.wait(lambda: len(helper.rows) == 2, 'hot plug again')
            assert any(method == 'PowerOff' for method, _ in disk.calls), disk.calls
            disk.emit(DRIVE, PREFIX + '.Drive', {'CanPowerOff': GLib.Variant('b', False),
                'MediaRemovable': GLib.Variant('b', True), 'Ejectable': GLib.Variant('b', False)})
            helper.wait(lambda: not next(row for row in helper.rows if not row['phone'])['canEject'], 'eject capability revoked')
            disk.emit(DRIVE, PREFIX + '.Drive', {'Ejectable': GLib.Variant('b', True)})
            helper.wait(lambda: next(row for row in helper.rows if not row['phone'])['canEject'], 'eject capability available')
            helper.act('eject')
            assert helper.result('eject')['ok']
            assert any(method == 'Eject' for method, _ in disk.calls), disk.calls
            helper.wait(lambda: len(helper.rows) == 1, 'removable media ejected')
            helper.act('open', 'mtp://omafiles-test/')
            result = helper.result('open')
            assert result['ok'] and result['path'] == str(phone.local_path), result
            helper.act('unmount', 'mtp://omafiles-test/')
            assert helper.result('unmount')['ok']
            for scheme in ('gphoto2', 'afc', 'mtp'):
                phone.uri = scheme + '://omafiles-test/'
                phone.emit('VolumeChanged', phone.volume_type, 'phone-test', phone.volume())
                helper.wait(lambda: any(row['phone'] and row['id'] == phone.uri for row in helper.rows), scheme + ' phone recognition')
            phone.local = False
            helper.act('open', 'mtp://omafiles-test/')
            result = helper.result('open')
            assert not result['ok'] and 'GVfs FUSE' in result['reason'], result
            assert phone.calls == ['VolumeMount', 'MountUnmount', 'VolumeMount'], phone.calls
            for method, args in disk.calls:
                assert not args or not args[0].get('force', False), (method, args)
            for invalid in ('[]\n', json.dumps({'id': 'test', 'action': 'format'}) + '\n', 'x' * (1024 * 1024 + 1)):
                refusal = subprocess.run([str(binary)], env=env, input=invalid, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
                assert refusal.returncode == 2, (refusal.returncode, refusal.stderr)
            print('ok: private UDisks discovery, mount-on-open, unmount, busy refusal, cancellation, eject, hot unplug; phone mount, unmount, missing FUSE refusal; bounded IPC')
        finally:
            if loop: loop.quit()
            if thread: thread.join(timeout=5)
            for process in reversed(processes):
                if process.poll() is None: process.terminate()
            for process in reversed(processes):
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if os.environ.get('OMAFILES_DEVICE_TEST_DEBUG'):
                for process in processes:
                    print('child exit:', process.returncode, 'stderr:', process.stderr.read(), flush=True)


if __name__ == '__main__':
    main()
