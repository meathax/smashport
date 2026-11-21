from pathlib import Path
import ctypes, ctypes.wintypes as wt, subprocess, os, time, json
root=Path.cwd()
import sys
route=sys.argv[1]
out=root/('build/room-dropin-'+route+'-'+str(time.time_ns()))
out.mkdir(exist_ok=False)
exe=root/'build/rom-player/SmashTV.exe'
user=ctypes.windll.user32
cb=ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
def window(pid):
    found=[]
    @cb
    def visit(hwnd, _):
        owner=wt.DWORD()
        user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        name=ctypes.create_unicode_buffer(128)
        user.GetClassNameW(hwnd,name,128)
        if owner.value==pid and name.value=='SmashTVNative': found.append(hwnd)
        return True
    user.EnumWindows(visit,0)
    return next(iter(found),0)
children=[]
fixture=(root/'build/audit-20261112-earned-highscore-initials/cmos.bin').read_bytes()
profile='wide' if route=='lan' else 'arcade'
cmos='cmos-l8-16x9.bin' if profile=='wide' else 'cmos-l8.bin'
def dialog(pid):
    found=[]
    @cb
    def dlg(hwnd,_):
        owner=wt.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(owner))
        title=ctypes.create_unicode_buffer(256);user.GetWindowTextW(hwnd,title,256)
        if owner.value==pid and 'Select ROM ZIP' in title.value:found.append(hwnd)
        return True
    user.EnumWindows(dlg,0)
    return next(iter(found),0)

print(out,flush=True)
def launch(role):
    d=out/role; data=d/'local/SmashTV';data.mkdir(parents=True); (d/'temp').mkdir()
    (data/'pc-settings.cfg').write_text('version=1\nmuted=1\nname='+role.upper()+'\n')
    env=dict(os.environ, LOCALAPPDATA=str(data.parent),TEMP=str(d/'temp'),TMP=str(d/'temp'),
        STV_HOST_ROM_ZIP=str(root/'original/user-supplied-roms/smashtv.zip'), STV_HOST_NOPAUSE='1',
        STV_FRONTEND_REPORT=str(d/'menu.jsonl'))
    if role=='host':
        (data/cmos).write_bytes(fixture)
        # The first-run picker is exercised independently.
    elif route=='lan':(data/'profile.cfg').write_text('reference')
    startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    p=subprocess.Popen([str(exe)],env=env,startupinfo=startup,cwd=exe.parent);children.append(p)
    if role=='first-run':
        end=time.monotonic()+12
        while not (picker:=dialog(p.pid)) and time.monotonic()<end:time.sleep(.1)
        assert picker,'first-run picker'
        path=ctypes.c_wchar_p(str(root/'original/user-supplied-roms/smashtv.zip'))
        time.sleep(.6)
        for attempt in range(4):
            if not user.IsWindow(picker):break
            user.SendMessageW(picker,1128,0x480,path)
            time.sleep(.15)
            user.PostMessageW(picker,273,1,0)
            time.sleep(.5)
    end=time.monotonic()+20
    while time.monotonic()<end:
        h=window(p.pid)
        if h:time.sleep(.3);return h
        time.sleep(.1)
    raise RuntimeError('no game window '+role)
def keys(h,*ks):
    for k in ks:user.PostMessageW(h,256,k,1);time.sleep(.10)
def records(role):
    path=out/role/'menu.jsonl'
    return [json.loads(s) for s in path.read_text().splitlines()] if path.exists() else []
def event(role,name,mincount=1,wait=12):
    end=time.monotonic()+wait
    while time.monotonic()<end:
        events=[e for e in records(role) if e['event']==name]
        if len(events)>=mincount:return events[-1]
        time.sleep(.1)
    raise AssertionError((role,name,records(role)[-5:]))
def join(role):
    h=launch(role)
    keys(h,40,13,40,40,40,40,13,*([40]*(3 if route=='wan' else 1)),13)
    if route=='wan':
        for c in '127.0.0.1':user.PostMessageW(h,258,ord(c),1)
        keys(h,13)
    event(role,'lobby_rooms');keys(h,40,13);event(role,'room_profile_selected')
    return h
try:
    host=launch('host')
    if route=='lan':
        keys(host,13);event('host','title_start');time.sleep(4)
        keys(host,112,40,13,40,13,40,40,40,40,13,13)
    else:keys(host,38,39,40,40,13,40,40,40,13)
    event('host','lobby_open');time.sleep(.4)
    if not [e for e in records('host') if e['event']=='room_solo']:
        keys(host,13);event('host','room_solo')
    time.sleep(6)   # the host's solo game runs before anyone arrives
    summary=[]
    for idx,name in enumerate(('guest','returning'),1):
        t0=time.monotonic()
        guest=join(name)
        b=event(name,'room_game_ready',wait=20); ready=time.monotonic()-t0
        a=event('host','room_game_ready',idx)
        assert a['detail'].endswith('snapshot') and b['detail'].endswith('snapshot'),(a,b)
        event(name,'lobby_established');event('host','lobby_established',idx)
        j=event(name,'player_joined',wait=40); joined=time.monotonic()-t0
        assert j['value']==2,j
        time.sleep(3)
        assert children[-1].poll() is None and children[0].poll() is None
        user.PostMessageW(guest,16,0,0);children[-1].wait(timeout=7)
        d=event('host','player_dropped',idx); c=event('host','room_continues',idx)
        time.sleep(2); assert children[0].poll() is None,'host exited'
        summary.append(dict(name=name,ready_s=round(ready,2),joined_s=round(joined,2),join_frames=j['detail'],dropped=d['detail']))
        print(summary[-1],flush=True)
    (out/'summary.json').write_text(json.dumps(summary,indent=1))
    print('PASS',flush=True)
finally:
    for p in children:
        if p.poll() is None:
            h=window(p.pid)
            if h:user.PostMessageW(h,16,0,0)
            try:p.wait(timeout=4)
            except subprocess.TimeoutExpired:p.terminate();p.wait()
