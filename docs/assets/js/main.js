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
    'cli-flags': 'pages/cli/flags.html',
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
    // If running locally from file system
    if (window.location.protocol === 'file:') {
        return './';
    }

    // If running on GitHub Pages
    if (window.location.hostname === 'galaxyhaze.github.io') {
        return '/Zith-Lang/docs/'; // FIXED: Capital 'L' in Zith-Lang
    }

    // Default for local server or custom domain
    const path = window.location.pathname;
    if (path.includes('/docs/')) {
        return '/docs/';
    }

    return './';
}

const basePath = getBasePath();

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
                // This adds the entry to history (allows "Back" button to work URL-wise)
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

    // Find the item that matches this pageId
    document.querySelectorAll('.sidebar-item').forEach(item => {
        const onclickAttr = item.getAttribute('onclick');
        if (onclickAttr && onclickAttr.includes(`navigate('${pageId}')`)) {
            item.classList.add('active');
        }
    });
}

function animatePageIn() {
    document.querySelectorAll('.animate-in').forEach((el, index) => {
        el.style.animation = 'none';
        setTimeout(() => {
            el.style.animation = '';
        }, 10 + (index * 20));
    });
}

function copyCode(button) {
    const codeBlock = button.closest('.code-block');
    if (!codeBlock) return;

    const preElement = codeBlock.querySelector('pre');
    if (!preElement) return;

    const code = preElement.textContent;

    navigator.clipboard.writeText(code).then(() => {
        const originalText = button.textContent;
        button.textContent = 'copied!';
        setTimeout(() => {
            button.textContent = originalText;
        }, 2000);
    }).catch(err => {
        console.error('Failed to copy:', err);
    });
}

// --- History Management (The Fix) ---
window.onpopstate = function(event) {
    // This runs when the user clicks Back or Forward
    const params = new URLSearchParams(window.location.search);
    const pageId = params.get('page') || 'intro';

    // We manually call loadPage directly to avoid pushState (creating a double history entry)
    const relativePath = pageMap[pageId];
    if (relativePath) {
        const fullPath = basePath + relativePath;

        // We need to fetch and update content, but NOT push state again
        fetch(fullPath)
            .then(response => {
                if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
                return response.text();
            })
            .then(html => {
                const content = document.querySelector('.content');
                if (content) {
                    content.innerHTML = html;
                    updateActiveNav(pageId);
                    animatePageIn();
                }
            })
            .catch(error => {
                console.error('Error loading page on back/forward:', error);
            });
    }
};

// Initialize page load
document.addEventListener('DOMContentLoaded', () => {
    const params = new URLSearchParams(window.location.search);
    const page = params.get('page') || 'intro';

    // We use loadPage directly here to set the initial state without adding an extra history entry
    const relativePath = pageMap[page];
    if (relativePath) {
        loadPage(basePath + relativePath, page);
    } else {
        navigate('intro'); // Fallback
    }
});