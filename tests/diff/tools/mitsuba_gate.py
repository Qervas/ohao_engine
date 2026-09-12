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

        d(mean pixel) / d(albedo) = L

    exactly, with no integration error of any kind to argue about.

Three numbers that must agree: Mitsuba's autodiff, the closed form, and (once
the conventions are reconciled) ours. If Mitsuba disagrees with the closed
form, the scene description is wrong and nothing about our renderer has been
learned yet -- which is why this script runs that leg FIRST and on its own.

    usage:  PYTHONPATH=build/mitsuba-pkgs python tests/diff/tools/mitsuba_gate.py

INSTALLED OUT OF TREE, deliberately. mitsuba pulls drjit and can move shared
dependencies, so it lives in build/mitsuba-pkgs via `pip install --target` and
is reached through PYTHONPATH rather than installed into whatever environment
happens to be active.
"""
import sys

# THE PRE-REGISTERED CRITERION, fixed before the first comparison.
ALBEDO = 0.5
ENV_RADIANCE = 1.0
SPP = 4096
# Monte Carlo, so the tolerance is a sampling allowance rather than a
# precision one: 4096 spp over a 16x16 film is 1e6 samples, and the estimator
# for this scene has low variance because every path terminates on the
# constant environment after one bounce.
REL_TOL = 5e-3


def closed_form_d_mean_d_albedo() -> float:
    """An ideal Lambertian under uniform illumination reflects a*L, so the
    derivative of every pixel with respect to the grey albedo is exactly L.

    WITH RESPECT TO THE GREY ALBEDO, which is the whole of the subtlety: the
    parameter is one value driving three channels, so the comparison must sum
    the three partials. See the note in mitsuba_d_mean_d_albedo.
    """
    return ENV_RADIANCE


def mitsuba_d_mean_d_albedo() -> float:
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
    return sum(channels)


def main() -> int:
    try:
        measured = mitsuba_d_mean_d_albedo()
    except ImportError:
        print('mitsuba_gate: mitsuba not importable. Install it out of tree:')
        print('  python -m pip install --target build/mitsuba-pkgs mitsuba')
        print('  PYTHONPATH=build/mitsuba-pkgs python tests/diff/tools/mitsuba_gate.py')
        return 2

    expected = closed_form_d_mean_d_albedo()
    rel = abs(measured - expected) / abs(expected)
    print('d(mean pixel)/d(albedo)')
    print('  mitsuba     %.9g' % measured)
    print('  closed form %.9g' % expected)
    print('  relative    %.3g (pre-registered tolerance %.3g, %d spp)' % (rel, REL_TOL, SPP))

    if rel > REL_TOL:
        print()
        print('LEG 1 FAILED: mitsuba disagrees with the closed form, so the SCENE '
              'DESCRIPTION is wrong and nothing has been learned about our renderer yet.')
        print('Likeliest causes, in order: the plate does not fill the frame (some pixel '
              'sees the constant emitter directly and reads L instead of a*L); max_depth '
              'admits interreflection the closed form excludes; or the reconstruction '
              'filter is spreading energy between pixels.')
        return 1

    print()
    print('LEG 1 OK: mitsuba and the closed form agree, so the scene means what it says.')
    print('LEG 2, still owed: the same quantity from diff_gpu_probe. That needs the '
          'camera, the environment and the albedo convention reconciled between the two '
          'renderers, which is the part worth budgeting time for -- see the item 5 notes '
          'in the remaining-road plan.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
