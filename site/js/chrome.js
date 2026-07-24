/* Shared textbook chrome: TOC active state, progress, reveal, KaTeX */
(() => {
  const root = document.body.dataset.root || ".";
  const pageId = document.body.dataset.page || "";

  // Mark active TOC link
  document.querySelectorAll(".toc a[data-page]").forEach((a) => {
    if (a.dataset.page === pageId) a.classList.add("active");
  });

  // Progress bar
  const progress = document.querySelector(".progress");
  function onScroll() {
    const doc = document.documentElement;
    const max = doc.scrollHeight - window.innerHeight;
    const p = max > 0 ? (window.scrollY / max) * 100 : 0;
    if (progress) progress.style.width = `${p}%`;
  }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // Reveal
  const reveals = document.querySelectorAll(".reveal");
  if ("IntersectionObserver" in window) {
    const io = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (e.isIntersecting) {
            e.target.classList.add("in");
            io.unobserve(e.target);
          }
        }
      },
      { rootMargin: "0px 0px -6% 0px", threshold: 0.06 }
    );
    reveals.forEach((el) => io.observe(el));
  } else {
    reveals.forEach((el) => el.classList.add("in"));
  }

  // Hero video
  const hero = document.querySelector(".cover-media video");
  if (hero) hero.play().catch(() => {});

  // ── KaTeX ──────────────────────────────────────────────────────────
  // Robust: wait until auto-render is present, render display + inline,
  // then re-render math-block nodes that still show raw TeX (failed parse).
  const KATEX_OPTS = {
    delimiters: [
      { left: "$$", right: "$$", display: true },
      { left: "\\[", right: "\\]", display: true },
      { left: "$", right: "$", display: false },
      { left: "\\(", right: "\\)", display: false },
    ],
    throwOnError: false,
    strict: "ignore",
    trust: false,
  };

  function renderMathBlocksManually() {
    if (typeof katex === "undefined" || typeof katex.render !== "function") return;
    document.querySelectorAll(".math-block").forEach((el) => {
      // Already rendered by auto-render
      if (el.querySelector(".katex")) return;
      let tex = el.textContent || "";
      // Strip \[ \] or $$ wrappers if present
      tex = tex.replace(/^\s*\\\[\s*/, "").replace(/\s*\\\]\s*$/, "");
      tex = tex.replace(/^\s*\$\$\s*/, "").replace(/\s*\$\$\s*$/, "");
      tex = tex.trim();
      if (!tex) return;
      try {
        katex.render(tex, el, {
          displayMode: true,
          throwOnError: false,
          strict: "ignore",
        });
      } catch (e) {
        console.warn("[OHAO] KaTeX math-block failed:", tex.slice(0, 80), e);
      }
    });
  }

  function tryKatex() {
    if (typeof renderMathInElement !== "function") return false;
    try {
      renderMathInElement(document.body, KATEX_OPTS);
    } catch (e) {
      console.warn("[OHAO] renderMathInElement failed", e);
    }
    // Fallback for any display blocks auto-render skipped
    renderMathBlocksManually();
    return true;
  }

  function whenKatexReady(cb, attempts) {
    attempts = attempts === undefined ? 40 : attempts;
    if (typeof renderMathInElement === "function" || typeof katex !== "undefined") {
      cb();
      return;
    }
    if (attempts <= 0) {
      console.warn("[OHAO] KaTeX did not load");
      return;
    }
    setTimeout(() => whenKatexReady(cb, attempts - 1), 50);
  }

  function bootMath() {
    whenKatexReady(() => {
      // After glossary may have run; math-blocks are skipped by glossary now
      tryKatex();
      // One more pass after fonts/layout
      setTimeout(tryKatex, 200);
      setTimeout(tryKatex, 800);
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootMath);
  } else {
    bootMath();
  }
  window.addEventListener("load", () => setTimeout(tryKatex, 0));
})();
