(function () {
  "use strict";

  window.RXWidgets = window.RXWidgets || {};

  function counts(name) {
    if (!window.RX || !(window.RACTOR_MODELS || {})[name]) return null;
    try {
      return RX.load(name).counts || null;
    } catch (e) {
      return null;
    }
  }

  function mount(root) {
    root.classList.add("rx-rules");
    const $ = (sel) => root.querySelector(sel);
    const $$ = (sel) => Array.from(root.querySelectorAll(sel));
    const cards = $$(".card");
    const chips = $$(".chip");
    const shown = $(".shown");
    const allRuby = $(".all-ruby");
    const totals = $(".totals");

    function applyFilter(topic) {
      let n = 0;
      for (const card of cards) {
        const show = topic === "all" || card.dataset.topic === topic || (topic === "fixed" && card.dataset.fixed);
        card.hidden = !show;
        if (show) n += 1;
      }
      if (shown) shown.textContent = `${n} of ${cards.length} rules`;
    }

    for (const chip of chips) {
      chip.addEventListener("click", () => {
        for (const c of chips) c.setAttribute("aria-pressed", String(c === chip));
        applyFilter(chip.dataset.topic);
      });
    }

    function setRuby(card, open) {
      const button = card.querySelector(".toggle");
      const pre = card.querySelector(".ruby");
      pre.hidden = !open;
      button.setAttribute("aria-expanded", String(open));
      button.textContent = open ? "Hide Ruby" : "Ruby";
    }

    for (const card of cards) {
      card.querySelector(".toggle").addEventListener("click", () => {
        setRuby(card, card.querySelector(".ruby").hidden);
      });
    }

    if (allRuby) {
      allRuby.addEventListener("click", () => {
        const open = allRuby.getAttribute("aria-pressed") !== "true";
        allRuby.setAttribute("aria-pressed", String(open));
        allRuby.textContent = open ? "Hide Ruby on every card" : "Show Ruby on every card";
        for (const card of cards) setRuby(card, open);
      });
    }

    const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    const observer = new IntersectionObserver((entries) => {
      for (const e of entries) e.target.classList.toggle("live", e.isIntersecting);
    }, { threshold: 0.15 });

    function arm(stage) {
      if (!reduced) {
        const button = document.createElement("button");
        button.className = "replay";
        button.type = "button";
        button.title = "Restart the loop";
        button.setAttribute("aria-label", "Restart the loop");
        button.textContent = "↻";
        button.addEventListener("click", () => {
          const fresh = stage.cloneNode(true);
          fresh.querySelector(".replay").remove();
          observer.unobserve(stage);
          stage.replaceWith(fresh);
          arm(fresh);
          fresh.classList.add("live");
        });
        stage.appendChild(button);
      }
      observer.observe(stage);
    }

    for (const stage of $$(".cards .stage")) arm(stage);

    const fmt = (n) => n.toLocaleString("en-US");
    const seen = new Map();
    for (const card of cards) {
      const name = card.dataset.module;
      const slot = card.querySelector(".count");
      const c = counts(name);
      if (!c) {
        slot.textContent = name;
        continue;
      }
      slot.textContent = `${fmt(c.states)} states · ${fmt(c.paths)} replays`;
      seen.set(name, c);
    }
    if (totals && seen.size) {
      let states = 0;
      let paths = 0;
      for (const c of seen.values()) {
        states += c.states;
        paths += c.paths;
      }
      const n = seen.size;
      totals.textContent = `Across the ${n} module${n === 1 ? "" : "s"} on these cards, TLC visited ${fmt(states)} states and CRuby replayed ${fmt(paths)} paths; every replay passed.`;
    }
  }

  window.RXWidgets.rules = { mount };
})();
