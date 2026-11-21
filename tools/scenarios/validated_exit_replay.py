"""Validate a prepared replay against its sealed baseline before execution."""
import json
from pathlib import Path
import prepare_recorded_exit_replay as prepared


def validate(plan, players, runtime, exe):
    r, gaps = prepared.r, prepared.gaps
    r.require(plan.get('schema') == 'stv.recorded_exit_replay.v1'
              and plan.get('result') == 'PREPARED_NOT_EXECUTED'
              and plan.get('verified_at') == runtime
              and type(plan.get('players')) is int and plan['players'] == players,
              'Prepared replay is not current or has the wrong player count')
    final = plan.get('final_frame')
    r.require(type(final) is int and final > 0, 'Invalid prepared endpoint')
    targets = plan.get('targets')
    r.require(isinstance(targets, list) and targets and all(
        isinstance(t, list) and len(t) == 2 and all(type(v) is int for v in t)
        for t in targets), 'Invalid prepared targets')
    paths = [gaps.checked(pin) for pin in plan['pins']]
    r.require(len(paths) == len(set(paths)), 'Duplicate preparation pins')
    r.require(Path(exe).resolve() in [p.resolve() for p in paths],
              'Current executable is not pinned by preparation')
    reports = []
    for path in paths:
        if path.suffix == '.json':
            report = json.loads(path.read_text())
            if report.get('result') == 'COMPLETE_OBSERVATIONS_GATE_OPEN':
                reports.append(report)
    r.require(len(reports) == 1, 'Baseline report is not uniquely sealed')
    data, observations, _, _ = prepared.select(reports[0], plan['source_case'],
        players, final, runtime, exe, targets)
    inputs = gaps.checked(plan['inputs'])
    r.require(inputs.read_bytes() == data, 'Prepared replay differs from original prefix')
    r.require(plan.get('source_observations') == observations,
              'Prepared target observations differ from baseline')
    return inputs, final
