"""Automatic local modpack discovery, URL retention and the actual Settings panel; no running server."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from teaserv_admin import caelum_files as cf


def pack(path, name='Beach Lobby', body=b'old', sealed=False):
    with zipfile.ZipFile(path,'w',zipfile.ZIP_DEFLATED) as z:
        z.writestr('mod.txt','name='+name+'\nauthor=Creator\nversion=1\nregion=any\n')
        z.writestr('caelum_server_pack.txt' if sealed else 'scene.rel',body)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.folder=Path(self.tmp.name)

    def test_names_hashes_uppercase_zip_and_plain_or_sealed_metadata(self):
        pack(self.folder/'a.zip');pack(self.folder/'b.ZIP','Server pack',sealed=True)
        rows,stamp=cf.scan_local_modpacks(self.folder)
        self.assertEqual([r[2] for r in rows],['Beach Lobby','Server pack'])
        for filename,digest,name,url in rows:
            self.assertEqual(digest,hashlib.sha256((self.folder/filename).read_bytes()).hexdigest())
            self.assertEqual(url,'')
        self.assertEqual(set(stamp),{'a.zip','b.ZIP'})

    def test_changed_filename_contents_preserve_url_and_saved_links(self):
        pack(self.folder/'beach.zip')
        rows,_=cf.scan_local_modpacks(self.folder)
        previous=[rows[0][:3]+('https://example.com/beach.zip',)]
        previous=cf.parse_modpack_sources(cf.format_modpack_sources(previous))
        pack(self.folder/'beach.zip','New beach name',b'rebuilt archive')
        rows,_=cf.scan_local_modpacks(self.folder,previous)
        self.assertNotEqual(rows[0][1],previous[0][1]);self.assertEqual(rows[0][2],'New beach name')
        self.assertEqual(rows[0][3],previous[0][3])

    def test_manifest_migration_rename_order_and_url_edits(self):
        pack(self.folder/'z.zip','First');pack(self.folder/'a.zip','Second')
        rows,_=cf.scan_local_modpacks(self.folder)
        manifest=[rows[1][1:3]+('https://example.com/first',),rows[0][1:3]+('https://example.com/second',)]
        rows,_=cf.scan_local_modpacks(self.folder,manifest=manifest)
        self.assertEqual([p[0] for p in rows],['z.zip','a.zip'])
        rows[0]=rows[0][:3]+('https://example.com/edited',)
        (self.folder/'z.zip').rename(self.folder/'renamed.zip')
        again,_=cf.scan_local_modpacks(self.folder,rows,manifest)
        self.assertEqual(next(p[3] for p in again if p[0]=='renamed.zip'),'https://example.com/edited')
        (self.folder/'a.zip').unlink();pack(self.folder/'new.zip','New')
        again,_=cf.scan_local_modpacks(self.folder,again,manifest)
        self.assertEqual({p[0] for p in again},{'renamed.zip','new.zip'})

    def test_invalid_and_duplicate_zips_do_not_silently_drop_entries(self):
        bad=self.folder/'broken.zip';bad.write_bytes(b'bad ZIP')
        with self.assertRaisesRegex(ValueError,'broken.zip'):cf.scan_local_modpacks(self.folder)
        bad.unlink();pack(self.folder/'a.zip');(self.folder/'b.zip').write_bytes((self.folder/'a.zip').read_bytes())
        with self.assertRaisesRegex(ValueError,'identical modpacks'):cf.scan_local_modpacks(self.folder)
        for p in self.folder.iterdir():p.unlink()
        with zipfile.ZipFile(bad,'w') as z:z.writestr('other.txt','not a mod')
        with self.assertRaisesRegex(ValueError,'mod.txt'):cf.scan_local_modpacks(self.folder)

    def test_mutation_during_hash_is_refused(self):
        pack(self.folder/'a.zip')
        original=cf.sha256_of_file
        def change(path):
            result=original(path);pack(path,body=b'changed during read');return result
        with patch.object(cf,'sha256_of_file',side_effect=change),self.assertRaisesRegex(ValueError,'changed while Refresh'):
            cf.scan_local_modpacks(self.folder)


class PanelTests(unittest.TestCase):
    def setUp(self):
        import gc
        gc.collect()  # dispose prior Tk windows on the main thread before starting another worker
        import tkinter as tk
        from teaserv_admin import ui
        from teaserv_admin.settings_window import ModpacksPanel
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.system=Path(self.tmp.name)/'system';self.system.mkdir()
        self.root=tk.Tk();self.root.withdraw();self.addCleanup(self.root.destroy)
        ui.setup_style(self.root)
        self.window=tk.Toplevel(self.root);self.window.withdraw()
        self.window.app=SimpleNamespace(settings=SimpleNamespace(system_dir=str(self.system)),open_path=lambda _:None)
        self.panel=ModpacksPanel(self.window,self.window)
        self.panel.scroll.pack(fill='both',expand=True)

    def wait(self):
        end=time.monotonic()+5
        while self.panel.refreshing and time.monotonic()<end:
            self.root.update();time.sleep(.01)
        self.assertFalse(self.panel.refreshing)
        self.assertEqual(self.panel.scan_error,'')

    def save_files(self):
        from teaserv_admin.settings_window import Changes
        changes=Changes();self.panel.collect(changes)
        for path,text in changes.files:Path(path).write_text(text,encoding='utf-8',newline='')   # as backup_and_write
        return changes

    def test_initial_missing_folder_readonly_fields_refresh_and_reopen(self):
        from tkinter import ttk
        self.panel.load();self.wait()
        self.assertTrue((self.system/'caelum-modpacks').is_dir())
        path=self.system/'caelum-modpacks/beach.zip';pack(path)
        self.panel.refresh();self.wait()
        self.assertEqual(self.panel.packs()[0][1],'Beach Lobby')
        box=self.panel.rows_frame.winfo_children()[0]
        entries=[w for w in box.winfo_children() if isinstance(w,ttk.Entry)]
        self.assertEqual([str(e.cget('state')) for e in entries],['readonly','normal','readonly'])
        self.panel.rows[0][2].set('https://example.com/beach.zip')
        old=self.panel.packs()[0][0]
        pack(path,body=b'new contents');self.panel.refresh();self.wait()
        self.assertNotEqual(self.panel.packs()[0][0],old)
        self.assertEqual(self.panel.packs()[0][2],'https://example.com/beach.zip')
        self.save_files()
        pack(path,'Renamed pack',b'changed while Admin was closed')
        self.panel.load();self.wait()
        self.assertEqual(self.panel.packs()[0][1:],('Renamed pack','https://example.com/beach.zip'))
        self.save_files()
        self.panel.load();self.wait()
        self.assertFalse(self.save_files().files)

    def test_refresh_is_background_and_preserves_edits_during_hash(self):
        from teaserv_admin.settings_window import Changes,PanelError
        self.panel.load();self.wait()
        pack(self.system/'caelum-modpacks/a.zip');self.panel.refresh();self.wait()
        entered=threading.Event();release=threading.Event();original=cf.scan_local_modpacks
        def slow(*args):entered.set();release.wait(3);return original(*args)
        with patch.object(cf,'scan_local_modpacks',side_effect=slow):
            self.panel.refresh();self.assertTrue(entered.wait(1))
            self.root.update()
            with self.assertRaises(PanelError):self.panel.collect(Changes())
            self.panel.rows[0][2].set('https://example.com/typed-during-refresh')
            release.set();self.wait()
        self.assertEqual(self.panel.packs()[0][2],'https://example.com/typed-during-refresh')

    def test_missing_installation_does_not_create_a_server_or_block_selecting_one(self):
        from teaserv_admin.settings_window import Changes
        self.system.rmdir();self.panel.load()
        end=time.monotonic()+5
        while self.panel.refreshing and time.monotonic()<end:self.root.update();time.sleep(.01)
        self.assertIn("server folder",self.panel.scan_error)
        self.assertFalse(self.system.exists())
        changes=Changes();self.panel.collect(changes);self.assertFalse(changes.files)

    def test_invalid_zip_and_changes_since_refresh_block_save(self):
        from teaserv_admin.settings_window import Changes,PanelError
        self.panel.load();self.wait()
        path=self.system/'caelum-modpacks/a.zip';pack(path)
        with self.assertRaises(PanelError):self.panel.collect(Changes())
        self.panel.refresh();self.wait()
        with self.assertRaises(PanelError):self.panel.collect(Changes()) # URL is required
        self.panel.rows[0][2].set('https://example.com/a.zip')
        path.write_bytes(b'broken archive');self.panel.refresh()
        end=time.monotonic()+5
        while self.panel.refreshing and time.monotonic()<end:self.root.update();time.sleep(.01)
        self.assertIn('a.zip',self.panel.scan_error)
        with self.assertRaises(PanelError):self.panel.collect(Changes())
        self.assertEqual(self.panel.packs()[0][2],'https://example.com/a.zip')

    def test_copy_public_key(self):
        from teaserv_admin import caelum_identity as ci
        self.panel.load();self.wait()
        self.assertEqual(self.panel.key_button.cget('text'),'Copy Public Key')
        self.panel.copy_public_key()
        self.assertIn('first time it starts',self.panel.error.cget('text'))
        (self.system/ci.IDENTITY_FILE).write_text('0001020304'+'05060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\n')
        self.panel.copy_public_key()
        expected='f65333fa6303b6a23defd7de2af8aa461cb047ccbf12d4edd29ef3b1eba6706b'   # Monocypher crypto_eddsa_key_pair
        self.assertEqual(self.window.clipboard_get(),expected)
        self.assertIn(expected,self.panel.progress.cget('text'))
        self.assertEqual(self.panel.error.cget('text'),'')
        (self.system/ci.IDENTITY_FILE).write_text('not hex\n')
        self.panel.copy_public_key()
        self.assertIn('not a valid identity file',self.panel.error.cget('text'))

    def test_server_pack_key_is_checked_and_saved(self):
        from teaserv_admin import caelum_serverpack as csp
        from teaserv_admin.settings_window import Changes,PanelError
        seed=bytes(range(32)).hex();own='f65333fa6303b6a23defd7de2af8aa461cb047ccbf12d4edd29ef3b1eba6706b'
        (self.system/'caelum-server-identity.key').write_text(seed+'\n')
        (self.system/'caelum-server-keys.txt').write_text('# mine\n'+'11'*16+' '+'22'*32+' Other pack\n',newline='')
        pack_id,key=bytes(range(16)),bytes(range(100,132))
        (self.system/'caelum-modpacks').mkdir()
        sealed_pack(self.system/'caelum-modpacks/beach_server.zip',pack_id,key,[own])
        self.panel.load();self.wait()
        self.panel.rows[0][2].set('https://example.com/beach_server.zip')
        row=self.panel.server_rows['beach_server.zip']
        self.assertEqual(str(row['entry'].cget('show')),'•')
        self.assertIn('Enter its key',row['status'].cget('text'))
        with self.assertRaises(PanelError):self.panel.collect(Changes())
        row['key_var'].set('33'*32)
        self.assertIn('does not open this pack',row['status'].cget('text'))
        row['key_var'].set('aa'*16+' '+key.hex()+' Beach Lobby')
        self.assertIn('another pack',row['status'].cget('text'))
        row['key_var'].set(pack_id.hex()+' '+key.hex()+' Beach Lobby')
        self.assertEqual(row['status'].cget('text'),'The key opens this pack, and the pack was made for this server.')
        changes=self.save_files()
        self.assertIn('Caelum server modpack keys',changes.names['login'])
        text=(self.system/'caelum-server-keys.txt').read_bytes().decode('utf-8')
        self.assertEqual(text,'# mine\n'+'11'*16+' '+'22'*32+' Other pack\n'+pack_id.hex()+' '+key.hex()+' Beach Lobby\n')
        self.assertEqual(csp.parse_keys_file(text)[pack_id],key)
        # reopened: the key comes from the file, and an unchanged key writes nothing
        self.panel.load();self.wait()
        self.assertEqual(self.panel.server_rows['beach_server.zip']['key_var'].get(),key.hex())
        self.assertFalse(self.save_files().files)
        # a pack made for another server: saved, with a warning
        sealed_pack(self.system/'caelum-modpacks/beach_server.zip',pack_id,key,['ab'*32])
        self.panel.refresh();self.wait()
        status=self.panel.server_rows['beach_server.zip']['status'].cget('text')
        self.assertIn('not made for this server',status);self.assertIn('The key opens this pack',status)

    def test_plain_pack_has_no_key_row(self):
        self.panel.load();self.wait()
        pack(self.system/'caelum-modpacks/a.zip');self.panel.refresh();self.wait()
        self.assertEqual(self.panel.server_rows,{})


def sealed_pack(path,pack_id,key,servers,files=(('scene.rel',b'data'),)):
    """A server modpack as the Modkit writes it, sealed with this module's own XChaCha20-Poly1305 (checked against
    Monocypher by temp/teaserv_pubkey_1117/aead_compare.py in the Caelum project)."""
    import struct
    from teaserv_admin import caelum_serverpack as csp
    def seal(label,plain):
        nonce=os.urandom(24);subkey=csp._hchacha20(key,nonce[:16]);n12=b'\0\0\0\0'+nonce[16:]
        stream=b''.join(csp._chacha20_block(subkey,c,n12) for c in range(1,2+len(plain)//64))
        ct=bytes(a^b for a,b in zip(plain,stream))
        ad=b'CSPK1'+pack_id+label.encode()
        mac=csp._poly1305(csp._chacha20_block(subkey,0,n12)[:32],ad+csp._pad16(ad)+ct+csp._pad16(ct)+struct.pack('<QQ',len(ad),len(ct)))
        return nonce+mac+ct
    with zipfile.ZipFile(path,'w') as z:
        z.writestr(csp.HEADER_FILE,'CSPK1\nid=%s\nservers=%s\n'%(pack_id.hex(),','.join(servers)))
        z.writestr('mod.txt','name=Beach Lobby\nauthor=Creator\nversion=1\n')
        index=''
        for n,(name,data) in enumerate(files):
            z.writestr('data/%06d.bin'%n,seal(str(n),data));index+='%d\t0\t%d\t%s\n'%(n,len(data),name)
        z.writestr(csp.INDEX_FILE,seal('index',index.encode()))


class ServerPackKeyTests(unittest.TestCase):
    def test_key_entries_and_keys_file(self):
        from teaserv_admin import caelum_serverpack as csp
        pid=bytes(range(16));key=bytes(range(32))
        self.assertEqual(csp.parse_key_entry('  '+key.hex().upper()+'\n',pid),key)
        self.assertEqual(csp.parse_key_entry(pid.hex()+' '+key.hex()+' Beach Lobby',pid),key)
        for bad in ('',key.hex()[:-1],'zz'*32,'ff'*16+' '+key.hex()):
            with self.assertRaises(ValueError,msg=bad):csp.parse_key_entry(bad,pid)
        self.assertTrue(csp.update_keys_file('',[(pid,key,'P')]).endswith(pid.hex()+' '+key.hex()+' P\n'))
        old='# c\r\n'+pid.hex()+' '+'00'*32+' Old\r\n'+'11'*16+' '+'22'*32+'\r\n'+pid.hex()+' '+'01'*32+'\r\n'
        self.assertEqual(csp.update_keys_file(old,[(pid,key,'New')],'\r\n'),
                         '# c\r\n'+pid.hex()+' '+key.hex()+' New\r\n'+'11'*16+' '+'22'*32+'\r\n')
        self.assertEqual(csp.parse_keys_file(old)[pid],bytes([1])*32)   # the server keeps the last line
        with tempfile.TemporaryDirectory() as folder:
            pack(Path(folder)/'plain.zip');self.assertIsNone(csp.read_header(Path(folder)/'plain.zip'))
            sealed_pack(Path(folder)/'s.zip',pid,key,['ab'*32])
            self.assertEqual(csp.read_header(Path(folder)/'s.zip'),(pid,['ab'*32]))
            self.assertTrue(csp.key_opens(Path(folder)/'s.zip',pid,key))
            self.assertFalse(csp.key_opens(Path(folder)/'s.zip',pid,bytes(32)))


class IdentityKeyTests(unittest.TestCase):
    """caelum_identity repeats Monocypher 4.0.2 crypto_eddsa_key_pair (EdDSA with BLAKE2b), checked against keys the
    server's own Monocypher produced (ref.c in the Caelum project's temp/teaserv_pubkey_1117, 23 seeds)."""

    def test_reference_keys(self):
        from teaserv_admin import caelum_identity as ci
        for seed,public in (('00'*32,'19d3d919475deed4696b5d13018151d1af88b2bd3bcff048b45031c1f36d1858'),
                            (bytes(range(32)).hex(),'f65333fa6303b6a23defd7de2af8aa461cb047ccbf12d4edd29ef3b1eba6706b'),
                            ('ff'*32,'2bc0c96667f2bba066f4b50d860bdd51c4a413b20e3d9ef28c3515a8dc4d1dfc')):
            self.assertEqual(ci.public_key_from_seed(bytes.fromhex(seed)).hex(),public)
        with self.assertRaises(ValueError):ci.public_key_from_seed(b'short')


class DownloadServerAddressTests(unittest.TestCase):
    """Addresses of the built-in download server (src/TeaservModpackHttp.cc serves /modpacks/<file name>)."""

    def test_addresses(self):
        self.assertEqual(cf.download_url('203.0.113.5',80,'Beach Lobby.zip'),'http://203.0.113.5/modpacks/Beach%20Lobby.zip')
        self.assertEqual(cf.download_url('pso.example.com',8080,'a+b.ZIP'),'http://pso.example.com:8080/modpacks/a%2Bb.ZIP')
        self.assertEqual(cf.download_url('example.com',None,'été.zip'),'http://example.com/modpacks/%C3%A9t%C3%A9.zip')
        for host in ('','http://example.com','example.com:80','example.com/x','999.1.1.1','-bad.example','a b'):
            with self.assertRaises(ValueError,msg=host):cf.download_url(host,80,'a.zip')
        for name in ('.hidden.zip','a.txt','a:b.zip','a\\b.zip','x'*252+'.zip'):
            with self.assertRaises(ValueError,msg=name):cf.download_url('example.com',80,name)
        for port in (0,65536):
            with self.assertRaises(ValueError):cf.download_url('example.com',port,'a.zip')
        self.assertTrue(cf.is_ipv4('192.168.1.20'));self.assertFalse(cf.is_ipv4('en0'))

    def test_unreachable_address_warnings(self):
        mine=['127.0.0.1','169.254.1.178','192.168.1.67']
        warning=cf.download_host_warning('10.0.1.5',mine)
        self.assertIn('not one of this PC',warning);self.assertIn('older config.example.json',warning)
        self.assertIn('192.168.1.67',warning);self.assertNotIn('169.254',warning)
        self.assertIn('not one of this PC',cf.download_host_warning('192.168.1.99',mine))
        self.assertIn('only this PC',cf.download_host_warning('127.0.0.1',mine))
        for fine in ('192.168.1.67','8.8.8.8','pso.example.com'):
            self.assertEqual(cf.download_host_warning(fine,mine),'',fine)
        self.assertIsInstance(cf.local_ipv4_addresses(),list)


class DownloadServerPanelTests(PanelTests):
    """The Caelum modpacks page with the built-in download server: generated, read-only addresses."""

    def setUp(self):
        super().setUp()
        from teaserv_admin import jsonc
        self.window.config_root=jsonc.parse('{"ExternalAddress": "203.0.113.5", "TeaservModpackHTTPEnabled": true, '
                                            '"TeaservModpackHTTPPort": 8080}')

    # the inherited panel tests assume the download server is off
    test_initial_missing_folder_readonly_fields_refresh_and_reopen=None
    test_refresh_is_background_and_preserves_edits_during_hash=None
    test_missing_installation_does_not_create_a_server_or_block_selecting_one=None
    test_invalid_zip_and_changes_since_refresh_block_save=None

    def url_entry(self):
        from tkinter import ttk
        box=self.panel.rows_frame.winfo_children()[0]
        return [w for w in box.winfo_children() if isinstance(w,ttk.Entry)][1]

    def test_generated_addresses_follow_the_fields_and_keep_typed_ones(self):
        from teaserv_admin.settings_window import Changes,PanelError
        self.panel.load();self.wait()
        pack(self.system/'caelum-modpacks/Beach Lobby.zip');self.panel.refresh();self.wait()
        generated='http://203.0.113.5:8080/modpacks/Beach%20Lobby.zip'
        self.assertEqual(self.panel.packs()[0][2],generated)
        self.assertEqual(str(self.url_entry().cget('state')),'readonly')
        self.assertIn('download server is on',self.panel.auto_note.cget('text'))
        changes=self.save_files()
        self.assertFalse(changes.config_edits)  # untouched settings are not rewritten
        manifest=(self.system/'caelum-modpacks.txt').read_text(encoding='utf-8')
        self.assertIn('\tBeach Lobby\t'+generated+'\n',manifest)
        self.assertEqual(cf.parse_modpack_sources((self.system/cf.MODPACK_SOURCES_FILE).read_text(encoding='utf-8'))[0][3],'')
        # own address and default port
        self.panel.address_editor.var.set('pso.example.com');self.panel.port_editor.var.set('')
        self.assertEqual(self.panel.packs()[0][2],'http://pso.example.com/modpacks/Beach%20Lobby.zip')
        # off: the address can be typed; on again: generated; off again: the typed one is back
        self.panel.enabled_editor.flag.set(False);self.panel.enabled_editor.touch()
        self.assertEqual(str(self.url_entry().cget('state')),'normal')
        self.assertEqual(self.panel.packs()[0][2],'')
        self.assertFalse(self.panel.auto_note.winfo_ismapped())
        with self.assertRaises(PanelError):self.panel.collect(Changes())  # an address is required again
        self.panel.rows[0][2].set('https://example.com/beach.zip')
        self.panel.enabled_editor.flag.set(True)
        self.assertEqual(self.panel.packs()[0][2],'http://pso.example.com/modpacks/Beach%20Lobby.zip')
        self.panel.enabled_editor.flag.set(False)
        self.assertEqual(self.panel.packs()[0][2],'https://example.com/beach.zip')
        changes=Changes();self.panel.collect(changes)
        self.assertIn((('TeaservModpackHTTPEnabled',),'false'),changes.config_edits)

    def test_unusable_external_address_blocks_saving(self):
        from teaserv_admin import jsonc
        from teaserv_admin.settings_window import Changes,PanelError
        self.window.config_root=jsonc.parse('{"ExternalAddress": "en0", "TeaservModpackHTTPEnabled": true}')
        self.panel.load();self.wait()
        pack(self.system/'caelum-modpacks/a.zip');self.panel.refresh();self.wait()
        self.assertEqual(self.panel.packs()[0][2],'')
        self.assertIn('not an IPv4 address',self.panel.auto_problem)
        with self.assertRaises(PanelError):self.panel.collect(Changes())
        self.panel.address_editor.var.set('203.0.113.9')
        self.assertEqual(self.panel.packs()[0][2],'http://203.0.113.9/modpacks/a.zip')
        changes=Changes();self.panel.collect(changes)
        self.assertIn((('TeaservModpackHTTPAddress',),'"203.0.113.9"'),changes.config_edits)


if __name__=='__main__':unittest.main()
