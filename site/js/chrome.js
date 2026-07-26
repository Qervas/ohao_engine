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

  // ── Floating GitHub FAB (all pages) ────────────────────────────────
  function setupGithubFab() {
    if (document.querySelector(".github-fab")) return;
    const a = document.createElement("a");
    a.className = "github-fab";
    a.href = "https://github.com/Qervas/ohao_engine";
    a.target = "_blank";
    a.rel = "noopener noreferrer";
    a.title = "Source on GitHub";
    a.setAttribute("aria-label", "Open OHAO Engine source code on GitHub");
    // Official Octicons mark path (MIT); inline so no extra request.
    a.innerHTML =
      '<svg class="github-fab-icon" viewBox="0 0 16 16" width="22" height="22" aria-hidden="true">' +
      '<path fill="currentColor" d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59' +
      ".4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23" +
      "-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87" +
      " 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15" +
      "-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27s1.36.09 2 .27" +
      "c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07" +
      "-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46" +
      '.55.38A8.01 8.01 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>' +
      '<span class="github-fab-label">Source</span>';
    document.body.appendChild(a);
  }
  setupGithubFab();

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
