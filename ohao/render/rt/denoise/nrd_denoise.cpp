// Sub-plan 4.A T1 placeholder — real implementation lands in T2.
// This file exists so the glob-based ohao_renderer target has a translation
// unit to compile for the NRD integration. When OHAO_NRD_ENABLED is undefined
// (i.e. -DOHAO_NRD=OFF at configure time), this file short-circuits to an
// empty translation unit so the build has no NRD dependency.

#ifdef OHAO_NRD_ENABLED

// Intentionally empty. T2 replaces this with the NRD wrapper
// (instance init, pass dispatch, descriptor layout, shader integration).

#endif // OHAO_NRD_ENABLED
