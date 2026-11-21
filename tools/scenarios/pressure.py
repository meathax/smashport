#!/usr/bin/env python3
"""Compare all emitted bot pressure observations, retaining both players/revisits.

These are sampled player-present frames, not elapsed clear time. A WAVESTAT is
flushed on a wave transition, not proof of clearing a room or completing a game.
Unfinished visits and ambiguous boundary life drops are retained separately.
Logs still lack seed/build identity.
Use run receipts and content/lifecycle evidence separately before claiming C4.
"""
import argparse
from fractions import Fraction
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re

STAT = re.compile(r"WAVESTAT([12]) f=(\d+) circuit=(\d+) wave=(\d+) frames=(\d+) deaths=(\d+) avg_enemies=(\d+(?:\.\d+)?) peak=(\d+)")
BOUND = re.compile(r"WAVEBOUND([12]) f=(\d+) from_circuit=(\d+) from_wave=(\d+) to_circuit=(\d+) to_wave=(\d+) life_decrease=(\d+)")
END = re.compile(r"native: frames=(\d+) cycles=(\d+) pc=([0-9a-fA-F]+) faults=(\d+) hcount_left=(\d+) input_left=(\d+) input_mismatch=(\d+) big_field=(\d+)")

def read_run(path, players):
    observations, partials, boundaries, previous, previous_bound, terminal = [], [], [], {}, {}, None
    raw = Path(path).read_bytes()
    for line_number, line in enumerate(raw.decode('utf-8-sig', errors='strict').splitlines(), 1):
        text = line.strip()
        if text.startswith('native:'):
            match = END.fullmatch(text)
            if not match or terminal is not None:
                raise ValueError(f'{path}:{line_number}: invalid/duplicate native terminal')
            frames, cycles, pc, faults, hcount, inputs, mismatches, big_field = match.groups()
            terminal = dict(frames=int(frames), cycles=int(cycles), pc=pc, faults=int(faults),
                            hcount_left=int(hcount), input_left=int(inputs), input_mismatch=int(mismatches), big_field=int(big_field))
        if 'WAVEBOUND' in text:
            match = BOUND.fullmatch(text)
            if not match or terminal is not None:
                raise ValueError(f'{path}:{line_number}: invalid/post-terminal WAVEBOUND')
            p,f,fc,fw,tc,tw,d = map(int,match.groups())
            if p>players or fc>2 or tc>2 or not 1<=fw<=65535 or not 1<=tw<=65535 or (fc,fw)==(tc,tw) or d<1 or d>65535 or f<=previous_bound.get(p,-1) or f<previous.get(p,-1):
                raise ValueError(f'{path}:{line_number}: impossible/out-of-order boundary decrease')
            previous_bound[p]=f
            boundaries.append(dict(player=p,frame=f,from_circuit=fc,from_wave=fw,to_circuit=tc,to_wave=tw,life_decrease=d,line=line_number))
            continue
        if 'WAVESTAT' not in text and 'WAVEPART' not in text:
            continue
        partial = text.startswith('WAVEPART')
        normalized = 'WAVESTAT'+text[len('WAVEPART'):] if partial else text
        match = STAT.fullmatch(normalized)
        if not match or terminal is not None:
            raise ValueError(f'{path}:{line_number}: invalid/post-terminal visit statistics')
        player, frame, circuit, wave, frames, deaths, average, peak = match.groups()
        p,f,c,w,n,d,a,k = int(player),int(frame),int(circuit),int(wave),int(frames),int(deaths),float(average),int(peak)
        if p>players or c>2 or not 1<=w<=65535 or n<1 or n>f+1 or f<=previous.get(p,-1) or f<previous_bound.get(p,-1) or not math.isfinite(a) or a>k+0.051:
            raise ValueError(f'{path}:{line_number}: impossible or out-of-order statistics')
        previous[p]=f
        visit=dict(player=p,frame=f,circuit=c,wave=w,frames=n,deaths=d,avg=a,peak=k,line=line_number)
        (partials if partial else observations).append(visit)
    if terminal is None or terminal['frames']<max([*previous.values(),*previous_bound.values(),1]):
        raise ValueError(f'{path}: missing/short native terminal; capture may be incomplete')
    if terminal['hcount_left'] or terminal['input_left'] or terminal['input_mismatch']:
        raise ValueError(f'{path}: unconsumed/mismatched replay inputs or HCOUNT')
    if {v['player'] for v in observations}!=set(range(1,players+1)):
        raise ValueError(f'{path}: no completed statistics for every requested player')
    return dict(path=str(path),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),terminal=terminal,
                observations=observations,partial_visits=partials,boundary_life_decreases=boundaries)

def aggregate(run, minimum_frames):
    groups, excluded = defaultdict(list), []
    for visit in run['observations']:
        if visit['frames'] < minimum_frames:
            excluded.append(dict(visit, reason='below_minimum_player_present_frames'))
        else:
            groups[(visit['player'],visit['circuit'],visit['wave'])].append(visit)
    result = {}
    for key, visits in groups.items():
        frames, deaths = sum(v['frames'] for v in visits), sum(v['deaths'] for v in visits)
        result[key] = dict(visits=len(visits), frames=frames, deaths=deaths, frames_per_visit=frames/len(visits),
                           deaths_per_visit=deaths/len(visits),
                           mean_enemies=sum(v['frames']*v['avg'] for v in visits)/frames,
                           peak=max(v['peak'] for v in visits), observation_lines=[v['line'] for v in visits])
    return result, excluded

