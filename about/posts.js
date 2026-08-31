document.addEventListener("DOMContentLoaded", function() {
    var list = document.getElementById("postList");
    var content = document.getElementById("postContent");

    function renderEntries(entries) {
        if (!list || !Array.isArray(entries)) return;
        list.innerHTML = entries.map(function(entry) {
            return '<li><span class="post-date">' + entry.date + '</span>' +
                '<a href="./posts/' + entry.file + '">' + entry.title + '</a></li>';
        }).join("");
    }

    function renderPost(url) {
        fetch(url)
            .then(function(response) { return response.text(); })
            .then(function(html) {
                if (!content) return;
                var match = html.match(/<body[^>]*>([\s\S]*)<\/body>/i);
                content.innerHTML = match ? match[1] : html;
                content.classList.remove("hidden");
            })
            .catch(function() {
                if (!content) return;
                content.innerHTML = '<p class="error">Failed to load entry.</p>';
                content.classList.remove("hidden");
            });
    }

    fetch("./posts/index.json")
        .then(function(response) { return response.json(); })
        .then(renderEntries)
        .catch(function() {
            if (list) list.innerHTML = '<li class="error">Failed to load entries.</li>';
        });
});
