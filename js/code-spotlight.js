/* Reusable Code Spotlight component.
   Markup:
   <div class="spotlight-grid" data-code-spotlight="json/code-spotlight.json"></div>
   Both the tabs column and code viewer are rendered from the JSON file. */
(function () {
    "use strict";

    function escapeHtml(text) {
        return String(text == null ? "" : text)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;");
    }

    function highlight(text) {
        return window.ZithHighlight ? window.ZithHighlight.highlight(text) : escapeHtml(text);
    }

    function copyText(text, button) {
        var fallback = function () {
            var textarea = document.createElement("textarea");
            textarea.value = text;
            textarea.setAttribute("readonly", "");
            textarea.style.position = "fixed";
            textarea.style.opacity = "0";
            document.body.appendChild(textarea);
            textarea.select();
            var copied = false;
            try {
                copied = document.execCommand("copy");
            } catch (error) {
                copied = false;
            }
            document.body.removeChild(textarea);
            if (copied) flashCopied(button);
        };

        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(function () {
                flashCopied(button);
            }).catch(fallback);
        } else {
            fallback();
        }
    }

    function flashCopied(button) {
        var original = button.textContent;
        button.textContent = "Copied!";
        setTimeout(function () { button.textContent = original; }, 1500);
    }

    function renderTabs(container, entries) {
        var tabs = document.createElement("div");
        tabs.className = "spotlight-tabs";
        entries.forEach(function (entry, index) {
            var button = document.createElement("button");
            button.type = "button";
            button.className = "tab-btn" + (index === 0 ? " active" : "");
            button.dataset.key = entry.key;
            button.textContent = entry.label;
            button.addEventListener("click", function () {
                selectSpotlight(container, entry.key);
            });
            tabs.appendChild(button);
        });
        container.appendChild(tabs);
    }

    function renderViewer(container, entry) {
        var viewer = document.createElement("div");
        viewer.className = "code-viewer";
        viewer.innerHTML =
            '<div class="code-window-bar">' +
            '  <div class="window-dots"><span class="dot red"></span><span class="dot amber"></span><span class="dot green"></span></div>' +
            '  <span class="window-filename"></span>' +
            '  <button class="copy-btn" type="button">Copy Code</button>' +
            "</div>" +
            '<div class="code-block-container"><pre><code></code></pre></div>' +
            '<div class="code-explanation"></div>';
        viewer.querySelector(".copy-btn").addEventListener("click", function () {
            var data = container.__codeSpotlight;
            if (data && data.currentEntry) copyText(data.currentEntry.code, this);
        });
        container.appendChild(viewer);
        renderEntry(container, entry);
    }

    function selectSpotlight(container, key) {
        var data = container.__codeSpotlight;
        if (!data || !data.entriesByKey[key]) return;
        data.selectedKey = key;
        renderEntry(container, data.entriesByKey[key]);
        updateActiveTab(container);
    }

    function updateActiveTab(container) {
        var data = container.__codeSpotlight;
        if (!data || !data.selectedKey) return;
        var selected = data.entriesByKey[data.selectedKey];
        container.querySelectorAll(".tab-btn").forEach(function (button) {
            button.classList.toggle("active", button.dataset.key === data.selectedKey || button.textContent === selected.label);
        });
    }

    function renderEntry(container, entry) {
        var viewer = container.querySelector(".code-viewer");
        if (!viewer) return;
        if (container.__codeSpotlight) container.__codeSpotlight.currentEntry = entry;
        viewer.querySelector(".window-filename").textContent = entry.file;
        var code = viewer.querySelector("code");
        var explanation = viewer.querySelector(".code-explanation");
        ZithTypewriter.animate(code, highlight(entry.code), { animate: true, perCharMs: 16 });
        ZithTypewriter.animate(explanation, entry.explanation, { animate: true, perCharMs: 14 });
    }

    function init(container) {
        var dataUrl = container.getAttribute("data-code-spotlight");
        if (!dataUrl) return;

        fetch(dataUrl)
            .then(function (response) { return response.ok ? response.json() : Promise.reject(new Error("spotlight fetch failed")); })
            .then(function (payload) {
                var entries = Array.isArray(payload) ? payload : payload.snippets;
                if (!Array.isArray(entries) || entries.length === 0) return;
                var byKey = {};
                entries.forEach(function (entry) {
                    byKey[entry.key] = entry;
                });
                container.__codeSpotlight = { entries: entries, entriesByKey: byKey };
                renderTabs(container, entries);
                container.__codeSpotlight.selectedKey = entries[0].key;
                renderViewer(container, entries[0]);
            })
            .catch(function () {
                container.innerHTML = '<p class="error">Failed to load code samples.</p>';
            });
    }

    window.ZithCodeSpotlight = {
        init: init
    };
})();
