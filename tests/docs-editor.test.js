const assert = require("node:assert/strict");
const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const { render, splitFrontMatter } = require("../js/docs-markdown.js");
const root = path.resolve(__dirname, "..");
const specRoot = path.resolve(root, "../Zith/docs");

const editorialPage = {
    repository: "GalaxyHaze/Zith-Lang",
    path: "docs/pages/zith--/guide/types.md",
    output: "zith--/guide/D-types.html",
    kind: "editorial"
};

const tree = {
    documentOutputs: {
        "reference-types": "zith/reference/D-type-system.html"
    },
    sourceOutputs: {
        "GalaxyHaze/Zith-Lang:docs/pages/zith--/guide/functions.md":
            "zith--/guide/D-functions.html"
    }
};

test("front matter is omitted from the live Markdown preview", () => {
    assert.equal(
        splitFrontMatter("---\ntitle: Test\n---\n# Heading\n"),
        "# Heading\n"
    );
    assert.throws(() => splitFrontMatter("# no metadata"), /front matter/);
});

test("the editor preview renders the documentation Markdown features and links", () => {
    const markdown = `---
id: guide-types
title: Types
---
# Types

Words **strong**, *emphasis*, and \`i32\`.

[Functions](functions.md#functions) and [the Zith reference](doc:reference-types).

> A callout quote.

| Type | Value |
| --- | --- |
| \`i32\` | **integer** |

\`\`\`zith
fn main() {}
\`\`\`
`;
    const html = render(markdown, editorialPage, tree);
    assert.match(html, /<h1 id="types">Types<\/h1>/);
    assert.match(html, /<strong>strong<\/strong>.*<em>emphasis<\/em>/);
    assert.match(html, /href="\.\/D-functions\.html#functions"/);
    assert.match(html, /data-docs-route="zith">the Zith reference<\/a>/);
    assert.match(html, /<blockquote><p>A callout quote\.<\/p><\/blockquote>/);
    assert.match(html, /<th>Type<\/th>/);
    assert.match(html, /class="language-zith"/);
});

test("live preview HTML matches the static build for every editable page", () => {
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "zith-doc-editor-"));
    const output = path.join(tempDir, "site");
    try {
        execFileSync("python3", [
            "tools/build.py", "--out", output
        ], { cwd: root, stdio: "inherit" });
        execFileSync("python3", [
            "tools/build_docs.py", "--site-dir", output, "--spec-dir", specRoot
        ], { cwd: root, stdio: "inherit" });
        const generatedTree = JSON.parse(
            fs.readFileSync(path.join(output, "json/tree.json"), "utf8")
        );

        Object.values(generatedTree.editSources).forEach(page => {
            const sourceRoot = page.repository === "GalaxyHaze/Zith"
                ? path.resolve(root, "../Zith")
                : root;
            const source = fs.readFileSync(path.join(sourceRoot, page.path), "utf8");
            const built = fs.readFileSync(path.join(output, "docs", page.output), "utf8");
            const contentStart = built.indexOf("<h1");
            assert.notEqual(contentStart, -1, `${page.path} should have a rendered H1`);
            assert.equal(
                render(source, page, generatedTree),
                built.slice(contentStart),
                `${page.path} preview should exactly match the static renderer`
            );
        });
    } finally {
        fs.rmSync(tempDir, { recursive: true, force: true });
    }
});

test("the preview blocks active-content links and rejects invalid document structure", () => {
    const html = render(
        "---\ntitle: Safe\n---\n# Safe\n\n[click](javascript:alert)\n",
        editorialPage,
        tree
    );
    assert.doesNotMatch(html, /href="javascript:/);
    assert.match(html, />click<\/p>/);
    assert.throws(
        () => render("---\ntitle: Missing heading\n---\nText only\n", editorialPage, tree),
        /exactly one H1/
    );
    assert.throws(
        () => render("---\ntitle: Broken\n---\n# Broken\n\n\`\`\`zith\nunclosed\n", editorialPage, tree),
        /not closed/
    );
});
