(function () {
  function boot() {
    const sections = window.ZithDocsConfig.sections;
    window.ZithDocsState = { sectionId: null, pageId: null };

    window.ZithDocsNav.renderSectionTabs(sections);
    window.ZithDocsNav.initMenuToggle();

    const router = window.ZithDocsRouterFactory.buildApi(sections);
    window.ZithDocsRouter = router;
    window.navigate = (pageId) => router.navigate(pageId);

    window.addEventListener('popstate', () => {
      const params = new URLSearchParams(window.location.search);
      const pageId = params.get('page') || 'intro';
      const page = router.pageMap[pageId];
      if (!page) return;
      router.openSection(page.sectionId, false);
      window.ZithDocsContent.load(page, true);
    });

    router.start();
  }

  document.addEventListener('DOMContentLoaded', boot);
})();
