# frozen_string_literal: true

# Checks every Ractor API model in this directory with TLC and replays every
# transition of each model against CRuby through api_harness.rb.
#
#   TLA2TOOLS_JAR=/path/tla2tools.jar ruby verify_api.rb [--module=NAME ...]
#
# Environment:
#   TLA2TOOLS_JAR             TLA+ Tools v1.7.4 jar
#   RACTOR_TLA_TIMEOUT_SCALE  multiplier for every timeout (default 1.0)
#   RACTOR_TLA_JOBS           parallel replay processes (default: min(cores, 8))
#   RACTOR_TLA_UNTAKEN        set to 1 to list the transitions that no replay reached
#   RUBY                      Ruby executable for child processes (default RbConfig.ruby)
#   JAVA_HOME                 Java installation; otherwise `java` from PATH
#
# The state graph of a model carries the last call in the variable `step`.
# The verifier merges states that differ only in `step`, so every pair of
# (state, call) is one transition.  It builds a breadth-first forest over the
# merged graph and replays one path per leaf and one per non-tree edge, so
# every transition runs once on CRuby in a fresh process.
#
# A call with several transitions from one state is a choice: the model
# allows each outcome and CRuby picks one.  Each step of a path lists the
# other outcomes of its call as `alternatives`.  When CRuby picks one of
# them, the harness stops the replay and reports the divergence.  The forest
# prefers routes through as few choices as possible, and the verifier reports
# the transitions that no replay reached.

require "etc"
require "json"
require "tmpdir"
require_relative "../tlc_tools"

