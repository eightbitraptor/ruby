# frozen_string_literal: true

# Checks sample/ractor_tla/RactorPool.tla with TLC, replays every complete
# schedule of its state graph against ractor_pool.rb in a fresh Ruby process,
# and runs one concurrent large-heap execution.
#
#   TLA2TOOLS_JAR=/path/tla2tools.jar ruby verify.rb
#   ruby verify.rb --stress-only
#
# Environment:
#   TLA2TOOLS_JAR             TLA+ Tools v1.7.4 jar (required without --stress-only)
#   HEAP_OBJECTS              objects per heap in the stress run (default 50000)
#   RACTOR_TLA_TIMEOUT_SCALE  multiplier for every timeout (default 1.0)
#   RUBY                      Ruby executable for child processes (default RbConfig.ruby)
#   JAVA_HOME                 Java installation; otherwise `java` from PATH

require "tmpdir"
require_relative "tlc_tools"

module RactorTLAVerifier
  SAMPLE_DIR = __dir__
  SPEC_PATH = File.join(SAMPLE_DIR, "RactorPool.tla")
  CONFIG_PATH = File.join(SAMPLE_DIR, "RactorPool.cfg")
  POOL_PATH = File.join(SAMPLE_DIR, "ractor_pool.rb")

  EXPECTED_STATES = 829
  EXPECTED_TREE_EDGES = 828
  EXPECTED_SCHEDULES = 80
  SCHEDULE_LENGTH = 17
  SCHEDULE_TAIL = [50, 61, 71, 62, 72].freeze
  TOKENS = [11, 12, 21, 22, 31, 32, 41, 42, 50, 61, 62, 71, 72].freeze
  TOKEN_FAMILIES = { 1 => "Dispatch", 2 => "Receive", 3 => "Process", 4 => "Collect",
                     5 => "GlobalCollect", 6 => "Stop", 7 => "Observe" }.freeze
  STATE_VARIABLES = %w[pending phase assignment result completed processedBy mainCanAccess
                       localCollections globalCollected valueTaken done history].freeze
  REPLAY_HEAP_OBJECTS = 32
  DEFAULT_HEAP_OBJECTS = 50_000

  TLC_TIMEOUT = 120
  STRESS_TIMEOUT = 120
  REPLAY_TIMEOUT = 30

  Failure = RactorTLA::Failure

  Node = Struct.new(:id, :history, :done, :initial)

  module_function

  def fail!(message)
    raise Failure, message
  end

  def parse_history(text)
    history = RactorTLA::Value.parse(text)
    fail!("history is not a sequence: #{text.inspect}") unless history.is_a?(Array)
    fail!("history holds a non-integer: #{text.inspect}") unless history.all?(Integer)
    history
  end

  def parse_dot(dot, distinct_states)
    graph = RactorTLA::Dot.parse(dot, variables: STATE_VARIABLES, stutter: true)
    nodes = graph.nodes.transform_values do |node|
      done = case node.fields["done"]
             when "TRUE" then true
             when "FALSE" then false
             else fail!("node #{node.id} has done = #{node.fields["done"].inspect}")
             end
      Node.new(node.id, parse_history(node.fields["history"]), done, node.initial)
    end
    check_graph(nodes, graph.edges, distinct_states)
  end

  def check_graph(nodes, edges, distinct_states)
    fail!("DOT has #{nodes.size} nodes, TLC found #{distinct_states}") unless nodes.size == distinct_states
    fail!("DOT has #{nodes.size} nodes, expected #{EXPECTED_STATES}") unless nodes.size == EXPECTED_STATES
    initial = nodes.values.select(&:initial)
    fail!("expected one initial node, found #{initial.size}") unless initial.size == 1
    fail!("initial node has history #{initial.first.history.inspect}") unless initial.first.history.empty?
    histories = nodes.values.map(&:history)
    fail!("DOT histories are not unique") unless histories.uniq.size == histories.size

    parents = Hash.new(0)
    children = Hash.new(0)
    terminal_loops = Hash.new(0)
    edges.each do |edge|
      from, to, label = edge.from, edge.to, edge.label
      if from == to
        fail!("self-loop on #{from} is labeled #{label}") unless label == "Terminal"
        fail!("Terminal loop on a node that is not done") unless nodes[from].done
        terminal_loops[from] += 1
        next
      end
      parent = nodes[from].history
      child = nodes[to].history
      fail!("edge #{from} -> #{to} does not extend the history by one token") unless child.size == parent.size + 1 && child[0...-1] == parent
      token = child.last
      fail!("unknown token #{token} on edge #{from} -> #{to}") unless TOKENS.include?(token)
      family = TOKEN_FAMILIES.fetch(token / 10)
      fail!("edge #{from} -> #{to} is labeled #{label} but token #{token} is #{family}") unless label == family
      parents[to] += 1
      children[from] += 1
    end
    tree_edges = parents.values.sum
    fail!("DOT has #{tree_edges} non-self edges, expected #{EXPECTED_TREE_EDGES}") unless tree_edges == EXPECTED_TREE_EDGES
    nodes.each_value do |node|
      if node.initial
        fail!("initial node has #{parents[node.id]} parents") unless parents[node.id].zero?
      else
        fail!("node #{node.id} has #{parents[node.id]} parents") unless parents[node.id] == 1
      end
      if node.done
        fail!("done node #{node.id} has outgoing edges") unless children[node.id].zero?
        fail!("done node #{node.id} has #{terminal_loops[node.id]} Terminal loops") unless terminal_loops[node.id] == 1
      else
        fail!("node #{node.id} has no outgoing edge and is not done") if children[node.id].zero?
        fail!("node #{node.id} has a Terminal loop but is not done") unless terminal_loops[node.id].zero?
      end
    end

    schedules = nodes.values.select(&:done).map(&:history).sort
    fail!("found #{schedules.size} complete schedules, expected #{EXPECTED_SCHEDULES}") unless schedules.size == EXPECTED_SCHEDULES
    schedules.each do |schedule|
      fail!("schedule #{schedule.inspect} has #{schedule.size} tokens") unless schedule.size == SCHEDULE_LENGTH
      fail!("schedule #{schedule.inspect} has tail #{schedule.last(5).inspect}") unless schedule.last(5) == SCHEDULE_TAIL
    end
    schedules
  end

  def pool_env
    { "RUBYOPT" => nil }
  end

  def check_pool_output(status, stdout, stderr, expected_line, what)
    fail!("#{what} timed out\nstdout:\n#{stdout}stderr:\n#{stderr}") if status.nil?
    fail!("#{what} exited with #{status.exitstatus.inspect}\nstdout:\n#{stdout}stderr:\n#{stderr}") unless status.success?
    fail!("#{what} wrote to stderr:\n#{stderr}") unless stderr.empty?
    fail!("#{what} printed #{stdout.inspect}, expected #{expected_line.inspect}") unless stdout == "#{expected_line}\n"
  end

  def run_stress(ruby, dir, scale, heap_objects)
    argv = [ruby, "--disable-gems", POOL_PATH, "--mode=free", "--heap-objects=#{heap_objects}"]
    status, stdout, stderr = RactorTLA.run_child(argv, env: pool_env, dir: dir, chdir: dir, timeout: STRESS_TIMEOUT, scale: scale, tag: "stress")
    check_pool_output(status, stdout, stderr, "ractor_pool: PASS mode=free heap_objects=#{heap_objects}", "stress run")
  end

  def run_replays(ruby, dir, scale, schedules)
    schedules.each_with_index do |schedule, index|
      tokens = schedule.join(",")
      argv = [ruby, "--disable-gems", POOL_PATH, "--mode=replay", "--schedule=#{tokens}"]
      status, stdout, stderr = RactorTLA.run_child(argv, env: pool_env, dir: dir, chdir: dir, timeout: REPLAY_TIMEOUT, scale: scale, tag: "replay")
      expected = "ractor_pool: PASS mode=replay heap_objects=#{REPLAY_HEAP_OBJECTS} schedule=#{tokens}"
      check_pool_output(status, stdout, stderr, expected, "replay #{index} (#{tokens})")
    end
  end

  def main(argv)
    stress_only = false
    argv.each do |arg|
      fail!("unknown argument #{arg.inspect}; only --stress-only is accepted") unless arg == "--stress-only"
      stress_only = true
    end
    scale = RactorTLA.timeout_scale
    heap_objects = RactorTLA.positive_integer("HEAP_OBJECTS", DEFAULT_HEAP_OBJECTS)
    ruby = RactorTLA.child_ruby
    [SPEC_PATH, CONFIG_PATH, POOL_PATH].each do |path|
      fail!("missing #{path}") unless File.file?(path)
    end
    Dir.mktmpdir("ractor_tla") do |dir|
      if stress_only
        run_stress(ruby, dir, scale, heap_objects)
        puts "PASS stress_heap_objects=#{heap_objects}"
      else
        jar = RactorTLA.check_jar
        java = RactorTLA.java_executable
        RactorTLA.check_java(java)
        distinct, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: SPEC_PATH, config: CONFIG_PATH,
                                          scale: scale, timeout: TLC_TIMEOUT, tag: "tlc")
        schedules = parse_dot(dot, distinct)
        run_stress(ruby, dir, scale, heap_objects)
        run_replays(ruby, dir, scale, schedules)
        puts "PASS tlc_states=#{distinct} schedules=#{schedules.size} replays=#{schedules.size} stress_heap_objects=#{heap_objects}"
      end
    end
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    RactorTLAVerifier.main(ARGV)
  rescue RactorTLA::Failure => e
    warn "FAIL: #{e.message}"
    exit 1
  end
end
