// Mounts every element with data-rx-widget="name" by calling
// RXWidgets[name].mount(element, options), where options are the element's
// data attributes (data-model="RactorPorts" becomes options.model).
// Load it after the widget scripts.
(function () {
  "use strict";
  window.RXWidgets = window.RXWidgets || {};

  function mountAll() {
    document.querySelectorAll("[data-rx-widget]").forEach((el) => {
      if (el.dataset.rxMounted) return;
      const widget = window.RXWidgets[el.dataset.rxWidget];
      if (!widget) {
        console.error(`no widget named ${el.dataset.rxWidget}`);
        return;
      }
      el.dataset.rxMounted = "1";
      el.classList.add("rx-widget");
      widget.mount(el, { ...el.dataset });
    });
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", mountAll);
  else mountAll();
})();
