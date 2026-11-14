/* The PDCA, gate and pipeline loops pause while their panel is off screen. */
(() => {
  if (!("IntersectionObserver" in window)) return;
  const watch = new IntersectionObserver((entries) => {
    entries.forEach((entry) => entry.target.classList.toggle("rqm-idle", !entry.isIntersecting));
  });
  document.querySelectorAll(".rqm").forEach((section) => watch.observe(section));
})();
