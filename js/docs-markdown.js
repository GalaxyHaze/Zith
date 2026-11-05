/* Browser preview mirrors tools/build_docs.py's deliberately small Markdown renderer. */
(function (root) {
    "use strict";

    const legacyAnchors = {
        "04-traits-interfaces.md|44-capabilities--built-in-reference": ["43-capabilities"],
        "02-module-system.md|23-namespace-access--scope-resolution": [],
        "08-error-handling.md|83-propagation--fallback": [],
        "08-error-handling.md|83-try-local-results-and-fallback": [
            "83-try-propagation-and-fallback"
        ],
        "09-control-flow.md|94-state-functions--state-machines": [
            "94-state-functions-and-state-machines"
        ]
    };

    function escapeHtml(value, quote = true) {
        const pattern = quote ? /[&<>"']/g : /[&<>]/g;
        return String(value).replace(pattern, character => ({
            "&": "&amp;",
            "<": "&lt;",
            ">": "&gt;",
            '"': "&quot;",
            "'": "&#x27;"
        })[character]);
    }

    function slug(text) {
        return text.replace(/<[^>]+>/g, "").trim().toLowerCase()
            .replace(/[^a-z0-9\s-]/g, "")
            .replace(/\s/g, "-")
            .replace(/^-+|-+$/g, "") || "section";
    }

    function normaliseSourcePath(path) {
        const parts = [];
        path.split("/").forEach(part => {
            if (!part || part === ".") return;
            if (part === "..") parts.pop();
            else parts.push(part);
        });
        return parts.join("/");
    }

    function relativeHref(sourceOutput, targetOutput) {
        const source = sourceOutput.replace(/\\/g, "/").split("/");
        const target = targetOutput.replace(/\\/g, "/").split("/");
        let common = 0;
        while (common < source.length && common < target.length &&
               source[common] === target[common]) common++;
        const up = Math.max(0, source.length - common - 1);
        const suffix = target.slice(common).join("/") || target[target.length - 1];
        return `${"../".repeat(up) || "./"}${suffix}`;
    }

    function documentationLink(source, target, label, anchor) {
        const scopeOf = output => output.startsWith("zith--/") ? "zith--" :
            output.startsWith("zith/") ? "zith" : "neutral";
        const sourceScope = scopeOf(source);
        const targetScope = scopeOf(target);
        let href;
        let routeAttribute = "";

        if (sourceScope !== targetScope) {
            if (targetScope === "neutral") href = `./?page=${target}`;
            else if (target === `${targetScope}/D-overview.html`) href = `./${targetScope}/`;
            else href = `./${targetScope}/?page=${target}`;
            routeAttribute = ` data-docs-route="${targetScope}"`;
        } else {
            href = relativeHref(source, target);
        }
        if (anchor) href += `#${anchor}`;
        return `<a href="${escapeHtml(href)}"${routeAttribute}>${label}</a>`;
    }

    function inline(text, page, tree) {
        let escaped = escapeHtml(text, false);
        escaped = escaped.replace(/`([^`]+)`/g, "<code>$1</code>");
        escaped = escaped.replace(/\[([^\]]+)\]\(([^)]+)\)/g, (match, label, rawTarget) => {
            const target = rawTarget.replace(/&amp;/g, "&").replace(/&quot;/g, '"')
                .replace(/&#x27;/g, "'").replace(/&lt;/g, "<").replace(/&gt;/g, ">");

            if (/^(?:javascript|data|vbscript):/i.test(target.trim())) {
                return label;
            }
            if (/^(?:https?:|mailto:)/i.test(target)) {
                return `<a href="${escapeHtml(target)}">${label}</a>`;
            }
            if (target.startsWith("#")) {
                return `<a href="${escapeHtml(target)}">${label}</a>`;
            }
            if (target.startsWith("route:")) {
                const route = target.slice(6);
                const routePaths = { zith: "./zith/", "zith--": "./zith--/" };
                if (!routePaths[route]) return label;
                return `<a href="${routePaths[route]}" data-docs-route="${route}">${label}</a>`;
            }

            const parts = target.split("#");
            const filePart = parts[0];
            const anchor = parts.length > 1 ? parts.slice(1).join("#") : "";
            const outputForId = id => tree.documentOutputs[id];

            if (target.startsWith("neutral:") || target.startsWith("doc:")) {
                const neutral = target.startsWith("neutral:");
                const [id, ...anchorParts] = target.slice(neutral ? 8 : 4).split("#");
                const targetOutput = outputForId(id);
                if (!targetOutput) return label;
                const targetAnchor = anchorParts.join("#");
                if (neutral) {
                    const href = `./?page=${targetOutput}${targetAnchor ? `#${targetAnchor}` : ""}`;
                    return `<a href="${escapeHtml(href)}" data-docs-route="neutral">${label}</a>`;
                }
                return documentationLink(page.output, targetOutput, label, targetAnchor);
            }

            const sourceParent = page.path.split("/").slice(0, -1).join("/");
            const resolved = normaliseSourcePath(`${sourceParent}/${filePart}`);
            const targetOutput = tree.sourceOutputs[`${page.repository}:${resolved}`];
            if (targetOutput) return documentationLink(page.output, targetOutput, label, anchor);

            const importedAliases = {
                "implementation-debt.md": "reference/D-implementation-status.html",
                "plans/branch-protocol.md": "zith/reference/D-concurrency.html",
                "adr/0010-allocator-inplace-drop.md": "zith/reference/D-standard-library.html"
            };
            if (page.kind === "spec" && importedAliases[filePart]) {
                return documentationLink(page.output, importedAliases[filePart], label, anchor);
            }
            return `<a href="${escapeHtml(target)}">${label}</a>`;
        });
        escaped = escaped.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
        escaped = escaped.replace(/(?<!\*)\*([^*]+)\*(?!\*)/g, "<em>$1</em>");
        return escaped;
    }

    function splitFrontMatter(source) {
        if (!source.startsWith("---\n")) {
            throw new Error("Markdown page is missing its front matter.");
        }
        const end = source.indexOf("\n---\n", 4);
        if (end < 0) throw new Error("Markdown front matter is not closed.");
        return source.slice(end + 5).trim() + "\n";
    }

    function render(source, page, tree) {
        const body = page.kind === "spec" ? source : splitFrontMatter(source);
        const lines = body.split(/\r?\n/);
        const result = [];
        const seenIds = new Map();
        let headingCount = 0;
        let index = 0;

        function addHeading(level, text) {
            let anchor = slug(text);
            const occurrence = seenIds.get(anchor) || 0;
            seenIds.set(anchor, occurrence + 1);
            if (occurrence) anchor += `-${occurrence + 1}`;
            if (level === 1) headingCount++;
            result.push(`<h${level} id="${anchor}">${inline(text, page, tree)}</h${level}>`);
            (legacyAnchors[`${page.path.split("/").pop()}|${anchor}`] || []).forEach(legacy => {
                result.push(`<span id="${legacy}" aria-hidden="true"></span>`);
            });
        }

        function renderTable(tableLines) {
            const rows = tableLines.filter(line => !/^\s*\|?[\s:|-]+\|[\s:| -]*$/.test(line))
                .map(line => line.trim().replace(/^\|/, "").replace(/\|$/, "")
                    .split("|").map(cell => cell.trim()));
            if (!rows.length) return "";
            let html = "<table>\n  <thead>\n    <tr>\n";
            rows[0].forEach(cell => { html += `      <th>${inline(cell, page, tree)}</th>\n`; });
            html += "    </tr>\n  </thead>";
            if (rows.length > 1) {
                html += "\n  <tbody>";
                rows.slice(1).forEach(row => {
                    html += "\n    <tr>\n";
                    row.forEach(cell => { html += `      <td>${inline(cell, page, tree)}</td>\n`; });
                    html += "    </tr>";
                });
                html += "\n  </tbody>";
            }
            return `${html}\n</table>`;
        }

        while (index < lines.length) {
            const line = lines[index];
            if (!line.trim()) {
                index++;
                continue;
            }
            const fence = line.match(/^```([^`]*)$/);
            if (fence) {
                const language = fence[1].trim();
                index++;
                const code = [];
                while (index < lines.length && !lines[index].startsWith("```")) {
                    code.push(lines[index++]);
                }
                if (index === lines.length) throw new Error("Code fence is not closed.");
                const className = language ? ` class="language-${escapeHtml(language)}"` : "";
                result.push(`<pre><code${className}>${escapeHtml(code.join("\n"))}</code></pre>`);
                index++;
                continue;
            }
            const heading = line.match(/^(#{1,6})\s+(.+?)\s*$/);
            if (heading) {
                let level = heading[1].length;
                if (page.kind === "spec" && headingCount === 0 && level > 1) level = 1;
                addHeading(level, heading[2]);
                index++;
                continue;
            }
            if (line.startsWith("> ")) {
                const quote = [];
                while (index < lines.length && lines[index].startsWith("> ")) {
                    quote.push(lines[index++].slice(2));
                }
                result.push(`<blockquote><p>${inline(quote.join(" "), page, tree)}</p></blockquote>`);
                continue;
            }
            if (line.startsWith("|")) {
                const table = [];
                while (index < lines.length && lines[index].startsWith("|")) {
                    table.push(lines[index++]);
                }
                result.push(renderTable(table));
                continue;
            }
            const listMatch = line.match(/^(\s*)([-*+]|\d+\.)\s+(.+)$/);
            if (listMatch) {
                const ordered = listMatch[2].endsWith(".");
                const tag = ordered ? "ol" : "ul";
                const entries = [];
                while (index < lines.length) {
                    const item = lines[index].match(/^\s*([-*+]|\d+\.)\s+(.+)$/);
                    if (!item || item[1].endsWith(".") !== ordered) break;
                    entries.push(item[2]);
                    index++;
                }
                result.push(`<${tag}>`, ...entries.map(item =>
                    `  <li>${inline(item, page, tree)}</li>`), `</${tag}>`);
                continue;
            }
            if (line === "---") {
                result.push("<hr>");
                index++;
                continue;
            }
            const paragraph = [line.trim()];
            index++;
            while (index < lines.length && lines[index].trim() &&
                   !/^(#{1,6}\s|```|> |\||[-*+]\s|\d+\. |---$)/.test(lines[index])) {
                paragraph.push(lines[index++].trim());
            }
            result.push(`<p>${inline(paragraph.join(" "), page, tree)}</p>`);
        }

        if (headingCount !== 1) {
            throw new Error(`This page must contain exactly one H1 heading (found ${headingCount}).`);
        }
        return result.join("\n") + "\n";
    }

    const api = { render, splitFrontMatter, slug };
    root.DocsMarkdown = api;
    if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof window !== "undefined" ? window : globalThis);
