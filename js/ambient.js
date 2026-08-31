(function() {
    "use strict";

    var STORAGE_KEY = "zith_ambient";
    var script = document.currentScript || document.querySelector('script[src*="js/ambient.js"]');
    var SRC = new URL("../media/audio/ambient.mp3", script.src).href;
    var audio = null;
    var lastPositionSave = 0;
    var positionResumed = false;

    function loadState() {
        try {
            var raw = localStorage.getItem(STORAGE_KEY);
            if (raw) {
                var parsed = JSON.parse(raw);
                if (parsed && typeof parsed.enabled === "boolean") {
                    return {
                        enabled: parsed.enabled,
                        currentTime: Number(parsed.currentTime) || 0,
                        savedAt: Number(parsed.savedAt) || Date.now()
                    };
                }
            }
        } catch (e) {
        }
        return { enabled: false, currentTime: 0, savedAt: Date.now() };
    }

    var state = loadState();

    function saveState(enabled, currentTime) {
        try {
            localStorage.setItem(STORAGE_KEY, JSON.stringify({
                enabled: !!enabled,
                currentTime: currentTime || 0,
                savedAt: Date.now()
            }));
        } catch (e) {
        }
    }

    function setToggle(playing) {
        var toggle = document.getElementById("audioToggle");
        if (!toggle) return;
        toggle.classList.toggle("is-playing", playing);
        toggle.setAttribute("aria-pressed", playing ? "true" : "false");
        toggle.setAttribute("aria-label", playing ? "Pause ambient audio" : "Play ambient audio");
    }

    function createAudio() {
        if (audio) return audio;
        audio = new Audio(SRC);
        audio.loop = true;
        audio.preload = "none";

        audio.addEventListener("timeupdate", function() {
            var now = Date.now();
            if (now - lastPositionSave > 2000) {
                lastPositionSave = now;
                saveState(true, audio.currentTime);
            }
        });
        audio.addEventListener("play", function() {
            setToggle(true);
        });
        audio.addEventListener("pause", function() {
            setToggle(false);
            saveState(false, audio.currentTime);
        });
        return audio;
    }

    function resumeState() {
        if (!audio) return;
        if (positionResumed) return;
        var elapsed = Math.max(0, (Date.now() - state.savedAt) / 1000);
        var position = state.currentTime + elapsed;
        if (audio.duration && position >= audio.duration) {
            position %= audio.duration;
        }
        audio.currentTime = position;
        positionResumed = true;
    }

    function startAmbient() {
        if (!audio) createAudio();
        resumeState();
        var promise = audio.play();
        if (promise && promise.catch) {
            promise.catch(function() {
                setToggle(false);
            });
        }
    }

    function stopAmbient() {
        state.enabled = false;
        if (audio) {
            saveState(false, audio.currentTime);
            audio.pause();
        } else {
            saveState(false, state.currentTime);
        }
        setToggle(false);
    }

    function toggleAmbient() {
        if (audio && !audio.paused) {
            stopAmbient();
            return;
        }
        state.enabled = true;
        state.savedAt = Date.now();
        positionResumed = false;
        saveState(true, audio ? audio.currentTime : state.currentTime);
        startAmbient();
    }

    function injectStyle() {
        if (document.getElementById("zith-ambient-style")) return;
        var style = document.createElement("style");
        style.id = "zith-ambient-style";
        style.textContent = [
            ".audio-toggle{position:fixed;right:18px;bottom:18px;z-index:99998;display:flex;align-items:center;gap:9px;padding:9px 13px;background:rgba(14,14,17,.92);border:1px solid #2a2a36;border-radius:20px;color:#8f8fa3;font-family:'Fira Code',ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;cursor:pointer;user-select:none}",
            ".audio-toggle:hover,.audio-toggle.is-playing{color:#00ffff;border-color:rgba(0,255,255,.45);transform:translateY(-1px)}",
            ".audio-visualizer{display:inline-flex;align-items:flex-end;gap:3px;height:14px}",
            ".audio-visualizer i{width:3px;height:4px;border-radius:1px;background:currentColor;opacity:.55}",
            ".audio-toggle.is-playing .audio-visualizer i{animation:zith-audio-bar .9s ease-in-out infinite alternate}",
            ".audio-toggle.is-playing .audio-visualizer i:nth-child(2){animation-delay:.22s}",
            ".audio-toggle.is-playing .audio-visualizer i:nth-child(3){animation-delay:.45s}",
            "@keyframes zith-audio-bar{from{height:4px}to{height:14px}}"
        ].join("");
        document.head.appendChild(style);
    }

    function injectToggle() {
        if (document.getElementById("audioToggle")) return;
        if (!document.body || !document.body.classList.contains("zith-portal")) return;
        var button = document.createElement("button");
        button.id = "audioToggle";
        button.className = "audio-toggle";
        button.setAttribute("aria-pressed", "false");
        button.setAttribute("aria-label", "Play ambient audio");
        button.innerHTML = '<span class="audio-visualizer"><i></i><i></i><i></i></span><span class="audio-label">Ambient</span>';
        document.body.appendChild(button);
    }

    function bindToggle() {
        var toggle = document.getElementById("audioToggle");
        if (!toggle) return;
        if (toggle.dataset.ambientBound === "true") return;
        toggle.dataset.ambientBound = "true";
        toggle.addEventListener("click", toggleAmbient);
    }

    function tryResume() {
        if (!state.enabled) return;
        createAudio();
        startAmbient();
    }

    injectStyle();

    if (document.body) {
        injectToggle();
        bindToggle();
    }

    document.addEventListener("DOMContentLoaded", function() {
        injectToggle();
        bindToggle();
        tryResume();
    });

    if (document.readyState !== "loading") {
        injectToggle();
        bindToggle();
        tryResume();
    }

    document.addEventListener("click", function() {
        if (state.enabled && audio && audio.paused) {
            startAmbient();
        }
    }, true);

    document.addEventListener("keydown", function() {
        if (state.enabled && audio && audio.paused) {
            startAmbient();
        }
    }, true);

    window.addEventListener("pagehide", function() {
        if (audio && !audio.paused) {
            saveState(true, audio.currentTime);
        }
    });

    window.ZithAmbient = {
        play: startAmbient,
        pause: stopAmbient,
        toggle: toggleAmbient,
        isPlaying: function() {
            return !!(audio && !audio.paused);
        }
    };
})();
