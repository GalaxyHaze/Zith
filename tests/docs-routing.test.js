const assert = require("node:assert/strict");
const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const {
    resolveDocsRoute,
    resolveLanguageSwitch,
    filterDocsNavigation
} = require("../js/docs-routing.js");
const root = path.resolve(__dirname, "..");
const specDir = [
    path.join(root, "Zith", "docs"),
    path.resolve(root, "..", "Zith", "docs")
].find(candidate => fs.existsSync(candidate));

test("the Code Spotlight places a direct C binding beside header-based interop", () => {
    const entries = JSON.parse(
        fs.readFileSync(path.join(root, "json/code-spotlight.json"), "utf8")
    );
    const keys = entries.map(entry => entry.key);
    const interopIndex = keys.indexOf("c_interop");
    const bindingIndex = keys.indexOf("c_bind");

    assert.notEqual(interopIndex, -1);
    assert.equal(bindingIndex, interopIndex + 1);
    assert.match(entries[bindingIndex].code, /extern fn puts\(message: \*char\): i32;/);
    assert.match(entries[bindingIndex].code, /puts\("Hello from C"\)/);
});

test("the docs root is the neutral comparison page", () => {
    assert.deepEqual(resolveDocsRoute("/docs/"), {
        scope: "neutral",
        page: "./D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/index.html"), {
        scope: "neutral",
        page: "./D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/preview/docs/zith/"), {
        scope: "zith",
        page: "./zith/D-overview.html"
    });
});

test("the language routes open their own overview, with or without a trailing slash", () => {
    assert.deepEqual(resolveDocsRoute("/docs/zith"), {
        scope: "zith",
        page: "./zith/D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/zith/"), {
        scope: "zith",
        page: "./zith/D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/zith/index.html"), {
        scope: "zith",
        page: "./zith/D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/zith--"), {
        scope: "zith--",
        page: "./zith--/D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/zith--/"), {
        scope: "zith--",
        page: "./zith--/D-overview.html"
    });
    assert.deepEqual(resolveDocsRoute("/docs/zith--/index.html"), {
        scope: "zith--",
        page: "./zith--/D-overview.html"
    });
});

test("language switching preserves equivalent topics and falls back to the target overview", () => {
    const equivalents = {
        "zith--/guide/D-types.html": {
            zith: "zith/reference/D-03-type-system.html"
        },
        "D-overview.html": {
            zith: "zith/D-overview.html",
            "zith--": "zith--/D-overview.html"
        }
    };

    assert.deepEqual(
        resolveLanguageSwitch("./zith--/guide/D-types.html", "zith", equivalents),
        { page: "./zith/reference/D-03-type-system.html", equivalent: true }
    );
    assert.deepEqual(
        resolveLanguageSwitch("./cli/D-overview.html", "zith--", equivalents),
        { page: "./zith--/D-overview.html", equivalent: false }
    );
    assert.deepEqual(
        resolveLanguageSwitch("./D-overview.html", "zith", equivalents),
        { page: "./zith/D-overview.html", equivalent: true }
    );
});

test("each documentation shell exposes only its own navigation scope", () => {
    const navigation = [{
        title: "Home",
        children: [
            { title: "FAQ" },
            { title: "Zith" },
            { title: "Zith--" }
        ]
    }];

    assert.deepEqual(
        filterDocsNavigation(navigation, "neutral")[0].children.map(item => item.title),
        ["FAQ"]
    );
    assert.deepEqual(
        filterDocsNavigation(navigation, "zith")[0].children.map(item => item.title),
        ["Zith"]
    );
    assert.deepEqual(
        filterDocsNavigation(navigation, "zith--")[0].children.map(item => item.title),
        ["Zith--"]
    );
});

