(function () {
  function slugify(input) {
    return input.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/(^-|-$)/g, '');
  }

  function withJumpLinks(container) {
    const targets = Array.from(container.querySelectorAll('h2, h3'));
    if (!targets.length) return;

    const links = targets.map((heading, index) => {
      if (!heading.id) heading.id = `${slugify(heading.textContent || 'section')}-${index}`;
      return `<li><a href="#${heading.id}">${heading.textContent}</a></li>`;
    }).join('');

    const jump = document.createElement('nav');
    jump.className = 'jump-links animate-in';
    jump.innerHTML = `<h3>Jump to key topics</h3><ul>${links}</ul>`;
    container.prepend(jump);
  }

  function resetScroll() {
    window.scrollTo({ top: 0, behavior: 'auto' });
    document.getElementById('mainContent')?.scrollTo({ top: 0, behavior: 'auto' });
    document.getElementById('content')?.scrollTo({ top: 0, behavior: 'auto' });
  }

  function load(page, replaceState) {
    const path = window.ZithDocsConfig.basePath + page.path;
    fetch(path)
      .then((response) => {
        if (!response.ok) throw new Error(`Failed to fetch ${page.path}`);
        return response.text();
      })
      .then((html) => {
        const content = document.getElementById('content');
        if (!content) return;
        content.innerHTML = html;
        withJumpLinks(content);

        window.ZithDocsState.pageId = page.id;
        window.ZithDocsState.sectionId = page.sectionId;

        const queryUrl = `?page=${page.id}`;
        if (replaceState) {
          history.replaceState({ page: page.id }, '', queryUrl);
        } else {
          history.pushState({ page: page.id }, '', queryUrl);
        }

        window.ZithDocsNav.updateSectionState(page.sectionId, page.id);
        window.ZithDocsNav.closeMenuOnMobile();
        resetScroll();
      })
      .catch(() => {
        const content = document.getElementById('content');
        if (!content) return;
        content.innerHTML = '<h1>Page not found</h1><p>The requested documentation page could not be loaded.</p>';
      });
  }

  function copyCode(button) {
    const block = button.closest('.code-block');
    const pre = block?.querySelector('pre');
    if (!pre) return;
    navigator.clipboard.writeText(pre.textContent || '').then(() => {
      const initial = button.textContent;
      button.textContent = 'copied!';
      setTimeout(() => { button.textContent = initial; }, 1800);
    });
  }

  window.copyCode = copyCode;
  window.ZithDocsContent = { load };
})();
