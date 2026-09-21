#!/usr/bin/env python3
"""
Off-board sanity check, added during the 2026-09-21 C3 luma-drift session
(see docs/notes/c3-h264-chroma-drift.md's "Session 2026-09-21 (continued)"
section for the full writeup this supports).

Question: can the DOCUMENTED, deliberate simplification in
approach1-compute-encoder/shaders/deblock_filter.comp -- "no separate 3-tap
strong intra filter for bS==4, reuse the bS==3 tc0-clamped equations instead"
(see that shader's own "DELIBERATE SIMPLIFICATION: no separate 3-tap strong
intra filter for bS==4" comment) -- produce a luma pixel delta anywhere near
the board-measured 37405/2073600 differing, max delta 57
(docs/DEVLOG.md sec.36, `lab drift --codec=h264 --qp=27 --real-decode`)?

This is NOT a board run and cannot be one - there is no board in this
environment. It is a hand-port of:
  - the WEAK filter deblock_filter.comp actually runs for every bS,
    including bS==4 (ITU-T 8.7.2.3's "normal" filter, tc0-clamped,
    transcribed directly from that shader's filter_edge()).
  - the REAL ITU-T 8.7.2.4 STRONG filter a real decoder runs for bS==4
    (macroblock-boundary edges of an intra frame), transcribed from the
    spec text (cross-checked against the well-known x264/ffmpeg shape from
    memory - this script makes no network/board access, so it cannot be
    checked against x264's actual source in this environment; treat the
    strong-filter transcription itself as the one unverified input here).

RESULT (recorded 2026-09-21, `python3 tools/deblock_strong_vs_weak_check.py`):
at QP=27 specifically (the QP the board review used), the two filters differ
by at most ~1-2 across a battery of synthetic edge shapes - because ITU-T's
own alpha/beta gate is small at QP=27 (alpha=17, beta=6), so only near-flat
regions pass the "is this a real blocking edge" gate at all, and the weak
and strong filters closely agree in that narrow regime. The two filters only
diverge by tens of levels at much higher QP (~35 by QP=50, where alpha/beta
are large enough to gate in genuine high-contrast content). CONCLUSION: the
missing-strong-filter simplification is real (and worth fixing on its own
merits at high QP), but it is NOT big enough to explain a max delta of 57 at
QP=27 - do not spend further time chasing it as *the* C3 luma root cause at
this operating point.
"""

ALPHA_TABLE = [
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,
    32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255
]
BETA_TABLE = [
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,
    9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18
]
TC0_TABLE = [
    (0,0,0)]*17 + [
    (0,0,1),(0,0,1),(0,0,1),(0,0,1),(0,1,1),(0,1,1),(1,1,1),(1,1,1),
    (1,1,1),(1,1,1),(1,1,2),(1,1,2),(1,1,2),(1,1,2),(1,2,3),(1,2,3),
    (2,2,3),(2,2,4),(2,3,4),(2,3,4),(3,3,5),(3,4,6),(3,4,6),(4,5,7),
    (4,5,8),(4,6,9),(5,7,10),(6,8,11),(6,8,13),(7,10,14),(8,11,16),(9,12,18),
    (10,13,20),(11,15,23),(13,17,25)
]
assert len(TC0_TABLE) == 52

def clip1(v):
    return max(0, min(255, v))

def weak_filter(p2,p1,p0,q0,q1,q2, qp, bs):
    """Verbatim port of deblock_filter.comp's filter_edge(), specialised to
    the bS value passed in (deblock_filter.comp reuses this same code for
    bS==3 AND bS==4 - that reuse is exactly what this script is testing)."""
    idxA = max(0, min(51, qp))
    alpha = ALPHA_TABLE[idxA]
    beta = BETA_TABLE[idxA]
    if alpha == 0 or beta == 0:
        return (p1,p0,q0,q1)  # gate: no filtering at all
    if abs(p0-q0) >= alpha or abs(p1-p0) >= beta or abs(q1-q0) >= beta:
        return (p1,p0,q0,q1)  # gate: not a real blocking edge

    tc0row = TC0_TABLE[idxA]
    tc0 = tc0row[0] if bs==1 else tc0row[1] if bs==2 else tc0row[2]

    ap = abs(p2-p0) < beta
    aq = abs(q2-q0) < beta
    tc = tc0 + (1 if ap else 0) + (1 if aq else 0)

    delta = max(-tc, min(tc, ((q0-p0)*4 + (p1-q1) + 4) >> 3))
    p0n = clip1(p0+delta)
    q0n = clip1(q0-delta)
    p1n = p1
    q1n = q1
    if ap:
        d = max(-tc0, min(tc0, ((p2 + ((p0+q0+1)>>1)) >> 1) - p1))
        p1n = clip1(p1+d)
    if aq:
        d = max(-tc0, min(tc0, ((q2 + ((p0+q0+1)>>1)) >> 1) - q1))
        q1n = clip1(q1+d)
    return (p1n, p0n, q0n, q1n)

