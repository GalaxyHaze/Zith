document.addEventListener('DOMContentLoaded', function() {

    var reducedMotion = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    var typewriterTimers = {};

    function cancelTypewriter(element) {
        if (!element) return;
        var key = typeof element.id === 'string' && element.id ? element.id : element;
        if (typewriterTimers[key]) {
            typewriterTimers[key].forEach(clearTimeout);
            delete typewriterTimers[key];
        }
    }

    function splitHtmlTokens(html) {
        var tokens = [];
        var re = /<[^>]+>|[^<]+/g;
        var match;
        while ((match = re.exec(html)) !== null) {
            if (match[0].charAt(0) === '<') {
                tokens.push({ html: match[0], text: "" });
            } else {
                var plain = match[0]
                    .replace(/&amp;/g, "&")
                    .replace(/&lt;/g, "<")
                    .replace(/&gt;/g, ">")
                    .replace(/&quot;/g, '"')
                    .replace(/&#39;/g, "'");
                tokens.push({ html: match[0], text: plain });
            }
        }
        return tokens;
    }

    function htmlPrefixForText(html, textCount) {
        var out = "";
        var decoded = 0;
        var i = 0;
        var entities = [
            ["&amp;", "&"],
            ["&lt;", "<"],
            ["&gt;", ">"],
            ["&quot;", '"'],
            ["&#39;", "'"]
        ];

        while (i < html.length && decoded < textCount) {
            var matched = false;
            entities.forEach(function(entity) {
                if (!matched && html.indexOf(entity[0], i) === i) {
                    out += entity[0];
                    i += entity[0].length;
                    matched = true;
                }
            });
            if (!matched) {
                out += html.charAt(i);
                i += 1;
            }
            decoded += 1;
        }
        return out;
    }

    function typeHtml(element, fullHtml) {
        cancelTypewriter(element);
        element.innerHTML = "";
        element.classList.remove('is-typing');

        if (reducedMotion) {
            element.innerHTML = fullHtml;
            return;
        }

        var tokens = splitHtmlTokens(fullHtml);
        var tokenKey = typeof element.id === 'string' && element.id ? element.id : element;
        typewriterTimers[tokenKey] = [];
        var progress = 0;
        element.classList.add('is-typing');

        function render() {
            var totalChars = tokens.reduce(function(sum, token) {
                return sum + (token.text ? token.text.length : 0);
            }, 0);

            if (progress >= totalChars) {
                element.innerHTML = fullHtml;
                element.classList.remove('is-typing');
                delete typewriterTimers[tokenKey];
                return;
            }

            var html = "";
            var remaining = progress;
            var currentDelay = 18;

            tokens.forEach(function(token) {
                if (token.html.charAt(0) === '<') {
                    html += token.html;
                    return;
                }
                if (remaining <= 0) return;
                var count = Math.min(remaining, token.text.length);
                html += htmlPrefixForText(token.html, count);
                remaining -= count;
            });

            element.innerHTML = html;
            progress++;

            // Pause briefly at line breaks so multiline code reads naturally.
            if (tokens.some(function(token) {
                return token.text && token.html.indexOf("\n") !== -1;
            })) {
                currentDelay = 30;
            }

            typewriterTimers[tokenKey].push(setTimeout(render, currentDelay));
        }

        render();
    }

    // --- Code Spotlight Data ---
    var highlight = window.ZithHighlight ? window.ZithHighlight.highlight : function(text) {
        return String(text == null ? "" : text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    };

    var codeSnippets = {
        hello: {
            filename: 'main.zith',
            rawText: `from std/io/console
            
fn main(){
    println("Hello, Zith!");
}`,
            explanation: `A simple & expressive <strong>Hello World</strong> in Zith.`
        },
        nra: {
            filename: 'memory-nra.zith',
            rawText: `struct Buffer {
    data: optional<*u8>,
    len: usize,
}

fn processBuffer(buf: view Buffer){
    println("Buffer length: {buf.len}");
}
    
fn main(){
    let buf = Buffer { data: null, len: 1024 };
    processBuffer(&buf); // 'view' borrows safely without moving ownership
}`,
            explanation: `<strong>Node Resource Analysis (NRA)</strong> inspects resource graph propagation. Keywords like <code>lend</code>, <code>view</code>, and <code>sink</code> specify ownership transfer or read-only borrowing at compile time without borrow checker annotations.`
        },
        c_interop: {
            filename: 'c-interop.zith',
            rawText: `import "stdio.h"

fn main(){
    let msg = "A direct C call!";
    raw{
        printf(msg);
    }
}            `,
            explanation: `<strong>Zero headache C Interop</strong> allows importing C headers directly with <code>import "path"</code>, calling C libraries without wrapper boilerplate`
        },
        pattern: {
            filename: 'pattern-matching.zith',
            rawText: `enum Status{
    Ok = 200,
    Error = 404,    
    Pending = 202
}\n\nfn handleStatus(status: Status) {
    when (status){
        (Status.Ok) ~> println("Success: {code}"),
        (Status.Error) ~> println("Error: {err}"),
        (Status.Pending) ~> println("Processing...")   
    }
}`,
            explanation: `Exhaustive pattern matching with <code>when</code> expression inspects tagged unions and enums cleanly, ensuring all branch variants are handled at compile time.`
        }
    };

    var currentTab = 'hello';

    function setCodeTab(tabKey) {
        if (!codeSnippets[tabKey]) return;
        currentTab = tabKey;
        var snippet = codeSnippets[tabKey];

        document.getElementById('codeFilename').textContent = snippet.filename;
        typeHtml(document.getElementById('codeDisplay'), highlight(snippet.rawText));
        var explanation = document.getElementById('codeExplanation');
        typeHtml(explanation, snippet.explanation);
        explanation.classList.add('typewriter-code');

        document.querySelectorAll('#codeTabs .tab-btn').forEach(function(btn) {
            btn.classList.toggle('active', btn.dataset.tab === tabKey);
        });
    }

    document.querySelectorAll('#codeTabs .tab-btn').forEach(function(btn) {
        btn.addEventListener('click', function() {
            setCodeTab(this.dataset.tab);
        });
    });

    // Initialize first code snippet
    setCodeTab('hello');

    // Typewriter for static gray sub-titles on page load.
    document.querySelectorAll('.typewriter').forEach(function(el) {
        typeHtml(el, el.innerHTML);
    });

    // --- Copy Code Button ---
    var copyCodeBtn = document.getElementById('copyCodeBtn');
    if (copyCodeBtn) {
        copyCodeBtn.addEventListener('click', function() {
            var snippet = codeSnippets[currentTab];
            if (!snippet) return;
            navigator.clipboard.writeText(snippet.rawText).then(function() {
                var orig = copyCodeBtn.textContent;
                copyCodeBtn.textContent = 'Copied!';
                setTimeout(function() { copyCodeBtn.textContent = orig; }, 1500);
            }).catch(function() {});
        });
    }

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
        typeHtml(installCmd, installCmds[pmKey]);

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
            navigator.clipboard.writeText(cmd).then(function() {
                var orig = copyInstallBtn.textContent;
                copyInstallBtn.textContent = 'Copied!';
                setTimeout(function() { copyInstallBtn.textContent = orig; }, 1500);
            }).catch(function() {});
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
                grid.querySelectorAll('.typewriter').forEach(function(p) {
                    typeHtml(p, p.innerHTML);
                });
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
