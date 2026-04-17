// Main entry point for Zith Docs
// This file bootstraps the documentation application

(function() {
  // Initialize mobile menu
  function initMobileMenu() {
    const sidebar = document.getElementById('sidebar');
    const overlay = document.getElementById('sidebarOverlay');
    const toggle = document.getElementById('menuToggle');

    if (!toggle || !sidebar || !overlay) return;

    function openMenu() {
      sidebar.classList.add('open');
      overlay.classList.add('visible');
    }

    function closeMenu() {
      sidebar.classList.remove('open');
      overlay.classList.remove('visible');
    }

    toggle.addEventListener('click', () => {
      if (sidebar.classList.contains('open')) {
        closeMenu();
      } else {
        openMenu();
      }
    });

    overlay.addEventListener('click', closeMenu);
  }

  // Boot the application
  function boot() {
    initMobileMenu();
    
    // Check if we're on the docs page (docs.html)
    if (document.getElementById('sectionTabs') && document.getElementById('sectionPanel')) {
      // Build the router API from config data
      const router = window.ZithDocsRouterFactory.buildApi(window.ZithDocsConfig.sections);
      window.ZithDocsRouter = router;
      
      // Initialize navigation UI
      window.ZithDocsNav.renderSectionTabs(window.ZithDocsConfig.sections);
      window.ZithDocsNav.initMenuToggle();
      
      // Set up state tracking
      window.ZithDocsState = {
        pageId: null,
        sectionId: null
      };
      
      // Start the router
      router.start();
    }
  }

  // Wait for DOM to be ready
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