test("the static build emits an index shell for each docs route", () => {
    assert.ok(specDir, "the canonical Zith specification should be checked out");
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "zith-doc-routes-"));
    const output = path.join(tempDir, "site");
    const buildSpecDir = path.join(tempDir, "Zith", "docs");

    try {
        fs.cpSync(specDir, buildSpecDir, { recursive: true });
        execFileSync("python3", [
            "tools/build.py", "--out", output, "--baseurl", "/preview"
        ], {
            cwd: root,
            stdio: "inherit"
        });
        execFileSync("python3", [
            "tools/build_docs.py", "--site-dir", output, "--spec-dir", buildSpecDir
        ], {
            cwd: root,
            stdio: "inherit"
        });
        execFileSync("python3", [
            "tools/build_docs.py", "--check", "--site-dir", output, "--spec-dir", buildSpecDir
        ], {
            cwd: root,
            stdio: "inherit"
        });

        for (const [route, scope] of [
            ["docs/D-home.html", "neutral"],
            ["docs/index.html", "neutral"],
            ["docs/zith/index.html", "zith"],
            ["docs/zith--/index.html", "zith--"]
        ]) {
            const shell = fs.readFileSync(path.join(output, route), "utf8");
            assert.match(shell, new RegExp(`data-docs-scope="${scope}"`));
            assert.doesNotMatch(shell, /\{\{baseurl\}\}|include:header/);
            assert.match(shell, /data-docs-base="\/preview\/docs\/"/);
            assert.match(shell, /data-tree-url="\/preview\/json\/tree\.json"/);
        }

        for (const target of [
            "docs/D-overview.html",
            "docs/zith/D-overview.html",
            "docs/zith--/D-overview.html",
            "docs/reference/D-implementation-status.html",
            "docs/zith--/guide/D-first-program.html",
            "docs/zith/D-faq.html",
            "docs/zith--/D-faq.html",
            "json/tree.json",
            "js/docs-routing.js",
            "js/loadTree.js"
        ]) {
            assert.ok(fs.existsSync(path.join(output, target)), `${target} should be published`);
        }

        const shell = fs.readFileSync(path.join(output, "docs/D-home.html"), "utf8");
        assert.match(shell, /data-docs-language="zith"/);
        assert.match(shell, /data-docs-language="zith--"/);
        const landing = fs.readFileSync(path.join(output, "docs/D-overview.html"), "utf8");
        assert.match(landing, /data-docs-route="zith"/);
        assert.match(landing, /data-docs-route="zith--"/);
        assert.match(landing, /data-docs-route="neutral"/);
        const zithMinusMinusOverview = fs.readFileSync(
            path.join(output, "docs/zith--/D-overview.html"),
            "utf8"
        );
        assert.match(
            zithMinusMinusOverview,
            /sibling of Zith, not a subset or an implementation stage/
        );
        const tree = JSON.parse(fs.readFileSync(path.join(output, "json/tree.json"), "utf8"));
        assert.equal(tree.version, 5);
        assert.equal(
            tree.topicEquivalents["zith--/guide/D-types.html"].zith,
            "zith/reference/D-type-system.html"
        );
        const neutralTitles = tree.navigation[0].children.map(item => item.title);
        assert.ok(neutralTitles.includes("Implementation Status"));
        assert.ok(neutralTitles.includes("CLI Reference"));
        assert.ok(neutralTitles.includes("Zith"));
        assert.ok(neutralTitles.includes("Zith--"));
        const zithSection = tree.navigation[0].children.find(item => item.title === "Zith");
        const zithMinusMinusSection = tree.navigation[0].children.find(
            item => item.title === "Zith--"
        );
        assert.match(zithSection.description, /Zith language design and specification/);
        assert.match(
            zithMinusMinusSection.description,
            /Zith-- language and compiler contract/
        );
        assert.doesNotMatch(zithMinusMinusSection.description, /subset of Zith/i);
        const scopedNeutralTitles = filterDocsNavigation(tree.navigation, "neutral")[0]
            .children.map(item => item.title);
        assert.ok(scopedNeutralTitles.includes("Implementation Status"));
        assert.ok(!scopedNeutralTitles.includes("Zith"));
        assert.ok(!scopedNeutralTitles.includes("Zith--"));
        const firstProgram = fs.readFileSync(
            path.join(output, "docs/zith--/guide/D-first-program.html"),
            "utf8"
        );
        assert.match(firstProgram, /<h1 id="first-program">First Program<\/h1>/);
        assert.match(firstProgram, /href="\.\/\?page=reference\/D-implementation-status\.html" data-docs-route="neutral"/);
        const whyZith = fs.readFileSync(
            path.join(output, "docs/getting-started/D-why-zith.html"),
            "utf8"
        );
        assert.match(whyZith, /data-docs-route="zith--"/);
        const status = fs.readFileSync(
            path.join(output, "docs/reference/D-implementation-status.html"),
            "utf8"
        );
        assert.match(status, /Zith-- Implementation Status/);
        const zithSpec = fs.readFileSync(
            path.join(output, "docs/zith/reference/D-specification.html"),
            "utf8"
        );
        assert.match(zithSpec, /experimental language design, not the behavior of <code>zithc<\/code>/);
        assert.match(zithSpec, /reports compiler support for sibling Zith-- only/);
        const zithMinusMinus = fs.readFileSync(
            path.join(output, "docs/zith--/reference/D-zith-subset.html"),
            "utf8"
        );
        assert.match(zithMinusMinus, /<strong>Beta:<\/strong> Zith-- is a sibling language to Zith/);
        assert.match(zithMinusMinus, /for compiler support and known restrictions/);
    } finally {
        fs.rmSync(tempDir, { recursive: true, force: true });
    }
});

test("the Pages workflow generates documentation fragments before deployment", () => {
    const workflow = fs.readFileSync(
        path.join(root, ".github/workflows/build.yml"),
        "utf8"
    );

    assert.match(workflow, /repository:\s*GalaxyHaze\/Zith/);
    assert.match(
        workflow,
        /python3 tools\/build_docs\.py --site-dir _site --spec-dir Zith\/docs/
    );
    assert.match(
        workflow,
        /python3 tools\/build_docs\.py --check --site-dir _site --spec-dir Zith\/docs/
    );
});
