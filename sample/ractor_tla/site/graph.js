"use strict";

(() => {
  const MIN_SCALE = 0.02;
  const MAX_SCALE = 4;
  const STEP = 1.4;
  const DRAG_THRESHOLD = 4;

  const esc = (text) => String(text).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);

  class Viewer {
    constructor(root) {
      this.root = root;
      this.stage = root.querySelector(".gv-stage");
      this.canvas = root.querySelector(".gv-canvas");
      this.svg = this.canvas.querySelector("svg");
      this.zoomLabel = root.querySelector(".gv-zoom");
      this.panel = root.querySelector(".gv-panel");
      this.find = root.querySelector(".gv-find");
      this.w = Number(root.dataset.w);
      this.h = Number(root.dataset.h);
      this.svg.setAttribute("width", this.w);
      this.svg.setAttribute("height", this.h);
      this.scale = 1;
      this.x = 0;
      this.y = 0;
      this.userMoved = false;
      this.stageWidth = 0;
      this.selected = null;

      const json = root.querySelector(".gv-data");
      this.data = json ? JSON.parse(json.textContent) : null;
      if (this.data) {
        this.incoming = {};
        for (const [from, list] of Object.entries(this.data.out)) {
          for (const e of list) (this.incoming[e.to] ||= []).push({ from, edge: e.edge, calls: e.calls });
        }
      }

      root.querySelector(".gv-bar").addEventListener("click", (event) => this.onBar(event));
      this.stage.addEventListener("pointerdown", (event) => this.onPointerDown(event));
      this.stage.addEventListener("wheel", (event) => this.onWheel(event), { passive: false });
      this.stage.addEventListener("dblclick", (event) => {
        event.preventDefault();
        this.zoomAt(STEP * STEP, event.clientX, event.clientY);
      });
      if (this.panel) this.panel.addEventListener("click", (event) => this.onPanel(event));
      if (this.find) {
        this.find.addEventListener("input", () => this.onFind());
        this.find.addEventListener("keydown", (event) => {
          if (event.key !== "Enter") return;
          const first = this.root.querySelector("g.node.match");
          if (first) this.select(first.id, true);
        });
      }
      document.addEventListener("fullscreenchange", () => {
        this.root.querySelector('[data-act="full"]').textContent = document.fullscreenElement === this.root ? "Exit full screen" : "Full screen";
        this.userMoved = false;
        requestAnimationFrame(() => this.fit());
      });
      new ResizeObserver(() => {
        const width = this.stage.clientWidth;
        if (width === this.stageWidth) return;
        this.stageWidth = width;
        if (!this.userMoved) this.fit();
      }).observe(this.stage);
    }

    fullscreen() {
      return document.fullscreenElement === this.root;
    }

    apply() {
      this.canvas.style.transform = `translate(${this.x}px, ${this.y}px) scale(${this.scale})`;
      this.zoomLabel.textContent = `${Math.round(this.scale * 100)}%`;
    }

    fit() {
      const width = this.stage.clientWidth;
      if (width === 0) return;
      let height;
      if (this.fullscreen()) {
        this.stage.style.height = "";
        height = this.stage.clientHeight;
        this.scale = Math.min(width / this.w, height / this.h, 1);
      } else {
        const maxHeight = Math.max(240, window.innerHeight * 0.75);
        this.scale = Math.min(width / this.w, maxHeight / this.h, 1);
        height = Math.max(160, Math.ceil(this.h * this.scale));
        this.stage.style.height = `${height}px`;
      }
      this.x = (width - this.w * this.scale) / 2;
      this.y = (height - this.h * this.scale) / 2;
      this.apply();
    }

    zoomAt(factor, clientX, clientY) {
      const rect = this.stage.getBoundingClientRect();
      const cx = clientX - rect.left;
      const cy = clientY - rect.top;
      const scale = Math.min(MAX_SCALE, Math.max(MIN_SCALE, this.scale * factor));
      this.x = cx - (cx - this.x) * (scale / this.scale);
      this.y = cy - (cy - this.y) * (scale / this.scale);
      this.scale = scale;
      this.userMoved = true;
      this.apply();
    }

    zoomCenter(factor) {
      const rect = this.stage.getBoundingClientRect();
      this.zoomAt(factor, rect.left + rect.width / 2, rect.top + rect.height / 2);
    }

    onBar(event) {
      const button = event.target.closest("button");
      if (!button) return;
      switch (button.dataset.act) {
        case "fit": this.userMoved = false; this.fit(); break;
        case "in": this.zoomCenter(STEP); break;
        case "out": this.zoomCenter(1 / STEP); break;
        case "one": this.zoomCenter(1 / this.scale); break;
        case "full":
          if (this.fullscreen()) document.exitFullscreen();
          else this.root.requestFullscreen();
          break;
      }
    }

    onPointerDown(event) {
      if (event.button !== 0) return;
      const startX = event.clientX;
      const startY = event.clientY;
      const originX = this.x;
      const originY = this.y;
      let dragging = false;
      const move = (e) => {
        const dx = e.clientX - startX;
        const dy = e.clientY - startY;
        if (!dragging && Math.hypot(dx, dy) < DRAG_THRESHOLD) return;
        if (!dragging) {
          dragging = true;
          this.stage.setPointerCapture(event.pointerId);
          this.stage.classList.add("dragging");
        }
        this.x = originX + dx;
        this.y = originY + dy;
        this.userMoved = true;
        this.apply();
      };
      const up = (e) => {
        this.stage.removeEventListener("pointermove", move);
        this.stage.removeEventListener("pointerup", up);
        this.stage.classList.remove("dragging");
        if (dragging || !this.data) return;
        const node = e.target.closest && e.target.closest("g.node");
        if (node && this.data.nodes[node.id]) this.select(node.id, false);
        else this.clear();
      };
      this.stage.addEventListener("pointermove", move);
      this.stage.addEventListener("pointerup", up);
    }

    onWheel(event) {
      if (!(event.ctrlKey || event.metaKey || this.fullscreen())) return;
      event.preventDefault();
      this.zoomAt(Math.exp(-event.deltaY * 0.002), event.clientX, event.clientY);
    }

    element(id) {
      return this.svg.querySelector(`#${CSS.escape(id)}`);
    }

    clear() {
      this.selected = null;
      this.root.classList.remove("focus");
      for (const e of this.svg.querySelectorAll(".sel, .hot, .src")) e.classList.remove("sel", "hot", "src");
      if (this.panel) this.panel.hidden = true;
    }

    select(id, center) {
      this.clear();
      this.selected = id;
      this.root.classList.add("focus");
      this.element(id)?.classList.add("sel");
      const out = this.data.out[id] || [];
      const inc = this.incoming[id] || [];
      for (const e of out) {
        this.element(e.edge)?.classList.add("hot");
        this.element(e.to)?.classList.add("hot");
      }
      for (const e of inc) {
        this.element(e.edge)?.classList.add("src");
        this.element(e.from)?.classList.add("src");
      }
      this.renderPanel(id, out, inc);
      if (center) this.centerOn(id);
    }

    centerOn(id) {
      const target = this.element(id);
      if (!target) return;
      if (this.scale < 0.6) {
        const rect = this.stage.getBoundingClientRect();
        this.zoomAt(0.6 / this.scale, rect.left + rect.width / 2, rect.top + rect.height / 2);
      }
      const box = target.getBoundingClientRect();
      const rect = this.stage.getBoundingClientRect();
      this.x += rect.left + rect.width / 2 - (box.left + box.width / 2);
      this.y += rect.top + rect.height / 2 - (box.top + box.height / 2);
      this.userMoved = true;
      this.apply();
    }

    renderPanel(id, out, inc) {
      const node = this.data.nodes[id];
      const loops = this.data.loops[id] || [];
      const link = (to, calls) =>
        `<li><button type="button" data-go="${esc(to)}">${esc(this.data.nodes[to].lines.join("  "))}</button>` +
        `<ul>${calls.map((c) => `<li><code>${esc(c)}</code></li>`).join("")}</ul></li>`;
      const parts = [
        `<button type="button" class="gv-close" data-close aria-label="Close">×</button>`,
        `<p class="label">${node.initial ? "Initial state" : "State"}</p>`,
        `<pre>${esc(node.lines.join("\n"))}</pre>`,
        `<p class="label">Calls that change the state (${out.reduce((n, e) => n + e.calls.length, 0)})</p>`,
        out.length ? `<ul class="gv-links out">${out.map((e) => link(e.to, e.calls)).join("")}</ul>` : `<p class="muted">none</p>`,
      ];
      if (loops.length) {
        parts.push(`<p class="label">Calls that change nothing (${loops.length})</p>`);
        parts.push(`<ul class="gv-loops">${loops.map((c) => `<li><code>${esc(c)}</code></li>`).join("")}</ul>`);
      }
      parts.push(`<p class="label">Reached from (${inc.length})</p>`);
      parts.push(inc.length ? `<ul class="gv-links in">${inc.map((e) => link(e.from, e.calls)).join("")}</ul>` : `<p class="muted">no other state</p>`);
      this.panel.innerHTML = parts.join("");
      this.panel.hidden = false;
      this.panel.scrollTop = 0;
    }

    onPanel(event) {
      if (event.target.closest("[data-close]")) {
        this.clear();
        return;
      }
      const go = event.target.closest("[data-go]");
      if (go) this.select(go.dataset.go, true);
    }

    onFind() {
      const query = this.find.value.trim().toLowerCase();
      for (const e of this.svg.querySelectorAll("g.node.match")) e.classList.remove("match");
      this.root.classList.toggle("finding", query !== "");
      if (query === "") return;
      for (const [id, node] of Object.entries(this.data.nodes)) {
        if (node.lines.join("\n").toLowerCase().includes(query)) this.element(id)?.classList.add("match");
      }
    }
  }

  const start = () => document.querySelectorAll("figure.gv").forEach((root) => new Viewer(root));
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
