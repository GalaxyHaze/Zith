(function () {
  function inferBasePath() {
    if (window.location.protocol === 'file:') return './';
    if (window.location.hostname === 'galaxyhaze.github.io') return '/Zith/docs/';

    const path = window.location.pathname;
    if (path.includes('/docs/')) return path.substring(0, path.indexOf('/docs/') + 6);
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
