// Navigation & Routing
const pageMap = {
    'intro': 'pages/intro.html',
    'installation': 'pages/installation.html',
    'quickstart': 'pages/quickstart.html',
    'cli-overview': 'pages/cli/overview.html',
    'cmd-check': 'pages/cli/check.html',
    'cmd-compile': 'pages/cli/compile.html',
    'cmd-build': 'pages/cli/build.html',
    'cmd-execute': 'pages/cli/execute.html',
    'cmd-run': 'pages/cli/run.html',
    'cmd-new': 'pages/cli/new.html',
    'cmd-clean': 'pages/cli/clean.html',
    'cmd-repl': 'pages/cli/repl.html',
    'cmd-fmt': 'pages/cli/fmt.html',
    'cmd-docs': 'pages/cli/docs.html',
    'cli-flags': 'pages/cli/flags.html',
    'advanced-overview': 'pages/advanced/overview.html',
    'adv-how-to-use': 'pages/advanced/how-to-use.html',
    'adv-traits': 'pages/advanced/traits.html',
    'adv-generics': 'pages/advanced/generics.html',
    'adv-metaprogramming': 'pages/advanced/metaprogramming.html',
    'adv-macros': 'pages/advanced/macros.html',
    'adv-data-structures': 'pages/advanced/data-structures.html',
    'project-toml': 'pages/project/toml.html',
    'lang-overview': 'pages/language/overview.html',
    'lang-syntax': 'pages/language/syntax.html',
    'lang-vars': 'pages/language/variables.html',
    'lang-types': 'pages/language/types.html',
    'lang-control': 'pages/language/control-flow.html',
    'lang-functions': 'pages/language/functions.html',
    'lang-memory': 'pages/language/memory.html',
    'lang-errors': 'pages/language/errors.html',
    'lang-modules': 'pages/language/modules.html',
    'lang-generics': 'pages/language/generics.html',
};

// Get the base path depending on environment
function getBasePath() {
    if (window.location.protocol === 'file:') {
        return './';
    }

    if (window.location.hostname === 'galaxyhaze.github.io') {
        return '/Zith/docs/';
    }

    const path = window.location.pathname;
    if (path.includes('/docs/')) {
        return '/docs/';
    }

    return './';
}

const basePath = getBasePath();

// --- Mobile Menu Toggle ---
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

    // Close menu when a sidebar item is clicked on mobile
    document.querySelectorAll('.sidebar-item').forEach(item => {
        item.addEventListener('click', () => {
            if (window.innerWidth <= 768) {
                closeMenu();
            }
        });
    });
}

function navigate(pageId) {
    const relativePath = pageMap[pageId];
    if (relativePath) {
        const fullPath = basePath + relativePath;
        loadPage(fullPath, pageId);
        updateActiveNav(pageId);
    } else {
        console.error('Page not found:', pageId);
    }
}

function loadPage(path, pageId) {
    fetch(path)
        .then(response => {
            if (!response.ok) {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            return response.text();
        })
        .then(html => {
            const content = document.querySelector('.content');
            if (content) {
                content.innerHTML = html;
                window.history.pushState({ page: pageId }, '', `?page=${pageId}`);
                animatePageIn();
            }
        })
        .catch(error => {
            console.error('Error loading page:', error);
            const content = document.querySelector('.content');
            if (content) {
                content.innerHTML = `
          <div class="breadcrumb">
            <span onclick="navigate('intro')">docs</span>
          </div>
          <div class="page-eyebrow animate-in">Error</div>
          <h1 class="animate-in">Page Not Found</h1>
          <p class="lead animate-in">Sorry, we couldn't load that page.</p>
          <div class="callout callout-warning">
            <span class="callout-icon">⚠</span>
            <div class="callout-body">
              <p>Path attempted: <code>${path}</code></p>
              <p>Error: ${error.message}</p>
              <p>Try <span onclick="navigate('intro')" style="cursor:pointer; color: #4d9fff;">returning home</span></p>
            </div>
          </div>
        `;
            }
        });
}

function updateActiveNav(pageId) {
    document.querySelectorAll('.sidebar-item').forEach(item => {
        item.classList.remove('active');
    });

    document.querySelectorAll('.sidebar-item').forEach(item => {
        const onclickAttr = item.getAttribute('onclick');
        if (onclickAttr && onclickAttr.includes(`navigate('${pageId}')`)) {
            item.classList.add('active');
        }
    });

    router.start();
  }

  document.addEventListener('DOMContentLoaded', boot);
})();
