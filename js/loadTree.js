const sidebar = document.getElementById("sidebar");
const content = document.getElementById("content");
const filebarPath = document.getElementById("filebarPath");
const defaultPage = "./getting-started/D-introduction.html";
const treeVersion = 2;

let treeModel = [];
let flatPages = [];
let pageMeta = new Map();
let aliasMap = new Map();

function sectionDescription(items, trail) {
    const section = trail[0] || items.title;
    const descriptions = {
        "Getting Started": "Install Zith, write your first program, and understand why the language is designed this way.",
        "Language Guide": "Practical guide to Zith syntax, types, memory, generics, concurrency, and systems programming.",
        "Language Reference": "Canonical Zith language reference, specification chapters, and implementation status.",
        "CLI Reference": "Reference for zithc commands: build, run, check, format, create, and project tooling.",
        "Project": "Project overview and current direction for the Zith programming language.",
        "FAQ": "Frequently asked questions about Zith stability, scope, and relation to other languages.",
        "Community": "Community resources for reporting issues and contributing to Zith documentation.",
    };
    return descriptions[section] || "Official documentation for the Zith programming language.";
}

(function loadMenu() {
    const cached = sessionStorage.getItem("tree_json");
    if (cached) {
        try {
            const menu = JSON.parse(cached);
            if (menu.version === treeVersion) {
                initialiseNavigation(menu);
                return;
            }
        } catch (_) {}
    }

    fetch("../../json/tree.json")
        .then(response => {
            if (!response.ok) throw new Error("Failed to load navigation");
            return response.json();
        })
        .then(menu => {
            try {
                sessionStorage.setItem("tree_json", JSON.stringify(menu));
            } catch (_) {}
            initialiseNavigation(menu);
        })
        .catch(() => {
            sidebar.innerHTML = "<p style='color:#ff6d1f;padding:8px'>Failed to load navigation</p>";
        });
})();

function initialiseNavigation(menuData) {
    const generatedTree = !Array.isArray(menuData) && Array.isArray(menuData.navigation);
    treeModel = generatedTree ? menuData.navigation : menuData;
    flatPages = [];
    pageMeta = new Map();
    aliasMap = new Map(Object.entries(generatedTree ? menuData.aliases || {} : {}));

    buildPageIndex(treeModel);
    renderMenu(treeModel, sidebar);

    const page = getPageFromURL() || defaultPage;
    loadPage(page, getAnchorFromURL(), { replaceHistory: !getPageFromURL() });
}

function renderMenu(items, parent) {
    const ul = document.createElement("ul");

    items.forEach(item => {
        const li = document.createElement("li");
        const a = document.createElement("a");
        a.textContent = item.title;
        a.href = item.link || "#";
        a.dataset.link = item.link || "#";

        if (item.title === "Home") {
            a.dataset.native = true;
        }

        li.appendChild(a);

        if (item.children) {
            renderMenu(item.children, li);
        }

        ul.appendChild(li);
    });

    parent.appendChild(ul);
}

function buildPageIndex(items, trail = []) {
    items.forEach(item => {
        const nextTrail = item.title === "Home" ? trail : [...trail, item.title];

        if (item.link && item.link !== "../home.html" && !pageMeta.has(item.link)) {
            flatPages.push(item.link);
            pageMeta.set(item.link, {
                title: item.title,
                trail: nextTrail,
                description: sectionDescription(item, nextTrail)
            });
        }

        if (item.children) {
            buildPageIndex(item.children, nextTrail);
        }
    });
}

function updateFilebar(path) {
    const meta = pageMeta.get(path);
    const filename = path.split("/").pop() || "_";
    filebarPath.textContent = meta ? meta.trail.join(" / ") : filename;
}

function resolveAlias(path) {
    return aliasMap.get(path) || path;
}

function setActiveLink(path) {
    const links = sidebar.querySelectorAll("a[data-link]");
    links.forEach(link => {
        link.classList.toggle("is-active", link.dataset.link === path);
    });
}

function getPagerLinks(path) {
    const currentIndex = flatPages.indexOf(path);
    if (currentIndex === -1) {
        return { previous: null, next: null };
    }

    return {
        previous: currentIndex > 0 ? flatPages[currentIndex - 1] : null,
        next: currentIndex < flatPages.length - 1 ? flatPages[currentIndex + 1] : null
    };
}

function createPagerLink(label, path, direction) {
    if (!path) return null;

    const link = document.createElement("a");
    const meta = pageMeta.get(path);
    const title = meta ? meta.title : path.split("/").pop();

    link.href = path;
    link.className = "doc-nav-link";
    link.innerHTML = `<span class="doc-nav-label">${label}</span><strong>${title}</strong>`;

    if (direction === "next") {
        link.classList.add("doc-nav-link-next");
    }

    return link;
}

