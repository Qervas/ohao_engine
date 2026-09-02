"""Mask ONLY the values that come out of a float atomicAdd accumulator.

The probe is not bit-deterministic: float atomicAdd into the gradient arena
and into the loss scalar is non-associative, so the same binary run twice
differs at ~1e-7.  Everything else -- every finite difference, bound, chi^2,
count, tolerance, verdict, string and ordering -- is stable and stays gated
byte for byte.

WHAT IS AND IS NOT ACCUMULATOR-DERIVED. The analytic gradient `g`, the scalar
loss, and everything computed from either ARE masked. D(h) and D(2h) are NOT,
and that is a judgement rather than a guarantee: they come out of the scalar
loss, so they are accumulator-derived too, but they agree to every one of the
nine digits printed and they are the substance of the convergence gates --
masking them would leave those lines saying nothing.

THEIR DIFFERENCE IS ANOTHER MATTER, and this file said otherwise until check
63 was added. An h^2 term is (D(2h) - D(h)) scaled, and for a case whose
truncation is IDENTICALLY ZERO by the derivation that subtraction cancels
nine digits and leaves the accumulator noise alone -- values around 1e-5 out
of operands around 2e3. Three runs happened to agree twice, which is exactly
the "it agreed this time" that --selfcheck exists to distrust; a fourth run
disagreed. Every h^2 and h^4 term is masked now.

  usage:  probe_normalise.py A.txt B.txt          compare two runs
          probe_normalise.py --selfcheck A B C..  report masks gone stale

THE SELF-CHECK EXISTS BECAUSE THIS FILE HAS GONE STALE TWICE. Stage 1 Task 6
added checks 46-49 and Stage 2 added 50-55; each printed accumulator-derived
values in formats these patterns did not know, and both times the staleness
was invisible until a baseline happened to be diffed against a second run of
the same binary -- once AFTER the hazard had been written down in a plan.
Extending the patterns again does not fix that. `--selfcheck` does: it runs
the comparison over several runs of ONE unmodified binary, where every
difference is by definition a mask that is missing, and names the lines.
Run it after adding any check that prints a gradient or a loss.
"""
import re
import sys

F = r'-?[\d.]+(?:[eE][-+]?\d+)?'


def sub(pattern, repl):
    return (re.compile(pattern), repl)


# Each pattern is keyed to the LABEL beside the value, never to the value's
# shape alone -- masking bare floats would hide the tolerances and counts that
# are the point of comparing at all.
SUBS = [
    # --- checks 37-45, the single-h gradient checks
    sub(r'(\|err\|/bound )(' + F + r')', r'\1<R>'),
    sub(r'(accumulated )(' + F + r')( into arena)', r'\1<G>\3'),
    # --- checks 46-49, the convergence gates
    sub(r'(  g )(' + F + r')', r'\1<A>'),
    sub(r'(gradient error \|T - e\(h\)\| = )(' + F + r')', r'\1<E>'),
    sub(r'(gradient error after removing BOTH )(' + F + r')', r'\1<E>'),
    sub(r'(which is )(' + F + r')( of the)', r'\1<R>\3'),
    sub(r'\((' + F + r')( of \|g\|\))', r'(<R>\2'),
    sub(r'(e\(2h\)/e\(h\) = )(' + F + r')', r'\1<R>'),
    sub(r'(Worst gradient error )(' + F + r')', r'\1<R>'),
    sub(r'(Gradient error )(' + F + r')', r'\1<R>'),
    # --- check 50, the adjoint seed
    sub(r'(give )(' + F + r')( and )(' + F + r')', r'\1<A>\3<A>'),
    sub(r'(agreeing to )(' + F + r')', r'\1<E>'),
    sub(r'(whole -- )(' + F + r')( \+ )(' + F + r')( = )(' + F + r')( against )(' + F + r')',
        r'\1<A>\3<A>\5<A>\7<A>'),
    sub(r'(, to )(' + F + r')( of a)', r'\1<R>\3'),
    # --- check 51, the loss kernel (its scalar is an atomicAdd too)
    sub(r'(the worst at )(' + F + r')', r'\1<R>'),
    sub(r'(OF ITS ALLOWANCE \(element )(\d+)(\))', r'\1<K>\3'),
    # --- check 53, the multi-view batch
    sub(r'(one scene: )(' + F + r')( and )(' + F + r')( separately, )(' + F + r')',
        r'\1<A>\3<A>\5<A>'),
    sub(r'(sum of )(' + F + r')', r'\1<A>'),
    sub(r'(measured at )(' + F + r')( of the )(' + F + r')( tolerance)',
        r'\1<R>\3<T>\5'),
    # --- check 57, the GPU boundary pass (float32 atomicAdd into the
    # vertex-gradient buffer, so the same non-determinism)
    sub(r'(to within )(' + F + r')( of the largest)', r'\1<R>\3'),
    sub(r'(worst relative )(' + F + r')( at component )(\d+)',
        r'\1<R>\3<K>'),
    # --- checks 54-55, the recovery gates
    sub(r'(worst )(' + F + r')( at element )(\d+)', r'\1<E>\3<K>'),
    sub(r'([Tt]he loss fell )(' + F + r')( -> )(' + F + r')', r'\1<L>\3<L>'),
    sub(r'(Loss )(' + F + r')( -> )(' + F + r')', r'\1<L>\3<L>'),
    # --- check 62, recovery THROUGH the parameterisation. Every number
    # here is the endpoint of an optimisation driven by the boundary
    # pass's atomicAdd gradients, so all of them are accumulator-derived
    # -- the control's included, and including any that happen to agree
    # across the runs this was written against.
    sub(r'(finished )(' + F + r')( from theta\\*)', r'\1<A>\3'),
    sub(r'(finishes )(' + F + r')( from theta\\*)', r'\1<A>\3'),
    sub(r'(the shape lands )(' + F + r')( from the target shape)', r'\1<A>\3'),
    sub(r'( loss )(' + F + r')( -> )(' + F + r')', r'\1<L>\3<L>'),
    # --- check 63, the projection gate. Its depth and lateral Jacobian
    # norms are pure arithmetic and stay gated; the run's endpoints are not.
    sub(r'(and finished )(' + F + r')( away)', r'\1<A>\3'),
    sub(r'(finishes at loss )(' + F + r')', r'\1<L>'),
    # --- checks 46-49, the DIFFERENCE of two nearly equal accumulator
    # values. See the module docstring: nine digits cancel and what is
    # left is noise, so these are masked where D(h) and D(2h) are not.
    sub(r'(h\^2 term )(' + F + r')', r'\1<C>'),
    sub(r'(h\^4 term )(' + F + r')', r'\1<C>'),
    sub(r'\((' + F + r')( x tolerance)', r'(<R>\2'),
    sub(r'(i\.e\. )(' + F + r')( of the h\^2 term)', r'\1<R>\3'),
    # --- check 64, the varying jump. These AGREED across the four
    # runs they were written against, and are masked anyway: they are
    # relative errors of an atomicAdd-accumulated gradient, and the
    # lesson three lines up is that agreeing on a given day is not
    # evidence of being deterministic. The jump's min, max and mean are
    # arithmetic on compile-time constants and stay gated.
    sub(r'(worst relative disagreement is )(' + F + r')( at component )(\d+)', r'\1<R>\3<K>'),
    sub(r'(misses the oracle by )(' + F + r')( at component )(\d+)', r'\1<R>\3<K>'),
    sub(r'(, )(' + F + r')(x the bar)', r'\1<R>\3'),
]

