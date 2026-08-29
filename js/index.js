document.addEventListener('DOMContentLoaded', function() {

    // --- Code Spotlight Data ---
    var codeSnippets = {
        hello: {
            filename: 'main.zt',
            rawText: `pub fn main() {\n    println("Hello, Zith!");\n}`,
            codeHtml: `<span class="kw">pub fn</span> <span class="fn">main</span>() {\n    <span class="fn">println</span>(<span class="st">"Hello, Zith!"</span>);\n}`,
            explanation: `A concise <strong>Hello World</strong> in Zith. Public entry point with clean string formatting and standard I/O.`
        },
        nra: {
            filename: 'memory_nra.zt',
            rawText: `struct Buffer {\n    data: raw<u8>,\n    len: usize,\n}\n\nfn process_buffer(view buf: &Buffer) {\n    println("Buffer length: {buf.len}");\n}\n\npub fn main() {\n    let mut buf = Buffer { data: null, len: 1024 };\n    process_buffer(&buf); // 'view' borrows safely without moving ownership\n}`,
            codeHtml: `<span class="kw">struct</span> <span class="ty">Buffer</span> {\n    data: <span class="ty">raw</span>&lt;<span class="ty">u8</span>&gt;,\n    len: <span class="ty">usize</span>,\n}\n\n<span class="kw">fn</span> <span class="fn">process_buffer</span>(view buf: &amp;<span class="ty">Buffer</span>) {\n    <span class="fn">println</span>(<span class="st">"Buffer length: {buf.len}"</span>);\n}\n\n<span class="kw">pub fn</span> <span class="fn">main</span>() {\n    <span class="kw">let mut</span> buf = <span class="ty">Buffer</span> { data: <span class="kw">null</span>, len: <span class="num">1024</span> };\n    <span class="fn">process_buffer</span>(&amp;buf); <span class="cm">// 'view' borrows safely without moving ownership</span>\n}`,
            explanation: `<strong>Node Resource Analysis (NRA)</strong> inspects resource graph propagation. Keywords like <code>lend</code>, <code>view</code>, and <code>sink</code> specify ownership transfer or read-only borrowing at compile time without borrow checker annotations.`
        },
        c_interop: {
            filename: 'c_abi.zt',
            rawText: `@foreign c {\n    fn printf(fmt: raw<c_char>, ...) : c_int;\n    fn malloc(size: usize) : raw<void>;\n    fn free(ptr: raw<void>);\n}\n\npub fn main() {\n    let msg = "Direct zero-cost C ABI calls from Zith!\\n";\n    unsafe {\n        printf("%s".ptr, msg.ptr);\n    }\n}`,
            codeHtml: `<span class="kw">@foreign</span> <span class="st">c</span> {\n    <span class="kw">fn</span> <span class="fn">printf</span>(fmt: <span class="ty">raw</span>&lt;<span class="ty">c_char</span>&gt;, ...) : <span class="ty">c_int</span>;\n    <span class="kw">fn</span> <span class="fn">malloc</span>(size: <span class="ty">usize</span>) : <span class="ty">raw</span>&lt;<span class="ty">void</span>&gt;;\n    <span class="kw">fn</span> <span class="fn">free</span>(ptr: <span class="ty">raw</span>&lt;<span class="ty">void</span>&gt;);\n}\n\n<span class="kw">pub fn</span> <span class="fn">main</span>() {\n    <span class="kw">let</span> msg = <span class="st">"Direct zero-cost C ABI calls from Zith!\\n"</span>;\n    <span class="kw">unsafe</span> {\n        <span class="fn">printf</span>(<span class="st">"%s"</span>.ptr, msg.ptr);\n    }\n}`,
            explanation: `<strong>Zero-Overhead C Interop</strong> allows declaring foreign C function signatures directly with <code>@foreign c</code> block syntax, calling C libraries without wrapper overhead.`
        },
        pattern: {
            filename: 'matching.zt',
            rawText: `enum Status {\n    Ok(u32),\n    Error(String),\n    Pending,\n}\n\nfn handle_status(status: Status) {\n    when status {\n        Status.Ok(code) => println("Success: {code}"),\n        Status.Error(err) => println("Error: {err}"),\n        Status.Pending => println("Processing..."),\n    }\n}`,
            codeHtml: `<span class="kw">enum</span> <span class="ty">Status</span> {\n    Ok(<span class="ty">u32</span>),\n    Error(<span class="ty">String</span>),\n    Pending,\n}\n\n<span class="kw">fn</span> <span class="fn">handle_status</span>(status: <span class="ty">Status</span>) {\n    <span class="kw">when</span> status {\n        <span class="ty">Status</span>.Ok(code) =&gt; <span class="fn">println</span>(<span class="st">"Success: {code}"</span>),\n        <span class="ty">Status</span>.Error(err) =&gt; <span class="fn">println</span>(<span class="st">"Error: {err}"</span>),\n        <span class="ty">Status</span>.Pending =&gt; <span class="fn">println</span>(<span class="st">"Processing..."</span>),\n    }\n}`,
            explanation: `Exhaustive pattern matching with <code>when</code> expression inspects tagged unions and enums cleanly, ensuring all branch variants are handled at compile time.`
        }
    };

    var currentTab = 'hello';

    function setCodeTab(tabKey) {
        if (!codeSnippets[tabKey]) return;
        currentTab = tabKey;
        var snippet = codeSnippets[tabKey];

        document.getElementById('codeFilename').textContent = snippet.filename;
        document.getElementById('codeDisplay').innerHTML = snippet.codeHtml;
        document.getElementById('codeExplanation').innerHTML = snippet.explanation;

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
        document.getElementById('installCmd').textContent = installCmds[pmKey];

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
                grid.innerHTML = topEntries.map(function(e) {
                    return '<div class="news-card">' +
                           '  <span class="news-date">' + e.date + '</span>' +
                           '  <h3><a href="./about/posts/' + e.file + '">' + e.title + '</a></h3>' +
                           '</div>';
                }).join('');
            })
            .catch(function() {
                // Keep static fallback rendered in HTML
            });
    }

    loadNews();

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
