#!/usr/bin/env python3
"""Fail-closed extraction of selected original boot/operator data, not CPU code.
No runtime assembler or CPU decoder is emitted. Input files are never changed.
"""
from __future__ import annotations
import argparse, ast, hashlib, json, pathlib, re

FILES=['SYS.INC','LINK.EQU','MENU.EQU','AUDIT.EQU','MENU.ASM','TEST.ASM','DIAG.ASM','TEXT.ASM','AUDIT.ASM','ADJUST.ASM','DEMO.ASM','HSTD.ASM','BACKUP/IMGPAL.ASM']

def clean(text):
    # Selected data uses YUNIT/DEBUG. Other IFs are assembler assertions, not data.
    out=[];enabled=[True];conditions=[]
    for raw in text.splitlines():
        t=raw.split(';',1)[0]
        if not t.strip() or t[0] in '*\x1a':continue
        m=re.match(r'\s*\.IF\s+(YUNIT|DEBUG)\s*$',t,re.I)
        if m:
            v=m[1].upper()=='YUNIT';enabled.append(enabled[-1] and v);conditions.append(v);continue
        if re.match(r'\s*\.ELSE\s*$',t,re.I) and conditions:
            enabled[-1]=enabled[-2] and not conditions[-1];continue
        if re.match(r'\s*\.ENDIF\s*$',t,re.I) and conditions:
            enabled.pop();conditions.pop();continue
        if enabled[-1]:out.append(t.rstrip())
    return out

def value(s,syms):
    s=s.strip().upper()
    s=re.sub(r'\b([0-9][0-9A-F]*)H\b',lambda m:str(int(m[1],16)),s)
    s=re.sub(r'>\s*([0-9A-F]+)',lambda m:str(int(m[1],16)),s)
    s=re.sub(r'\b([01]+)B\b',lambda m:str(int(m[1],2)),s)
    s=re.sub(r'\b0+([0-9]+)\b',lambda m:str(int(m[1])),s)
    def ev(n):
        if isinstance(n,ast.Constant) and type(n.value) is int:return n.value
        if isinstance(n,ast.Name):return syms[n.id]
        if isinstance(n,ast.UnaryOp):
            a=ev(n.operand)
            if isinstance(n.op,ast.USub):return -a
            if isinstance(n.op,ast.Invert):return ~a
        if isinstance(n,ast.BinOp):
            a,b=ev(n.left),ev(n.right)
            if isinstance(n.op,ast.Add):return a+b
            if isinstance(n.op,ast.Sub):return a-b
            if isinstance(n.op,ast.Mult):return a*b
            if isinstance(n.op,ast.Div):return a//b
            if isinstance(n.op,ast.BitOr):return a|b
            if isinstance(n.op,ast.BitAnd):return a&b
            if isinstance(n.op,ast.BitXor):return a^b
        raise ValueError('Unsupported expression '+s)
    return ev(ast.parse(s,mode='eval').body)

def blocks(lines):
    result={};names=[];body=[]
    for t in lines:
        if re.match(r'^\w+:?\s+\.?(?:EQU|SET)\s+',t,re.I):continue
        m=re.match(r'^([A-Za-z_][\w]*):?\s*(.*)$',t)
        if m:
            for name in names:result[name]=body
            label,rest=m.groups();names=[label.upper()];body=[];t=rest
        if t.strip():body.append(t.strip())
    for name in names:result[name]=body
    return result

