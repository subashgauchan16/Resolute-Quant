/* Decoration only: drifting gold dust in the hero, and a slight lean of the logo towards the pointer. */
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
