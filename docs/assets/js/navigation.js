(function () {
  const state = {
    activeSectionId: null,
    activePageId: null
  };

  function renderSectionTabs(sections) {
    const tabs = document.getElementById('sectionTabs');
    if (!tabs) return;
    tabs.innerHTML = sections.map((section) => (
      `<button class="section-tab" data-section-id="${section.id}">${section.title}</button>`
    )).join('');

    tabs.querySelectorAll('.section-tab').forEach((button) => {
      button.addEventListener('click', () => {
        window.ZithDocsRouter.openSection(button.dataset.sectionId, true);
      });
    });
  }

  function renderSectionTree(section) {
    const panel = document.getElementById('sectionPanel');
    if (!panel) return;

    panel.innerHTML = `
      <div class="section-overview">
        <button class="overview-button" data-page-id="${section.overviewId}">Overview</button>
      </div>
      <div class="topic-tree">
        ${section.items.filter(item => item.id !== section.overviewId).map((item) => (
          `<button class="topic-leaf" data-page-id="${item.id}">${item.title}</button>`
        )).join('')}
      </div>
    `;

    panel.querySelector('.overview-button')?.addEventListener('click', (event) => {
      window.ZithDocsRouter.navigate(event.target.dataset.pageId);
    });

    panel.querySelectorAll('.topic-leaf').forEach((topic) => {
      topic.addEventListener('click', (event) => {
        window.ZithDocsRouter.navigate(event.target.dataset.pageId);
      });
    });
  }

  function updateSectionState(sectionId, pageId) {
    state.activeSectionId = sectionId;
    state.activePageId = pageId;

    document.querySelectorAll('.section-tab').forEach((tab) => {
      tab.classList.toggle('active', tab.dataset.sectionId === sectionId);
    });

    document.querySelectorAll('.overview-button, .topic-leaf').forEach((button) => {
      button.classList.toggle('active', button.dataset.pageId === pageId);
    });
  }

  function initMenuToggle() {
    const sidebar = document.getElementById('sidebar');
    const overlay = document.getElementById('sidebarOverlay');
    const toggle = document.getElementById('menuToggle');
    if (!sidebar || !overlay || !toggle) return;

    toggle.addEventListener('click', () => {
      sidebar.classList.toggle('open');
      overlay.classList.toggle('visible');
    });

    overlay.addEventListener('click', () => {
      sidebar.classList.remove('open');
      overlay.classList.remove('visible');
    });
  }

  function closeMenuOnMobile() {
    if (window.innerWidth > 900) return;
    document.getElementById('sidebar')?.classList.remove('open');
    document.getElementById('sidebarOverlay')?.classList.remove('visible');
  }

  window.ZithDocsNav = {
    renderSectionTabs,
    renderSectionTree,
    updateSectionState,
    initMenuToggle,
    closeMenuOnMobile
  };
})();
