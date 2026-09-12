"""GATE 4 -- the Mitsuba 3 oracle (spec 8.4), which has been owed since Stage 1.

Every other gate in this subsystem compares our renderer against a closed form
we derived, a finite difference of our own forward pass, or a supersampled
version of our own coverage. Those are strong -- they caught a sign error, a
3x orientation error and a factor-of-two pixel-seam doubling -- but they all
share one thing: the expectation comes from OUR understanding of the problem.
An independent renderer does not.

WHY THIS IS A THREE-WAY COMPARISON, not a two-way one. A disagreement between
two numbers tells you something is wrong and not which. So the scene here is
chosen so the answer is ALSO known in closed form:

    A Lambertian surface filling the frame, lit by a CONSTANT environment of
    radiance L. An ideal diffuse surface under uniform illumination reflects
    radiance a*L, so every pixel reads a*L and

        mean pixel               = a * L
        d(mean pixel) / d(albedo) = L

    exactly, with no integration error of any kind to argue about.

Three numbers per quantity that must agree: Mitsuba's, the closed form's, and
ours. If Mitsuba disagrees with the closed form, the scene description is
wrong and nothing about our renderer has been learned yet -- which is why this
script runs that leg FIRST and on its own.

    usage:  PYTHONPATH=build/mitsuba-pkgs python tests/diff/tools/mitsuba_gate.py

INSTALLED OUT OF TREE, deliberately. mitsuba pulls drjit and can move shared
dependencies, so it lives in build/mitsuba-pkgs via `pip install --target` and
is reached through PYTHONPATH rather than installed into whatever environment
happens to be active.

LEG 2 RUNS OUR RENDERER RATHER THAN QUOTING IT. diff_gpu_probe's check 72
computes the same two numbers on the GPU and prints them on a line beginning
MITSUBA-GATE; this script runs that binary and parses it. Quoting numbers from
a previous run into a comment would make this file a record rather than a
gate.
"""
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
CHECK_SOURCE = os.path.join(REPO_ROOT, 'tests', 'diff', 'probe', 'checks_mitsuba_scale.cpp')

# THE PRE-REGISTERED CRITERION, fixed before the first comparison.
ALBEDO = 0.5
ENV_RADIANCE = 1.0
SPP = 4096
# Monte Carlo, so the tolerance is a sampling allowance rather than a
# precision one: 4096 spp over a 16x16 film is 1e6 samples, and the estimator
# for this scene has low variance because every path terminates on the
# constant environment after one bounce.
REL_TOL = 5e-3
# Leg 2 compares two Monte Carlo estimates, so its allowance is built from
# BOTH sides' sampling error: mitsuba's REL_TOL above and the standard error
# our own probe measures and reports for itself.
LEG2_SIGMAS = 4.0


def closed_form():
    """An ideal Lambertian under uniform illumination reflects a*L, so every
    pixel reads a*L and the derivative with respect to the grey albedo is
    exactly L.

    WITH RESPECT TO THE GREY ALBEDO, which is the whole of the subtlety for
    the derivative: the parameter is one value driving three channels, so the
    comparison must sum the three partials. See the note in mitsuba_measure.
    """
    return {'mean pixel': ALBEDO * ENV_RADIANCE,
            'd(mean pixel)/d(albedo)': ENV_RADIANCE}


