/* OHAO textbook — reading progress + TOC + reveal */
(() => {
  const progress = document.querySelector(".progress");
  const tocLinks = [...document.querySelectorAll(".toc a[href^='#']")];
  const chapters = tocLinks
    .map((a) => document.querySelector(a.getAttribute("href")))
    .filter(Boolean);

  function onScroll() {
    const doc = document.documentElement;
    const max = doc.scrollHeight - window.innerHeight;
    const p = max > 0 ? (window.scrollY / max) * 100 : 0;
    if (progress) progress.style.width = `${p}%`;

    let current = chapters[0];
    for (const el of chapters) {
      if (el.getBoundingClientRect().top <= 120) current = el;
    }
    tocLinks.forEach((a) => {
      a.classList.toggle("active", a.getAttribute("href") === `#${current?.id}`);
    });
  }

  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // Reveal on enter
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
      { rootMargin: "0px 0px -8% 0px", threshold: 0.08 }
    );
    reveals.forEach((el) => io.observe(el));
  } else {
    reveals.forEach((el) => el.classList.add("in"));
  }

  // Prefer video play when in view
  const hero = document.querySelector(".cover-media video");
  if (hero) {
    hero.play().catch(() => {});
  }
})();
