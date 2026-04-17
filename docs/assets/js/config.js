(function () {
  function inferBasePath() {
    if (window.location.protocol === 'file:') return './';
    if (window.location.hostname === 'galaxyhaze.github.io') return '/Zith/docs/';

    const path = window.location.pathname;
    // If we're in a /docs/ directory, use relative paths from there
    if (path.includes('/docs/')) {
      const docsIndex = path.indexOf('/docs/');
      const docsDir = path.substring(0, docsIndex + 6);
      // Return relative path from docs directory
      return './';
    }
    return './';
  }

  function readNavData() {
    const template = document.getElementById('navData');
    if (!template) return [];
    const fragment = template.content;

    return Array.from(fragment.querySelectorAll('section')).map((section) => ({
      id: section.dataset.section,
      title: section.dataset.title,
      overviewId: section.dataset.overviewId,
      overviewPath: section.dataset.overviewPath,
      items: Array.from(section.querySelectorAll('item')).map((item) => ({
        id: item.dataset.id,
        title: item.dataset.title,
        path: item.dataset.path
      }))
    }));
  }

  window.ZithDocsConfig = {
    basePath: inferBasePath(),
    sections: readNavData(),
    sectionPathMap: {
      '/cli': 'cli',
      '/language': 'language',
      '/project': 'project',
      '/raw': 'raw',
      '/extras': 'extras'
    }
  };
})();
