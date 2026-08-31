/* Reusable character-by-character reveal engine.
   - animate(): types HTML content immediately through a callback.
   - observe(): starts animations for .typewriter elements only when they enter
     the viewport; each element animates only once. */
(function () {
    "use strict";

    var reducedMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    function elementKey(element) {
        return element && element.id ? element.id : element;
    }

    function splitHtmlTokens(html) {
        var tokens = [];
        var re = /<[^>]+>|[^<]+/g;
        var match;
        while ((match = re.exec(html)) !== null) {
            if (match[0].charAt(0) === "<") {
                tokens.push({ html: match[0], text: "" });
            } else {
                tokens.push({
                    html: match[0],
                    text: match[0].replace(/&amp;/g, "&").replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&#39;/g, "'")
                });
            }
        }
        return tokens;
    }

    function htmlPrefix(html, textCount) {
        var out = "";
        var decoded = 0;
        var i = 0;
        var entities = [["&amp;", "&"], ["&lt;", "<"], ["&gt;", ">"], ["&quot;", '"'], ["&#39;", "'"]];
        while (i < html.length && decoded < textCount) {
            var matched = false;
            entities.forEach(function (entity) {
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

    function typeHtml(element, fullHtml, settings) {
        var key = elementKey(element);
        if (element.__typewriterTimers) {
            element.__typewriterTimers.forEach(clearTimeout);
            element.__typewriterTimers = [];
        }

        element.innerHTML = "";
        element.classList.remove("is-typing");

        if (reducedMotion || !settings.animate) {
            element.innerHTML = fullHtml;
            return;
        }

        var tokens = splitHtmlTokens(fullHtml);
        var perCharMs = settings.perCharMs || 18;
        var progress = 0;
        element.__typewriterTimers = [];
        element.classList.add("is-typing");

        function render() {
            var totalChars = tokens.reduce(function (sum, token) {
                return sum + token.text.length;
            }, 0);

            if (progress >= totalChars) {
                element.innerHTML = fullHtml;
                element.classList.remove("is-typing");
                delete element.__typewriterTimers;
                if (settings.onDone) settings.onDone();
                return;
            }

            var html = "";
            var remaining = progress;
            tokens.forEach(function (token) {
                if (token.html.charAt(0) === "<") {
                    html += token.html;
                } else if (remaining > 0) {
                    var count = Math.min(remaining, token.text.length);
                    html += htmlPrefix(token.html, count);
                    remaining -= count;
                }
            });
            element.innerHTML = html;
            progress += 1;

            var delay = perCharMs;
            if (settings.delayAtNewline && String(fullHtml).indexOf("\n") !== -1) delay = settings.delayAtNewline;
            element.__typewriterTimers.push(setTimeout(render, delay));
        }

        render();
    }

    function observe() {
        var targets = Array.prototype.slice.call(document.querySelectorAll(".typewriter"));
        if (!("IntersectionObserver" in window)) {
            targets.forEach(function (el) {
                if (el.hasAttribute("data-typewriter-done")) return;
                el.setAttribute("data-typewriter-done", "true");
                typeHtml(el, el.getAttribute("data-typewriter-html") || el.innerHTML, { animate: !reducedMotion });
            });
            return;
        }

        if (reducedMotion) {
            targets.forEach(function (el) {
                if (el.hasAttribute("data-typewriter-done")) return;
                el.setAttribute("data-typewriter-done", "true");
                typeHtml(el, el.getAttribute("data-typewriter-html") || el.innerHTML, { animate: false });
            });
            return;
        }

        var observer = new IntersectionObserver(function (entries) {
            entries.forEach(function (entry) {
                if (!entry.isIntersecting || entry.target.hasAttribute("data-typewriter-done")) return;
                var el = entry.target;
                el.setAttribute("data-typewriter-done", "true");
                observer.unobserve(el);
                typeHtml(el, el.getAttribute("data-typewriter-html") || el.innerHTML, { animate: true });
            });
        }, { threshold: 0.25 });

        targets.forEach(function (el) {
            el.setAttribute("data-typewriter-html", el.innerHTML);
            el.innerHTML = "";
            observer.observe(el);
        });
    }

    window.ZithTypewriter = {
        animate: typeHtml,
        observe: observe
    };
})();