# ---------------------------------------------------------------------------
# The tie: both renderers must be describing the SAME scene
# ---------------------------------------------------------------------------
def assert_constants_tie():
    """Read kAlbedo and kEnvRadiance out of the C++ check and require them to
    equal this file's ALBEDO and ENV_RADIANCE.

    WITHOUT THIS THE THREE-WAY COMPARISON IS NOMINAL. Two renderers agreeing
    about different scenes is not agreement, and nothing else would notice:
    change kAlbedo to 0.6 and check 72 keeps passing against its own closed
    form, this script keeps passing leg 1 against its own, and leg 2 quietly
    compares a scene of albedo 0.6 against one of 0.5. So the constant is read
    from the source rather than trusted -- the same enforcement
    checkNeeStrideTie gives the GLSL/C++ record stride.
    """
    try:
        with open(CHECK_SOURCE, 'r', encoding='utf-8') as handle:
            source = handle.read()
    except OSError as exc:
        raise SystemExit('mitsuba_gate: cannot read %s (%s). The constants tie '
                         'cannot be checked, and without it leg 2 is not a '
                         'comparison of one scene.' % (CHECK_SOURCE, exc))

    wanted = {'kAlbedo': ALBEDO, 'kEnvRadiance': ENV_RADIANCE}
    for name, expected in sorted(wanted.items()):
        match = re.search(r'constexpr\s+float\s+' + name + r'\s*=\s*([0-9.eE+-]+)f?\s*;', source)
        if match is None:
            raise SystemExit('mitsuba_gate: %s is not declared in %s as a constexpr float. '
                             'If it was renamed, rename it here too -- do not delete the tie.'
                             % (name, os.path.relpath(CHECK_SOURCE, REPO_ROOT)))
        found = float(match.group(1))
        if found != expected:
            raise SystemExit('mitsuba_gate: THE TWO RENDERERS ARE NOT DESCRIBING THE SAME SCENE. '
                             '%s is %.9g in %s but this script uses %.9g. Leg 2 would be '
                             'comparing two different scenes and calling the difference a '
                             'renderer disagreement.'
                             % (name, found, os.path.relpath(CHECK_SOURCE, REPO_ROOT), expected))
    print('scene constants tied: albedo %.9g, environment radiance %.9g, read from %s'
          % (ALBEDO, ENV_RADIANCE, os.path.relpath(CHECK_SOURCE, REPO_ROOT)))


# ---------------------------------------------------------------------------
# Leg 1: mitsuba against the closed form
# ---------------------------------------------------------------------------
def mitsuba_measure():
    import mitsuba as mi
    mi.set_variant('cuda_ad_rgb')
    import drjit as dr

    scene = mi.load_dict({
        'type': 'scene',
        # max_depth 2 is camera -> surface -> environment: ONE bounce, which
        # is what makes the closed form exact. Deeper paths would add
        # interreflection the analytic answer does not contain.
        'integrator': {'type': 'path', 'max_depth': 2},
        'sensor': {
            'type': 'perspective',
            'fov': 30.0,
            'to_world': mi.ScalarTransform4f().look_at(
                origin=[0, 0, 2], target=[0, 0, 0], up=[0, 1, 0]),
            'film': {'type': 'hdrfilm', 'width': 16, 'height': 16,
                     # A box filter, so a pixel is the mean of its samples and
                     # nothing is spread between neighbours.
                     'rfilter': {'type': 'box'}},
            'sampler': {'type': 'independent', 'sample_count': SPP},
        },
        'emitter': {'type': 'constant', 'radiance': ENV_RADIANCE},
        # Scaled well past the frame at this fov and distance, so EVERY pixel
        # sees the surface and none sees the environment directly. A pixel
        # that saw the background would read L rather than a*L and the mean
        # would no longer have the closed form above.
        'plate': {
            'type': 'rectangle',
            'to_world': mi.ScalarTransform4f().scale([8.0, 8.0, 1.0]),
            'bsdf': {'type': 'diffuse',
                     'reflectance': {'type': 'rgb', 'value': ALBEDO}},
        },
    })

    params = mi.traverse(scene)
    key = 'plate.bsdf.reflectance.value'
    if key not in params:
        raise SystemExit('mitsuba_gate: %r is not a traversable parameter' % key)
    dr.enable_grad(params[key])
    params.update()

    image = mi.render(scene, params, spp=SPP)
    mean = dr.mean(mi.TensorXf(image), axis=None)
    # The FORWARD quantity, read before the backward pass consumes the graph.
    # Every channel is a*L here, so the mean over all three is a*L too and is
    # directly comparable to our film's mean over its own three.
    # `mean` is a 0-dimensional TensorXf, so it has no index to subscript --
    # its one element lives in the flat array behind it. Reading the value
    # does not detach it: the backward pass below still runs on this node.
    mean_pixel = float(mean.array[0])

    dr.backward(mean)
    grad = dr.grad(params[key])
    channels = [float(grad[i][0]) for i in range(3)]
    # Grey scene, so the three channels must agree. Asserted rather than
    # assumed, because if they did not the sum below would be meaningless.
    if max(channels) - min(channels) > 1e-6:
        raise SystemExit('mitsuba_gate: channels disagree %r -- the scene is not grey' % channels)

    # THE SUM, not one channel, and getting this wrong is what the first run
    # of this script did.
    #
    # The parameter is ONE grey albedo driving all three channels, so the
    # derivative with respect to it is the sum of the three partials. Reading
    # a single channel instead measured d(mean over 3 channels)/d(albedo_r),
    # which is a third of the answer because dr.mean divides by 16*16*3 while
    # only the R pixels depend on albedo_r. It came out 0.3335 against a
    # closed form of 1, and a factor that close to 1/3 is a statement about
    # which quantity was computed rather than about either renderer.
    #
    # That is exactly what the three-way design is for: the disagreement
    # localised to the definition of the quantity, and neither renderer was
    # implicated.
    return {'mean pixel': mean_pixel,
            'd(mean pixel)/d(albedo)': sum(channels)}