def main(src,out):
    lines={f:clean((src/f).read_text('latin1')) for f in FILES}
    syms={'YUNIT':1,'DEBUG':0};pending=[]
    coin_lines=lines['MENU.ASM']
    start=next(i for i,t in enumerate(coin_lines) if re.match(r'^CSELCT:',t))
    end=next(i for i,t in enumerate(coin_lines) if re.match(r'^CS_ENTS\s',t))
    coin_records=[]
    for t in coin_lines[start+1:end]:
        m=re.fullmatch(r'\s*\.(WORD|LONG)\s+(.+)',t,re.I)
        if not m:raise ValueError('Nonliteral coin preset '+t)
        coin_records.extend((m[1].upper(),x.strip()) for x in m[2].split(','))
    shape=['WORD']*13+['LONG']*5+['WORD','LONG']
    if len(coin_records)%len(shape):raise ValueError('Coin record count')
    syms['CS_ENTS']=len(coin_records)//len(shape)
    if any(k!=shape[i%len(shape)] for i,(k,_) in enumerate(coin_records)):raise ValueError('Coin record shape')
    for fs in lines.values():
        for t in fs:
            m=re.match(r'^(\w+):?\s+\.?(?:EQU|SET)\s+(.+)$',t,re.I)
            if m:pending.append(m.groups())
    for _ in range(40):
        changed=False
        for k,s in pending:
            try:v=value(s,syms)
            except (KeyError,ValueError,SyntaxError,ZeroDivisionError):continue
            if k.upper() not in syms:syms[k.upper()]=v;changed=True
        if not changed:break
    b={f:blocks(ls) for f,ls in lines.items()}
    strings={};messages={}
    for f in ['MENU.ASM','TEST.ASM','AUDIT.ASM','ADJUST.ASM','DIAG.ASM']:
        for label,body in b[f].items():
            ss=[];ms=[];metadata=None
            for t in body:
                m=re.match(r'MESS_MAC\s+(.+)',t,re.I)
                if m:
                    vals=[x.strip() for x in m[1].split(',')]
                    try:metadata=[vals[0],value(vals[1],syms),value(vals[2],syms),value(vals[3],syms),value(vals[4],syms),vals[5]]
                    except (KeyError,ValueError):metadata=None
                m=re.match(r'\.STRING\s+(.+)',t,re.I)
                if m:
                    text=''.join(re.findall(r'"([^"\n]*)"',m[1]));ss.append(text)
                    if metadata:ms.append([*metadata,text]);metadata=None
            if ss:strings[f+':'+label]=''.join(ss)
            elif body and re.fullmatch(r'\.BYTE\s+0',body[0],re.I):strings[f+':'+label]=''
            if ms:messages[f+':'+label]=ms
    def text(f,label):
        # Empty labels are aliases, e.g. U_CLR_CRED/MESS_CLCRED. Walk original order.
        key=f+':'+label.upper()
        if key in strings:return strings[key]
        fs=lines[f];start=None
        for i,t in enumerate(fs):
            if re.match(r'^'+re.escape(label)+r':?(?:\s|$)',t,re.I):start=i;break
        if start is None:raise ValueError('Missing original string '+key)
        for t in fs[start:start+18]:
            m=re.search(r'\.STRING\s+(.+)',t,re.I)
            if m:return ''.join(re.findall(r'"([^"\n]*)"',m[1]))
        raise ValueError('Unresolved string alias '+key)
    for label in ['MESS_RESET','MESS_FAC']:
        fs=lines['MENU.ASM']
        begin=next(i for i,t in enumerate(fs) if re.match(r'^'+label+r':?(?:\s|$)',t))
        meta=None
        for t in fs[begin:begin+14]:
            mm=re.search(r'MESS_MAC\s+(.+)',t,re.I)
            if mm:
                v=[x.strip() for x in mm[1].split(',')]
                meta=[v[0],*[value(x,syms) for x in v[1:5]],v[5]]
            st=re.search(r'\.STRING\s+(.+)',t,re.I)
            if st and meta:
                literal=''.join(re.findall(r'"([^"\n]*)"',st[1]))
                messages['MENU.ASM:'+label]=[[*meta,literal]]
                strings['MENU.ASM:'+label]=literal
                break
        else:raise ValueError('Boot message alias not resolved '+label)
    menus=[]
    for f,names in [('MENU.ASM',['MEN_MAIN','MEN_UTIL','MEN_DIAG','MEN_PATTERNS','MEN_YN','MEN_AP1','MEN_AP2','MEN_DETAIL']),('ADJUST.ASM',['MEN_ADJ','MEN_CUST'])]:
        for name in names:
            rs=[]
            for line in b[f][name]:
                m=re.match(r'\.(LONG|WORD)\s+(.+)',line,re.I)
                if not m:raise ValueError('Nonliteral menu '+name+': '+line)
                rs += [(m[1].upper(),x.strip()) for x in m[2].split(',')]
            if [r[0] for r in rs[:13]]!=['LONG']*3+['WORD']*10:raise ValueError('Menu header shape '+name)
            head=[value(s,syms) for _,s in rs[3:13]]
            items=[];p=13
            while p<len(rs) and rs[p][1]!='0':
                row=rs[p:p+3];p+=3
                if len(row)!=3 or any(x[0]!='LONG' for x in row):raise ValueError('Menu item shape')
                lab,routine,helpname=[x[1] for x in row]
                tf=f if f+':'+lab.upper() in strings else 'MENU.ASM'
                items.append({'label':lab,'text':text(tf,lab),'routine':routine,'help':helpname})
            if p!=len(rs)-1:raise ValueError('Menu lacks exact terminator '+name)
            title=text(f,rs[2][1]);menus.append({'file':f,'label':name,'title':title,'header':head,'items':items})
    fonts=[]
    for label in ['RD7FONT','RD15FONT']:
        glyphs=[]
        for t in b['TEXT.ASM'][label]:
            m=re.match(r'\.LONG\s+(.+)',t,re.I)
            if not m:raise ValueError('Nonliteral font '+t)
            glyphs.extend(x.strip() for x in m[1].split(','))
        if len(glyphs)!=93:raise ValueError(('Font length',label,len(glyphs)))
        fonts.append(glyphs)
    diag=[]
    for t in b['DIAG.ASM']['SYSFONT']:
        m=re.fullmatch(r'\.BYTE\s+\((T2_\w+)-T2_SP\)/32',t,re.I)
        if not m:raise ValueError('Diagnostic directory '+t)
        body=b['DIAG.ASM'][m[1].upper()]
        dims=value(body[0].split(None,1)[1],syms)
        img=body[1].split(None,1)[1].split('-')[0].upper()
        pixels=[]
        for r in b['DIAG.ASM'][img]:
            if r.upper()=='.END':break
            if not r.upper().startswith('.BYTE'):raise ValueError(('Diagnostic glyph bytes',img,r))
            pixels.extend(value(x,syms) for x in r.split(None,1)[1].split(','))
        expected=((dims>>4)+7)//8*(dims&15)
        if len(pixels)!=expected or expected>16:raise ValueError(('Glyph length',img,len(pixels),expected))
        diag.append([dims>>4,dims&15,*pixels,*([0]*(16-len(pixels)))])
    chips=[]
    for t in lines['DIAG.ASM']:
        m=re.match(r'^(\w+)\s+CHIPMAC\s+(.+)',t,re.I)
        if m:
            fields=[x.strip() for x in m[2].split(',')]
            if len(fields)!=12:raise ValueError('Chip record size')
            chips.append([m[1],*[value(s,syms) for s in fields[:7]],text('DIAG.ASM',fields[7]),*[value(s,syms) for s in fields[8:]]])
    vecs={}
    for name in ['CPU_VECS','MON_VECS','MON_RECS']:
        if name not in b['DIAG.ASM']:continue
        rows=[]
        for t in b['DIAG.ASM'][name]:
            m=re.match(r'VECMAC\s+(.+)',t,re.I)
            if m:rows.append([value(x,syms) for x in m[1].split(',')])
        vecs[name]=rows
    dots=[]
    for t in b['DIAG.ASM']['MON_DOTS']:
        m=re.match(r'DOTMAC\s+(.+)',t,re.I)
        if m:dots.append([value(x,syms) for x in m[1].split(',')])
    switches={}
    for label in ['BUT_TABLE','JOY_TABLE']:
        rows=[]
        for t in b['DIAG.ASM'][label]:
            m=re.match(r'BUT_MAC\s+(.+)',t,re.I)
            if m:rows.append([value(x,syms) for x in m[1].split(',')])
            elif not re.fullmatch(r'\.LONG\s+0',t,re.I):raise ValueError('Switch table '+t)
        switches[label]=rows
    switch_strings=[]
    for t in b['DIAG.ASM']['STR_TABLE']:
        m=re.match(r'STR_MAC\s+(.+)',t,re.I)
        if m:
            x,y,st,c,d=[x.strip() for x in m[1].split(',')]
            switch_strings.append([value(x,syms),value(y,syms),text('DIAG.ASM',st),value(c,syms),value(d,syms)])
        elif not re.fullmatch(r'\.LONG\s+0',t,re.I):raise ValueError('Switch string table '+t)
    spectrum=[]
    for t in b['DEMO.ASM']['COLTAB']:
        m=re.match(r'\.WORD\s+(.+)',t,re.I)
        if not m:raise ValueError('COLTAB not literal')
        spectrum.extend(value(x,syms) for x in m[1].split(','))
    # COLEND begins with signed 0xf000, the actual CYCSPECT sentinel.
    if not b['DEMO.ASM']['COLEND'][0].upper().startswith('.WORD'):raise ValueError('No COLEND sentinel')
    highscores=[];highscore_guards=[]
    for label in ['ALL_TIME_ROM_TABLE','TODAYS_ROM_TABLE']:
        rows=[];score=None
        for t in b['HSTD.ASM'][label]:
            if t.upper()=='.END':break
            m=re.match(r'SCR_M\s+(.+)',t,re.I)
            if m:
                if score is not None:raise ValueError('Unpaired high score')
                bs=[value(x,syms) for x in m[1].split(',')]
                if len(bs)!=4:raise ValueError('High score shape')
                score=int.from_bytes(bytes(bs),'big');continue
            m=re.match(r'INIT_M\s+(.+)',t,re.I)
            if m:
                initials=''.join(re.findall(r'"([^"]*)"',m[1]))
                if len(initials)!=3 or score is None:raise ValueError('High score initials shape')
                rows.append([score,initials]);score=None;continue
            raise ValueError('Nonliteral high score '+t)
        if len(rows)!=11:raise ValueError('Expected 11 source high score records')
        highscores.append(rows);highscore_guards.append(score)
    coin_presets=[]
    for i in range(syms['CS_ENTS']):
        rs=coin_records[i*20:(i+1)*20]
        coin_presets.append({'title':text('MENU.ASM',rs[16][1]),'adjustments':[value(x[1],syms) for x in rs[:9]],'values':[value(x[1],syms) for x in rs[9:13]],'prefix':rs[13][1],'suffix':rs[14][1],'prompt':rs[15][1],'list':rs[17][1],'pick':value(rs[18][1],syms),'slots':rs[19][1]})
    for preset in coin_presets:
        preset['prefix_text']=text('MENU.ASM',preset['prefix'])
        preset['suffix_text']=text('MENU.ASM',preset['suffix'])
        preset['slots_text']=text('MENU.ASM',preset['slots'])
        heads=[]
        if preset['list']!='0':
            for t in b['MENU.ASM'][preset['list']]:
                m=re.fullmatch(r'\.LONG\s+(.+)',t,re.I)
                if not m:raise ValueError('Coin header list '+t)
                for name in m[1].split(','):
                    if name.strip()!='0':heads.append(text('MENU.ASM',name.strip()))
        preset['header_lines']=heads
    audits=[]
    for name in ['COIN_AUDS','COIN1_AUDS','GAME_AUDS','GAME_AUD2']:
        head=[];rows=[]
        for t in b['MENU.ASM'][name]:
            m=re.fullmatch(r'\.WORD\s+(.+)',t,re.I)
            if m:head.extend(value(x,syms) for x in m[1].split(','));continue
            m=re.fullmatch(r'AMAC\s+(.+)',t,re.I)
            if m:
                lab,fn,index=[x.strip() for x in m[1].split(',')]
                rows.append({'text':text('MENU.ASM',lab),'routine':fn,'index':value(index,syms)});continue
            if not re.fullmatch(r'\.LONG\s+0',t,re.I):raise ValueError('Audit table '+t)
        if len(head)!=4:raise ValueError('Audit header shape')
        audits.append({'name':name,'header':head,'rows':rows})
    # Header size followed by palette words.
    palettes={}
    for name in ['DIAGP','NUP1','NUARENAP']:
        body=b['BACKUP/IMGPAL.ASM'][name];words=[]
        for t in body:
            m=re.match(r'\.WORD\s+(.+)',t,re.I)
            if not m:raise ValueError('Nonliteral palette')
            words.extend(value(x,syms) for x in m[1].split(','))
        if words[0]!=len(words)-1:raise ValueError('Palette size')
        palettes[name]=words[1:]
    defaults=[]
    for t in b['AUDIT.ASM']['FACTORY_TABLE']:
        m=re.match(r'\.WORD\s+(.+)',t,re.I)
        if not m:raise ValueError('Factory table shape')
        defaults.extend(value(x,syms) for x in m[1].split(','))
    if len(defaults)!=24:raise ValueError('Expected 24 original adjustment defaults')
    adjustments=[]
    for label,body in b['ADJUST.ASM'].items():
        if not label.startswith('DO_') or not body or not re.fullmatch(r'\.LONG\s+GEN_ADJ',body[0],re.I):continue
        rs=[]
        for t in body:
            m=re.match(r'\.(LONG|WORD)\s+(.+)',t,re.I)
            if not m:raise ValueError('Unexpected adjustment descriptor '+label)
            rs.extend((m[1].upper(),s.strip()) for s in m[2].split(','))
        if len(rs)!=12:raise ValueError(('Adjustment size',label,len(rs)))
        choices=[]
        if rs[8][1]=='LIST_1LINE':
            l=rs[10][1].upper()
            for t in b['ADJUST.ASM'][l]:
                m=re.match(r'\.LONG\s+(.+)',t,re.I)
                if not m:raise ValueError(('List choices',l,t))
                for s in m[1].split(','):
                    if s.strip()!='0':choices.append(text('ADJUST.ASM',s.strip()))
        adjustments.append({'label':label,'title':text('ADJUST.ASM',rs[1][1]),'help':rs[2][1],'index':value(rs[3][1],syms),'min':value(rs[4][1],syms),'max':value(rs[5][1],syms),'delta':value(rs[6][1],syms),'zero':value(rs[7][1],syms),'formatter':rs[8][1],'flags':value(rs[9][1],syms),'choices':choices,'name_pointer':rs[10][1]})
    # Help structures are selected through the real menu/adjustment pointers.
    helps=[]
    names={r['help'] for m in menus for r in m['items']} | {a['help'] for a in adjustments}
    for name in sorted(names-{'0','1','ADJ_HELP','DO_ADJH'}):
        candidates=[f for f in ['MENU.ASM','ADJUST.ASM','TEST.ASM'] if name in b[f]]
        if not candidates:continue # routine-valued help is a separately translated service
        f=candidates[0];rs=[]
        for t in b[f][name]:
            m=re.fullmatch(r'\.(WORD|LONG)\s+(.+)',t,re.I)
            if not m:break
            rs.extend((m[1].upper(),x.strip()) for x in m[2].split(','))
        if len(rs)<3 or [x[0] for x in rs[:3]]!=['WORD']*3:continue
        just,col,count=[value(x[1],syms) for x in rs[:3]]
        if len(rs)!=count+3 or any(x[0]!='LONG' for x in rs[3:]):raise ValueError('Help shape '+name)
        hs=[]
        for _,lab in rs[3:]:
            tf=next((ff for ff in [f,'MENU.ASM','ADJUST.ASM','TEST.ASM'] if lab in b[ff]),None)
            if not tf:raise ValueError('Help literal missing '+lab)
            hs.append(text(tf,lab))
        helps.append({'name':name,'justification':just,'color':col,'lines':hs})
    intensity=[]
    for t in b['DIAG.ASM']['INTENSITIES_START']:
        if t.upper().startswith('.LONG'):break
        m=re.fullmatch(r'\.WORD\s+(.+)',t,re.I)
        if not m:raise ValueError('Intensity table shape')
        intensity.extend(value(x,syms) for x in m[1].split(','))
    if len(intensity)!=16:raise ValueError('Expected 8 intensity pairs')
    report={'source_files' :{f:hashlib.sha256((src/f).read_bytes()).hexdigest() for f in FILES},'revision':text('AUDIT.ASM','ROM_NAME'),'helps':helps,'audits':audits,'intensities':intensity,'menus':menus,'fonts':fonts,'diagnostic_glyphs':diag,'chips':chips,'vectors':vecs,'palettes':palettes,'defaults':defaults,'adjustments':adjustments,'strings':strings,'messages':messages,'coin_presets':coin_presets,'spectrum':spectrum,'switches':switches,'switch_strings':switch_strings,'monitor_dots':dots,'highscores':highscores,'highscore_guards':highscore_guards,'constants':{s:syms[s] for s in ['UP_BITS','DOWN_BITS','BUTTONS','TYPO_STALL','MENU_TYPO','TIT_ULX','TIT_ULY','TIT_LRX','TIT_LRY','INST_ULY','INST_LRY','MENU_X','MENU_Y','TM_X','TM_Y','DONEMASK','CS_ENTS','AM_HX','ADJ_ULY','CUR_ULY','CUR_LRY']}}
    def js(x):return json.dumps(x,ensure_ascii=True)
    cpp=['// Generated from original literal data. See frontend_tables.json.','#include "stv/frontend.hpp"','namespace stv {',f'const char source_ui_revision[]={js(report["revision"])};']
    profile=hashlib.sha256(json.dumps(report['source_files'],sort_keys=True,separators=(',',':')).encode()).digest()
    cpp+=['const u8 source_ui_profile[32] = {'+','.join(str(x) for x in profile)+'};']
    for i,h in enumerate(helps):cpp+=['static const char* const help_lines_%d[] = {'%i+','.join(js(x) for x in h['lines'])+'};']
    cpp+=['const UiHelp source_ui_helps[] = {']+['{'+js(h['name'])+',%d,%d,help_lines_%d,%d},'%(h['justification'],h['color'],i,len(h['lines'])) for i,h in enumerate(helps)]+['};',f'const unsigned source_ui_help_count={len(helps)};']
    cpp+=['const u16 source_ui_intensities[16] = {'+','.join(str(x) for x in intensity)+'};']
    cpp+=['const char* const source_ui_font_names[2][93] = {']+['{'+','.join(js(g) for g in row)+'},' for row in fonts]+['};']
    cpp+=['const DiagGlyph source_diag_glyphs[] = {']+['{%d,%d,{%s}},'%(g[0],g[1],','.join(str(x) for x in g[2:])) for g in diag]+['};',f'const unsigned source_diag_glyph_count={len(diag)};']
    cpp+=['const DiagChip source_diag_chips[] = {']+['{'+','.join(js(x) if isinstance(x,str) else str(x) for x in row)+'},' for row in chips]+['};',f'const unsigned source_diag_chip_count={len(chips)};']
    for name,rows in vecs.items():
        cpp+=['const DiagLine source_'+name.lower()+'[] = {']+['{'+','.join(str(x) for x in row)+'},' for row in rows]+['};',f'const unsigned source_{name.lower()}_count={len(rows)};']
    for name,words in palettes.items():cpp+=['const u16 source_ui_'+name.lower()+'[] = {'+','.join(str(w) for w in words)+'};',f'const unsigned source_ui_{name.lower()}_count={len(words)};']
    cpp+=['const u32 source_factory_adjustments[24] = {'+','.join(str(x) for x in defaults)+'};']
    for i,m in enumerate(menus):cpp+=['static const UiMenuItem menu_items_%d[] = {'%i]+['{'+','.join(js(row[k]) for k in ['text','routine','help'])+'},' for row in m['items']]+['};']
    cpp+=['const UiMenu source_ui_menus[] = {']+['{'+js(m['label'])+','+js(m['title'])+',{'+','.join(str(x) for x in m['header'])+'},menu_items_%d,%d},'%(i,len(m['items'])) for i,m in enumerate(menus)]+['};',f'const unsigned source_ui_menu_count={len(menus)};']
    for i,a in enumerate(adjustments):
        if a['choices']:cpp+=['static const char* const choices_%d[] = {'%i+','.join(js(x) for x in a['choices'])+'};']
    cpp+=['const UiAdjustment source_ui_adjustments[] = {']
    for i,a in enumerate(adjustments):cpp+=['{'+','.join(js(a[k]) for k in ['label','title','formatter','help'])+','+','.join(str(a[k]) for k in ['index','min','max','delta','zero','flags'])+','+(f'choices_{i}' if a['choices'] else 'nullptr')+','+str(len(a['choices']))+','+js(text('ADJUST.ASM',a['name_pointer']) if a['formatter']=='BIN_OR_MESS' else '')+'},']
    cpp+=['};',f'const unsigned source_ui_adjustment_count={len(adjustments)};']
    cpp+=['const u16 source_ui_spectrum[] = {'+','.join(str(x) for x in spectrum)+'};',f'const unsigned source_ui_spectrum_count={len(spectrum)};']
    cpp+=['const DiagDot source_mon_dots[] = {']+['{'+','.join(str(x) for x in row)+'},' for row in dots]+['};',f'const unsigned source_mon_dot_count={len(dots)};']
    for key,rows in switches.items():
        cpp+=['const UiSwitch source_'+key.lower()+'[] = {']+['{'+','.join(str(x) for x in row)+'},' for row in rows]+['};',f'const unsigned source_{key.lower()}_count={len(rows)};']
    cpp+=['const UiSwitchText source_switch_text[] = {']+['{'+','.join(js(x) if isinstance(x,str) else str(x) for x in row)+'},' for row in switch_strings]+['};',f'const unsigned source_switch_text_count={len(switch_strings)};']
    for i,a in enumerate(audits):cpp+=['static const UiAuditRow audit_rows_%d[] = {'%i]+['{'+js(r['text'])+','+js(r['routine'])+','+str(r['index'])+'},' for r in a['rows']]+['};']
    cpp+=['const UiAuditTable source_ui_audits[] = {']+['{'+js(a['name'])+',{'+','.join(str(x) for x in a['header'])+'},audit_rows_%d,%d},'%(i,len(a['rows'])) for i,a in enumerate(audits)]+['};',f'const unsigned source_ui_audit_count={len(audits)};']
    for i,preset in enumerate(coin_presets):
        if preset['header_lines']:cpp+=['static const char* const coin_heads_%d[] = {'%i+','.join(js(x) for x in preset['header_lines'])+'};']
    cpp+=['const UiHighScore source_factory_scores[2][11] = {']
    for rows in highscores:cpp+=['{'+','.join('{'+str(n)+'u,'+js(st)+'}' for n,st in rows)+'},']
    cpp+=['};','const UiCoinPreset source_ui_coin_presets[] = {']
    for i,r in enumerate(coin_presets):
        cpp+=['{'+js(r['title'])+',{'+','.join(str(x) for x in r['adjustments'])+'},{'+','.join(str(x) for x in r['values'])+'},'+','.join(js(r[k]) for k in ['prefix_text','suffix_text','slots_text'])+','+(f'coin_heads_{i}' if r['header_lines'] else 'nullptr')+','+str(len(r['header_lines']))+'},']
    cpp+=['};',f'const unsigned source_ui_coin_preset_count={len(coin_presets)};']
    cpp+=['const UiMessage source_ui_messages[] = {']
    msgcount=0
    for key,rows in sorted(messages.items()):
        for font,space,x,y,c,just,st in rows:
            cpp+=['{'+js(key)+','+js(st)+','+','.join(str(v) for v in [int(font=='RD15FONT'),space,x,y,c,{'STRLNRM':0,'STRCNRM':1,'STRRNRM':2}[just]])+'},'];msgcount+=1
    cpp+=['};',f'const unsigned source_ui_message_count={msgcount};']
    # Messages preserve literal strings / coordinates without linking unused routines.
    cpp+=['const UiLiteral source_ui_literals[] = {']+['{'+js(k)+','+js(v)+'},' for k,v in sorted(strings.items())]+['};',f'const unsigned source_ui_literal_count={len(strings)};','}']
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_bytes(('\n'.join(cpp)+'\n').encode('utf-8'))
    out.with_suffix('.json').write_bytes((json.dumps(report,indent=2)+'\n').encode('utf-8'))
    print(f'Extracted {len(menus)} menus, {sum(len(m["items"]) for m in menus)} entries, {len(adjustments)} adjustments, {len(chips)} chip drawings and original fonts')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);a=p.parse_args();main(a.source,a.output)