def read_rooms(path):
    if path is None:
        return None
    rooms = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    if not isinstance(rooms,list) or not rooms:
        raise ValueError('Required rooms must be a nonempty list of [zero-based circuit, wave]')
    for row in rooms:
        if not isinstance(row,list) or len(row)!=2 or any(type(v) is not int for v in row) or not 0<=row[0]<=2 or not 1<=row[1]<=65535:
            raise ValueError('Invalid required-room row')
    if len({tuple(r) for r in rooms}) != len(rooms):
        raise ValueError('Duplicate required-room row')
    return {tuple(r) for r in rooms}

def compare(reference, enhanced, *, players, minimum_frames, lower, upper, required_rooms=None):
    r, re = aggregate(reference,minimum_frames)
    e, ee = aggregate(enhanced,minimum_frames)
    common = r.keys() & e.keys()
    if not common:
        raise ValueError('No eligible common player/room observations')
    key = lambda v: dict(player=v[0],circuit=v[1],wave=v[2])
    required = {(p,c,w) for p in range(1,players+1) for c,w in (required_rooms or set())}
    missing = sorted(required - common)
    unmatched = dict(reference_only=[key(k) for k in sorted(r.keys()-e.keys())],enhanced_only=[key(k) for k in sorted(e.keys()-r.keys())])
    rows=[]
    for k in sorted(common):
        rv, ev = r[k], e[k]
        time_ratio = Fraction(ev['frames']*rv['visits'], rv['frames']*ev['visits'])
        death_ratio = Fraction((ev['deaths']+ev['visits'])*rv['visits'],
                               (rv['deaths']+rv['visits'])*ev['visits'])
        low, high = Fraction(str(lower)), Fraction(str(upper))
        rows.append(dict(key(k),reference=rv,enhanced=ev,player_present_frames_ratio=float(time_ratio),
                         smoothed_deaths_per_visit_ratio=float(death_ratio),
                         mean_enemies_ratio=ev['mean_enemies']/rv['mean_enemies'] if rv['mean_enemies'] else None,
                         in_band=low<=time_ratio<=high and low<=death_ratio<=high))
    boundary_units = {name:sum(v['life_decrease'] for v in run.get('boundary_life_decreases',[])) for name,run in [('reference',reference),('enhanced',enhanced)]}
    incomplete = bool(missing or unmatched['reference_only'] or unmatched['enhanced_only'] or any(boundary_units.values()))
    result = 'INCOMPLETE' if incomplete else ('OUT_OF_BAND' if any(not row['in_band'] for row in rows) else 'OBSERVATIONS_IN_BAND')
    return dict(result=result,gate_proof=False,players=players,minimum_player_present_frames=minimum_frames,band=[lower,upper],
                unattributed_boundary_life_decreases=boundary_units,partial_visit_counts=dict(reference=len(reference.get("partial_visits",[])),enhanced=len(enhanced.get("partial_visits",[]))),
                required_room_inventory_supplied=required_rooms is not None, missing_required=[key(k) for k in missing],
                unmatched=unmatched,excluded=dict(reference=re,enhanced=ee),rows=rows,
                limitations=['Seed, command, executable/commit and scenario identity are not present in WAVESTAT logs; verify separately.',
                             'Includes all emitted revisits; no guessed first-game boundary. Eligible visit counts may differ.',
                             'Frames count only when own player object is present; partial visits are retained and excluded from completed-visit means.',
                             'The deaths field counts observed within-visit life-counter units; ambiguous boundary drops remain unattributed and make the comparison incomplete.',
                             'Enemy count uses the current bot type filter and +/-400-pixel coordinate box, not a radial distance.',
                             'Rounded one-decimal visit means are frame-weighted estimates; transitions and short exclusions do not prove room clears.',
                             'Without a required-room inventory, paired rows prove no claim of all-room coverage.'])

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference'); parser.add_argument('enhanced')
    parser.add_argument('--players',type=int,choices=(1,2),default=1)
    parser.add_argument('--min-frames',type=int,default=1000)
    parser.add_argument('--band-min',type=float,default=0.67); parser.add_argument('--band-max',type=float,default=1.5)
    parser.add_argument('--rooms',help='Required-room inventory: [[zero-based circuit,wave],...]')
    parser.add_argument('--json',type=Path)
    args=parser.parse_args(argv)
    try:
        if args.min_frames<1 or not (math.isfinite(args.band_min) and math.isfinite(args.band_max) and 0<args.band_min<=1<=args.band_max):
            raise ValueError('Invalid minimum frames or band')
        if args.json and args.json.exists():
            raise ValueError('Preserve existing comparison output')
        reference,enhanced=read_run(args.reference,args.players),read_run(args.enhanced,args.players)
        result=compare(reference,enhanced,players=args.players,minimum_frames=args.min_frames,lower=args.band_min,upper=args.band_max,required_rooms=read_rooms(args.rooms))
        result['runs']=dict(reference=reference,enhanced=enhanced)
        if args.rooms:
            raw=Path(args.rooms).read_bytes(); result['room_inventory']=dict(path=args.rooms,sha256=hashlib.sha256(raw).hexdigest())
        if args.json:
            args.json.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n',encoding='utf-8',newline='\n')
        print(json.dumps(dict(result=result['result'],players=args.players,paired_rows=len(result['rows']),out_of_band=sum(not row['in_band'] for row in result['rows']),gate_proof=False)))
        return 3 if result['result']=='INCOMPLETE' else (1 if result['result']=='OUT_OF_BAND' else 0)
    except (ValueError,OSError) as error:
        parser.exit(2,f'pressure: {error}\n')

if __name__=='__main__':
    raise SystemExit(main())
