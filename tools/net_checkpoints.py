"""Strict periodic RAM comparison for actual executable peers.
The offline runner and host use the protocol's exchanged-hash period. A requested
fault waives only joining-peer checkpoints before its first actual resync.
The selected checkpoint must match the last observed injection hash. Later
waived differences are retained without asserting their individual cause.
Every checkpoint is retained; later or host mismatches are failures.
"""
import re

SCHEMA = "smash.net.periodic-ram.v1"
PERIOD = 30


def require(condition, message):
    if not condition:
        raise ValueError(message)


def checked(report, frames, role):
    require(type(frames) is int and frames > 0, "invalid requested checkpoint extent")
    data = report.get("periodic_checkpoints")
    require(isinstance(data, dict) and data.get("schema") == SCHEMA,
            role + ": missing periodic checkpoint report")
    require(data.get("enabled") is True, role + ": periodic audit was disabled")
    require(type(data.get("frames_requested")) is int and data["frames_requested"] == frames,
            role + ": checkpoint extent differs")
    require(type(data.get("period")) is int and data["period"] == PERIOD,
            role + ": checkpoint period differs from the protocol")
    points = data.get("checkpoints")
    expected = range(0, frames, PERIOD)
    require(isinstance(points, list) and len(points) == len(expected),
            role + ": incomplete or extra periodic checkpoints")
    for frame, entry in zip(expected, points):
        require(isinstance(entry, list) and len(entry) == 2 and
                type(entry[0]) is int and entry[0] == frame,
                role + ": checkpoint frames are missing, repeated or out of order")
        require(isinstance(entry[1], str) and re.fullmatch(r"[0-9a-f]{8}", entry[1]),
                role + ": invalid checkpoint RAM hash")
    bases = data.get("resync_bases")
    require(isinstance(bases, list), role + ": missing resync boundary evidence")
    previous = 0
    for base in bases:
        require(type(base) is int and previous < base <= frames,
                role + ": invalid, repeated or out-of-run resync boundary")
        previous = base
    return points, bases


def final_injection_hash(report, requested, effective):
    observed = report.get("fault_injection")
    require(isinstance(observed, dict) and
            observed.get("schema") == "smash.net.fault-injection.v1" and
            observed.get("mode") == "single_exchanged_hash_frame_xor_8000" and
            observed.get("enabled") is True,
            "selected checkpoint lacks the observed injection contract")
    for name, wanted in (("requested_frame", requested), ("effective_frame", effective),
                         ("hash_period", PERIOD)):
        require(type(observed.get(name)) is int and observed[name] == wanted,
                "selected checkpoint and injection boundary differ")
    events = observed.get("events")
    require(isinstance(events, list) and events and
            type(observed.get("attempts")) is int and observed["attempts"] == len(events),
            "selected checkpoint lacks complete recorded injection events")
    for event in events:
        require(isinstance(event, dict) and type(event.get("frame")) is int and
                event["frame"] == effective and isinstance(event.get("after_hash"), str) and
                re.fullmatch(r"[0-9a-f]{8}", event["after_hash"]),
                "invalid recorded injection hash or frame")
    # Replays restore before-frame state, then repeat the mutation before hashing.
    # Confirmation follows the final such step; earlier prediction hashes do not
    # establish the selected confirmed checkpoint.
    return events[-1]["after_hash"]


def compare_periodic(reference, host, join, frames, fault=0):
    require(type(fault) is int and 0 <= fault < frames,
            "fault must be disabled or inside the checkpoint extent")
    expected, reference_bases = checked(reference, frames, "offline")
    left, host_bases = checked(host, frames, "host")
    right, join_bases = checked(join, frames, "join")
    require(not reference_bases and not host_bases,
            "offline/authoritative host cannot claim a joining-peer resync load")
    if fault:
        effective = fault + (-fault % PERIOD)
        require(effective < frames, "fault's exchanged hash falls outside the run")
        require(join_bases and join_bases[0] > effective,
                "fault lacks an actual later resync boundary")
        require(type(join.get("resyncs")) is int and join["resyncs"] == len(join_bases),
                "joining resync count and recorded boundaries differ")
        recovery = join_bases[0]
        require(type(join.get('first_resync_base')) is int and join['first_resync_base'] == recovery,
                'first resync observation and checkpoint boundary differ')
        selected_hash = final_injection_hash(join, fault, effective)
    else:
        require(not join_bases, "uninjected run has unexpected resync boundaries")
        effective = recovery = selected_hash = None
    injected = []
    host_matched = join_matched = 0
    for (frame, wanted), (_, actual_host), (_, actual_join) in zip(expected, left, right):
        require(actual_host == wanted, f"host RAM differs from offline at periodic frame {frame}")
        host_matched += 1
        if fault and frame == effective:
            require(actual_join == selected_hash,
                    "selected injection checkpoint differs from the final recorded injection hash")
        if actual_join == wanted:
            join_matched += 1
        else:
            require(fault and effective <= frame < recovery,
                    f"join RAM differs from offline outside the injected interval at frame {frame}")
            injected.append({"frame": frame, "offline": wanted, "join": actual_join,
                             "classification": ("observed_selected_injection_hash_difference"
                                                if frame == effective else
                                                "waived_pre_resync_difference_unattributed")})
    return {"result": "PASS_PERIODIC_OFFLINE_RAM", "frames": frames, "period": PERIOD,
            "checkpoints_per_peer": len(expected), "host_matching": host_matched,
            "join_matching": join_matched, "injected_pre_resync_differences": injected,
            "first_resync_base": recovery,
            "selected_injection_checkpoint": ({"frame": effective, "after_hash": selected_hash}
                                               if fault else None),
            "scope": ("Every requested protocol-period checkpoint; the selected injection hash is bound "
                      "to its final observed mutation. Later pre-resync join differences are explicitly "
                      "waived and retained without a causal claim."),
            "complete_C6_gate_proof": False, "two_pc_proof": False, "physical_audio_proof": False}
