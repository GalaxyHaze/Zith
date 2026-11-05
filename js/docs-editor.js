(function () {
    "use strict";

    const content = document.getElementById("content");
    const editButton = document.getElementById("editPageButton");
    const draftPrefix = "zith-doc-draft-v1:";
    let activePath = "";
    let activeSource = null;
    let original = "";
    let sourceText = null;

    function draftKey(source) {
        return draftPrefix + `${source.repository}:${source.path}`;
    }

    function readDraft(key) {
        try {
            const value = localStorage.getItem(key);
            return value ? JSON.parse(value) : null;
        } catch (_) {
            return null;
        }
    }

    function setStatus(message, state = "") {
        const status = content.querySelector(".docs-editor-status");
        if (!status) return;
        status.textContent = message;
        status.dataset.state = state;
    }

    function createWorkspace() {
        content.className = "docs-content docs-editor";
        content.innerHTML = `
            <section aria-label="Documentation editor">
                <div class="docs-editor-heading">
                    <h2>Edit this page</h2>
                    <div class="docs-editor-actions">
                        <button class="button button-secondary" type="button" data-editor-close>Done</button>
                        <button class="button button-secondary" type="button" data-editor-discard>Discard changes</button>
                    </div>
                </div>
                <p class="docs-editor-status" role="status" aria-live="polite">Loading Markdown source…</p>
                <div class="docs-editor-grid" hidden>
                    <label class="docs-editor-source">
                        <h3>Markdown</h3>
                        <textarea class="docs-editor-textarea" spellcheck="false"
                                  aria-label="Markdown source"></textarea>
                    </label>
                    <section class="docs-editor-preview" aria-label="Live preview">
                        <h3>Preview</h3>
                        <div class="docs-content"></div>
                    </section>
                </div>
            </section>`;
        content.querySelector("[data-editor-close]").addEventListener("click", closeEditor);
        content.querySelector("[data-editor-discard]").addEventListener("click", discardDraft);
        sourceText = content.querySelector(".docs-editor-textarea");
        sourceText.addEventListener("input", onSourceInput);
    }

    function renderPreview() {
        const preview = content.querySelector(".docs-editor-preview .docs-content");
        if (!preview || !sourceText || !activeSource) return;
        try {
            const html = window.DocsMarkdown.render(sourceText.value, activeSource, window.DocsPageData);
            const warning = activeSource.output.startsWith("zith--/")
                ? '<div class="callout callout-warning"><p><strong>Beta:</strong> Zith-- is a sibling language to Zith with its own language contract. See <a href="./?page=reference/D-implementation-status.html" data-docs-route="neutral">Implementation Status</a> for compiler support and known restrictions.</p></div>\n'
                : activeSource.output.startsWith("zith/")
                    ? '<div class="callout callout-warning"><p><strong>Language design:</strong> This page describes Zith\'s experimental language design, not the behavior of <code>zithc</code>. <a href="./?page=reference/D-implementation-status.html" data-docs-route="neutral">Implementation Status</a> reports compiler support for sibling Zith-- only.</p></div>\n'
                    : "";
            preview.innerHTML = warning + html;
            if (window.ZithHighlight) {
                preview.querySelectorAll("pre > code.language-zith").forEach(code => {
                    code.innerHTML = window.ZithHighlight.highlight(code.textContent);
                });
            }
            return true;
        } catch (error) {
            preview.replaceChildren();
            const message = document.createElement("p");
            message.className = "docs-editor-error";
            message.textContent = error.message;
            preview.appendChild(message);
            return false;
        }
    }

    function onSourceInput() {
        const key = draftKey(activeSource);
        const draft = {
            ...activeSource,
            original,
            content: sourceText.value,
            updatedAt: new Date().toISOString()
        };
        try {
            if (draft.content === original) localStorage.removeItem(key);
            else localStorage.setItem(key, JSON.stringify(draft));
            setStatus(draft.content === original
                ? "No local changes. Drafts are saved only in this browser."
                : "Saved automatically in this browser. The published page is unchanged.", "success");
        } catch (_) {
            setStatus("Browser storage is unavailable; these edits may be lost if you leave this page.", "error");
        }
        renderPreview();
    }

    async function getSource(descriptor) {
        const response = await fetch(new URL(descriptor.sourceUrl, document.baseURI));
        if (!response.ok) throw new Error("Could not load the Markdown source.");
        return response.text();
    }

    async function openEditor(path) {
        const descriptor = window.DocsPageData && window.DocsPageData.editSources[path];
        if (!descriptor) return;
        activePath = path;
        activeSource = descriptor;
        createWorkspace();
        const key = draftKey(descriptor);
        const saved = readDraft(key);

        try {
            if (saved) {
                original = saved.original;
                sourceText.value = saved.content;
            } else {
                const loadedSource = await getSource(descriptor);
                if (activePath !== path || activeSource !== descriptor) return;
                original = loadedSource;
                sourceText.value = original;
            }
            content.querySelector(".docs-editor-grid").hidden = false;
            renderPreview();
            setStatus(saved
                ? "Restored this page’s draft from this browser."
                : "Edits save automatically in this browser. The published page is unchanged.");
            sourceText.focus();
        } catch (error) {
            setStatus(error.message, "error");
        }
    }

    function closeEditor() {
        const path = activePath;
        activePath = "";
        activeSource = null;
        sourceText = null;
        content.className = "docs-content";
        if (window.DocsReloadCurrentPage) window.DocsReloadCurrentPage(path);
    }

    function discardDraft() {
        if (!activeSource || !sourceText) return;
        try {
            localStorage.removeItem(draftKey(activeSource));
        } catch (_) {}
        sourceText.value = original;
        renderPreview();
        setStatus("Local changes discarded.", "success");
    }
    editButton.addEventListener("click", () => openEditor(editButton.dataset.page));
    window.addEventListener("docs-page-loaded", event => {
        if (activePath) {
            activePath = "";
            activeSource = null;
            sourceText = null;
            content.className = "docs-content";
        }
    });

})();
