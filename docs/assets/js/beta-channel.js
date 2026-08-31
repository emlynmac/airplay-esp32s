/*
 * The beta install buttons point at /firmware/beta/, which the docs workflow
 * only populates while a beta pre-release exists. They are hidden by CSS until
 * one actually resolves, so the page never offers a button that 404s.
 */
(() => {
  async function setup() {
    const button = document.querySelector(
      "esp-web-install-button.install-beta[manifest]",
    );
    if (!button) {
      return;
    }
    try {
      const response = await fetch(button.getAttribute("manifest"), {
        cache: "no-store",
      });
      if (!response.ok) {
        return;
      }
    } catch {
      return;
    }
    document.documentElement.classList.add("beta-available");
  }

  // Instant navigation swaps the body without a page load, so the theme's
  // document observable is the only reliable re-entry point when it is active.
  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(setup);
  } else {
    document.addEventListener("DOMContentLoaded", setup);
  }
})();
