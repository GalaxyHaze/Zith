(function () {
  function pageLookup(sections) {
    const lookup = {};
    sections.forEach((section) => {
      section.items.forEach((item) => {
        lookup[item.id] = { ...item, sectionId: section.id };
      });
    });
    return lookup;
  }

  function detectRouteFromPath(sections) {
    const params = new URLSearchParams(window.location.search);
    const pageParam = params.get('page');
    if (pageParam) return pageParam;

    const pathname = window.location.pathname;
    const shortPath = pathname.replace(/\/index\.html$/, '');

    for (const [routePath, sectionId] of Object.entries(window.ZithDocsConfig.sectionPathMap)) {
      if (shortPath.endsWith(routePath)) {
        const section = sections.find((entry) => entry.id === sectionId);
        return section?.overviewId || 'intro';
      }
    }

    return 'intro';
  }

  function buildApi(sections) {
    const pageMap = pageLookup(sections);

    function openSection(sectionId, navigateToOverview) {
      const section = sections.find((entry) => entry.id === sectionId);
      if (!section) return;

      window.ZithDocsNav.renderSectionTree(section);
      window.ZithDocsNav.updateSectionState(section.id, window.ZithDocsState.pageId || section.overviewId);

      if (navigateToOverview) {
        navigate(section.overviewId);
      }
    }

    function navigate(pageId) {
      const page = pageMap[pageId];
      if (!page) return;
      window.ZithDocsContent.load(page, false);
    }

    function start() {
      const initialPage = detectRouteFromPath(sections);
      const initial = pageMap[initialPage] ? initialPage : 'intro';
      const sectionId = pageMap[initial]?.sectionId || sections[0]?.id;
      openSection(sectionId, false);
      window.ZithDocsContent.load(pageMap[initial], true);
    }

    return { openSection, navigate, start, pageMap };
  }

  window.ZithDocsRouterFactory = { buildApi };
})();
