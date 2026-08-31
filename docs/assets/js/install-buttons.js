/*
 * Hides install buttons whose manifest is not on the site. The docs workflow
 * drops a manifest whenever the release it bundles has no matching binary, so
 * a board can appear on this page before any release carries a build for it,
 * and the beta channel is missing entirely until a beta is published.
 */
(() => {
  async function isPublished(url) {
    try {
      const response = await fetch(url, { method: "HEAD", cache: "no-store" });
      return response.ok;
    } catch {
      return false;
    }
  }

  function setup() {
    document.querySelectorAll("esp-web-install-button[manifest]").forEach(
      async (button) => {
        const published = await isPublished(button.getAttribute("manifest"));
        button.classList.add(published ? "install-available" : "install-missing");
      },
    );
  }

  // Instant navigation swaps the body without a page load, so the theme's
  // document observable is the only reliable re-entry point when it is active.
  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(setup);
  } else {
    document.addEventListener("DOMContentLoaded", setup);
  }
})();