def strong_filter(p3,p2,p1,p0,q0,q1,q2,q3, qp):
    """ITU-T H.264 8.7.2.4, bS==4 (luma), transcribed from the spec text.
    Returns (p2',p1',p0',q0',q1',q2') - up to 3 samples deep each side."""
    idxA = max(0, min(51, qp))
    alpha = ALPHA_TABLE[idxA]
    beta = BETA_TABLE[idxA]
    if alpha == 0 or beta == 0:
        return (p2,p1,p0,q0,q1,q2)
    if abs(p0-q0) >= alpha or abs(p1-p0) >= beta or abs(q1-q0) >= beta:
        return (p2,p1,p0,q0,q1,q2)

    small_gap = abs(p0-q0) < ((alpha >> 2) + 2)
    ap = abs(p2-p0) < beta
    aq = abs(q2-q0) < beta

    if small_gap and ap:
        p0n = (p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3
        p1n = (p2 + p1 + p0 + q0 + 2) >> 2
        p2n = (2*p3 + 3*p2 + p1 + p0 + q0 + 4) >> 3
    else:
        p0n = (2*p1 + p0 + q1 + 2) >> 2
        p1n = p1
        p2n = p2

    if small_gap and aq:
        q0n = (q2 + 2*q1 + 2*q0 + 2*p0 + p1 + 4) >> 3
        q1n = (q2 + q1 + q0 + p0 + 2) >> 2
        q2n = (2*q3 + 3*q2 + q1 + q0 + p0 + 4) >> 3
    else:
        q0n = (2*q1 + q0 + p1 + 2) >> 2
        q1n = q1
        q2n = q2

    return (clip1(p2n), clip1(p1n), clip1(p0n), clip1(q0n), clip1(q1n), clip1(q2n))

def sweep():
    patterns = []
    for v in (0, 64, 128, 200, 255):
        patterns.append(("flat_%d" % v, [v]*8))
    for step in (1,2,3,5,8):
        patterns.append(("ramp_%d" % step, [max(0,min(255, 128 + i*step - 28)) for i in range(8)]))
    for lo, hi in ((16,235),(0,255),(32,224),(64,192)):
        patterns.append(("step_%d_%d" % (lo,hi), [lo,lo,lo,lo, hi,hi,hi,hi]))
    patterns.append(("textured_step", [10,14,9,13, 240,236,241,238]))

    worst_delta = 0
    worst_case = None
    worst_bound = 0
    worst_at_qp27 = 0
    worst_at_qp27_case = None
    two_pass_worst_at_qp27 = 0
    two_pass_case = None
    for qp in range(0, 52):
        for name, samp in patterns:
            p3,p2,p1,p0,q0,q1,q2,q3 = samp
            w_p1,w_p0,w_q0,w_q1 = weak_filter(p2,p1,p0,q0,q1,q2, qp, bs=3)
            s_p2,s_p1,s_p0,s_q0,s_q1,s_q2 = strong_filter(p3,p2,p1,p0,q0,q1,q2,q3, qp)

            deltas = {
                "p1": abs(s_p1 - w_p1),
                "p0": abs(s_p0 - w_p0),
                "q0": abs(s_q0 - w_q0),
                "q1": abs(s_q1 - w_q1),
                "p2": abs(s_p2 - p2),  # weak filter never touches p2/q2 at all
                "q2": abs(s_q2 - q2),
            }
            local_worst = max(deltas.values())
            if local_worst > worst_delta:
                worst_delta = local_worst
                worst_case = (qp, name, samp, deltas)
            if qp == 27 and local_worst > worst_at_qp27:
                worst_at_qp27 = local_worst
                worst_at_qp27_case = (name, samp, deltas)

            # Crude two-pass compounding estimate at qp=27: a MB CORNER
            # pixel sits on both a vertical and a horizontal MB-boundary
            # edge, so - IF the missing-strong-filter gap applied there
            # too - it could pick up roughly the sum of both passes'
            # individual gaps. Rough magnitude estimate only, not a real
            # 2D simulation of the actual dispatch.
            if qp == 27:
                two_pass = deltas["q0"] + deltas["p0"]
                if two_pass > two_pass_worst_at_qp27:
                    two_pass_worst_at_qp27 = two_pass
                    two_pass_case = (name, samp, deltas)

            idxA = max(0, min(51, qp))
            tc0row = TC0_TABLE[idxA]
            bound = tc0row[2] + 2  # deblock_bound(qp, "luma") from bc250_lab.sh
            if bound > worst_bound:
                worst_bound = bound

    print("At QP=27 specifically: max |strong - weak| = %d, case=%s"
          % (worst_at_qp27, worst_at_qp27_case))
    print("At QP=27, crude 2-pass (vertical+horizontal at a MB corner) "
          "compounding estimate: %d, case=%s" % (two_pass_worst_at_qp27, two_pass_case))
    print()
    print("Max |strong(real decode, bS=4) - weak(this shader's bS=4)| over qp 0..51: %d"
          % worst_delta)
    print("  at:", worst_case)
    print("Max single-edge weak-filter bound (tools/bc250_lab.sh deblock_bound, luma) "
          "over qp 0..51: %d" % worst_bound)
    print()
    print("Reference: board-measured (docs/DEVLOG.md sec.36, --real-decode, qp=27):")
    print("  luma differs 37405/2073600, max delta 57")
    idxA = 27
    tc0row = TC0_TABLE[idxA]
    print("  deblock_bound(27, luma) = %d  (what the harness currently checks against)"
          % (tc0row[2] + 2))

if __name__ == "__main__":
    sweep()
