# frozen_string_literal: true

# Builds a static site that explains Ractors from the TLA+ models in this
# directory.  Every table, trace, and graph comes from the TLC state graphs
# of this run, and every transition is replayed on CRuby before the site is
# written, so the site cannot show a rule that this build does not follow.
#
#   cd build
#   TLA2TOOLS_JAR=/path/tla2tools.jar ../tool/runruby.rb --srcdir=.. --archdir=. -- \
#     ../sample/ractor_tla/site/build_site.rb --out=/tmp/ractor-site
#
# Requires Graphviz `dot` on PATH in addition to the verifier requirements.

require "cgi"
require "json"
require "fileutils"
require "open3"
require "tmpdir"
require_relative "../tlc_tools"
require_relative "../verify"
require_relative "../api/verify_api"
require_relative "markdown"
require_relative "model_data"

module RactorSite
  SAMPLE_DIR = File.expand_path("..", __dir__)
  API_DIR = File.join(SAMPLE_DIR, "api")
  HARNESS = File.join(API_DIR, "api_harness.rb")
  SITE_DIR = __dir__
  EXPLORE_DIR = File.join(SITE_DIR, "explore")

  # Stylesheets and scripts of each widget, in load order, from explore/.
  WIDGET_ASSETS = {
    "theatre" => [%w[scene.css theatre.css], %w[scene.js theatre.js lifecycle.js]],
    "explorer" => [%w[scene.css explorer.css], %w[scene.js explorer.js]],
    "multiverse" => [%w[multiverse.css], %w[multiverse.js]],
    "rules" => [%w[rules.css], %w[rules.js]],
    "timeline" => [%w[timeline.css], %w[timeline.js]]
  }.freeze
  WIDGET_SHARED = %w[rx.css common.js widgets.js].freeze
  RULES_START = "<!-- rx:rules:start -->"
  RULES_END = "<!-- rx:rules:end -->"

  Failure = RactorTLA::Failure
  Def = Struct.new(:name, :params, :comment, :body, :line)
  Spec = Struct.new(:name, :path, :header, :defs, :variables, :invariants, :properties, :actions, :constants, :text, :arg_names, :targets)

  SUMMARIES = {
    "RactorPorts" => "Ports, send, receive, close, and Ractor.select",
    "RactorTermination" => "monitor, join, value, and the successor rule",
    "RactorTransfer" => "Copy, move, and shareable objects",
    "RactorIsolation" => "Which Ractor may touch classes, constants, and globals",
    "RactorLocal" => "Ractor-local storage",
    "RactorBlocking" => "Blocking calls and the events that end them",
    "RactorOrder" => "Message order and the choice among ready sources",
    "RactorProcess" => "Exit, fork, and signals with Ractors",
    "RactorPool" => "A complete worker-pool protocol"
  }.freeze

  module_function

  def fail!(message)
    raise Failure, message
  end

  def h(text)
    CGI.escapeHTML(text.to_s)
  end

  def slug(text)
    text.to_s.gsub(/[^A-Za-z0-9]+/, "-").gsub(/\A-|-\z/, "")
  end

  # ---------------------------------------------------------------- spec parsing

  def comment_text(lines)
    lines.map { |l| (l[/\A\s*\(\*+(.*?)\*+\)\s*\z/, 1] || l.sub(/\A\s*\(\*+/, "").sub(/\*+\)\s*\z/, "")).strip }
         .join("\n").gsub(/\n{3,}/, "\n\n").strip
  end

  # For each action, one map per argument position from a value to the
  # constant name the spec uses for it, read from the `\E x \in S` bindings
  # in Next.  A position whose domain is a set of tuples maps whole tuples.
  def argument_names(defs, next_body)
    numbers = defs.values.filter_map do |d|
      m = d.params.nil? && d.body.match(/\A\w+ == (-?\d+)\s*\z/)
      [d.name, Integer(m[1], 10)] if m
    end.to_h
    domain = lambda do |set|
      body = defs[set]&.body.to_s.split("==", 2).last.to_s.strip
      if (tuples = body.scan(/<<([^>]*)>>/)).any?
        tuples.to_h do |(inner)|
          names = inner.split(",").map(&:strip)
          [names.map { |n| numbers[n] }, "<<#{names.join(", ")}>>"]
        end
      elsif body.match?(/\A\{[\w,\s]+\}\z/)
        body[1...-1].split(",").map(&:strip).filter_map { |n| [numbers[n], n] if numbers.key?(n) }.to_h
      else
        {}
      end
    end
    names = {}
    next_body.each_line do |line|
      m = line.match(/\\E (.*?) : (\w+)\(([^)]*)\)/) or next
      bound = m[1].scan(/(\w+) \\in (\w+)/).to_h
      names[m[2]] = m[3].split(",").map { |param| bound[param.strip] ? domain.(bound[param.strip]) : {} }
    end
    names
  end

  def parse_spec(path, cfg_path)
    text = File.read(path)
    lines = text.lines(chomp: true)
    name = lines.first[/MODULE (\w+)/, 1] or fail!("#{path}: no MODULE line")
    header_lines = lines[1..].take_while { |l| l.start_with?("(*") }
    starts = []
    lines.each_with_index do |line, i|
      match = line.match(/\A([A-Za-z_]\w*)(?:\(([^)]*)\))?\s*==/)
      starts << [i, match[1], match[2]] if match
    end
    defs = {}
    starts.each_with_index do |(i, dname, params), k|
      stop = k + 1 < starts.size ? starts[k + 1][0] : lines.size
      body = lines[i...stop]
      body.pop while body.any? && (body.last.strip.empty? || body.last.start_with?("(*") ||
                                   body.last.match?(/\A[-=]{4,}/) || body.last.start_with?("===="))
      comment = []
      j = i - 1
      while j >= 0 && lines[j].start_with?("(*")
        comment.unshift(lines[j])
        j -= 1
      end
      comment = [] if j < header_lines.size + 1 && comment == header_lines
      defs[dname] = Def.new(dname, params, comment_text(comment), body.join("\n"), i + 1)
    end
    variables = []
    if (vi = lines.index { |l| l.start_with?("VARIABLES") })
      lines[(vi + 1)..].each do |l|
        break if l.strip.empty?
        m = l.match(/\A\s*(\w+),?\s*(?:\\\*\s*(.*))?\z/) or next
        variables << [m[1], m[2].to_s]
      end
    end
    cfg = File.read(cfg_path)
    section = lambda do |key|
      body = cfg[/^#{key}\s*\n((?:[ \t]+\S.*\n?)+)/, 1].to_s
      body.split.map(&:strip)
    end
    next_body = defs.fetch("Next").body
    actions = next_body.scan(/(?:\\\/\s+|:\s*)([A-Z][A-Za-z0-9]*)\b/).flatten.uniq.select { |a| defs.key?(a) }
    constants = defs.values.select { |d| d.params.nil? && d.body.lines.size == 1 &&
                                          d.body.match?(/==\s*(-?\d+|<<[^>]*>>|\{[^}]*\})\s*\z/) }
    Spec.new(name, path, comment_text(header_lines), defs, variables, section.("INVARIANTS"),
             section.("PROPERTIES"), actions, constants, text, argument_names(defs, next_body))
  end

  # ---------------------------------------------------------- TLA+ highlighting

  KEYWORDS = %w[IF THEN ELSE CASE OTHER LET IN EXCEPT CHOOSE UNCHANGED ENABLED SUBSET UNION DOMAIN
                EXTENDS VARIABLES VARIABLE CONSTANTS CONSTANT MODULE ASSUME TRUE FALSE BOOLEAN WF_vars SF_vars].freeze
  TOKEN = %r{
    (?<comment>\\\*.*)
    |(?<string>"(?:[^"\\]|\\.)*")
    |(?<op>/\\|\\/|=>|\[\]|<>|\|->|==|/=|\\in\b|\\notin\b|\\E\b|\\A\b|\\cup\b|\\cap\b|\\subseteq\b|\\o\b|~|')
    |(?<word>[A-Za-z_][A-Za-z0-9_]*)
    |(?<num>\d+)
    |(?<other>.)
  }x

  # targets maps each defined name to the id of the element that shows it.
  # With anchors, each definition line gets the id src-Name.
  def highlight(code, targets, anchors: false)
    in_block = false
    code.lines(chomp: true).map do |line|
      out = +""
      if anchors && !in_block && (name = line[/\A([A-Za-z_]\w*)(?:\([^)]*\))?\s*==/, 1]) && targets.key?(name)
        out << %(<span id="src-#{name}"></span>)
      end
      rest = line
      until rest.empty?
        if in_block || rest.start_with?("(*")
          close = rest.index("*)")
          if close
            out << %(<span class="c">#{h(rest[0, close + 2])}</span>)
            rest = rest[(close + 2)..]
            in_block = false
          else
            out << %(<span class="c">#{h(rest)}</span>)
            rest = ""
            in_block = true
          end
          next
        end
        stop = rest.index("(*") || rest.size
        chunk = rest[0, stop]
        rest = rest[stop..]
        chunk.scan(TOKEN) do
          m = Regexp.last_match
          out << if m[:comment] then %(<span class="c">#{h(m[0])}</span>)
                 elsif m[:string] then %(<span class="s">#{h(m[0])}</span>)
                 elsif m[:op] then %(<span class="o">#{h(m[0])}</span>)
                 elsif m[:word] && KEYWORDS.include?(m[0]) then %(<span class="k">#{m[0]}</span>)
                 elsif m[:word] && targets.key?(m[0]) then %(<a class="d" href="##{targets[m[0]]}">#{m[0]}</a>)
                 elsif m[:num] then %(<span class="n">#{m[0]}</span>)
                 else h(m[0])
                 end
        end
      end
      out
    end.join("\n")
  end

  def tla_block(spec, def_)
    %(<pre class="tla">#{highlight(def_.body, spec.targets)}</pre>)
  end

  def source_block(spec)
    %(<pre class="tla">#{highlight(spec.text, spec.targets, anchors: true)}</pre>)
  end

  # Names shown in their own section link there; every other name links to
  # its line in the source listing.
  def assign_targets(spec, sectioned)
    spec.targets = spec.defs.keys.to_h { |n| [n, sectioned.include?(n) ? "def-#{n}" : "src-#{n}"] }
  end

  # --------------------------------------------------------------- value helpers

  def fmt(value)
    case value
    when Array then "<<#{value.map { |v| fmt(v) }.join(", ")}>>"
    when Hash then "[#{value.map { |k, v| "#{k} |-> #{fmt(v)}" }.join(", ")}]"
    when Set then "{#{value.to_a.map { |v| fmt(v) }.join(", ")}}"
    when String then %("#{value}")
    when true then "TRUE"
    when false then "FALSE"
    else value.to_s
    end
  end

  def parse(text)
    RactorTLA::Value.parse(text)
  end

  def components(value)
    case value
    when Array then value.each_with_index.map { |v, i| [i + 1, v] }
    when Hash then value.to_a
    end
  end

  # -------------------------------------------------------------------- graphviz

  Rendered = Struct.new(:svg, :raw, :width, :height)

  def render_dot(source)
    raw, err, status = Open3.capture3("dot", "-Tsvg", stdin_data: source)
    fail!("dot failed: #{err}") unless status.success?
    size = raw.match(/<svg width="([\d.]+)pt" height="([\d.]+)pt"/) or fail!("dot wrote no SVG size")
    svg = raw.sub(/\A.*?(?=<svg)/m, "").gsub(/<!--.*?-->\n?/m, "")
    Rendered.new(svg, raw, Float(size[1]), Float(size[2]))
  end

  def dot_svg(source)
    render_dot(source).svg
  end

  # Renders the graph left to right and top to bottom and keeps the layout
  # whose shape is closer to a 16:10 screen.
  def best_layout
    %w[LR TB].map { |dir| render_dot(yield(dir)) }
             .min_by { |g| (Math.log(g.width / g.height) - Math.log(1.6)).abs }
  end

  def next_graph_id
    @graph_seq = (@graph_seq || 0) + 1
    "g#{@graph_seq}"
  end

  INLINE_WIDTH = 760

  # A small graph without state data is drawn inline.  Any other graph gets
  # the viewer of graph.js: fitted to the page, with zoom, pan, full screen,
  # and, when data is given, a panel that shows the state a click selects.
  def graph_view(graph, caption: nil, legend: nil, data: nil, attrs: "")
    cap = caption ? "<figcaption>#{caption}</figcaption>" : ""
    return %(<figure class="graph"#{attrs}>#{cap}#{graph.svg}</figure>) if data.nil? && graph.width <= INLINE_WIDTH
    @graph_files << graph.raw
    file = "graphs/g#{@graph_files.size}.svg"
    json = data && %(<script type="application/json" class="gv-data">#{JSON.generate(data).gsub("</", "<\\/")}</script>)
    find = data && %(<input type="search" class="gv-find" placeholder="Find a state" aria-label="Find a state">)
    hint = data ? "Click a state for its calls. " : ""
    <<~HTML
      <figure class="gv" data-w="#{(graph.width * 4 / 3).round}" data-h="#{(graph.height * 4 / 3).round}"#{attrs}>
      #{cap}#{legend && %(<p class="gv-legend">#{legend}</p>)}
      <div class="gv-bar"><button type="button" data-act="fit">Fit</button><button type="button" data-act="out" aria-label="Zoom out">−</button><span class="gv-zoom"></span><button type="button" data-act="in" aria-label="Zoom in">+</button><button type="button" data-act="one">1:1</button><button type="button" data-act="full">Full screen</button><a href="#{file}" target="_blank" rel="noopener">Open SVG</a>#{find}<span class="gv-hint">#{hint}Drag to pan. Ctrl+scroll or double-click to zoom.</span></div>
      <div class="gv-stage"><div class="gv-canvas">#{graph.svg}</div></div>
      #{data && '<div class="gv-panel" hidden></div>'}
      #{json}
      </figure>
    HTML
  end

  # Lets lifecycle.js find the node of each state: data-rx-nodes lists, for
  # every core state in site order, the SVG id of its node or null.
  def node_attrs(model, node_ids)
    %( data-rx-model="#{h(model)}" data-rx-nodes="#{h(JSON.generate(node_ids))}")
  end

  def short_value(text)
    text.gsub("TRUE", "T").gsub("FALSE", "F").delete('"').gsub(", ", ",")
  end

  def op_summary(calls)
    calls.map { |c| c[/\A\w+/] }.tally.map { |op, n| n > 1 ? "#{op} ×#{n}" : op }.each_slice(2).map { |s| s.join(", ") }.join("\\n")
  end

  def dq(text)
    %("#{text.to_s.gsub("\\", "\\\\\\\\").gsub('"', '\"')}")
  end

  def graph_header(rankdir = "LR")
    <<~DOT
      digraph G {
        rankdir=#{rankdir}; bgcolor="transparent"; nodesep=0.3; ranksep=0.5;
        node [shape=box, style="rounded", fontname="Helvetica", fontsize=10, margin="0.08,0.04"];
        edge [fontname="Helvetica", fontsize=9, arrowsize=0.6];
    DOT
  end

  # ---------------------------------------------------------------- API analysis

  ApiData = Struct.new(:mod, :spec, :states, :cores, :transitions, :paths, :ruby, :coverage, keyword_init: true)

  def step_text(spec, step)
    maps = spec.arg_names.fetch(step["op"], [])
    args = step["args"]
    shown = if maps.size == 1 && maps.first.key?(args)
              [maps.first[args]]
            else
              args.each_with_index.map { |a, i| maps[i]&.fetch(a, nil) || fmt(a) }
            end
    "#{step["op"]}(#{shown.join(", ")})"
  end

  def bfs_parents(cores, transitions)
    outgoing = Hash.new { |hash, k| hash[k] = [] }
    transitions.each { |t| outgoing[t.from] << t }
    parent = {}
    queue = cores.values.select(&:initial).map(&:key)
    queue.each { |k| parent[k] = nil }
    until queue.empty?
      key = queue.shift
      outgoing[key].each do |t|
        next if parent.key?(t.to)
        parent[t.to] = t
        queue << t.to
      end
    end
    parent
  end

  def route(parent, key)
    steps = []
    while (t = parent[key])
      steps.unshift(t)
      key = t.from
    end
    steps
  end

  def harness_ruby(module_name)
    src = File.read(HARNESS)
    start = src.index(/^    module #{module_name}\n/) or return {}
    stop = src.index(/^    module Ractor\w+\n/, start + 1) || src.size
    lines = src[start...stop].lines(chomp: true)
    calls = Hash.new { |hash, k| hash[k] = [] }
    lines.each_with_index do |line, i|
      m = line.match(/\A(\s*)when "([A-Z]\w*)"(?: then (.*))?\z/) or next
      indent, op, expr = m[1], m[2], m[3]
      unless expr
        body = lines[(i + 1)..].take_while { |l| l.start_with?("#{indent}  ") }
        expr = body.map { |l| l.delete_prefix("#{indent}  ") }.join("\n")
      end
      calls[op] << expr unless expr.empty? || calls[op].include?(expr)
    end
    calls
  end

  def analyze_api(mod, dir, jar, java, ruby, scale, jobs)
    spec = parse_spec(File.join(API_DIR, "#{mod.name}.tla"), File.join(API_DIR, "#{mod.name}.cfg"))
    assign_targets(spec, spec.actions + spec.invariants + spec.properties + ["Init"])
    states, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: spec.path,
                                    config: File.join(API_DIR, "#{mod.name}.cfg"), options: mod.options,
                                    scale: scale, timeout: RactorAPIVerifier::TLC_TIMEOUT, tag: "site-#{mod.name}")
    graph = RactorTLA::Dot.parse(dot, variables: mod.variables, stutter: false, sinks: mod.sinks?)
    fail!("#{mod.name}: DOT has #{graph.nodes.size} nodes, TLC found #{states}") unless graph.nodes.size == states
    cores, transitions = RactorAPIVerifier.quotient(graph, mod.variables)
    paths = RactorAPIVerifier.paths_for(cores, transitions, mod.observables)
    RactorAPIVerifier.check_counts(mod, states, cores, transitions, paths)
    coverage = RactorAPIVerifier.replay(mod, paths, ruby, dir, scale, jobs)
    warn "site: #{mod.name} states=#{states} transitions=#{transitions.size} replays=#{paths.size} PASS"
    ApiData.new(mod: mod, spec: spec, states: states, cores: cores, transitions: transitions,
                paths: paths.size, ruby: harness_ruby(mod.name), coverage: coverage)
  end

  # ------------------------------------------------------------------ HTML shell

  def nav(pages, current)
    links = pages.map do |file, title|
      cls = file == current ? ' class="here"' : ""
      %(<a href="#{file}"#{cls}>#{h(title)}</a>)
    end
    %(<nav>#{links.join}</nav>)
  end

  # A link into the collapsed source listing must open it first.
  OPEN_TARGET = <<~JS
    function openTarget() {
      const target = document.getElementById(decodeURIComponent(location.hash.slice(1)));
      if (!target) return;
      for (let e = target.parentElement; e; e = e.parentElement) if (e.tagName === "DETAILS") e.open = true;
      target.scrollIntoView();
    }
    addEventListener("hashchange", openTarget);
    addEventListener("DOMContentLoaded", openTarget);
  JS

  # widgets: names from WIDGET_ASSETS used on the page; data: scripts under
  # rx/data/ (without .js) that the widgets read.
  def page(pages, file, title, body, replay_ruby, widgets: [], data: [])
    css = widgets.flat_map { |w| WIDGET_ASSETS.fetch(w)[0] }.uniq
    js = widgets.flat_map { |w| WIDGET_ASSETS.fetch(w)[1] }.uniq
    head = widgets.empty? ? "" : ["rx.css", *css].map { |f| %(<link rel="stylesheet" href="rx/#{f}">) }.join("\n")
    tail = if widgets.empty? then ""
           else
             (data.map { |d| %(<script src="rx/data/#{d}.js"></script>) } +
              ["common.js", *js, "widgets.js"].map { |f| %(<script src="rx/#{f}"></script>) }).join("\n")
           end
    <<~HTML
      <!doctype html>
      <html lang="en">
      <head>
      <meta charset="utf-8">
      <meta name="viewport" content="width=device-width, initial-scale=1">
      <title>#{h(title)} · Ractors, formally</title>
      <link rel="stylesheet" href="style.css">
      #{head}
      <script src="graph.js" defer></script>
      <script>#{OPEN_TARGET}</script>
      </head>
      <body>
      <header><a class="brand" href="index.html">Ractors, formally</a>#{nav(pages, file)}</header>
      <main>
      #{body}
      </main>
      <footer>Generated by <code>sample/ractor_tla/site/build_site.rb</code> from the TLA+ models and the TLC state graphs of one run. The CRuby replays ran on #{h(replay_ruby)} before the site was written. Each model page states how many replays passed and how many choice outcomes CRuby never took.</footer>
      #{tail}
      </body>
      </html>
    HTML
  end

  def widget(name, **options)
    attrs = options.map { |k, v| v == true ? %( data-#{k}) : %( data-#{k}="#{h(v)}") }.join
    %(<div data-rx-widget="#{name}"#{attrs}></div>)
  end

  # The story player at the top of a model page, with a link to the explorer.
  def watch_section(name)
    <<~HTML
      <section class="watch" id="highlights">
      <h2>Highlights</h2>
      <p>Each highlight plays one short path through the model. The highlights show a few chosen outcomes; <a href="#calls">Calls</a> has an example of every outcome, each with its own ▶ Play button. While a path plays, the <a href="#lifecycles">lifecycle diagrams</a> mark the current state. <a href="explorer.html?model=#{name}">Choose the steps yourself in the explorer.</a></p>
      #{widget("theatre", model: name, calls: "#calls", lifecycles: "#lifecycles", dock: true)}
      </section>
    HTML
  end

  def para(text)
    text.split(/\n{2,}/).map { |p| "<p>#{inline_code(h(p.gsub(/\s*\n\s*/, " ")))}</p>" }.join("\n")
  end

  def inline_code(html)
    html.gsub(/`([^`]+)`/, '<code>\1</code>')
  end

  def details(summary, body, open: false)
    %(<details#{open ? " open" : ""}><summary>#{summary}</summary>\n#{body}\n</details>)
  end

  def choice_stats(coverage)
    return "" unless coverage[:choices].positive?
    %(<div><b>#{coverage[:choices]}</b><span>choice transitions</span></div>) +
      %(<div><b>#{coverage[:diverged]}</b><span>replays that met another allowed choice</span></div>) +
      %(<div><b>#{coverage[:untaken]}</b><span>transitions CRuby never took</span></div>)
  end

  def untaken_note(coverage)
    return "" unless coverage[:untaken].positive?
    " Where a call has several allowed outcomes, CRuby takes one of them. #{coverage[:untaken]} transitions " \
      "are outcomes that the model allows and that no replay observed."
  end

  def outcome_badge(outcome)
    cls = case outcome
          when "ok", "true", "false", "nil", "blocks", "runs", "exit" then "ok"
          else "err"
          end
    %(<span class="badge #{cls}">#{h(outcome)}</span>)
  end

  # --------------------------------------------------------------- API sections

  def variables_table(spec)
    rows = spec.variables.map { |n, c| "<tr><td><code>#{h(n)}</code></td><td>#{inline_code(h(c))}</td></tr>" }
    %(<table><thead><tr><th>Variable</th><th>Meaning</th></tr></thead><tbody>#{rows.join}</tbody></table>)
  end

  def constants_table(spec)
    rows = spec.constants.map do |d|
      value = d.body.split("==", 2).last.strip
      "<tr><td><code>#{h(d.name)}</code></td><td><code>#{h(value)}</code></td></tr>"
    end
    %(<table class="compact"><thead><tr><th>Name</th><th>Value</th></tr></thead><tbody>#{rows.join}</tbody></table>)
  end

  def property_section(spec, names, kind)
    names.map do |n|
      d = spec.defs[n] or next ""
      <<~HTML
        <div class="prop" id="def-#{n}">
        <h4>#{h(n)} <span class="tag">#{kind}</span></h4>
        #{para(d.comment)}
        #{tla_block(spec, d)}
        </div>
      HTML
    end.join
  end

  def diff_fields(before, after)
    after.filter_map do |name, value|
      next if name == "aspect" || before[name] == value
      "<code>#{h(name)}</code>: <code>#{h(before[name])}</code> → <code>#{h(value)}</code>"
    end
  end

  def trace_html(data, steps, aspect)
    first = data.cores.fetch(steps.first.from)
    init = first.fields.reject { |k, _| k == "aspect" }.map { |k, v| "<code>#{h(k)} = #{h(v)}</code>" }.join("<br>")
    rows = steps.each_with_index.map do |t, i|
      before = data.cores.fetch(t.from).fields
      after = data.cores.fetch(t.to).fields
      changes = diff_fields(before, after)
      changes = ["<em>no change</em>"] if changes.empty?
      last = i == steps.size - 1 ? ' class="focus"' : ""
      value = t.step["value"].to_s.empty? ? "" : " <code>#{h(t.step["value"])}</code>"
      value += t.step.fetch("wakes", []).map do |w|
        "<br>wakes thread #{w["thread"]}: #{outcome_badge(w["outcome"])}#{w["value"].to_s.empty? ? "" : " <code>#{h(w["value"])}</code>"}"
      end.join
      "<tr#{last}><td>#{i + 1}</td><td><code>#{h(step_text(data.spec, t.step))}</code></td><td>#{outcome_badge(t.step["outcome"])}#{value}</td><td>#{changes.join("<br>")}</td></tr>"
    end
    <<~HTML
      <p class="init">Aspect <code>#{h(aspect)}</code>. Initial state:<br>#{init}</p>
      <table class="trace"><thead><tr><th>#</th><th>Call</th><th>Outcome</th><th>State change</th></tr></thead><tbody>#{rows.join}</tbody></table>
    HTML
  end

  def action_section(data)
    spec = data.spec
    parent = bfs_parents(data.cores, data.transitions)
    index = data.transitions.each_with_index.to_h.compare_by_identity
    several = data.cores.values.map { |c| c.fields.fetch("aspect") }.uniq.size > 1
    aspect_of = ->(key) { parse(data.cores.fetch(key).fields.fetch("aspect")) }
    by_op = data.transitions.group_by { |t| t.step["op"] }
    spec.actions.map do |action|
      d = spec.defs.fetch(action)
      ts = by_op.fetch(action, [])
      writes = ts.flat_map { |t| diff_fields(data.cores[t.from].fields, data.cores[t.to].fields).map { |c| c[%r{<code>([^<]+)</code>}, 1] } }.uniq
      groups = ts.group_by { |t| [aspect_of.(t.from), t.step["outcome"]] }.sort_by { |(a, o), _| [a.to_s, o] }
      rows = groups.map do |(aspect, outcome), list|
        loops = list.count { |t| t.from == t.to }
        witness = list.min_by { |t| [route(parent, t.from).size, step_text(data.spec, t.step)] }
        steps = route(parent, witness.from) + [witness]
        change = if loops == list.size then %(<span class="muted">never</span>)
                 elsif loops.zero? then "always"
                 else "#{list.size - loops} of #{list.size}"
                 end
        calls = "#{steps.size} call#{steps.size == 1 ? "" : "s"}"
        trace = %(<details class="witness"><summary title="The steps of the shortest example, as a table">) +
                %(Steps <span class="n">#{calls}</span></summary>\n#{trace_html(data, steps, aspect)}\n</details>)
        label = "#{action} → #{outcome}#{several ? " · scenario #{aspect}" : ""}"
        play = %(<button type="button" class="rx-play" data-rx-play data-model="#{h(spec.name)}" ) +
               %(data-path="#{steps.map { |t| index.fetch(t) }.join(",")}" data-label="#{h(label)}">▶ Play</button>)
        "<tr><td><code>#{h(aspect)}</code></td><td>#{outcome_badge(outcome)}</td><td class=\"num\">#{list.size}</td><td>#{change}</td><td><div class=\"example\">#{play}#{trace}</div></td></tr>"
      end
      ruby = data.ruby.fetch(action, [])
      ruby_html = if ruby.empty?
                    action == "FinishWorker" || action == "Finish" ? %(<p class="muted">The harness ends the Ractor's block (<code>finish_worker</code>).</p>) : ""
                  else
                    %(<p class="label">Ruby that the replay harness runs</p><pre class="ruby">#{h(ruby.join("\n"))}</pre>)
                  end
      <<~HTML
        <section class="action" id="def-#{action}">
        <h3>#{h(action)}#{d.params ? "<span class=\"params\">(#{h(d.params)})</span>" : ""}</h3>
        #{para(d.comment)}
        <div class="cols">
        <div>#{tla_block(spec, d)}</div>
        <div>#{ruby_html}
        <p class="label">Variables it can change</p>
        <p>#{writes.empty? ? '<span class="muted">none: every outcome leaves the state alone</span>' : writes.map { |w| "<code>#{h(w)}</code>" }.join(" ")}</p>
        </div>
        </div>
        <table class="outcomes"><thead><tr><th>Aspect</th><th>Outcome</th><th>Transitions</th><th>Changes state</th><th>Example</th></tr></thead><tbody>#{rows.join}</tbody></table>
        </section>
      HTML
    end.join
  end

  def projection_graphs(data)
    spec = data.spec
    names = spec.variables.map(&:first) - %w[aspect step]
    names.flat_map do |name|
      values = data.cores.values.map { |c| parse(c.fields.fetch(name)) }
      sample = values.first
      parts = components(sample)
      targets = if parts && parts.size <= 4
                  parts.map { |key, _| ["#{name}[#{key}]", ->(v) { v.is_a?(Array) ? v[key - 1] : v[key] }] }
                else
                  [[name, ->(v) { v }]]
                end
      targets.filter_map do |title, pick|
        edges = Hash.new { |hash, k| hash[k] = Set.new }
        nodes = Set.new
        initial = Set.new
        data.cores.each_value do |c|
          v = fmt(pick.(parse(c.fields.fetch(name))))
          nodes << v
          initial << v if c.initial
        end
        next if nodes.size > 24 || nodes.size < 2
        data.transitions.each do |t|
          a = fmt(pick.(parse(data.cores[t.from].fields.fetch(name))))
          b = fmt(pick.(parse(data.cores[t.to].fields.fetch(name))))
          edges[[a, b]] << "#{t.step["op"]} → #{t.step["outcome"]}" unless a == b
        end
        next if edges.empty?
        gid = next_graph_id
        ids = nodes.to_a.sort.each_with_index.to_h { |v, i| [v, "#{gid}-v#{i}"] }
        graph = best_layout do |dir|
          dot = +graph_header(dir)
          ids.each do |v, id|
            style = initial.include?(v) ? ', penwidth=2, style="rounded,bold"' : ""
            dot << "  #{dq(id)} [id=#{dq(id)}, label=#{dq(v)}#{style}];\n"
          end
          edges.each do |(a, b), labels|
            shown = labels.to_a.sort
            text = shown.first(5).join("\\n")
            text += "\\n+#{shown.size - 5} more" if shown.size > 5
            dot << "  #{dq(ids[a])} -> #{dq(ids[b])} [id=#{dq("#{ids[a]}--#{ids[b]}")}, label=\"#{text.gsub('"', '\"')}\"];\n"
          end
          dot << "}\n"
        end
        per_core = data.cores.values.map { |c| ids.fetch(fmt(pick.(parse(c.fields.fetch(name))))) }
        graph_view(graph, caption: "<code>#{h(title)}</code>", attrs: node_attrs(spec.name, per_core))
      end
    end.join
  end

  # One graph per aspect.  A node shows only the values of the variables, in
  # the order of the legend; an edge names the calls that take it.  The
  # viewer panel shows the full state and every call with its outcome.
  def full_graphs(data)
    names = data.spec.variables.map(&:first) - %w[aspect step]
    legend = "A node lists #{names.map { |n| "<code>#{h(n)}</code>" }.join(" · ")}; T and F stand for TRUE and FALSE. " \
             "The bold node is the initial state."
    aspects = data.cores.values.map { |c| parse(c.fields.fetch("aspect")) }.uniq.sort
    aspects.map do |aspect|
      gid = next_graph_id
      keys = data.cores.values.select { |c| parse(c.fields["aspect"]) == aspect }.map(&:key)
      ids = keys.each_with_index.to_h { |k, i| [k, "#{gid}-s#{i}"] }
      edges = Hash.new { |hash, k| hash[k] = [] }
      loops = Hash.new { |hash, k| hash[k] = [] }
      data.transitions.each do |t|
        next unless ids.key?(t.from)
        call = "#{step_text(data.spec, t.step)} → #{t.step["outcome"]}"
        (t.from == t.to ? loops[ids[t.from]] : edges[[t.from, t.to]]) << call
      end
      graph = best_layout do |dir|
        dot = +graph_header(dir)
        keys.each do |k|
          core = data.cores[k]
          label = names.map { |n| short_value(core.fields.fetch(n)) }.join("  ")
          style = core.initial ? ', penwidth=2, style="rounded,bold"' : ""
          dot << "  #{dq(ids[k])} [id=#{dq(ids[k])}, label=#{dq(label)}#{style}];\n"
        end
        edges.each do |(a, b), calls|
          dot << "  #{dq(ids[a])} -> #{dq(ids[b])} [id=#{dq("#{ids[a]}--#{ids[b]}")}, label=\"#{op_summary(calls.sort)}\"];\n"
        end
        dot << "}\n"
      end
      nodes = keys.to_h do |k|
        core = data.cores[k]
        [ids[k], { "lines" => names.map { |n| "#{n} = #{core.fields.fetch(n)}" }, "initial" => core.initial }]
      end
      out = Hash.new { |hash, k| hash[k] = [] }
      edges.each { |(a, b), calls| out[ids[a]] << { "to" => ids[b], "edge" => "#{ids[a]}--#{ids[b]}", "calls" => calls.sort } }
      loop_count = loops.values.sum(&:size)
      summary = "Aspect <code>#{h(aspect)}</code>: #{keys.size} states, #{edges.values.sum(&:size)} state-changing transitions, " \
                "#{loop_count} transitions that change nothing (listed in the panel, not drawn)"
      per_core = data.cores.keys.map { |k| ids[k] }
      details(summary, graph_view(graph, legend: legend, attrs: node_attrs(data.spec.name, per_core),
                                         data: { "nodes" => nodes, "out" => out, "loops" => loops.transform_values(&:sort) }))
    end.join
  end

  def api_page(data)
    spec = data.spec
    aspects = data.cores.values.map { |c| parse(c.fields.fetch("aspect")) }.uniq.sort
    outcomes = data.transitions.map { |t| t.step["outcome"] }.tally.sort_by { |o, n| [-n, o] }
    loops = data.transitions.count { |t| t.from == t.to }
    <<~HTML
      <h1>#{h(spec.name)}</h1>
      <p class="lede">#{h(SUMMARIES[spec.name])}</p>
      #{watch_section(spec.name)}
      #{spec.name == "RactorProcess" ? fork_section : ""}
      <h2>The model</h2>
      #{para(spec.header)}
      <div class="stats">
      <div><b>#{data.states}</b><span>TLC states</span></div>
      <div><b>#{data.cores.size}</b><span>distinct API states</span></div>
      <div><b>#{data.transitions.size}</b><span>(state, call) transitions</span></div>
      <div><b>#{loops}</b><span>calls that change nothing</span></div>
      <div><b>#{data.paths}</b><span>CRuby replays, all passed</span></div>
      #{choice_stats(data.coverage)}
      <div><b>#{aspects.size}</b><span>aspect#{aspects.size == 1 ? "" : "s"}</span></div>
      </div>
      <p>Aspects: #{aspects.map { |a| "<code>#{h(a)}</code>" }.join(" ")}. Outcomes seen: #{outcomes.map { |o, n| "#{outcome_badge(o)}&nbsp;#{n}" }.join(" ")}</p>
      <h2>State</h2>
      <div class="cols"><div>#{variables_table(spec)}</div><div>#{constants_table(spec)}</div></div>
      <h3 id="def-Init">Init</h3>
      #{tla_block(spec, spec.defs.fetch("Init"))}
      <h2 id="lifecycles">Lifecycles</h2>
      <p>Each diagram projects the reachable state graph onto one variable or one component. An edge lists every call and outcome that moves the value. The bold node is the initial value. While a highlight or an example plays, a ring marks the current value in every diagram and the last step lights up its edge.</p>
      <div class="graphs">#{projection_graphs(data)}</div>
      <h2 id="calls">Calls</h2>
      <p>Each section shows the TLA+ action, the Ruby call it stands for, and every outcome TLC reached. The example is the shortest call sequence from <code>Init</code> that produces that outcome; ▶ Play animates it. CRuby produced the same outcome and the same observable state.#{untaken_note(data.coverage)}</p>
      #{action_section(data)}
      <h2>Guarantees</h2>
      <p>TLC checked these over every reachable state. An invariant holds in every state. A temporal property holds for every step or every behavior.</p>
      #{property_section(spec, spec.invariants, "invariant")}
      #{property_section(spec, spec.properties, "temporal property")}
      <h2>Complete state graph</h2>
      <p>One graph per aspect, after merging states that differ only in the last call. Self-loops are calls that raise or return without a state change; the panel lists them, the graph does not draw them. Open an aspect, then click a state to see every call from it and where it leads.</p>
      #{full_graphs(data)}
      <h2 id="source">Specification source</h2>
      #{details("<code>#{h(File.basename(spec.path))}</code>, #{spec.text.lines.size} lines", source_block(spec))}
    HTML
  end

  # ------------------------------------------------------------------- pool page

  TOKEN_NAMES = { 1 => "Dispatch", 2 => "Receive", 3 => "Process", 4 => "Collect", 5 => "GlobalCollect", 6 => "Stop", 7 => "Observe" }.freeze

  def token_text(token)
    family = TOKEN_NAMES.fetch(token / 10)
    token == 50 ? family : "#{family}(#{token % 10})"
  end

  def analyze_pool(dir, jar, java, ruby, scale)
    v = RactorTLAVerifier
    spec = parse_spec(v::SPEC_PATH, v::CONFIG_PATH)
    assign_targets(spec, spec.actions + spec.invariants + spec.properties + ["Init"] + pool_helpers(spec))
    distinct, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: v::SPEC_PATH, config: v::CONFIG_PATH,
                                      scale: scale, timeout: v::TLC_TIMEOUT, tag: "site-pool")
    schedules = v.parse_dot(dot, distinct)
    v.run_replays(ruby, dir, scale, schedules)
    graph = RactorTLA::Dot.parse(dot, variables: v::STATE_VARIABLES, stutter: true)
    warn "site: RactorPool states=#{distinct} schedules=#{schedules.size} replays=#{schedules.size} PASS"
    { spec: spec, states: distinct, schedules: schedules, graph: graph }
  end

  def pool_helpers(spec)
    spec.defs.keys - spec.actions - spec.invariants - spec.properties - %w[Init Next Spec vars] - spec.constants.map(&:name)
  end

  def pool_graph(graph)
    key = ->(node) { node.fields.reject { |n, _| n == "history" }.sort.map { |n, val| "#{n}=#{val}" }.join("\n") }
    gid = next_graph_id
    merged = {}
    graph.nodes.each_value { |n| merged[key.(n)] ||= [n, "#{gid}-m#{merged.size}"] }
    edges = Set.new
    graph.edges.each do |e|
      next if e.from == e.to
      to = graph.nodes[e.to]
      token = parse(to.fields["history"]).last
      edges << [merged[key.(graph.nodes[e.from])][1], merged[key.(to)][1], token]
    end
    rendered = best_layout do |dir|
      dot = +graph_header(dir)
      dot << "  node [shape=circle, label=\"\", width=0.18, height=0.18, fixedsize=true];\n"
      merged.each_value do |node, id|
        fill = if node.initial then ', style=filled, fillcolor="#4c8bf5"'
               elsif node.fields["done"] == "TRUE" then ', style=filled, fillcolor="#3fb950"'
               else ""
               end
        dot << "  #{dq(id)} [id=#{dq(id)}#{fill}];\n"
      end
      edges.each { |a, b, t| dot << "  #{dq(a)} -> #{dq(b)} [id=#{dq("#{a}--#{b}")}, label=#{dq(token_text(t))}];\n" }
      dot << "}\n"
    end
    nodes = merged.values.to_h do |node, id|
      [id, { "lines" => node.fields.reject { |n, _| n == "history" }.map { |n, val| "#{n} = #{val}" }, "initial" => node.initial }]
    end
    out = Hash.new { |hash, k| hash[k] = [] }
    edges.each { |a, b, t| out[a] << { "to" => b, "edge" => "#{a}--#{b}", "calls" => [token_text(t)] } }
    view = graph_view(rendered, legend: "The blue node is <code>Init</code>; green nodes are complete runs.",
                                data: { "nodes" => nodes, "out" => out, "loops" => {} })
    [merged.size, edges.size, view]
  end

  def pool_walkthrough(graph, schedule)
    by_history = graph.nodes.values.to_h { |n| [parse(n.fields["history"]), n] }
    names = RactorTLAVerifier::STATE_VARIABLES - %w[history]
    rows = (0..schedule.size).map do |i|
      node = by_history.fetch(schedule.first(i))
      prev = i.zero? ? nil : by_history.fetch(schedule.first(i - 1))
      cells = names.map do |n|
        changed = prev && prev.fields[n] != node.fields[n]
        "<td#{changed ? ' class="chg"' : ""}><code>#{h(node.fields[n])}</code></td>"
      end
      "<tr><td>#{i.zero? ? "Init" : h(token_text(schedule[i - 1]))}</td>#{cells.join}</tr>"
    end
    %(<div class="scroll"><table class="trace walk"><thead><tr><th>After</th>#{names.map { |n| "<th>#{h(n)}</th>" }.join}</tr></thead><tbody>#{rows.join}</tbody></table></div>)
  end

  def pool_page(pool, readme)
    spec = pool[:spec]
    merged, edge_count, view = pool_graph(pool[:graph])
    actions = spec.actions.map do |a|
      d = spec.defs.fetch(a)
      %(<section class="action" id="def-#{a}"><h3>#{h(a)}#{d.params ? "<span class=\"params\">(#{h(d.params)})</span>" : ""}</h3>#{para(d.comment)}#{tla_block(spec, d)}</section>)
    end.join
    helpers = pool_helpers(spec)
    schedules = pool[:schedules].map { |s| "<li><code>#{s.map { |t| token_text(t) }.join(" · ")}</code></li>" }.join
    <<~HTML
      <h1>RactorPool</h1>
      <p class="lede">#{h(SUMMARIES["RactorPool"])}</p>
      <section class="watch">
      <h2>Watch a schedule</h2>
      <p>Each of the #{pool[:schedules].size} complete schedules of the model plays as swimlanes. CRuby replayed every one of them. Pick a schedule from the list or from the grid below the player.</p>
      #{widget("timeline", view: "pool")}
      </section>
      <h2>The model</h2>
      #{para(spec.header)}
      <div class="stats">
      <div><b>#{pool[:states]}</b><span>TLC states</span></div>
      <div><b>#{merged}</b><span>states without history</span></div>
      <div><b>#{pool[:schedules].size}</b><span>complete schedules</span></div>
      <div><b>#{pool[:schedules].size}</b><span>CRuby replays, all passed</span></div>
      </div>
      #{Markdown.section(readme, "Protocol", level: 2)}
      #{Markdown.section(readme, "Feature matrix", level: 2)}
      <h2>State</h2>
      #{variables_table(spec)}
      <h3 id="def-Init">Init</h3>
      #{tla_block(spec, spec.defs.fetch("Init"))}
      <h2>Protocol graph</h2>
      <p>TLC keeps every schedule prefix as its own state through <code>history</code>. Here states that differ only in <code>history</code> are merged. Click a node to see its state and the actions that leave it.</p>
      #{view}
      <p class="muted">#{merged} states, #{edge_count} distinct transitions.</p>
      <h2>One complete run</h2>
      <p>The first schedule in sort order, step by step. Highlighted cells changed in that step.</p>
      #{pool_walkthrough(pool[:graph], pool[:schedules].first)}
      #{details("All #{pool[:schedules].size} schedules", "<ol class=\"sched\">#{schedules}</ol>")}
      <h2>Actions</h2>
      #{actions}
      <h2>Guarantees</h2>
      #{property_section(spec, spec.invariants, "invariant")}
      #{property_section(spec, spec.properties, "temporal property")}
      <h2>Helper definitions</h2>
      #{helpers.map { |n| d = spec.defs[n]; %(<div id="def-#{n}">#{para(d.comment)}#{tla_block(spec, d)}</div>) }.join}
      #{Markdown.section(readme, "Verification boundary", level: 2)}
      <h2 id="source">Specification source</h2>
      #{details("<code>RactorPool.tla</code>, #{spec.text.lines.size} lines", source_block(spec))}
    HTML
  end

  # ----------------------------------------------------------- overview pages

  def pipeline_svg
    dot = graph_header("LR") + <<~DOT
      tla [label="TLA+ model\\n(.tla, .cfg)"];
      tlc [label="TLC\\nexhaustive check"];
      dump [label="State graph\\n(DOT dump)"];
      core [label="Merged states\\nand transitions"];
      paths [label="Call paths\\ncovering every transition"];
      ruby [label="CRuby replay\\none process per path"];
      site [label="This site"];
      tla -> tlc -> dump -> core -> paths -> ruby;
      core -> site; ruby -> site [label="all pass"];
      }
    DOT
    dot_svg(dot)
  end

  def index_page(apis, pool, api_readme)
    rows = apis.map do |d|
      "<tr><td><a href=\"#{d.spec.name}.html\">#{d.spec.name}</a></td><td>#{h(SUMMARIES[d.spec.name])}</td><td class=\"num\">#{d.states}</td><td class=\"num\">#{d.cores.size}</td><td class=\"num\">#{d.transitions.size}</td><td class=\"num\">#{d.paths}</td></tr>"
    end
    rows << "<tr><td><a href=\"RactorPool.html\">RactorPool</a></td><td>#{h(SUMMARIES["RactorPool"])}</td><td class=\"num\">#{pool[:states]}</td><td class=\"num\">–</td><td class=\"num\">–</td><td class=\"num\">#{pool[:schedules].size}</td></tr>"
    total_states = apis.sum(&:states) + pool[:states]
    total_replays = apis.sum(&:paths) + pool[:schedules].size
    <<~HTML
      <h1>Ractors, formally</h1>
      <p class="lede">A Ractor is Ruby's unit of parallel execution. Ractors share no mutable state; they talk through ports. This site states the rules of that model as TLA+ specifications and shows that CRuby follows them.</p>
      <div class="stats">
      <div><b>#{apis.size + 1}</b><span>TLA+ modules</span></div>
      <div><b>#{total_states}</b><span>states checked by TLC</span></div>
      <div><b>#{total_replays}</b><span>CRuby replays, all passed</span></div>
      </div>
      <h2>Every possible future</h2>
      <p>Why can this site claim that CRuby follows the rules, and not only in the cases somebody thought to test? The animation below answers that question for one model. It shows the two steps of the method: the model checker TLC finds every situation that the Ractors can reach, and then every step between those situations runs on CRuby.</p>
      <div class="cols reading">
      <div>
      <h3>How to read the picture</h3>
      <ul>
      <li><b>A dot is a state:</b> one situation of the model, for example "main and the worker run, and port P1 holds one message". Click a dot to see that situation drawn as Ractors and ports, and the calls that leave it.</li>
      <li><b>A line is a call</b> that moves the model from one state to another, for example <code>p1.send(:msg)</code>. Calls that change nothing, such as most errors, are counted but not drawn.</li>
      <li><b>The large dot is the start.</b> Each ring around it is one more call away from the start. A model with several scenarios starts with one large dot per scenario, and each scenario has its own sector and colour.</li>
      <li><b>A dashed line is a choice:</b> the same call from the same state may end in more than one way, and the rules allow each of them.</li>
      </ul>
      </div>
      <div>
      <h3>What the three steps show</h3>
      <ol>
      <li><b>Every dot is a state.</b> Only the start is drawn. The caption describes it in words.</li>
      <li><b>TLC visits every state.</b> Press play. From each state TLC tries every call that any Ractor could make next, valid or not, and adds each new state it reaches. The search grows outward ring by ring and stops when no call leads anywhere new. Then the picture holds every state that the model allows. A unit test checks one sequence of calls; this search checks all of them, within the model's bounds.</li>
      <li><b>Every line runs on CRuby.</b> The verifier cuts the graph into call sequences that together use every line at least once. Each sequence runs in a fresh Ruby process and checks every outcome against the model. A line turns green when its sequence has passed; when the counter stops, every line is green.</li>
      </ol>
      </div>
      </div>
      <p>The models are small on purpose: two or three Ractors, a few ports, and at most two queued messages. Within those bounds nothing is left out. The bounds of each model are listed under <a href="rules.html">Rules</a>. The picture opens on RactorPorts; pick another model from its list.</p>
      #{widget("multiverse", model: "RactorPorts")}
      <h2>How to read this site</h2>
      <ol>
      <li>Read <a href="primer.html">the primer</a> if TLA+ is new to you. It explains the notation and how one API call becomes one action.</li>
      <li>Each module page starts with short animated stories. Below them it shows the state, a lifecycle diagram per variable, every call with its outcomes, a shortest example per outcome, and the guarantees TLC checked.</li>
      <li><a href="explorer.html">The explorer</a> lets you choose every step yourself and shows where CRuby may choose.</li>
      <li><a href="rules.html">The rules page</a> shows every rule as an animated card and keeps the full rule matrix, which compares each rule with the prose documentation.</li>
      <li><a href="RactorPool.html">RactorPool</a> combines the rules into a complete worker pool.</li>
      </ol>
      <h2>Modules</h2>
      <table><thead><tr><th>Module</th><th>Covers</th><th>TLC states</th><th>API states</th><th>Transitions</th><th>CRuby replays</th></tr></thead><tbody>#{rows.join}</tbody></table>
      <h2>How the evidence was made</h2>
      <div class="graph">#{pipeline_svg}</div>
      <p>TLC explores every reachable state of each bounded model. The builder merges states that differ only in the last call. Each (state, call) pair is one transition with one outcome. A breadth-first forest gives call paths that cover every transition. Each path runs in a fresh CRuby process, which compares every outcome and every observable value with the model.</p>
      #{Markdown.section(api_readme, "What the result proves", level: 2)}
      #{Markdown.section(api_readme, "Outcome vocabulary", level: 2)}
    HTML
  end

  def primer_page
    File.read(File.join(SITE_DIR, "primer.html"))
  end

  def fork_section
    <<~HTML
      <section class="watch">
      <h2>The timeline of a fork</h2>
      <p>One path of the model drawn as lanes for main's main thread, a second thread of main, and a thread of the worker. At <code>fork</code> the timeline splits into the parent and the child.</p>
      #{widget("timeline", view: "fork")}
      </section>
    HTML
  end

  def rule_cards
    source = File.read(File.join(EXPLORE_DIR, "rules.html"))
    start = source.index(RULES_START) or fail!("explore/rules.html has no #{RULES_START} marker")
    stop = source.index(RULES_END, start) or fail!("explore/rules.html has no #{RULES_END} marker")
    source[(start + RULES_START.size)...stop].strip
  end

  def rules_page(api_readme)
    matrix = Markdown.section(api_readme, "Rule matrix", level: 2)
    <<~HTML
      <h1>Rules</h1>
      <p class="lede">Each card states one rule of the Ractor API and shows it as a short animation. TLC checked every rule in its model, and CRuby passed every replay of it. The tags say whether the documentation states the rule.</p>
      #{rule_cards}
      #{details("The full rule matrix, with every rule of every model", matrix)}
      #{Markdown.section(api_readme, "Observed but not modeled", level: 2)}
      #{Markdown.section(api_readme, "Changes to CRuby in this work", level: 2)}
      #{Markdown.section(api_readme, "Modeling bounds", level: 2)}
    HTML
  end

  def explorer_page
    <<~HTML
      <h1>Explorer</h1>
      <p class="lede">You choose what happens next. The list shows every call that the model allows in the current state. Guess the outcome, then watch it. Where the model allows more than one outcome, CRuby takes one of them.</p>
      #{widget("explorer", urlparams: true)}
    HTML
  end

  # rx/: the widget assets; rx/data/: one script per model, and counts.js
  # with the counts of every model for pages that load no state graph.
  def write_widget_assets(out, apis, pool)
    rx = File.join(out, "rx")
    FileUtils.mkdir_p(File.join(rx, "data"))
    files = WIDGET_SHARED + WIDGET_ASSETS.values.flatten.uniq
    files.each do |f|
      path = File.join(EXPLORE_DIR, f)
      fail!("missing widget asset #{path}") unless File.file?(path)
      FileUtils.cp(path, rx)
    end
    apis.each do |d|
      data = RactorModelData.api(d.mod, d.spec, SUMMARIES[d.spec.name], d.states, d.cores, d.transitions)
      File.write(File.join(rx, "data", "#{d.spec.name}.js"), RactorModelData.script(d.spec.name, data))
    end
    File.write(File.join(rx, "data", "RactorPool.js"),
               RactorModelData.script("RactorPool", RactorModelData.pool(SUMMARIES["RactorPool"], pool[:states], pool[:schedules])))
    counts = apis.map do |d|
      RactorModelData.script(d.spec.name, { name: d.spec.name, summary: SUMMARIES[d.spec.name],
                                            counts: d.mod.expected.merge(states: d.states), cores: [], transitions: [] })
    end
    File.write(File.join(rx, "data", "counts.js"), counts.join)
  end

  # ------------------------------------------------------------------------ main

  def main(argv)
    out = nil
    argv.each do |arg|
      m = arg.match(/\A--out=(.+)\z/) or fail!("unknown argument #{arg.inspect}; usage: build_site.rb --out=DIR")
      out = File.expand_path(m[1])
    end
    fail!("--out=DIR is required") unless out
    _, status = Open3.capture2e("dot", "-V")
    fail!("Graphviz `dot` is required on PATH") unless status.success?
    scale = RactorTLA.timeout_scale
    jobs = RactorTLA.positive_integer("RACTOR_TLA_JOBS", RactorAPIVerifier::DEFAULT_JOBS)
    ruby = RactorTLA.child_ruby
    replay_ruby, status = Open3.capture2e(ruby, "-v")
    fail!("#{ruby} -v failed: #{replay_ruby}") unless status.success?
    replay_ruby = replay_ruby.lines.first.strip
    jar = RactorTLA.check_jar
    java = RactorTLA.java_executable
    RactorTLA.check_java(java)
    api_readme = File.read(File.join(API_DIR, "README.md"))
    pool_readme = File.read(File.join(SAMPLE_DIR, "README.md"))
    apis = pool = nil
    Dir.mktmpdir("ractor_site") do |dir|
      apis = RactorAPIVerifier::MODULES.map { |mod| analyze_api(mod, dir, jar, java, ruby, scale, jobs) }
      pool = analyze_pool(dir, jar, java, ruby, scale)
    end
    pages = [["index.html", "Overview"], ["primer.html", "TLA+ primer"]] +
            apis.map { |d| ["#{d.spec.name}.html", d.spec.name.delete_prefix("Ractor")] } +
            [["RactorPool.html", "Pool"], ["rules.html", "Rules"], ["explorer.html", "Explorer"]]
    FileUtils.rm_rf(out)
    FileUtils.mkdir_p(out)
    FileUtils.cp(File.join(SITE_DIR, "style.css"), out)
    FileUtils.cp(File.join(SITE_DIR, "graph.js"), out)
    write_widget_assets(out, apis, pool)
    @graph_files = []
    @graph_seq = 0
    write = lambda do |file, title, body, widgets: [], data: []|
      File.write(File.join(out, file), page(pages, file, title, body, replay_ruby, widgets: widgets, data: data))
    end
    every_model = apis.map { |d| d.spec.name }
    write.("index.html", "Overview", index_page(apis, pool, api_readme), widgets: %w[multiverse], data: every_model)
    write.("primer.html", "TLA+ primer", primer_page)
    apis.each do |d|
      name = d.spec.name
      widgets = name == "RactorProcess" ? %w[theatre timeline] : %w[theatre]
      write.("#{name}.html", name, api_page(d), widgets: widgets, data: [name])
    end
    write.("RactorPool.html", "RactorPool", pool_page(pool, pool_readme), widgets: %w[timeline], data: %w[RactorPool])
    write.("rules.html", "Rules", rules_page(api_readme), widgets: %w[rules], data: %w[counts])
    write.("explorer.html", "Explorer", explorer_page, widgets: %w[explorer], data: every_model)
    FileUtils.mkdir_p(File.join(out, "graphs"))
    @graph_files.each_with_index { |raw, i| File.write(File.join(out, "graphs", "g#{i + 1}.svg"), raw) }
    puts "PASS site=#{out} pages=#{pages.size}"
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    RactorSite.main(ARGV)
  rescue RactorTLA::Failure => e
    warn "FAIL: #{e.message}"
    exit 1
  end
end
