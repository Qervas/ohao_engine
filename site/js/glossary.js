/**
 * Auto-wrap glossary terms in article content with hover tooltips.
 * Skips code, pre, SVG, existing terms, math, and nav/chrome.
 */
(() => {
  const G = window.OHAO_GLOSSARY;
  if (!G) return;

  // Longest keys first so "DLSS-RR" wins over "DLSS", "G-buffer" over shorter bits
  const terms = Object.keys(G).sort((a, b) => b.length - a.length);

  // Escape regex special chars in keys
  const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

  // Word-ish boundary: avoid matching inside identifiers like myNEEBuffer when possible.
  // Allow start/end and non-alphanumeric edges. Terms may include - 
  const pattern = new RegExp(
    `(?<![A-Za-z0-9_])(${terms.map(esc).join("|")})(?![A-Za-z0-9_])`,
    "g"
  );

  const SKIP = new Set([
    "SCRIPT",
    "STYLE",
    "CODE",
    "PRE",
    "KBD",
    "TEXTAREA",
    "SVG",
    "MATH",
    "ANNOTATION",
  ]);

  function shouldSkip(node) {
    // HTML inline SVG uses lowercase tagName ("svg", "text", "tspan") — never wrap
    // those text nodes or diagrams break (HTML <span> inside SVG is invalid).
    if (node.parentElement?.closest?.("svg")) return true;

    let el = node.parentElement;
    while (el) {
      const tag = (el.tagName || "").toUpperCase();
      if (SKIP.has(tag)) return true;
      // SVG namespace / foreignObject edge cases
      if (tag === "SVG" || tag === "TEXT" || tag === "TSPAN" || tag === "FOREIGNOBJECT")
        return true;
      if (el.classList?.contains("term")) return true;
      if (el.classList?.contains("toc")) return true;
      if (el.classList?.contains("mobile-toc")) return true;
      if (el.classList?.contains("listing")) return true;
      if (el.classList?.contains("algo")) return true;
      if (el.classList?.contains("file-map")) return true;
      if (el.classList?.contains("katex")) return true;
      if (el.classList?.contains("svg-frame")) return true;
      if (el.classList?.contains("plate-frame")) return true;
      // Never touch math — wrapping GGX/PDF/NEE inside \( ... \) or \[ ... \] breaks KaTeX
      if (el.classList?.contains("math-block")) return true;
      if (el.classList?.contains("math-inline")) return true;
      if (el.classList?.contains("katex-display")) return true;
      if (el.classList?.contains("dual-pane") && el.querySelector("pre") && el.contains(node) && node.parentElement?.closest("pre"))
        return true;
      if (el.getAttribute?.("data-no-glossary") != null) return true;
      el = el.parentElement;
    }
    // Also skip raw text that looks like a TeX display/inline delimiter run
    const t = node.nodeValue || "";
    if (t.includes("\\[") || t.includes("\\(") || t.includes("\\begin{")) return true;
    return false;
  }

  function wrapTextNode(textNode) {
    const text = textNode.nodeValue;
    if (!text || !pattern.test(text)) {
      pattern.lastIndex = 0;
      return;
    }
    pattern.lastIndex = 0;

    const frag = document.createDocumentFragment();
    let last = 0;
    let m;
    while ((m = pattern.exec(text)) !== null) {
      const key = m[1];
      const entry = G[key] || G[key.toUpperCase()];
      if (!entry) continue;
      if (m.index > last) {
        frag.appendChild(document.createTextNode(text.slice(last, m.index)));
      }
      const span = document.createElement("span");
      span.className = "term";
      span.tabIndex = 0;
      span.setAttribute("data-term", key);
      span.setAttribute(
        "aria-label",
        `${key}: ${entry.expand}. ${entry.def}`
      );
      span.textContent = key;

      const tip = document.createElement("span");
      tip.className = "term-tip";
      tip.setAttribute("role", "tooltip");
      tip.innerHTML = `<span class="term-tip-key">${escapeHtml(key)}</span>` +
        (entry.expand
          ? `<span class="term-tip-expand">${escapeHtml(entry.expand)}</span>`
          : "") +
        `<span class="term-tip-def">${escapeHtml(entry.def)}</span>`;
      span.appendChild(tip);
      frag.appendChild(span);
      last = m.index + key.length;
    }
    if (last === 0) return;
    if (last < text.length) {
      frag.appendChild(document.createTextNode(text.slice(last)));
    }
    textNode.parentNode.replaceChild(frag, textNode);
  }

  function escapeHtml(s) {
    return s
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function walk(root) {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode(node) {
        if (!node.nodeValue || !node.nodeValue.trim()) {
          return NodeFilter.FILTER_REJECT;
        }
        if (shouldSkip(node)) return NodeFilter.FILTER_REJECT;
        return NodeFilter.FILTER_ACCEPT;
      },
    });
    const nodes = [];
    let n;
    while ((n = walker.nextNode())) nodes.push(n);
    nodes.forEach(wrapTextNode);
  }

  function positionTips() {
    // Flip tooltip if near viewport edge
    document.querySelectorAll(".term:hover .term-tip, .term:focus .term-tip").forEach((tip) => {
      const r = tip.getBoundingClientRect();
      tip.classList.toggle("tip-left", r.right > window.innerWidth - 12);
      tip.classList.toggle("tip-up", r.bottom > window.innerHeight - 12);
    });
  }

  function init() {
    const roots = document.querySelectorAll(
      "main.book, article.page, .page, .prose, .aside, .decision, .nee-step, .stage, .thesis-box, .compare, .jargon, .why-callout, .plate-cap, .on-this-page, table.data-table, .contract"
    );
    if (roots.length === 0) {
      walk(document.body);
    } else {
      roots.forEach(walk);
    }

    document.addEventListener("mouseover", (e) => {
      if (e.target.closest?.(".term")) positionTips();
    });
    document.addEventListener("focusin", (e) => {
      if (e.target.classList?.contains("term")) positionTips();
    });
  }

  // Run after deferred KaTeX scripts (DOMContentLoaded) so math is already
  // .katex nodes (skipped). Math-block text is also hard-skipped above.
  function bootGlossary() {
    setTimeout(init, 0);
    setTimeout(init, 600); // catch late content; skips .katex / .math-block
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bootGlossary);
  } else {
    bootGlossary();
  }
})();
