// Stage 2 Task 2, check 51: the L2 loss kernel.
#include "probe/checks_loss.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace ohao::diff::probe {

bool checkLossL2(ohao::diff::GpuProbeContext& ctx) {
    // Small and NOT a multiple of the 64-wide local size, so the dispatch's
    // tail is exercised: 37 floats means the last workgroup has 27 threads
    // that must write nothing at all. A length that divided evenly would
    // leave the `i >= floatCount` guard untested.
    constexpr std::size_t kN = 37;
    constexpr double kRelTol = 1e-5;

    // --- ASSERTION 1: THE CLOSED FORM, on a case computable by hand.
    //
    // This is what stops the rest of the check being circular. Assertions 2
    // and 3 below compare two OUTPUTS OF THE SAME KERNEL against each other,
    // which cannot detect an error common to both; this one compares the
    // kernel's loss against a number derived on paper.
    //
    // film = 0, target = c  =>  L = (1/N) * SUM_i c^2 = c^2, for every N.
    {
        constexpr float kC = 0.25f;
        const std::vector<float> film(kN, 0.0f);
        const std::vector<float> target(kN, kC);
        std::vector<float> seed;
        double loss = 0.0;
        if (!ctx.runLossL2Probe(film, target, seed, loss)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 51 closed-form dispatch\n");
            return false;
        }
        const double expected = static_cast<double>(kC) * static_cast<double>(kC);
        if (!(std::fabs(loss - expected) <= kRelTol * expected)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 51 -- for film = 0 and target = %.9g the "
                         "loss is c^2 = %.12g for ANY N, and the kernel returned %.12g. This is "
                         "the one assertion here derived on paper rather than from another of "
                         "the kernel's own outputs, so it is what pins the SCALE: a loss that "
                         "averaged over pixels instead of floats would be three times this, and "
                         "nothing downstream would notice -- Gate 5 would absorb the factor "
                         "into the learning rate and still converge\n",
                         static_cast<double>(kC), expected, loss);
            return false;
        }
        // The gradient of that same case is 2(0 - c)/N, uniform.
        const double expectedSeed = 2.0 * (0.0 - static_cast<double>(kC)) / static_cast<double>(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            if (!(std::fabs(static_cast<double>(seed[i]) - expectedSeed) <=
                  kRelTol * std::fabs(expectedSeed))) {
                std::fprintf(stderr,
                             "[diff_gpu_probe] FAIL: check 51 -- seed[%zu] is %.12g, and for "
                             "film = 0, target = %.9g it must be 2(0 - c)/N = %.12g at every "
                             "element\n",
                             i, static_cast<double>(seed[i]), static_cast<double>(kC),
                             expectedSeed);
                return false;
            }
        }
    }

    // --- A NON-TRIVIAL film and target for the rest.
    std::vector<float> film(kN, 0.0f);
    std::vector<float> target(kN, 0.0f);
    for (std::size_t i = 0; i < kN; ++i) {
        film[i] = 0.3f + 0.05f * static_cast<float>(i % 7u);
        target[i] = 0.2f + 0.03f * static_cast<float>(i % 5u);
    }
    std::vector<float> seed;
    double baseLoss = 0.0;
    if (!ctx.runLossL2Probe(film, target, seed, baseLoss)) {
        std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 51 base dispatch\n");
        return false;
    }
    if (!(baseLoss > 0.0) || !std::isfinite(baseLoss)) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 51 -- the loss on a film that differs from "
                     "its target everywhere is %.9g, and must be finite and strictly positive "
                     "or the finite difference below is measuring nothing\n",
                     baseLoss);
        return false;
    }

    // --- ASSERTION 2: THE FINITE DIFFERENCE, AND IT IS EXACT.
    //
    // L is a QUADRATIC in the film, so a central difference has identically
    // zero truncation error -- the same exactness Stage 1 Task 6 found for
    // the albedo at one and two bounces, and for the same reason (a central
    // difference is exact through degree 2). So this is not an agreement
    // within a truncation bound: any disagreement beyond float roundoff is a
    // real error, and the step size does not need deriving.
    //
    // NO RENDERER IS INVOLVED. The film is a host array here, not something
    // traced, so a failure is unambiguously the loss kernel's -- which is the
    // whole reason this task's gate is an FD on the loss alone.
    // THE STEP IS LARGE, AND DELIBERATELY SO. Truncation is identically zero
    // at EVERY h (L is a quadratic), so the model imposes no upper bound on h
    // at all -- which leaves cancellation as the only error term, and it
    // falls as 1/h. That inverts the usual trade-off: where a general
    // objective balances truncation against roundoff, here the step should be
    // as large as the domain tolerates.
    //
    // Measured at 2^-7 the disagreement was 4.8e-5 relative, above the 1e-5
    // floor and NOT a kernel error: L is accumulated by float32 atomicAdd
    // over kN terms and then differenced, so the difference of two nearly
    // equal sums retains ~sqrt(kN)*ulp(L)/(2h) of each sum's own rounding. At
    // L ~ 2e-2 that is ~4.5e-9/(2h), which at 2^-7 is ~4.5e-5 of a difference
    // of ~1e-4 -- the observed value. 2^-2 is 32x larger and brings it to
    // ~1.4e-6, inside the floor, at no cost in truncation because there is
    // none to pay.
    constexpr float kStep = 0.25f;  // 2^-2, exact in binary
    double worstRel = 0.0;
    std::size_t worstIndex = 0;
    for (std::size_t k = 0; k < kN; ++k) {
        std::vector<float> plus = film;
        std::vector<float> minus = film;
        plus[k] += kStep;
        minus[k] -= kStep;
        std::vector<float> ignored;
        double lPlus = 0.0;
        double lMinus = 0.0;
        if (!ctx.runLossL2Probe(plus, target, ignored, lPlus) ||
            !ctx.runLossL2Probe(minus, target, ignored, lMinus)) {
            std::fprintf(stderr, "[diff_gpu_probe] FAIL: check 51 FD dispatch at element %zu\n",
                         k);
            return false;
        }
        const double hActual =
            0.5 * (static_cast<double>(plus[k]) - static_cast<double>(minus[k]));
        const double fd = (lPlus - lMinus) / (2.0 * hActual);
        const double analytic = static_cast<double>(seed[k]);
        // TWO TERMS, AND THE GATE TAKES THE LOOSER -- the same shape Task 6's
        // convergence gates settled on, for the same reason.
        //
        // The finite difference's error here is ABSOLUTE, not relative to the
        // gradient: it is the float32 rounding of an accumulation of
        // `kN` terms, divided by 2h, and it does not care how large dL/dI_k
        // happens to be. Gating it relative to the gradient therefore fails
        // wherever the gradient is SMALL -- which it was, at element 14,
        // whose gradient is 1/6 of the largest and whose relative error came
        // out at 1.3e-5 from an absolute error of 1.4e-8.
        //
        // The bound: a float32 sum of kN terms carries at most
        // kN * eps * |L| of rounding, and the difference of two such sums,
        // divided by 2h, carries that over h. kSafety covers the second sum
        // and the readback.
        constexpr double kFloat32Eps = 1.1920928955078125e-07;
        constexpr double kSafety = 4.0;
        const double absBound = kSafety * static_cast<double>(kN) * kFloat32Eps *
                                std::fabs(baseLoss) / (2.0 * hActual);
        const double tol = std::max(kRelTol * std::fabs(analytic), absBound);
        const double rel = std::fabs(fd - analytic) / tol;
        if (rel > worstRel) {
            worstRel = rel;
            worstIndex = k;
        }
        if (!(std::fabs(fd - analytic) <= tol)) {
            std::fprintf(stderr,
                         "[diff_gpu_probe] FAIL: check 51 -- THE SEED IS NOT dL/d(film) AT "
                         "ELEMENT %zu.\n"
                         "  (L(+h) - L(-h)) / 2h = %.12g\n"
                         "  the kernel's seed     = %.12g\n"
                         "  difference %.6g of what the accumulation floor allows\n"
                         "  L is a QUADRATIC in the film, so this central difference has "
                         "IDENTICALLY ZERO truncation error at every h. The only remaining "
                         "term is the float32 cancellation in differencing two accumulated "
                         "losses, which falls as 1/h and is why the step here is large rather "
                         "than small. No renderer is involved either: the film is a host "
                         "array, so this is the loss kernel's own derivative and nothing "
                         "else's.\n",
                         k, fd, analytic, rel);
            return false;
        }
    }

    // --- ASSERTION 3: THE TAIL WROTE NOTHING.
    //
    // The dispatch is ceil(37/64)*64 = 64 threads over 37 elements. The 27
    // beyond the end must write nothing -- and "nothing" is checkable here
    // only because the seed buffer is exactly kN long, so an out-of-range
    // write would be a buffer overrun the validation layer catches. What is
    // checked instead is the observable half: every element in range got a
    // value, and none is the 0.0f the buffer was created with unless the
    // gradient genuinely is zero there.
    std::size_t untouched = 0;
    for (std::size_t i = 0; i < kN; ++i) {
        if (seed[i] == 0.0f && film[i] != target[i]) ++untouched;
    }
    if (untouched != 0) {
        std::fprintf(stderr,
                     "[diff_gpu_probe] FAIL: check 51 -- %zu of %zu seed elements are exactly "
                     "0.0f while their film and target differ, so those threads did not run or "
                     "wrote nothing. The dispatch covers %zu threads for %zu elements; a guard "
                     "that rejected too many is indistinguishable from a gradient that happens "
                     "to be zero, except that here it cannot be zero\n",
                     untouched, kN, ((kN + 63u) / 64u) * 64u, kN);
        return false;
    }

    std::printf(
        "[diff_gpu_probe] OK: check 51 -- THE L2 LOSS KERNEL, and its gate is a finite "
        "difference ON THE LOSS ALONE with no renderer involved: the film is a host array, so a "
        "failure here is unambiguously this kernel's. THE DIFFERENCE IS EXACT, not merely "
        "close: L = (1/N) SUM (I-T)^2 is a QUADRATIC in the film and a central difference is "
        "exact through degree 2, so the truncation error is identically zero AT EVERY h -- the "
        "same exactness Task 6 found for the albedo at one and two bounces. That leaves float32 "
        "cancellation as the ONLY error term, and it falls as 1/h, so the step is chosen LARGE "
        "(2^-2) rather than small: the usual trade-off does not exist here, and at 2^-7 the "
        "cancellation alone put the comparison at 4.8e-5. All %zu elements agree, the worst at "
        "%.3g OF ITS ALLOWANCE (element %zu). THE ALLOWANCE HAS TWO TERMS and takes the looser: "
        "%.3g x |dL/dI|, or an absolute float32 accumulation floor. Two, because the "
        "difference's error is ABSOLUTE -- the rounding of a %zu-term sum over 2h -- and does "
        "not shrink with the gradient, so a purely relative gate fails wherever the gradient is "
        "small. It did, at element 14, where a 1.4e-8 absolute error read as 1.3e-5 relative. "
        "N IS THE FLOAT COUNT, not the pixel count, and that is pinned by a closed form derived "
        "on paper rather than from the kernel: film = 0 against target = c gives L = c^2 for "
        "any N, and a mean over pixels would return three times it -- a factor nothing "
        "downstream would notice, since Gate 5 would absorb it into the learning rate and still "
        "converge. The dispatch is deliberately %zu elements over %zu threads so the tail guard "
        "is exercised.\n",
        kN, worstRel, worstIndex, kRelTol, kN, kN, ((kN + 63u) / 64u) * 64u);
    return true;
}

}  // namespace ohao::diff::probe
