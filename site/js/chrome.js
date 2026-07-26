/* Shared textbook chrome: TOC active state, mobile drawer, progress, reveal, KaTeX */
(() => {
  const root = document.body.dataset.root || ".";
  const pageId = document.body.dataset.page || "";

  // Mark active TOC link
  document.querySelectorAll(".toc a[data-page]").forEach((a) => {
    if (a.dataset.page === pageId) a.classList.add("active");
  });

  // ── Mobile TOC drawer (phone: no fixed sidebar) ────────────────────
  // Desktop keeps the left nav. Below 920px the same .toc slides in as a drawer
  // when the sticky top bar is tapped — so readers can skip / select chapters.
  function setupMobileToc() {
    let toc = document.querySelector("nav.toc");
    if (!toc) return;

    let bar = document.querySelector(".mobile-toc");
    if (!bar) {
      bar = document.createElement("div");
      bar.className = "mobile-toc";
      const book = document.querySelector("main.book");
      if (book) book.parentNode.insertBefore(bar, book);
      else document.body.insertBefore(bar, document.body.firstChild);
    }

    // Promote plain text bar into an accessible control
    if (!bar.querySelector(".mobile-toc-btn")) {
      const prev = (bar.textContent || "OHAO · Textbook").trim() || "OHAO · Textbook";
      bar.textContent = "";
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "mobile-toc-btn";
      btn.setAttribute("aria-expanded", "false");
      btn.setAttribute("aria-controls", "site-toc");
      btn.innerHTML =
        '<span class="mobile-toc-icon" aria-hidden="true">' +
        "<span></span><span></span><span></span></span>" +
        '<span class="mobile-toc-label"></span>' +
        '<span class="mobile-toc-hint">Menu</span>';
      btn.querySelector(".mobile-toc-label").textContent = prev;
      bar.appendChild(btn);
    }

    toc.id = toc.id || "site-toc";
    const btn = bar.querySelector(".mobile-toc-btn");
    if (btn) btn.setAttribute("aria-controls", toc.id);

    if (!toc.querySelector(".toc-close")) {
      const close = document.createElement("button");
      close.type = "button";
      close.className = "toc-close";
      close.setAttribute("aria-label", "Close chapter menu");
      close.textContent = "Close";
      toc.insertBefore(close, toc.firstChild);
    }

    let backdrop = document.querySelector(".toc-backdrop");
    if (!backdrop) {
      backdrop = document.createElement("button");
      backdrop.type = "button";
      backdrop.className = "toc-backdrop";
      backdrop.setAttribute("aria-label", "Close chapter menu");
      document.body.appendChild(backdrop);
    }

    function isMobileNav() {
      return window.matchMedia("(max-width: 920px)").matches;
    }

    function openToc() {
      if (!isMobileNav()) return;
      toc.classList.add("is-open");
      document.body.classList.add("toc-open");
      if (btn) btn.setAttribute("aria-expanded", "true");
      const closeBtn = toc.querySelector(".toc-close");
      if (closeBtn) closeBtn.focus({ preventScroll: true });
    }

    function closeToc() {
      toc.classList.remove("is-open");
      document.body.classList.remove("toc-open");
      if (btn) btn.setAttribute("aria-expanded", "false");
    }

    function toggleToc() {
      if (toc.classList.contains("is-open")) closeToc();
      else openToc();
    }

    if (btn && !btn.dataset.tocBound) {
      btn.dataset.tocBound = "1";
      btn.addEventListener("click", (e) => {
        e.preventDefault();
        toggleToc();
      });
    }

    const closeBtn = toc.querySelector(".toc-close");
    if (closeBtn && !closeBtn.dataset.tocBound) {
      closeBtn.dataset.tocBound = "1";
      closeBtn.addEventListener("click", (e) => {
        e.preventDefault();
        closeToc();
        if (btn) btn.focus({ preventScroll: true });
      });
    }

    if (!backdrop.dataset.tocBound) {
      backdrop.dataset.tocBound = "1";
      backdrop.addEventListener("click", closeToc);
    }

    // Close after choosing a chapter (navigation may be same-page hash)
    if (!toc.dataset.tocLinkBound) {
      toc.dataset.tocLinkBound = "1";
      toc.addEventListener("click", (e) => {
        const a = e.target.closest("a[href]");
        if (!a || !isMobileNav()) return;
        closeToc();
      });
    }

    if (!window.__ohaoTocEsc) {
      window.__ohaoTocEsc = true;
      document.addEventListener("keydown", (e) => {
        if (e.key === "Escape" && document.body.classList.contains("toc-open")) {
          closeToc();
          if (btn) btn.focus({ preventScroll: true });
        }
      });
    }

    // Leaving mobile layout should not leave body locked
    if (!window.__ohaoTocMq) {
      window.__ohaoTocMq = true;
      const mq = window.matchMedia("(max-width: 920px)");
      const onChange = () => {
        if (!mq.matches) closeToc();
      };
      if (mq.addEventListener) mq.addEventListener("change", onChange);
      else if (mq.addListener) mq.addListener(onChange);
    }

    // Show current chapter name in the bar when available
    const active = toc.querySelector("a.active");
    const label = bar.querySelector(".mobile-toc-label");
    if (active && label) {
      const t = (active.textContent || "").replace(/^[·▸\s]+/, "").trim();
      if (t) label.textContent = t;
    }
  }

  // TOC is injected by nav-tree.js (same defer order, runs first). Retry once
  // if a page loads chrome before the mount finishes.
  setupMobileToc();
  if (!document.querySelector("nav.toc")) {
    setTimeout(setupMobileToc, 0);
    setTimeout(setupMobileToc, 50);
  }

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
