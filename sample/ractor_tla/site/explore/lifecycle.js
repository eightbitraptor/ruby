// Marks the state that a player shows on every state diagram of the same
// model.  Players dispatch "rx:state" on document with detail
// {model, state, prev}; a diagram carries data-rx-model and data-rx-nodes,
// the SVG node id of each core state (null when the diagram lacks it).  An
// edge between two nodes has the id "<from>--<to>".
(function () {
  "use strict";
  const marks = new Map();
  const nodeLists = new WeakMap();

  function nodesOf(figure) {
    if (!nodeLists.has(figure)) nodeLists.set(figure, JSON.parse(figure.dataset.rxNodes));
    return nodeLists.get(figure);
  }

  function find(figure, id) {
    return id ? figure.querySelector(`#${CSS.escape(id)}`) : null;
  }

  function clear(figure) {
    for (const element of marks.get(figure) || []) element.classList.remove("rx-here", "rx-step");
    marks.delete(figure);
    figure.classList.remove("rx-marked");
  }

  document.addEventListener("rx:state", (event) => {
    const { model, state, prev } = event.detail || {};
    if (!model) return;
    document.querySelectorAll(`figure[data-rx-nodes][data-rx-model="${CSS.escape(model)}"]`).forEach((figure) => {
      clear(figure);
      if (state == null) return;
      const nodes = nodesOf(figure);
      const here = find(figure, nodes[state]);
      if (!here) return;
      here.classList.add("rx-here");
      const marked = [here];
      if (prev != null && nodes[prev] && nodes[prev] !== nodes[state]) {
        const edge = find(figure, `${nodes[prev]}--${nodes[state]}`);
        if (edge) {
          edge.classList.add("rx-step");
          marked.push(edge);
        }
      }
      marks.set(figure, marked);
      figure.classList.add("rx-marked");
    });
  });
})();