function injectPageNavigation(path) {
    const meta = pageMeta.get(path);
    if (!meta) return;

    const existing = content.querySelector(".doc-shell");
    if (!existing) {
        const shell = document.createElement("div");
        shell.className = "doc-shell";

        while (content.firstChild) {
            shell.appendChild(content.firstChild);
        }

        content.appendChild(shell);
    }

    const shell = content.querySelector(".doc-shell");
    const existingHeader = shell.querySelector(".doc-page-header");
    const existingNav = shell.querySelector(".doc-page-nav");

    if (existingHeader) existingHeader.remove();
    if (existingNav) existingNav.remove();

    const header = document.createElement("div");
    header.className = "doc-page-header";
    header.innerHTML = `
        <p class="doc-kicker">Documentation Flow</p>
        <p class="doc-breadcrumb">${meta.trail.join(" / ")}</p>
    `;
    shell.prepend(header);

    const { previous, next } = getPagerLinks(path);
    if (!previous && !next) return;

    const nav = document.createElement("nav");
    nav.className = "doc-page-nav";
    nav.setAttribute("aria-label", "Documentation pagination");

    const previousLink = createPagerLink("Previous", previous, "previous");
    const nextLink = createPagerLink("Next", next, "next");

    if (previousLink) nav.appendChild(previousLink);
    if (nextLink) nav.appendChild(nextLink);

    shell.appendChild(nav);
}

let initialLoad = true;

function loadPage(path, anchor = null, opts = {}) {
    const canonicalPath = resolveAlias(path);
    fetch(canonicalPath)
        .then(res => {
            if (!res.ok) throw new Error("Page not found");
            return res.text();
        })
        .then(html => {
            const wasInitialLoad = initialLoad;
            content.innerHTML = html;

            updateFilebar(canonicalPath);
            setActiveLink(canonicalPath);
            injectPageNavigation(canonicalPath);

            if (window.Prism) {
                Prism.highlightAllUnder(content);
            }

            const filename = canonicalPath.split("/").pop() || "_";
            const titleMatch = html.match(/<h[12][^>]*>(.*?)<\/h[12]>/i);
            const pageTitle = titleMatch
                ? titleMatch[1].replace(/<[^>]+>/g, "").trim()
                : filename;
            const fullTitle = pageTitle + " — Zith Documentation";
            const meta = pageMeta.get(canonicalPath);
            const description = meta && meta.description ? meta.description : "Official documentation for the Zith programming language.";
            const pageURL = "https://zith-lang.org/html/documentation/D-home.html?page=" + encodeURIComponent(path.replace("./", ""));

            document.title = fullTitle;

            const descriptionMeta = document.querySelector('meta[name="description"]');
            const ogTitle = document.querySelector('meta[property="og:title"]');
            const ogDescription = document.querySelector('meta[property="og:description"]');
            const ogURL = document.querySelector('meta[property="og:url"]');
            const twitterTitle = document.querySelector('meta[name="twitter:title"]');
            const twitterDescription = document.querySelector('meta[name="twitter:description"]');
            const twitterURL = document.querySelector('meta[name="twitter:url"]');
            const canonical = document.querySelector('link[rel="canonical"]');

            if (descriptionMeta) descriptionMeta.setAttribute("content", description);
            if (ogTitle) ogTitle.setAttribute("content", fullTitle);
            if (ogDescription) ogDescription.setAttribute("content", description);
            if (ogURL) ogURL.setAttribute("content", pageURL);
            if (twitterTitle) twitterTitle.setAttribute("content", fullTitle);
            if (twitterDescription) twitterDescription.setAttribute("content", description);
            if (twitterURL) twitterURL.setAttribute("content", pageURL);
            if (canonical) canonical.setAttribute("href", pageURL);

            if (initialLoad) {
                initialLoad = false;
            }
            const historyURL = "?page=" + path.replace("./", "") +
                (anchor ? "#" + encodeURIComponent(anchor) : "");
            if (opts.replaceHistory) {
                history.replaceState(
                    { path: path, anchor: anchor },
                    "", historyURL
                );
            } else if (!opts.fromPop && !wasInitialLoad) {
                history.pushState(
                    { path: path, anchor: anchor },
                    "", historyURL
                );
            }

            requestAnimationFrame(() => {
                if (anchor) {
                    requestAnimationFrame(() => {
                        const target = content.querySelector(`#${anchor}`);
                        if (!target) return;

                        const container = document.querySelector(".bottom");
                        const offsetTop = target.offsetTop;

                        container.scrollTo({
                            top: offsetTop,
                            behavior: "smooth"
                        });
                    });
                }
            });
        })
        .catch(() => {
            content.innerHTML = "<p style='color:#ff6d1f'>Failed to load page content.</p>";
        });
}

function getPageFromURL() {
    const params = new URLSearchParams(window.location.search);
    const page = params.get("page");
    if (page) {
        return "./" + page.replace(/^\.\//, "");
    }
    return null;
}

function getAnchorFromURL() {
    return window.location.hash ? decodeURIComponent(window.location.hash.slice(1)) : null;
}

document.addEventListener("click", e => {
    const link = e.target.closest("a");
    if (!link) return;

    const href = link.getAttribute("href");
    if (!href || href === "#") return;
    if (href.startsWith("http")) return;
    if (link.dataset.native === "true") return;

    e.preventDefault();

    const [file, anchor] = href.split("#");
    loadPage(file, anchor || null);
});

window.addEventListener("popstate", e => {
    if (e.state && e.state.path) {
        loadPage(e.state.path, e.state.anchor || null, { fromPop: true });
    } else {
        const page = getPageFromURL();
        if (page) {
            loadPage(page, getAnchorFromURL(), { fromPop: true });
        }
    }
});

{
    // Initial page load is triggered once the navigation tree is ready.
}
