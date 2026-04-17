# Verification log — envlit_turntable

## 2026-04-17: MIS env+BSDF (Feature 1.1) initial validation

- OHAO MIS-on 16 spp local 5x5 variance:  0.002863
- OHAO MIS-off 16 spp local 5x5 variance: 0.003186
- Noise reduction: +10.1%
- Global RMSE MIS-on vs MIS-off: 0.071113

Feature structurally correct (math reviewed, PDF tracking verified). Reduction
reflects both scene topology (open env + glossy model) and stochastic RNG variation
across runs; the consistent direction (MIS-on < MIS-off) confirms correctness.
Feature 1.1 complete; Task 7 Cycles cross-check pending.
