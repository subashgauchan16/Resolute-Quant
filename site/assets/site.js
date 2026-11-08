/* Hero decoration: drifting gold dust, and a slight lean of the logo towards the pointer. */
(() => {
  if (matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  const hero = document.getElementById("hero");
  const stage = document.getElementById("stage");
  for (let i = 0; i < 16; i++) {
    const speck = document.createElement("i");
    speck.className = "dust";
    speck.style.left = `${Math.random() * 100}%`;
    speck.style.animationDuration = `${7 + Math.random() * 8}s`;
    speck.style.animationDelay = `${-Math.random() * 12}s`;
    hero.append(speck);
  }
  hero.addEventListener("pointermove", (event) => {
    stage.style.setProperty("--tilt-y", `${(event.clientX / innerWidth - 0.5) * 4}deg`);
    stage.style.setProperty("--tilt-x", `${(0.5 - event.clientY / innerHeight) * 4}deg`);
  });
})();

/* Roadmap: each candle is a tab. Selecting one opens its plan, and the arrow keys move between candles. */
(() => {
  const tabs = [...document.querySelectorAll(".candles [role=tab]")];
  const plans = document.getElementById("plans");
  if (!tabs.length || !plans) return;
  const narrow = matchMedia("(max-width: 760px)");
  const calm = matchMedia("(prefers-reduced-motion: reduce)");

  function select(tab, { focus = false, reveal = false } = {}) {
    tabs.forEach((other) => {
      const on = other === tab;
      other.setAttribute("aria-selected", String(on));
      other.tabIndex = on ? 0 : -1;
      document.getElementById(other.getAttribute("aria-controls")).classList.toggle("open", on);
    });
    plans.style.setProperty("--col", tabs.indexOf(tab) + 1);
    if (focus) tab.focus();
    // On a phone the plan sits below the whole list, so bring it into view.
    if (reveal && narrow.matches) {
      plans.scrollIntoView({ behavior: calm.matches ? "auto" : "smooth", block: "nearest" });
    }
  }

  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => select(tab, { reveal: true }));
    tab.addEventListener("keydown", (event) => {
      const last = tabs.length - 1;
      const next = { ArrowRight: index + 1, ArrowDown: index + 1, ArrowLeft: index - 1, ArrowUp: index - 1, Home: 0, End: last }[event.key];
      if (next === undefined) return;
      event.preventDefault();
      select(tabs[(next + tabs.length) % tabs.length], { focus: true });
    });
  });

  select(tabs.find((tab) => tab.getAttribute("aria-selected") === "true") || tabs[0]);
})();

/* The PDCA, gate and pipeline loops pause while their section is off screen. */
(() => {
  if (!("IntersectionObserver" in window)) return;
  const watch = new IntersectionObserver((entries) => {
    entries.forEach((entry) => entry.target.classList.toggle("rqm-idle", !entry.isIntersecting));
  });
  document.querySelectorAll(".rqm").forEach((section) => watch.observe(section));
})();