module RactorAPIVerifier
  API_DIR = __dir__
  HARNESS_PATH = File.join(API_DIR, "api_harness.rb")

  TLC_TIMEOUT = 300
  REPLAY_TIMEOUT = 30
  DEFAULT_JOBS = [Etc.nprocessors, 8].min

  Model = Struct.new(:name, :variables, :observables, :expected, :tlc_options, keyword_init: true) do
    def options
      tlc_options || []
    end

    def sinks?
      options.include?("-deadlock")
    end
  end

  MODULES = [
    Model.new(
      name: "RactorPorts",
      variables: %w[aspect workerAlive portOpen queue step],
      observables: %w[portOpen workerAlive],
      expected: { states: 2107, core_states: 96, transitions: 2266, paths: 2219 }
    ),
    Model.new(
      name: "RactorTermination",
      variables: %w[aspect alive monitors tokens successor step],
      observables: %w[alive],
      expected: { states: 740, core_states: 72, transitions: 762, paths: 728 }
    ),
    Model.new(
      name: "RactorTransfer",
      variables: %w[aspect mainState shareable inbox workerHas argRactor step],
      observables: %w[mainState shareable workerHas],
      expected: { states: 5201, core_states: 570, transitions: 6074, paths: 5793 }
    ),
    Model.new(
      name: "RactorIsolation",
      variables: %w[aspect alive slot slotSet defined included frozenClass step],
      observables: %w[alive slotSet defined included frozenClass],
      expected: { states: 586, core_states: 69, transitions: 674, paths: 651 }
    ),
    Model.new(
      name: "RactorLocal",
      variables: %w[aspect stored step],
      observables: %w[stored],
      expected: { states: 1945, core_states: 81, transitions: 2592, paths: 2553 }
    ),
    Model.new(
      name: "RactorBlocking",
      variables: %w[aspect alive portOpen queue ts wait taken token step],
      observables: %w[alive portOpen ts],
      expected: { states: 1195, core_states: 233, transitions: 1434, paths: 1354 }
    ),
    Model.new(
      name: "RactorOrder",
      variables: %w[aspect queue sent portOpen alive taken step],
      observables: %w[portOpen alive],
      expected: { states: 2411, core_states: 366, transitions: 2592, paths: 2392 }
    ),
    Model.new(
      name: "RactorProcess",
      variables: %w[aspect side alive ts unwound queued atExit result handled ended step],
      observables: %w[side alive ts unwound handled],
      expected: { states: 648, core_states: 166, transitions: 701, paths: 621 },
      tlc_options: ["-deadlock"]
    )
  ].freeze

  Failure = RactorTLA::Failure

  Core = Struct.new(:key, :fields, :initial)
  Transition = Struct.new(:from, :to, :step)

  module_function

  def fail!(message)
    raise Failure, message
  end

  # Merges DOT nodes by every variable except step.  Returns [cores, transitions].
  def quotient(graph, variables)
    cores = {}
    graph.nodes.each_value do |node|
      fields = node.fields.reject { |name, _| name == "step" }
      key = fields.sort.map { |name, text| "#{name}=#{text}" }.join("\n")
      core = (cores[key] ||= Core.new(key, fields, false))
      core.initial = true if node.initial
    end
    transitions = {}
    graph.edges.each do |edge|
      from = graph.nodes[edge.from]
      to = graph.nodes[edge.to]
      step = RactorTLA::Value.parse(to.fields.fetch("step"))
      fail!("step #{to.fields["step"]} is not a record") unless step.is_a?(Hash) && %w[op args outcome].all? { |k| step.key?(k) }
      fail!("edge #{edge.from} -> #{edge.to} is labeled #{edge.label} but step op is #{step["op"]}") unless edge.label == step["op"]
      from_key = core_key(from.fields)
      to_key = core_key(to.fields)
      id = [from_key, to.fields.fetch("step")]
      if (known = transitions[id])
        fail!("nondeterministic step #{step.inspect} from #{from_key.inspect}") unless known.to == to_key
      else
        transitions[id] = Transition.new(from_key, to_key, step)
      end
    end
    variables.each do |name|
      next if name == "step"
      cores.each_value { |core| fail!("core state lacks #{name}") unless core.fields.key?(name) }
    end
    [cores, transitions.values]
  end

  def core_key(fields)
    fields.reject { |name, _| name == "step" }.sort.map { |name, text| "#{name}=#{text}" }.join("\n")
  end

  def observe(core, observables)
    observables.to_h do |name|
      value = RactorTLA::Value.parse(core.fields.fetch(name))
      fail!("observable #{name} is a set; use a function instead") if value.is_a?(Set)
      [name, value]
    end
  end

  def signature(step)
    { "outcome" => step.fetch("outcome"), "value" => step.fetch("value", ""), "wakes" => step.fetch("wakes", []) }
  end

  # Transitions that share their state and call with another one, mapped to
  # the signatures of the others.
  def choices(transitions)
    alternatives = {}.compare_by_identity
    transitions.group_by { |t| [t.from, t.step["op"], t.step["args"]] }.each_value do |group|
      next if group.size == 1
      group.each { |t| alternatives[t] = (group - [t]).map { |other| signature(other.step) } }
    end
    alternatives
  end

  # Breadth-first forest from the initial cores, in layers: layer k holds the
  # cores whose shortest route crosses k choices.  One path per leaf and per
  # non-tree transition.  Each path is a list of steps with observables.
  def paths_for(cores, transitions, observables)
    outgoing = Hash.new { |h, k| h[k] = [] }
    transitions.each { |t| outgoing[t.from] << t }
    alternatives = choices(transitions)
    initial = cores.values.select(&:initial).map(&:key)
    fail!("no initial core state") if initial.empty?
    parent = {}
    order = []
    queue = initial.dup
    initial.each { |key| parent[key] = nil }
    until queue.empty?
      crossings = []
      until queue.empty?
        key = queue.shift
        order << key
        outgoing[key].each do |t|
          next if parent.key?(t.to)
          next crossings << t if alternatives.key?(t)
          parent[t.to] = t
          queue << t.to
        end
      end
      crossings.each do |t|
        next if parent.key?(t.to)
        parent[t.to] = t
        queue << t.to
      end
    end
    fail!("#{cores.size - order.size} core states are unreachable from the initial states") unless order.size == cores.size
    aspect_of = ->(key) { RactorTLA::Value.parse(cores.fetch(key).fields.fetch("aspect")) }
    route_to = lambda do |key|
      steps = []
      while (t = parent[key])
        steps.unshift(t)
        key = t.from
      end
      steps
    end
    paths = []
    order.each do |key|
      leaf = outgoing[key].none? { |t| parent[t.to].equal?(t) }
      paths << route_to.call(key) if leaf && parent[key]
    end
    transitions.each do |t|
      next if parent[t.to].equal?(t)
      paths << route_to.call(t.from) + [t]
    end
    paths.map do |steps|
      aspect = aspect_of.call(steps.first.from)
      { "aspect" => aspect,
        "steps" => steps.map do |t|
          json = t.step.merge("observe" => observe(cores.fetch(t.to), observables))
          alternatives.key?(t) ? json.merge("alternatives" => alternatives[t]) : json
        end,
        transitions: steps,
        alternatives: alternatives }
    end
  end

  def check_counts(mod, states, cores, transitions, paths)
    actual = { states: states, core_states: cores.size, transitions: transitions.size, paths: paths.size }
    return if actual == mod.expected
    fail!("#{mod.name}: counts #{actual.inspect} differ from the pinned #{mod.expected.inspect}; " \
          "inspect the model change before updating the pins")
  end

  # Replays every path.  Returns { choices:, diverged:, untaken: }: the
  # transitions that are choices, the replays that stopped where CRuby picked
  # another allowed outcome, and the transitions that no replay reached.
  def replay(mod, paths, ruby, dir, scale, jobs)
    queue = Queue.new
    paths.each_with_index { |path, index| queue << [index, path] }
    queue.close
    failure = nil
    reached = {}
    lock = Mutex.new
    threads = Array.new(jobs) do
      Thread.new do
        while (item = queue.pop)
          index, path = item
          break if failure
          tag = "replay-#{mod.name}-#{index}"
          file = File.join(dir, "#{tag}.json")
          File.write(file, JSON.generate({ "module" => mod.name, "aspect" => path["aspect"], "steps" => path["steps"] }))
          argv = [ruby, "--disable-gems", HARNESS_PATH, "--module=#{mod.name}", "--path=#{file}"]
          status, stdout, stderr = RactorTLA.run_child(argv, env: { "RUBYOPT" => nil }, dir: dir, chdir: dir,
                                                        timeout: REPLAY_TIMEOUT, scale: scale, tag: tag)
          prefix = "api_harness: %s module=#{mod.name} aspect=#{path["aspect"]}"
          diverged = stdout.match(/\A#{Regexp.escape(prefix % "DIVERGED")} step=(\d+) chose=(.+)\n\z/)
          problem = if status.nil? then "timed out"
                    elsif !status.success? then "exited with #{status.exitstatus.inspect}"
                    elsif !stderr.empty? then "wrote to stderr"
                    elsif diverged then nil
                    elsif stdout != "#{prefix % "PASS"} steps=#{path["steps"].size}\n" then "printed #{stdout.inspect}"
                    end
          lock.synchronize do
            if problem
              failure ||= { index: index, path: path, problem: problem, stdout: stdout, stderr: stderr }
              queue.clear
            else
              reached[index] = diverged ? [Integer(diverged[1], 10), JSON.parse(diverged[2])] : nil
            end
          end
        end
      end
    end
    threads.each(&:join)
    if failure
      calls = failure[:path]["steps"].map { |s| "#{s["op"]}#{s["args"].inspect} -> #{s["outcome"]}" }
      fail!("#{mod.name} aspect #{failure[:path]["aspect"]} replay #{failure[:index]} #{failure[:problem]}\n" \
            "steps:\n  #{calls.join("\n  ")}\nstdout:\n#{failure[:stdout]}stderr:\n#{failure[:stderr]}")
    end
    coverage(paths, reached)
  end

  def coverage(paths, reached)
    all = paths.flat_map { |path| path[:transitions] }.uniq(&:object_id)
    alternatives = paths.first ? paths.first[:alternatives] : {}
    siblings = all.group_by { |t| [t.from, t.step["op"], t.step["args"]] }
    covered = {}.compare_by_identity
    paths.each_with_index do |path, index|
      stop, chose = reached.fetch(index)
      path[:transitions].each_with_index do |t, position|
        break if stop && position == stop
        covered[t] = true
      end
      next unless stop
      t = path[:transitions][stop]
      taken = siblings.fetch([t.from, t.step["op"], t.step["args"]]).find { |s| signature(s.step) == chose }
      fail!("replay #{index} diverged to #{chose.inspect}, which no transition allows") unless taken
      covered[taken] = true
    end
    { choices: alternatives.size, diverged: reached.count { |_, value| value },
      untaken: all.count { |t| !covered.key?(t) },
      untaken_steps: all.reject { |t| covered.key?(t) }.map(&:step) }
  end

  def verify_module(mod, jar, java, ruby, dir, scale, jobs)
    spec = File.join(API_DIR, "#{mod.name}.tla")
    config = File.join(API_DIR, "#{mod.name}.cfg")
    [spec, config].each { |path| fail!("missing #{path}") unless File.file?(path) }
    states, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: spec, config: config, options: mod.options,
                                    scale: scale, timeout: TLC_TIMEOUT, tag: "tlc-#{mod.name}")
    graph = RactorTLA::Dot.parse(dot, variables: mod.variables, stutter: false, sinks: mod.sinks?)
    fail!("#{mod.name}: DOT has #{graph.nodes.size} nodes, TLC found #{states}") unless graph.nodes.size == states
    cores, transitions = quotient(graph, mod.variables)
    paths = paths_for(cores, transitions, mod.observables)
    check_counts(mod, states, cores, transitions, paths)
    result = replay(mod, paths, ruby, dir, scale, jobs)
    { states: states, core_states: cores.size, transitions: transitions.size, paths: paths.size }.merge(result)
  end

  def main(argv)
    selected = []
    argv.each do |arg|
      match = arg.match(/\A--module=([A-Za-z]+)\z/)
      fail!("unknown argument #{arg.inspect}; only --module=NAME is accepted") unless match
      mod = MODULES.find { |m| m.name == match[1] }
      fail!("unknown module #{match[1]}; known: #{MODULES.map(&:name).join(", ")}") unless mod
      selected << mod
    end
    selected = MODULES if selected.empty?
    scale = RactorTLA.timeout_scale
    jobs = RactorTLA.positive_integer("RACTOR_TLA_JOBS", DEFAULT_JOBS)
    ruby = RactorTLA.child_ruby
    fail!("missing #{HARNESS_PATH}") unless File.file?(HARNESS_PATH)
    jar = RactorTLA.check_jar
    java = RactorTLA.java_executable
    RactorTLA.check_java(java)
    totals = Hash.new(0)
    Dir.mktmpdir("ractor_tla_api") do |dir|
      selected.each do |mod|
        counts = verify_module(mod, jar, java, ruby, dir, scale, jobs)
        counts.each { |key, value| totals[key] += value if value.is_a?(Integer) }
        line = "module #{mod.name} states=#{counts[:states]} core=#{counts[:core_states]} transitions=#{counts[:transitions]} replays=#{counts[:paths]}"
        line += " choices=#{counts[:choices]} diverged=#{counts[:diverged]} untaken=#{counts[:untaken]}" if counts[:choices].positive?
        puts line
        if ENV["RACTOR_TLA_UNTAKEN"] == "1"
          counts[:untaken_steps].map { |s| JSON.generate(signature(s).merge("op" => s["op"], "args" => s["args"])) }
                                .tally.sort.each { |text, n| puts "  untaken #{n}x #{text}" }
        end
      end
    end
    line = "PASS modules=#{selected.size} states=#{totals[:states]} transitions=#{totals[:transitions]} replays=#{totals[:paths]}"
    line += " untaken=#{totals[:untaken]}" if totals[:choices].positive?
    puts line
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    RactorAPIVerifier.main(ARGV)
  rescue RactorTLA::Failure => e
    warn "FAIL: #{e.message}"
    exit 1
  end
end
