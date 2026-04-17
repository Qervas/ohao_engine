#ifndef OHAO_MIS_GLSL
#define OHAO_MIS_GLSL

// Balance heuristic for two-strategy MIS.
// Returns the weight for strategy A given its PDF and the PDF of the other strategy
// at the same sample.
float misBalanceHeuristic(float pdfA, float pdfB) {
    return pdfA / max(pdfA + pdfB, 1e-6);
}

// Power heuristic (beta=2) — slightly better in practice but more expensive.
float misPowerHeuristic(float pdfA, float pdfB) {
    float a = pdfA * pdfA;
    float b = pdfB * pdfB;
    return a / max(a + b, 1e-6);
}

#endif