def run_leg1():
    try:
        measured = mitsuba_measure()
    except ImportError:
        print('mitsuba_gate: mitsuba not importable. Install it out of tree:')
        print('  python -m pip install --target build/mitsuba-pkgs mitsuba')
        print('  PYTHONPATH=build/mitsuba-pkgs python tests/diff/tools/mitsuba_gate.py')
        return None

    expected = closed_form()
    worst = 0.0
    for name in sorted(expected):
        rel = abs(measured[name] - expected[name]) / abs(expected[name])
        worst = max(worst, rel)
        print('  %-26s mitsuba %.9g   closed form %.9g   relative %.3g'
              % (name, measured[name], expected[name], rel))

    if worst > REL_TOL:
        print()
        print('LEG 1 FAILED: mitsuba disagrees with the closed form by %.3g, above the '
              'pre-registered %.3g at %d spp. The SCENE DESCRIPTION is wrong and nothing has '
              'been learned about our renderer yet.' % (worst, REL_TOL, SPP))
        print('Likeliest causes, in order: the plate does not fill the frame (some pixel '
              'sees the constant emitter directly and reads L instead of a*L); max_depth '
              'admits interreflection the closed form excludes; or the reconstruction '
              'filter is spreading energy between pixels.')
        return None

    print('LEG 1 OK: mitsuba and the closed form agree to %.3g, so the scene means what it says.'
          % worst)
    return measured


# ---------------------------------------------------------------------------
# Leg 2: our renderer, run rather than quoted
# ---------------------------------------------------------------------------
def find_probe():
    """The probe binary, wherever this platform's build put it."""
    candidates = [
        os.path.join(REPO_ROOT, 'build', 'Release', 'diff_gpu_probe.exe'),
        os.path.join(REPO_ROOT, 'build', 'Release', 'diff_gpu_probe'),
        os.path.join(REPO_ROOT, 'build', 'diff_gpu_probe.exe'),
        os.path.join(REPO_ROOT, 'build', 'diff_gpu_probe'),
    ]
    for path in candidates:
        if os.path.isfile(path):
            return path
    return None


