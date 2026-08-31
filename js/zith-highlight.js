(function () {
    "use strict";

    var escapeHtml = function (text) {
        return text
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;");
    };

    var rules = [
        {
            name: "comment",
            token: "cm",
            re: /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)/g
        },
        {
            name: "attribute",
            token: "kw",
            re: /(@foreign\b|@[\w-]+)/g
        },
        {
            name: "keyword",
            token: "kw",
            re: /(\b(?:pub|fn|struct|enum|when|let|mut|unsafe|null|view|lend|sink|return|if|else|while|for|loop|break|continue|import|from|as|type|static|const|var|true|false|component|entity|scene|use|mod|impl|trait|match|in|ref|new|drop|self)\b)/g
        },
        {
            name: "function",
            token: "fn",
            re: /(\b[a-z_][A-Za-z0-9_]*)(?=\s*\()/g
        },
        {
            name: "type",
            token: "ty",
            re: /(\b(?:u\d+|i\d+|usize|isize|f(?:32|64)|bool|char|string|str|c_char|c_int|c_long|void|raw|[A-Z][A-Za-z0-9_]*)\b)/g
        },
        {
            name: "string",
            token: "st",
            re: /("(?:\\.|[^"\\])*"(?:\.[A-Za-z_]\w*)?|'(?:\\.|[^'\\])*')/g
        },
        {
            name: "number",
            token: "num",
            re: /\b(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b/g
        }
    ];

    function combineMatches(matches) {
        var sorted = matches
            .filter(function (match) { return match.text.length > 0; })
            .sort(function (a, b) {
                return a.start - b.start || a.end - b.end || b.priority - a.priority;
            });
        var accepted = [];
        var lastEnd = -1;
        sorted.forEach(function (match) {
            if (match.start < lastEnd) return;
            accepted.push(match);
            lastEnd = match.end;
        });
        return accepted;
    }

    function highlight(text) {
        var textStr = String(text == null ? "" : text);
        var matches = [];

        rules.forEach(function (rule) {
            var re = new RegExp(rule.re.source, rule.re.flags.replace(/g/g, "") + "g");
            var match;
            var priority = 0;
            if (rule.name === "keyword" || rule.name === "attribute") priority = 2;
            if (rule.name === "function" || rule.name === "type") priority = 1;
            while ((match = re.exec(textStr)) !== null) {
                var groups = match.slice(1, 3);
                var raw = groups.find(Boolean) || match[0];
                matches.push({
                    start: match.index,
                    end: match.index + match[0].length,
                    groupIndex: groups.indexOf(raw) >= 0 ? groups.indexOf(raw) : 0,
                    text: raw,
                    token: rule.token,
                    priority: priority
                });
            }
        });

        matches = combineMatches(matches);
        if (!matches.length) return escapeHtml(textStr);

        var html = "";
        var cursor = 0;
        matches.forEach(function (match) {
            html += escapeHtml(textStr.slice(cursor, match.start));
            html += '<span class="' + match.token + '">' + escapeHtml(match.text) + "</span>";
            cursor = match.end;
        });
        html += escapeHtml(textStr.slice(cursor));
        return html;
    }

    window.ZithHighlight = {
        escapeHtml: escapeHtml,
        highlight: highlight
    };
})();