# `vs analytic` and `|err|` mean the arena only on the lines that pair them.
PAIRED = [sub(r'(vs analytic )(' + F + r')', r'\1<A>'),
          sub(r'(\|err\| )(' + F + r')', r'\1<E>')]

# A line that names a WORST element is reporting a case the run CHOSE, by a
# criterion computed from the accumulator. Which element wins moves between
# runs (check 49 has reported both 18 and 20 of 36), and so do its D(h) and
# D(2h) -- so on these lines the difference values are masked as well. Where
# the case is fixed by the derivation instead, D(h) and D(2h) stay gated:
# they agree to all nine printed digits and they are what those lines are
# for.
WORST = [sub(r'(element )(\d+)( of )(\d+)', r'\1<K>\3\4'),
         sub(r'(D\(h\) )(' + F + r')', r'\1<D>'),
         sub(r'(D\(2h\) )(' + F + r')', r'\1<D>')]


def norm(line):
    if 'vs analytic' in line:
        for rx, rep in PAIRED:
            line = rx.sub(rep, line)
    if ' element ' in line and 'D(h)' in line:
        for rx, rep in WORST:
            line = rx.sub(rep, line)
    for rx, rep in SUBS:
        line = rx.sub(rep, line)
    return line


def load(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return [norm(l.rstrip('\n')) for l in f]


def raw(p):
    with open(p, encoding='utf-8', errors='replace') as f:
        return [l.rstrip('\n') for l in f]


def selfcheck(paths):
    """Every difference between runs of ONE binary is a missing mask."""
    runs = [raw(p) for p in paths]
    n = min(len(r) for r in runs)
    stale = []
    for i in range(n):
        vals = {r[i] for r in runs}
        if len(vals) > 1 and len({norm(v) for v in vals}) > 1:
            stale.append(i)
    for i in stale[:12]:
        print('STALE MASK at line %d:' % (i + 1))
        for p, r in zip(paths, runs):
            print('  %-12s %s' % (p.split('/')[-1], norm(r[i])[:170]))
    print('--- %d line(s) vary across %d runs of one binary and are NOT masked ---'
          % (len(stale), len(paths)))
    return 1 if stale else 0


def compare(a_path, b_path):
    a, b = load(a_path), load(b_path)
    bad = 0
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else '<missing>'
        y = b[i] if i < len(b) else '<missing>'
        if x != y:
            bad += 1
            if bad <= 20:
                print('%d:\n  A %s\n  B %s' % (i + 1, x, y))
    print('--- %d differing line(s) after normalisation ---' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--selfcheck':
        sys.exit(selfcheck(sys.argv[2:]))
    sys.exit(compare(sys.argv[1], sys.argv[2]))