def probe_measure():
    """Run diff_gpu_probe and parse check 72's MITSUBA-GATE line.

    The probe is run WHOLE rather than with a flag selecting check 72: it has
    no such flag, and adding one to make this script cheaper would let leg 2
    pass on a binary whose other 71 checks were failing.
    """
    probe = find_probe()
    if probe is None:
        print('mitsuba_gate: diff_gpu_probe not built. Build it first:')
        print('  cmake --build build --config Release --target diff_gpu_probe')
        return None

    print('running %s (this takes about 20 seconds and needs a Vulkan device with ray query '
          'and float atomics)' % os.path.relpath(probe, REPO_ROOT))
    completed = subprocess.run([probe], cwd=REPO_ROOT, capture_output=True, text=True)
    haystack = completed.stdout + completed.stderr
    match = re.search(r'MITSUBA-GATE\s+(.*)', haystack)
    if match is None:
        print('mitsuba_gate: the probe printed no MITSUBA-GATE line (exit %d). Check 72 either '
              'did not run or failed before reaching it; its own failure message will say '
              'which. Last lines of its output:' % completed.returncode)
        for line in haystack.strip().splitlines()[-6:]:
            print('  ' + line)
        return None

    fields = dict(re.findall(r'(\w+)=([0-9.eE+-]+)', match.group(1)))
    required = ('albedo', 'env', 'samples', 'meanFilm', 'seMeanFilm',
                'dMeanDAlbedo', 'seDMeanDAlbedo')
    missing = [k for k in required if k not in fields]
    if missing:
        print('mitsuba_gate: the MITSUBA-GATE line is missing %r. It reads: %s'
              % (missing, match.group(1)))
        return None

    # THE SCENE, AS THE BINARY ACTUALLY RAN IT. The source tie above reads the
    # constants the file declares; this reads what the executable printed. A
    # stale binary passes the first and fails this one, which is the whole
    # difference between checking a source file and checking a measurement.
    if float(fields['albedo']) != ALBEDO or float(fields['env']) != ENV_RADIANCE:
        print('mitsuba_gate: THE BINARY RAN A DIFFERENT SCENE than this script describes: it '
              'reports albedo %s and environment radiance %s against %.9g and %.9g here. The '
              'source constants tie above passed, so the executable is STALE -- rebuild '
              'diff_gpu_probe.' % (fields['albedo'], fields['env'], ALBEDO, ENV_RADIANCE))
        return None

    if completed.returncode != 0:
        print('mitsuba_gate: the probe printed its numbers but exited %d, so some check failed. '
              'Leg 2 is not run against a binary that is failing its own gates.'
              % completed.returncode)
        return None

    return {
        'samples': int(float(fields['samples'])),
        'mean pixel': (float(fields['meanFilm']), float(fields['seMeanFilm'])),
        'd(mean pixel)/d(albedo)': (float(fields['dMeanDAlbedo']),
                                    float(fields['seDMeanDAlbedo'])),
    }


def run_leg2(mitsuba):
    ours = probe_measure()
    if ours is None:
        return False

    expected = closed_form()
    print('our renderer, over %d primary paths:' % ours['samples'])
    ok = True
    for name in sorted(expected):
        measured, std_err = ours[name]
        # BOTH SIDES ARE MONTE CARLO, so the allowance carries both: our own
        # measured standard error at LEG2_SIGMAS, plus mitsuba's pre-registered
        # sampling allowance on its own value. Using only one side's would
        # charge this comparison for noise it did not produce.
        allowed = LEG2_SIGMAS * std_err + REL_TOL * abs(mitsuba[name])
        diff = abs(measured - mitsuba[name])
        verdict = 'ok' if diff <= allowed else 'FAILED'
        print('  %-26s ours %.9g +/- %.3g   mitsuba %.9g   |difference| %.3g against %.3g   %s'
              % (name, measured, std_err, mitsuba[name], diff, allowed, verdict))
        if diff > allowed:
            ok = False

    if not ok:
        print()
        print('LEG 2 FAILED: our renderer and Mitsuba disagree by more than both sampling '
              'errors allow, and leg 1 has already established that Mitsuba agrees with the '
              'closed form. So the disagreement is OURS.')
        print('This is an ABSOLUTE SCALE, which no other gate in this subsystem can see: checks '
              '33-34 compare our film against a reference integrator that shares its '
              'derivation, and check 37 compares our gradient against a finite difference of '
              'that same film. Divide ours by mitsuba above -- pi, 1/pi, 2, 1/2 and 4*pi each '
              'name a different suspect.')
        return False

    print()
    print('LEG 2 OK: all three agree. Our renderer, Mitsuba 3 and the closed form give the same '
          'forward radiance and the same derivative, to within both sides\' sampling error.')
    return True


def main():
    print('GATE 4: a Lambertian plate filling the frame under a constant environment,')
    print('the one scene whose answer is known exactly.')
    print()
    assert_constants_tie()
    print()
    print('LEG 1 -- mitsuba against the closed form, run first and alone:')
    mitsuba = run_leg1()
    if mitsuba is None:
        return 1
    print()
    print('LEG 2 -- our renderer against both:')
    return 0 if run_leg2(mitsuba) else 1


if __name__ == '__main__':
    sys.exit(main())
