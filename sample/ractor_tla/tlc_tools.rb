# frozen_string_literal: true

# Shared TLC runner, DOT state-graph parser, and child-process helpers for
# the Ractor TLA+ samples.  The TLA+ Tools version is pinned; the DOT dump
# of that version is parsed as a versioned input contract and every other
# line shape is an error.

require "digest"
require "rbconfig"
require "set"

module RactorTLA
  TLA_TOOLS_VERSION = "v1.7.4"
  TLA_TOOLS_URL = "https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar"
  JAR_SIZE = 2_274_532
  JAR_SHA1 = "bee4a54f3ee3d4afc347c3240ec2d9e93b075104"
  MIN_JAVA = 11
  KILL_GRACE = 2

  TLC_SUCCESS = 2193
  TLC_STATS = 2199
  TLC_FINISHED = 2186
  REJECTED_CODES = [2110, 2114, 2121, 2217, 2102].freeze
  REJECTED_CLASSES = { 1 => "ERROR", 2 => "TLCBUG", 3 => "WARNING" }.freeze

  class Failure < StandardError; end

  module_function

  def fail!(message)
    raise Failure, message
  end

  def positive_integer(name, default)
    value = ENV[name]
    return default if value.nil? || value.empty?
    fail!("#{name} must be a positive integer, got #{value.inspect}") unless value.match?(/\A[1-9][0-9]*\z/)
    Integer(value, 10)
  end

  def timeout_scale
    value = ENV["RACTOR_TLA_TIMEOUT_SCALE"]
    return 1.0 if value.nil? || value.empty?
    scale = Float(value, exception: false)
    fail!("RACTOR_TLA_TIMEOUT_SCALE must be a positive finite number, got #{value.inspect}") unless scale&.finite? && scale.positive?
    scale
  end

  def child_ruby
    ruby = ENV["RUBY"]
    ruby = RbConfig.ruby if ruby.nil? || ruby.empty?
    fail!("Ruby executable #{ruby.inspect} is not executable") unless File.executable?(ruby)
    ruby
  end

  def java_executable
    home = ENV["JAVA_HOME"]
    return File.join(home, "bin", "java") if home && !home.empty?
    "java"
  end

  def check_java(java)
    output = begin
      IO.popen([java, "-version"], err: [:child, :out], &:read)
    rescue SystemCallError => e
      fail!("cannot run #{java}: #{e.message}")
    end
    fail!("#{java} -version failed: #{output}") unless $?.success?
    match = output.match(/version "(\d+)(?:\.(\d+))?/)
    fail!("cannot parse Java version from #{output.inspect}") unless match
    major = match[1] == "1" ? Integer(match[2], 10) : Integer(match[1], 10)
    fail!("Java #{MIN_JAVA} or newer is required, found #{major}") if major < MIN_JAVA
  end

  def check_jar
    jar = ENV["TLA2TOOLS_JAR"]
    fail!("TLA2TOOLS_JAR must point to tla2tools.jar #{TLA_TOOLS_VERSION} from #{TLA_TOOLS_URL}") if jar.nil? || jar.empty?
    fail!("TLA2TOOLS_JAR #{jar} is not a readable file") unless File.file?(jar) && File.readable?(jar)
    size = File.size(jar)
    fail!("#{jar} has #{size} bytes, expected #{JAR_SIZE} for #{TLA_TOOLS_VERSION}") unless size == JAR_SIZE
    sha1 = Digest::SHA1.file(jar).hexdigest
    fail!("#{jar} has SHA-1 #{sha1}, expected #{JAR_SHA1} for #{TLA_TOOLS_VERSION}") unless sha1 == JAR_SHA1
    File.expand_path(jar)
  end

  # Runs argv with stdout and stderr captured in files under dir.  Returns
  # [status, stdout, stderr]; status is nil when the child was killed on timeout.
  def run_child(argv, env:, dir:, chdir:, timeout:, scale:, tag:)
    out_path = File.join(dir, "#{tag}.out")
    err_path = File.join(dir, "#{tag}.err")
    pid = Process.spawn(env, *argv, chdir: chdir, in: File::NULL, out: out_path, err: err_path)
    waiter = Thread.new { Process.wait2(pid).last }
    status = waiter.join(timeout * scale)&.value
    return [status, File.read(out_path), File.read(err_path)] if status
    signal(pid, "TERM")
    signal(pid, "KILL") unless waiter.join(KILL_GRACE * scale)
    waiter.join
    [nil, File.read(out_path), File.read(err_path)]
  end

  def signal(pid, name)
    Process.kill(name, pid)
  rescue SystemCallError
    nil
  end

  # Runs TLC on spec with config.  Returns [distinct_state_count, dot_text].
  # options are extra TLC flags, such as -deadlock for a model whose final
  # states have no successor.
  def run_tlc(jar:, java:, dir:, spec:, config:, scale:, timeout:, tag:, options: [])
    metadir = File.join(dir, "#{tag}-metadir")
    dot_path = File.join(dir, "#{tag}.dot")
    argv = [java, "-Djava.io.tmpdir=#{dir}", "-XX:+UseParallelGC", "-Xmx512m", "-jar", jar,
            "-tool", "-workers", "1", "-fp", "0", "-seed", "1", *options, "-metadir", metadir,
            "-config", config, "-dump", "dot,actionlabels", dot_path, spec]
    status, stdout, stderr = run_child(argv, env: {}, dir: dir, chdir: dir, timeout: timeout, scale: scale, tag: tag)
    fail!("TLC timed out after #{timeout * scale} seconds\n#{stdout}") if status.nil?
    fail!("TLC exited with #{status.exitstatus.inspect}\n#{stdout}\n#{stderr}") unless status.success?
    distinct = check_tlc_messages(stdout)
    fail!("TLC did not write #{dot_path}") unless File.file?(dot_path)
    [distinct, File.read(dot_path)]
  end

  def check_tlc_messages(stdout)
    messages = []
    current = nil
    stdout.each_line(chomp: true) do |line|
      if (start = line.match(/\A@!@!@STARTMSG (\d+):(\d+) @!@!@\z/))
        fail!("nested TLC message at #{line}") if current
        current = { code: Integer(start[1], 10), klass: Integer(start[2], 10), body: +"" }
      elsif (finish = line.match(/\A@!@!@ENDMSG (\d+) @!@!@\z/))
        fail!("unmatched TLC message end at #{line}") unless current && current[:code] == Integer(finish[1], 10)
        messages << current
        current = nil
      elsif current
        current[:body] << line << "\n"
      end
    end
    fail!("unterminated TLC message #{current[:code]}") if current
    fail!("TLC produced no tool messages") if messages.empty?
    messages.each do |message|
      if (klass = REJECTED_CLASSES[message[:klass]])
        fail!("TLC #{klass} message #{message[:code]}:\n#{message[:body]}")
      end
      fail!("TLC reported failure code #{message[:code]}:\n#{message[:body]}") if REJECTED_CODES.include?(message[:code])
    end
    codes = messages.map { |message| message[:code] }
    fail!("TLC did not report success (#{TLC_SUCCESS}); codes: #{codes.inspect}") unless codes.include?(TLC_SUCCESS)
    fail!("TLC did not report statistics (#{TLC_STATS})") unless codes.include?(TLC_STATS)
    fail!("TLC did not finish with message #{TLC_FINISHED}; last code #{codes.last}") unless codes.last == TLC_FINISHED
    stats = messages.find { |message| message[:code] == TLC_STATS }
    match = stats[:body].match(/\A(\d+) states generated, (\d+) distinct states found, (\d+) states left on queue\.\n\z/)
    fail!("unexpected TLC statistics: #{stats[:body].inspect}") unless match
    fail!("TLC left #{match[3]} states on the queue") unless match[3] == "0"
    Integer(match[2], 10)
  end

  # Reads the TLC printing of a value: integers, strings, booleans, tuples,
  # records, and sets of integers (enumerated or as a range).
  module Value
    module_function

    def parse(text)
      reader = Reader.new(text)
      value = reader.value
      reader.skip_space
      RactorTLA.fail!("trailing text in TLA+ value #{text.inspect}") unless reader.eos?
      value
    end

    class Reader
      def initialize(text)
        @text = text
        @pos = 0
      end

      def eos?
        @pos >= @text.size
      end

      def skip_space
        @pos += 1 while !eos? && @text[@pos] == " "
      end

      def value
        skip_space
        case @text[@pos]
        when "<" then tuple
        when "[" then record
        when "{" then set
        when '"' then string
        when "T", "F" then boolean
        when "-", /\d/ then integer_or_range
        else RactorTLA.fail!("unknown TLA+ value at #{@pos} in #{@text.inspect}")
        end
      end

      def expect(literal)
        skip_space
        RactorTLA.fail!("expected #{literal.inspect} at #{@pos} in #{@text.inspect}") unless @text[@pos, literal.size] == literal
        @pos += literal.size
      end

      def peek(literal)
        skip_space
        @text[@pos, literal.size] == literal
      end

      def tuple
        expect("<<")
        items = []
        unless peek(">>")
          loop do
            items << value
            break unless peek(",")
            expect(",")
          end
        end
        expect(">>")
        items
      end

      def record
        expect("[")
        fields = {}
        loop do
          skip_space
          name = @text[@pos..].match(/\A[A-Za-z_][A-Za-z0-9_]*/)&.to_s
          RactorTLA.fail!("expected a record field at #{@pos} in #{@text.inspect}") unless name
          @pos += name.size
          expect("|->")
          RactorTLA.fail!("duplicate record field #{name}") if fields.key?(name)
          fields[name] = value
          break unless peek(",")
          expect(",")
        end
        expect("]")
        fields
      end

      def set
        expect("{")
        items = Set.new
        unless peek("}")
          loop do
            item = value
            RactorTLA.fail!("duplicate set element #{item.inspect}") if items.include?(item)
            items << item
            break unless peek(",")
            expect(",")
          end
        end
        expect("}")
        items
      end

      def string
        expect('"')
        result = +""
        loop do
          RactorTLA.fail!("unterminated string in #{@text.inspect}") if eos?
          char = @text[@pos]
          @pos += 1
          case char
          when '"' then return result
          when "\\"
            escaped = @text[@pos]
            @pos += 1
            RactorTLA.fail!("unknown string escape in #{@text.inspect}") unless ['"', "\\"].include?(escaped)
            result << escaped
          else
            result << char
          end
        end
      end

      def boolean
        if peek("TRUE")
          expect("TRUE")
          true
        else
          expect("FALSE")
          false
        end
      end

      def integer_or_range
        match = @text[@pos..].match(/\A-?\d+/)
        @pos += match[0].size
        low = Integer(match[0], 10)
        return low unless peek("..")
        expect("..")
        high_match = @text[@pos..].match(/\A-?\d+/)
        RactorTLA.fail!("bad range in #{@text.inspect}") unless high_match
        @pos += high_match[0].size
        Set.new(low..Integer(high_match[0], 10))
      end
    end
  end

  # State graph written by `-dump dot,actionlabels` of TLA+ Tools v1.7.4.
  class Dot
    HEADER = ["strict digraph DiskGraph {", "nodesep=0.35;", "subgraph cluster_graph {", 'color="white";'].freeze
    INITIAL_NODE = /\A(-?\d+) \[label="(.*)",style = filled\]\z/
    NODE = /\A(-?\d+) \[label="(.*)"\];\z/
    ACTION_EDGE = /\A(-?\d+) -> (-?\d+) \[label="([A-Za-z]+)",color="black",fontcolor="black"\];\z/
    STUTTER_EDGE = /\A(-?\d+) -> (-?\d+) \[style="dashed"\];\z/
    RANK_LINE = /\A\{rank = same; ((?:-?\d+;)+)\}\z/

    Node = Struct.new(:id, :fields, :initial)
    Edge = Struct.new(:from, :to, :label)

    attr_reader :nodes, :edges, :initial_ids

    # variables: exact state-variable names every node must carry.
    # stutter: true requires one dashed self-loop per node (TLC adds it under
    # liveness checking); false requires none.
    # sinks: true for a model checked with -deadlock.  TLC leaves states with
    # no successor out of the rank footer; every other state must be in it.
    def self.parse(text, variables:, stutter:, sinks: false)
      new(text, variables, stutter, sinks)
    end

    def initialize(text, variables, stutter, sinks)
      lines = text.lines(chomp: true)
      RactorTLA.fail!("DOT header is #{lines.first(4).inspect}") unless lines.first(4) == HEADER
      RactorTLA.fail!("DOT does not end with two closing braces") unless lines.last(2) == ["}", "}"]
      @nodes = {}
      @edges = []
      @initial_ids = []
      loops = Hash.new(0)
      ranked = []
      lines[4...-2].each do |line|
        if (match = line.match(INITIAL_NODE))
          add_node(match[1], match[2], true, variables)
        elsif (match = line.match(NODE))
          add_node(match[1], match[2], false, variables)
        elsif (match = line.match(ACTION_EDGE))
          @edges << Edge.new(match[1], match[2], match[3])
        elsif (match = line.match(STUTTER_EDGE))
          RactorTLA.fail!("dashed edge #{line} is not a self-loop") unless match[1] == match[2]
          loops[match[1]] += 1
        elsif (match = line.match(RANK_LINE))
          ranked.concat(match[1].split(";"))
        else
          RactorTLA.fail!("unknown DOT line: #{line.inspect}")
        end
      end
      RactorTLA.fail!("DOT has no initial node") if @initial_ids.empty?
      @edges.each do |edge|
        RactorTLA.fail!("edge #{edge.from} -> #{edge.to} has an unknown endpoint") unless @nodes.key?(edge.from) && @nodes.key?(edge.to)
      end
      expected_loops = stutter ? 1 : 0
      @nodes.each_key do |id|
        RactorTLA.fail!("node #{id} has #{loops[id]} dashed stuttering loops, expected #{expected_loops}") unless loops[id] == expected_loops
      end
      RactorTLA.fail!("dashed loops name unknown nodes") unless loops.keys.all? { |id| @nodes.key?(id) }
      RactorTLA.fail!("rank footer names unknown nodes") unless ranked.all? { |id| @nodes.key?(id) }
      if sinks
        sources = @edges.map(&:from).uniq
        unranked = @nodes.keys - ranked
        RactorTLA.fail!("rank footer misses a state that has a successor") unless (unranked & sources).empty?
      else
        RactorTLA.fail!("rank footer does not cover every node") unless ranked.uniq.size == @nodes.size
      end
    end

    def self.unescape_label(label)
      label.gsub(/\\(.)/) do
        case Regexp.last_match(1)
        when "\\" then "\\"
        when '"' then '"'
        when "n" then "\n"
        else RactorTLA.fail!("unknown escape in DOT label: #{Regexp.last_match(0).inspect}")
        end
      end
    end

    private

    def add_node(id, label, initial, variables)
      RactorTLA.fail!("duplicate node #{id}") if @nodes.key?(id)
      fields = {}
      current = nil
      Dot.unescape_label(label).each_line(chomp: true) do |line|
        if (match = line.match(/\A\/\\ ([A-Za-z_][A-Za-z0-9_]*) = (.*)\z/))
          RactorTLA.fail!("duplicate variable #{match[1]} in node #{id}") if fields.key?(match[1])
          current = match[1]
          fields[current] = match[2]
        elsif current && line.start_with?(" ")
          fields[current] = "#{fields[current]} #{line.strip}"
        else
          RactorTLA.fail!("unexpected state line #{line.inspect} in node #{id}")
        end
      end
      RactorTLA.fail!("node #{id} has variables #{fields.keys.sort.inspect}") unless fields.keys.sort == variables.sort
      @nodes[id] = Node.new(id, fields, initial)
      @initial_ids << id if initial
    end
  end
end
