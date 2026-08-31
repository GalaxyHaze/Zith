document.addEventListener('DOMContentLoaded', function() {

    function copyPlainText(text, button) {
        function showCopied() {
            var original = button.textContent;
            button.textContent = 'Copied!';
            setTimeout(function() { button.textContent = original; }, 1500);
        }

        function fallback() {
            var textarea = document.createElement('textarea');
            textarea.value = text;
            textarea.setAttribute('readonly', '');
            textarea.style.position = 'fixed';
            textarea.style.opacity = '0';
            document.body.appendChild(textarea);
            textarea.select();
            var copied = false;
            try {
                copied = document.execCommand('copy');
            } catch (err) {
                copied = false;
            }
            document.body.removeChild(textarea);
            if (copied) showCopied();
        }

        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(showCopied).catch(fallback);
        } else {
            fallback();
        }
    }

    // Initialize the reusable code spotlight component.
    var codeSpotlight = document.querySelector('[data-code-spotlight]');
    if (codeSpotlight) window.ZithCodeSpotlight.init(codeSpotlight);

    // Start subtitle animations as they enter the viewport.
    window.ZithTypewriter.observe();

    // --- Installation Commands ---
    var installCmds = {
        brew: 'brew tap galaxyhaze/zithc && brew install zithc',
        scoop: 'scoop bucket add zithc https://github.com/GalaxyHaze/Zith.git && scoop install zithc',
        curl: 'curl -fsSL https://raw.githubusercontent.com/GalaxyHaze/Zith/main/scripts/install.sh | sh',
        ps1: 'Invoke-RestMethod https://raw.githubusercontent.com/GalaxyHaze/Zith/main/scripts/install.ps1 | Invoke-Expression'
    };

    var currentInstallPm = 'brew';

    function setInstallPm(pmKey) {
        if (!installCmds[pmKey]) return;
        currentInstallPm = pmKey;
        var installCmd = document.getElementById('installCmd');
        installCmd.classList.add('typewriter-install');
        ZithTypewriter.animate(installCmd, installCmds[pmKey], { animate: true, perCharMs: 16 });

        document.querySelectorAll('#installTabs .install-tab').forEach(function(tab) {
            tab.classList.toggle('active', tab.dataset.pm === pmKey);
        });
    }

    document.querySelectorAll('#installTabs .install-tab').forEach(function(tab) {
        tab.addEventListener('click', function() {
            setInstallPm(this.dataset.pm);
        });
    });

    var copyInstallBtn = document.getElementById('copyInstallBtn');
    if (copyInstallBtn) {
        copyInstallBtn.addEventListener('click', function() {
            var cmd = installCmds[currentInstallPm];
            if (!cmd) return;
            copyPlainText(cmd, copyInstallBtn);
        });
    }

    // --- News / Dev Log Loader ---
    function loadNews() {
        var grid = document.getElementById('newsGrid');
        if (!grid) return;

        fetch('./about/posts/index.json')
            .then(function(r) { return r.json(); })
            .then(function(entries) {
                if (!Array.isArray(entries) || entries.length === 0) return;
                var topEntries = entries.slice(0, 3);
                var descriptions = {
                    'Operators, and Where Ownership Belongs': 'Operator semantics, move and borrow boundaries, and where the ownership proof belongs in the pipeline.',
                    'Macros Land in the Zith Pipeline': 'Compile-time macro expansion lands alongside C ABI interop improvements in zithc.',
                    'Structs and the Modern Pipeline': 'Memory allocation, field alignment, and Node Resource Analysis graph propagation across the compiler.'
                };
                grid.innerHTML = topEntries.map(function(e) {
                    var text = descriptions[e.title] || 'Latest entry from the Zith development diary.';
                    return '<article class="news-card">' +
                           '  <span class="news-date">' + e.date + '</span>' +
                           '  <h3><a href="./about/posts/' + e.file + '">' + e.title + '</a></h3>' +
                           '  <p class="typewriter">' + text + '</p>' +
                           '</article>';
                }).join('');
                window.ZithTypewriter.observe();
            })
            .catch(function() {
            });
    }

    loadNews();

    // --- Dev Log Post Reader ---
    var newsGrid = document.getElementById('newsGrid');
    var postModal = document.getElementById('postModal');
    var postModalBody = document.getElementById('postModalBody');
    var closePostBtn = document.getElementById('closePostBtn');

    function openPostModal(url) {
        if (!postModal || !postModalBody) return;
        fetch(url)
            .then(function(r) { return r.text(); })
            .then(function(html) {
                var match = html.match(/<body[^>]*>([\s\S]*)<\/body>/i);
                var body = match ? match[1] : html;
                body = body.replace(/<script[\s\S]*?<\/script>/gi, '');
                postModalBody.innerHTML = body;
                postModal.classList.remove('hidden');
            })
            .catch(function() {
                postModalBody.innerHTML = '<p class="error">Failed to load this entry.</p>';
                postModal.classList.remove('hidden');
            });
    }

    function closePostModal() {
        if (postModal) postModal.classList.add('hidden');
    }

    if (newsGrid) {
        newsGrid.addEventListener('click', function(e) {
            var link = e.target.closest('a');
            if (!link || !link.href || link.href.indexOf('/about/posts/') === -1) return;
            e.preventDefault();
            openPostModal(link.getAttribute('href'));
        });
    }

    if (closePostBtn) closePostBtn.addEventListener('click', closePostModal);

    if (postModal) {
        postModal.addEventListener('click', function(e) {
            if (e.target === postModal) closePostModal();
        });
    }

    // --- CRT Settings Modal ---
    var settingsModal = document.getElementById('settingsModal');
    var openSettingsBtn = document.getElementById('openSettingsBtn');
    var footerSettingsBtn = document.getElementById('footerSettingsBtn');
    var closeSettingsBtn = document.getElementById('closeSettingsBtn');

    function openModal() {
        if (settingsModal) settingsModal.classList.remove('hidden');
    }

    function closeModal() {
        if (settingsModal) settingsModal.classList.add('hidden');
    }

    if (openSettingsBtn) openSettingsBtn.addEventListener('click', openModal);
    if (footerSettingsBtn) footerSettingsBtn.addEventListener('click', openModal);
    if (closeSettingsBtn) closeSettingsBtn.addEventListener('click', closeModal);

    if (settingsModal) {
        settingsModal.addEventListener('click', function(e) {
            if (e.target === settingsModal) closeModal();
        });
    }

    // --- Mobile Menu Toggle ---
    var mobileMenuToggle = document.getElementById('mobileMenuToggle');
    var navLinks = document.getElementById('navLinks');

    if (mobileMenuToggle && navLinks) {
        mobileMenuToggle.addEventListener('click', function() {
            navLinks.classList.toggle('open');
        });
    }

});
