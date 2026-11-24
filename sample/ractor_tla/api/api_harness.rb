# frozen_string_literal: true

# Replays one path of a Ractor API model against CRuby.
#
#   ruby api_harness.rb --module=RactorPorts --path=steps.json
#
# The JSON file names the module and aspect and lists the steps.  Each step
# carries the call (`op`, `args`), the outcome the model expects (`ok`, `nil`,
# `true`, `false`, or an exception class name), and the values of the
# observable variables after the call.  The harness performs every call in
# the named Ractor, compares the outcome, then compares the observables.
# Worker Ractors act only on command through a private control port and
# answer on one main-owned event port.  Those ports, the ready handshake, and
# the termination monitor are instrumentation and are not modeled.
#
# Modules that set SIGNATURE run their calls on modeled threads (agents).
# An agent is a Ruby thread of its Ractor that takes commands from a
# Thread::Queue and reports what each call returned on a main-owned wake
# port.  A call that does not return is "blocks": the agent is inside the
# call and sleeps.  After every step the harness waits for the threads that
# the step woke and compares the outcome, the value, and the wakes with the
# model.  When they match one of the step's `alternatives` instead, CRuby
# took another allowed choice: the harness prints DIVERGED and stops.

require "json"
require "objspace"

Warning[:experimental] = false

$iso_gv = 1

module RactorAPIHarness
  class Failure < StandardError; end

  class IsolationK; end

  MESSAGE = :msg
  SCALE = Float(ENV.fetch("RACTOR_TLA_TIMEOUT_SCALE", "1"))
  STEP_TIMEOUT = 5 * SCALE
  OBSERVE_TIMEOUT = 2 * SCALE
  GRACE = 0.003
  FINAL_GRACE = 0.02

  def self.clock
    Process.clock_gettime(Process::CLOCK_MONOTONIC)
  end

  # A modeled thread.  `perform` runs one call and returns [outcome, label,
  # detail]; a call that never returns because the thread was killed
  # reports "killed" from its ensure clause.
  class Agent
    attr_reader :thread

    def initialize(tid, wakes, announce: false, &perform)
      @tid = tid
      @commands = Thread::Queue.new
      @phase = :idle
      @thread = Thread.new do
        Thread.current.report_on_exception = false
        while (command = @commands.pop)
          perform_call(command, wakes, announce, perform)
        end
      end
    end

    def call(op, args)
      @commands << [op, args]
    end

    def status
      return "dead" if @phase == :killed || !@thread.alive?
      return "idle" unless @phase == :in_call
      @thread.status == "sleep" ? "blocked" : "running"
    end

    private

    def perform_call((op, args), wakes, announce, perform)
      @phase = :in_call
      result = nil
      begin
        result = perform.call(op, args)
      ensure
        @phase = result ? :idle : :killed
        if result.nil? && announce
          $stdout.puts "unwound #{@tid}"
          $stdout.flush
        end
        wakes << [@tid, *(result || ["killed", "", nil])]
      end
    end
  end

  # Waits briefly for agents that a send to their Ractor woke to sleep again.
  def self.settled_statuses(agents)
    deadline = clock + 0.05
    loop do
      statuses = agents.transform_values(&:status)
      return statuses if statuses.none? { |_, status| status == "running" } || clock > deadline
      sleep 0.001
    end
  end

  # Ractor::Error carries several rules; the message tells them apart.
  MESSAGE_TAGS = {
    "only allowed from the creator Ractor of this port" => "creator",
    "closing port by other ractors is not allowed" => "closer",
    "The value was already taken" => "taken",
    "Only the successor ractor can take a value" => "successor"
  }.freeze

  class ExpectedAbort < StandardError; end

  # Raised by the driver to end the main script normally (RactorProcess).
  class MainReturn < Exception; end

  # The detail is text only: results travel between Ractors as messages.
  # check, when given, returns a problem string for a returned value that
  # differs from the value the model states.
  def self.classify(check = nil)
    value = yield
    outcome = case value
              when nil then "nil"
              when true then "true"
              when false then "false"
              else "ok"
              end
    detail = outcome == "ok" ? detail_of(value) : nil
    problem = check&.call(value)
    problem ? ["value-mismatch", "#{problem}, got #{detail_of(value)}"] : [outcome, detail]
  rescue StandardError => e
    tag = MESSAGE_TAGS[e.message]
    [tag ? "#{e.class.name}:#{tag}" : e.class.name, e.message]
  end

  # Like classify, with the value named by `labeler` instead of checked.
  def self.classify_label(labeler)
    value = yield
    outcome = case value
              when nil then "nil"
              when true then "true"
              when false then "false"
              else "ok"
              end
    label = outcome == "ok" ? (labeler.call(value) rescue "unlabeled") : ""
    [outcome, label, detail_of(value)]
  rescue StandardError, Interrupt => e
    tag = MESSAGE_TAGS[e.message]
    [tag ? "#{e.class.name}:#{tag}" : e.class.name, "", e.message]
  end

  def self.signature(step)
    { "outcome" => step.fetch("outcome"), "value" => step.fetch("value", ""), "wakes" => step.fetch("wakes", []) }
  end

  def self.detail_of(value)
    return "Ractor::MovedObject" if Ractor::MovedObject === value
    value.inspect[0, 80]
  rescue StandardError
    "unprintable"
  end

  def self.worker_main(mod, id, events, wakes, setup_args)
    Thread.current.report_on_exception = false
    control = Ractor::Port.new
    state = mod.worker_setup(id, setup_args)
    if mod.respond_to?(:worker_agents)
      announce = mod.const_defined?(:ANNOUNCE, false)
      state[:agents] = mod.worker_agents(state).to_h do |tid|
        agent = Agent.new(tid, wakes, announce: announce) do |op, args|
          RactorAPIHarness.classify_label(->(value) { mod.worker_label(state, op, value) }) do
            mod.worker_thread_call(state, tid, op, args)
          end
        end
        [tid, agent]
      end
    end
    events << [:ready, id, control, mod.worker_announce(state), state.fetch(:agents, {}).keys]
    loop do
      command, op, args, value = control.receive
      case command
      when :call
        check = value.nil? || value.empty? ? nil : mod.worker_value_check(state, value)
        events << [:outcome, id, RactorAPIHarness.classify(check) { mod.worker_call(state, op, args) }]
      when :agent_call
        state[:agents].fetch(op).call(*args)
      when :agent_status
        events << [:agent_status, id, RactorAPIHarness.settled_statuses(state[:agents])]
      when :run
        mod.worker_run(state, op)
      when :observe
        events << [:observed, id, mod.worker_observe(state)]
      when :finish
        break
      when :abort
        raise ExpectedAbort, "expected failure"
      else
        raise Failure, "worker #{id}: unknown command #{command.inspect}"
      end
    end
    mod.worker_value(state)
  rescue ExpectedAbort
    raise
  rescue Exception => e
    $stderr.puts "worker #{id} failed: #{e.class}: #{e.message}" unless e.is_a?(SystemExit)
    raise
  end

  class Driver
    attr_reader :workers, :ports, :state, :announced

    def initialize(mod, aspect)
      @mod = mod
      @aspect = aspect
      @events = Ractor::Port.new
      @monitor = Ractor::Port.new
      @wakes = Ractor::Port.new
      @workers = {}
      @controls = {}
      @ports = {}
      @state = {}
      @announced = {}
      @value = ""
      @agents = {}
      @agent_home = {}
      @arrived = {}
      @killed = {}
    end

    # The agent reported "killed": it ran its ensure clauses inside a call.
    def killed?(tid)
      @killed.key?(tid)
    end

    # Classifies a call made in main against the value of the current step.
    def classify(&block)
      check = @value.empty? ? nil : @mod.main_value_check(self, @value)
      RactorAPIHarness.classify(check, &block)
    end

    def spawn_worker(id, setup_args)
      worker = Ractor.new(@mod, id, @events, @wakes, setup_args, name: "api-worker-#{id}") do |*args|
        RactorAPIHarness.worker_main(*args)
      end
      port, message = Ractor.select(@events, worker)
      raise Failure, "worker #{id} terminated before its handshake with #{message.inspect}" unless port.equal?(@events)
      event, ready_id, control, announced, agent_ids = message
      raise Failure, "worker #{id} did not announce itself: #{[event, ready_id].inspect}" unless event == :ready && ready_id == id
      @workers[id] = worker
      @controls[id] = control
      @announced[id] = announced
      agent_ids.each { |tid| @agent_home[tid] = id }
      worker
    end

    # Starts the modeled threads of main.
    def start_agents(tids, announce: false)
      tids.each do |tid|
        @agents[tid] = Agent.new(tid, @wakes, announce: announce) do |op, args|
          RactorAPIHarness.classify_label(->(value) { @mod.main_label(self, op, value) }) do
            @mod.main_thread_call(self, tid, op, args)
          end
        end
      end
    end

    def agent_thread(tid)
      @agents.fetch(tid).thread
    end

    def run_in_worker(id, op)
      @controls.fetch(id) << [:run, op]
    end

    # Status of every agent: idle, blocked, running, or dead.  The agents of
    # a terminated worker are dead.
    def agent_statuses
      statuses = @agents.transform_values(&:status)
      @agent_home.group_by { |_, id| id }.each do |id, pairs|
        unless @workers.fetch(id).alive?
          pairs.each { |tid, _| statuses[tid] = "dead" }
          next
        end
        @controls.fetch(id) << [:agent_status]
        event, from, result = @events.receive(timeout: STEP_TIMEOUT)
        raise Failure, "expected agent statuses from worker #{id}, got #{[event, from].inspect}" unless event == :agent_status && from == id
        statuses.merge!(result)
      end
      statuses
    end

    def drain_wakes
      while (message = @wakes.receive(timeout: 0))
        tid, *result = message
        raise Failure, "thread #{tid} returned twice: #{@arrived[tid].inspect}, then #{result.inspect}" if @arrived.key?(tid)
        @arrived[tid] = result
      end
    end

    # Runs op on agent tid.  Returns [outcome, label, detail]; "blocks" when
    # the model expects the call to block and the agent sleeps inside it.
    def agent_step(tid, op, args, expected)
      if (agent = @agents[tid])
        agent.call(op, args)
      else
        @controls.fetch(@agent_home.fetch(tid)) << [:agent_call, tid, [op, args]]
      end
      deadline = RactorAPIHarness.clock + STEP_TIMEOUT
      loop do
        drain_wakes
        return @arrived.delete(tid) if @arrived.key?(tid)
        if %w[blocks runs].include?(expected) && agent_statuses[tid] == (expected == "blocks" ? "blocked" : "running")
          sleep GRACE
          drain_wakes
          return @arrived.delete(tid) if @arrived.key?(tid)
          return [expected, "", nil] if agent_statuses[tid] == (expected == "blocks" ? "blocked" : "running")
        end
        return ["blocks", "", "no return within #{STEP_TIMEOUT}s"] if RactorAPIHarness.clock > deadline
        sleep 0.0005
      end
    end

    # A call made by the driver itself, in main's main thread.
    def classify_driver(&block)
      RactorAPIHarness.classify_label(->(_) { "" }, &block)
    end

    # A call that the model gives to main, made by the driver thread.
    def main_step(op, args)
      RactorAPIHarness.classify_label(->(value) { @mod.main_label(self, op, value) }) do
        @mod.main_thread_call(self, 0, op, args)
      end
    end

    # Waits until the threads that returned form the wake set of the step or
    # of one of its alternatives, then a little longer for late ones.
    def collect_wakes(step)
      sets = ([step] + step.fetch("alternatives", [])).map { |s| s.fetch("wakes", []).map { |w| w["thread"] }.sort }.uniq
      deadline = RactorAPIHarness.clock + STEP_TIMEOUT
      loop do
        drain_wakes
        if sets.include?(@arrived.keys.sort)
          sleep GRACE
          drain_wakes
          break if sets.include?(@arrived.keys.sort)
        end
        break if RactorAPIHarness.clock > deadline
        sleep 0.0005
      end
      wakes = @arrived.sort.map { |tid, (outcome, label, _)| { "thread" => tid, "outcome" => outcome, "value" => label } }
      wakes.each { |wake| @killed[wake["thread"]] = true if wake["outcome"] == "killed" }
      details = @arrived.transform_values { |result| result[2] }
      @arrived.clear
      [wakes, details]
    end

    def check_observables(expected, where)
      deadline = RactorAPIHarness.clock + OBSERVE_TIMEOUT
      loop do
        observed = @mod.observe(self)
        name, value = expected.find { |n, v| !observed.key?(n) || observed[n] != v }
        return unless name
        if RactorAPIHarness.clock > deadline
          raise Failure, "#{where}: #{name} is #{observed[name].inspect}, model says #{value.inspect}"
        end
        sleep 0.001
      end
    end

    def run_signature(steps)
      steps.each_with_index do |step, index|
        op = step.fetch("op")
        args = step.fetch("args")
        where = "step #{index} #{op}#{args.inspect}"
        outcome, label, detail = @mod.call(self, op, args, step.fetch("outcome"))
        wakes, details = collect_wakes(step)
        observed = { "outcome" => outcome, "value" => label, "wakes" => wakes }
        expected = RactorAPIHarness.signature(step)
        unless observed == expected
          return [:diverged, index, observed] if step.fetch("alternatives", []).include?(observed)
          raise Failure, "#{where}: expected #{JSON.generate(expected)}, observed #{JSON.generate(observed)} " \
                         "(#{detail.inspect}, wakes #{details.inspect})"
        end
        check_observables(step.fetch("observe"), where)
      end
      sleep FINAL_GRACE
      drain_wakes
      raise Failure, "after the last step: threads returned late: #{@arrived.inspect}" unless @arrived.empty?
      check_observables(steps.last.fetch("observe"), "after the last step") unless steps.empty?
      [:passed]
    end

    def worker_alive?(id)
      @workers.fetch(id).alive?
    end

    def ask_worker(id, op, args)
      @controls.fetch(id) << [:call, op, args, @value]
      event, from, result = @events.receive
      raise Failure, "expected an outcome from worker #{id}, got #{[event, from].inspect}" unless event == :outcome && from == id
      result
    end

    def observe_worker(id)
      @controls.fetch(id) << [:observe]
      event, from, result = @events.receive
      raise Failure, "expected observations from worker #{id}, got #{[event, from].inspect}" unless event == :observed && from == id
      result
    end

    # The instrumentation monitor is registered last, so its token arrives
    # after every token of the modeled monitor ports.
    def finish_worker(id, kind = :exit)
      worker = @workers.fetch(id)
      raise Failure, "worker #{id} could not be monitored" unless worker.monitor(@monitor) == true
      @controls.fetch(id) << [kind == :abort ? :abort : :finish]
      token = @monitor.receive
      expected = [worker, kind == :abort ? :aborted : :exited]
      raise Failure, "unexpected monitor token #{token.inspect}, expected #{expected.inspect}" unless token == expected
      token
    end

    # Runs op in the worker's main thread (worker_run) and waits for the
    # worker to terminate.  Returns the termination kind.
    def terminate_worker(id, op)
      worker = @workers.fetch(id)
      raise Failure, "worker #{id} could not be monitored" unless worker.monitor(@monitor) == true
      run_in_worker(id, op)
      token = @monitor.receive
      raise Failure, "unexpected monitor token #{token.inspect}" unless token[0] == worker
      token[1]
    end

    def run(steps)
      @mod.setup(self, @aspect)
      return run_signature(steps) if @mod.const_defined?(:SIGNATURE, false)
      steps.each_with_index do |step, index|
        op = step.fetch("op")
        args = step.fetch("args")
        @value = step.fetch("value", "")
        outcome, detail = @mod.call(self, op, args)
        expected = step.fetch("outcome")
        unless outcome == expected
          raise Failure, "step #{index} #{op}#{args.inspect}: expected #{expected}, observed #{outcome} (#{detail.inspect})"
        end
        observed = @mod.observe(self)
        step.fetch("observe").each do |name, value|
          unless observed.key?(name) && observed[name] == value
            raise Failure, "step #{index} #{op}#{args.inspect}: #{name} is #{observed[name].inspect}, model says #{value.inspect}"
          end
        end
      end
      [:passed]
    end
  end

  module Modules
    # RactorPorts.tla: ports 1 (worker default), 2 and 3 (main-owned).
    module RactorPorts
      MAIN = 1
      WORKER = 2
      DEFAULT = 1

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless aspect == "ports"
        driver.ports[2] = Ractor::Port.new
        driver.ports[3] = Ractor::Port.new
        worker = driver.spawn_worker(WORKER, [driver.ports[2], driver.ports[3]])
        driver.ports[DEFAULT] = worker.default_port
      end

      def self.call(driver, op, args)
        return driver.classify { driver.finish_worker(WORKER) } if op == "FinishWorker"
        caller = args.fetch(0)
        if caller == MAIN
          driver.classify { main_call(driver, op, args) }
        else
          driver.ask_worker(caller, op, args)
        end
      end

      def self.main_call(driver, op, args)
        worker = driver.workers.fetch(WORKER)
        port = ->(id) { driver.ports.fetch(id) }
        case op
        when "Send" then args[1] == DEFAULT ? worker.send(MESSAGE) : port.(args[1]).send(MESSAGE)
        when "Receive" then port.(args[1]).receive
        when "Poll" then port.(args[1]).receive(timeout: 0)
        when "Close" then args[1] == DEFAULT ? worker.close : port.(args[1]).close
        when "Select" then Ractor.select(*args[1..].map(&port))
        when "SelectPoll" then Ractor.select(*args[1..].map(&port), timeout: 0)
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.observe(driver)
        {
          "portOpen" => [1, 2, 3].map { |id| !driver.ports.fetch(id).closed? },
          "workerAlive" => driver.worker_alive?(WORKER)
        }
      end

      def self.worker_setup(_id, setup_args)
        p1, p2 = setup_args
        { ports: { 2 => p1, 3 => p2 } }
      end

      def self.worker_call(state, op, args)
        port = ->(id) { id == DEFAULT ? Ractor.current.default_port : state[:ports].fetch(id) }
        case op
        when "Send" then args[1] == DEFAULT ? Ractor.current.send(MESSAGE) : port.(args[1]).send(MESSAGE)
        when "Receive" then args[1] == DEFAULT ? Ractor.receive : port.(args[1]).receive
        when "Poll" then args[1] == DEFAULT ? Ractor.receive(timeout: 0) : port.(args[1]).receive(timeout: 0)
        when "Close" then args[1] == DEFAULT ? Ractor.current.close : port.(args[1]).close
        when "Select" then Ractor.select(*args[1..].map(&port))
        when "SelectPoll" then Ractor.select(*args[1..].map(&port), timeout: 0)
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end

      VALUE_PORTS = { "D" => 1, "P1" => 2, "P2" => 3 }.freeze

      # "msg": the message itself; a port name: [that port, the message].
      def self.value_check(value, port)
        expected = value == "msg" ? MESSAGE : [port.(VALUE_PORTS.fetch(value)), MESSAGE]
        ->(result) { "expected #{value}" unless result == expected }
      end

      def self.main_value_check(driver, value)
        value_check(value, ->(id) { driver.ports.fetch(id) })
      end

      def self.worker_value_check(state, value)
        value_check(value, ->(id) { id == DEFAULT ? Ractor.current.default_port : state[:ports].fetch(id) })
      end
    end

    # RactorTermination.tla: Ractor 2 terminates, main and Ractor 3 observe.
    module RactorTermination
      MAIN = 1
      TARGET = 2
      OBSERVER = 3
      M = 1

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless %w[exit abort].include?(aspect)
        driver.state[:aspect] = aspect
        target = driver.spawn_worker(TARGET, [])
        driver.spawn_worker(OBSERVER, [target])
        driver.ports[M] = Ractor::Port.new
      end

      def self.call(driver, op, args)
        if op == "Finish"
          return driver.classify { driver.finish_worker(TARGET, driver.state[:aspect] == "abort" ? :abort : :exit) }
        end
        caller = args.fetch(0)
        if caller == MAIN
          driver.classify { main_call(driver, op, args) }
        else
          driver.ask_worker(caller, op, args)
        end
      end

      def self.main_call(driver, op, args)
        target = driver.workers.fetch(TARGET)
        port = driver.ports.fetch(M)
        case op
        when "Monitor" then target.monitor(port)
        when "Unmonitor" then target.unmonitor(port)
        when "ReceiveToken" then port.receive(timeout: 0)
        when "Join" then target.join
        when "Value" then target.value
        when "SelectRactor" then Ractor.select(target, timeout: 0)
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.observe(driver)
        { "alive" => driver.worker_alive?(TARGET) }
      end

      def self.worker_setup(id, setup_args)
        return {} if id == TARGET
        { target: setup_args.fetch(0), port: Ractor::Port.new }
      end

      def self.worker_call(state, op, _args)
        target = state.fetch(:target)
        port = state.fetch(:port)
        case op
        when "Monitor" then target.monitor(port)
        when "Unmonitor" then target.unmonitor(port)
        when "ReceiveToken" then port.receive(timeout: 0)
        when "Join" then target.join
        when "Value" then target.value
        when "SelectRactor" then Ractor.select(target, timeout: 0)
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end

      # The target's block returns :finished (worker_value).
      def self.value_check(value, target)
        expected = case value
                   when "exited", "aborted" then [target, value.to_sym]
                   when "finished" then :finished
                   when "selected" then [target, :finished]
                   when "self" then target
                   else raise Failure, "unknown value #{value.inspect}"
                   end
        ->(result) { "expected #{value}" unless result == expected }
      end

      def self.main_value_check(driver, value)
        value_check(value, driver.workers.fetch(TARGET))
      end

      def self.worker_value_check(state, value)
        value_check(value, state.fetch(:target))
      end
    end

    # RactorTransfer.tla: main owns a root (1) and maybe a leaf (2); objects
    # carry their tag as the string digits or the first Array element.  The
    # worker reports each held object's object_id, frozen flag, and whether
    # the root kept the aliases or the cycle of its aspect.
    module RactorTransfer
      MAIN = 1
      WORKER = 2
      ROOT = 1
      LEAF = 2

      def self.setup(driver, aspect)
        objects = {}
        leaf = String.new("obj-2")
        case aspect
        when "string" then objects[ROOT] = String.new("obj-1")
        when "pair"
          objects[ROOT] = [ROOT, leaf, leaf]
          objects[LEAF] = leaf
        when "cycle"
          root = [ROOT, leaf]
          root << root
          objects[ROOT] = root
          objects[LEAF] = leaf
        when "frozen"
          objects[ROOT] = [ROOT, leaf].freeze
          objects[LEAF] = leaf
        when "thread"
          thread = Thread.new {}
          thread.join
          objects[ROOT] = thread
        else raise Failure, "unknown aspect #{aspect}"
        end
        driver.state[:objects] = objects
        driver.state[:ids] = objects.transform_values(&:object_id)
        driver.spawn_worker(WORKER, [aspect])
      end

      def self.call(driver, op, args)
        return driver.ask_worker(WORKER, op, args) if op == "Receive" || (op == "Touch" && args.fetch(0) == WORKER)
        driver.classify { main_call(driver, op, args) }
      end

      def self.main_call(driver, op, args)
        worker = driver.workers.fetch(WORKER)
        object = driver.state.fetch(:objects).fetch(args.last)
        case op
        when "Freeze" then object.freeze
        when "MakeShareable" then Ractor.make_shareable(object)
        when "MakeShareableCopy" then Ractor.make_shareable(object, copy: true)
        when "SendCopy" then worker.send(object)
        when "SendMove" then worker.send(object, move: true)
        when "Touch" then object.class
        when "NewRactorArg" then Ractor.new(object) { |arg| arg.class }.join
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.observe(driver)
        objects = driver.state.fetch(:objects)
        states = [ROOT, LEAF].map do |tag|
          if !objects.key?(tag) then "absent"
          elsif Ractor::MovedObject === objects[tag] then "moved"
          elsif objects[tag].frozen? then "frozen"
          else "mutable"
          end
        end
        shareable = [ROOT, LEAF].map { |tag| objects.key?(tag) && Ractor.shareable?(objects[tag]) }
        ids = driver.state.fetch(:ids)
        held = driver.observe_worker(WORKER).fetch("held")
        worker_has = [ROOT, LEAF].zip(held).map do |tag, (object_id, frozen, shape_kept)|
          if object_id.nil? then "none"
          elsif !shape_kept then "broken"
          elsif object_id == ids[tag] then "shared"
          elsif frozen then "frozen"
          else "mutable"
          end
        end
        { "mainState" => states, "shareable" => shareable, "workerHas" => worker_has }
      end

      def self.worker_setup(_id, setup_args)
        { held: {}, aspect: setup_args.fetch(0) }
      end

      def self.worker_call(state, op, args)
        case op
        when "Receive"
          object = Ractor.receive(timeout: 0)
          collect(object, state[:held], {}.compare_by_identity) unless object.nil?
          object
        when "Touch" then state[:held].fetch(args.fetch(1)).class
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.collect(object, held, seen)
        return if seen[object]
        seen[object] = true
        case object
        when String then held[Integer(object[/\d+/], 10)] = object
        when Array
          held[object[0]] = object
          object[1..].each { |child| collect(child, held, seen) unless child.is_a?(Integer) }
        end
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(state)
        held = [ROOT, LEAF].map do |tag|
          object = state[:held][tag]
          object.nil? ? [nil, nil, nil] : [object.object_id, object.frozen?, shape_kept?(object, tag, state[:aspect])]
        end
        { "held" => held }
      end

      def self.shape_kept?(object, tag, aspect)
        return true unless tag == ROOT && object.is_a?(Array)
        case aspect
        when "pair" then object.size == 3 && object[1].equal?(object[2])
        when "cycle" then object.size == 3 && object[2].equal?(object)
        when "frozen" then object.size == 2
        else true
        end
      end

      def self.worker_value(_state)
        :finished
      end
    end

    # RactorIsolation.tla: class K (1) belongs to main, class L (2) to the
    # worker.  Both sides run the same calls through `perform`.
    module RactorIsolation
      MAIN = 1
      WORKER = 2
      K = 1
      L = 2
      OWN_OBJECT = 3
      SHARED_STRING = "shared"

      def self.setup(driver, aspect)
        driver.state[:aspect] = aspect
        driver.state[:module] = Module.new
        driver.spawn_worker(WORKER, [aspect])
        l, worker_module = driver.announced.fetch(WORKER)
        driver.state[:classes] = { K => IsolationK, L => l }
        driver.state[:modules] = { K => driver.state[:module], L => worker_module }
      end

      def self.call(driver, op, args)
        return driver.classify { driver.finish_worker(WORKER) } if op == "FinishWorker"
        caller = args.fetch(0)
        if caller == MAIN
          env = { aspect: driver.state[:aspect], classes: driver.state[:classes], module: driver.state[:module] }
          driver.classify { perform(op, args, env) }
        else
          driver.ask_worker(caller, op, args)
        end
      end

      def self.perform(op, args, env)
        klass = ->(t) { env.fetch(:classes).fetch(t) }
        case op
        when "Set" then set_slot(klass.(args[1]), env[:aspect], args[2] == 1 ? 1 : [1])
        when "Get" then get_slot(klass.(args[1]), env[:aspect])
        when "DefineMethod" then klass.(args[1]).class_eval("def slot_method; end")
        when "RemoveMethod" then klass.(args[1]).send(:remove_method, :slot_method)
        when "AliasMethod" then klass.(args[1]).alias_method(:slot_alias, :slot_method)
        when "Include" then klass.(args[1]).include(env.fetch(:module))
        when "FreezeClass" then klass.(args[1]).freeze
        when "GlobalGet" then $iso_gv
        when "GlobalSet" then $iso_gv = 2
        when "StderrGet" then $stderr.class
        when "CallUnsafe" then ObjectSpace.memsize_of("x")
        when "AtExit" then at_exit {}
        when "RegexpTimeout" then Regexp.timeout = nil
        when "DefineFinalizer"
          ObjectSpace.define_finalizer(args[1] == OWN_OBJECT ? Object.new : klass.(args[1]), proc {})
        when "ShareableProc" then shareable_proc(args[1])
        when "NewRactor" then new_ractor(args[1])
        when "DupClass" then klass.(args[1]).dup
        when "Instantiate" then klass.(args[1]).new
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.set_slot(klass, aspect, value)
        case aspect
        when "const", "readonly"
          verbose = $VERBOSE
          $VERBOSE = nil
          begin
            klass.const_set(:SLOT, value)
          ensure
            $VERBOSE = verbose
          end
        when "ivar" then klass.instance_variable_set(:@slot, value)
        when "cvar" then klass.class_variable_set(:@@slot, value)
        else raise Failure, "no slot in aspect #{aspect}"
        end
      end

      def self.get_slot(klass, aspect)
        case aspect
        when "const", "readonly" then klass.const_get(:SLOT, false)
        when "ivar" then klass.instance_variable_get(:@slot)
        when "cvar" then klass.class_variable_get(:@@slot)
        else raise Failure, "no slot in aspect #{aspect}"
        end
      end

      def self.slot_set?(klass, aspect)
        case aspect
        when "const", "readonly" then klass.const_defined?(:SLOT, false)
        when "ivar" then klass.instance_variable_defined?(:@slot)
        when "cvar" then klass.class_variable_defined?(:@@slot)
        else false
        end
      end

      def self.shareable_proc(capture)
        case capture
        when 1 then Ractor.shareable_proc { 1 }
        when 2
          shared = SHARED_STRING
          Ractor.shareable_proc { shared }
        when 3
          unshareable = []
          Ractor.shareable_proc { unshareable }
        when 4
          reassigned = 1
          reassigned = 2
          Ractor.shareable_proc { reassigned }
        when 5 then Ractor.shareable_proc(self: SHARED_STRING) { self }
        when 6 then Ractor.shareable_proc(self: +"self") { self }
        else raise Failure, "unknown capture #{capture}"
        end
      end

      def self.new_ractor(capture)
        ractor = case capture
                 when 1 then Ractor.new { 1 }
                 when 2
                   shared = SHARED_STRING
                   Ractor.new { shared }
                 when 3
                   unshareable = []
                   Ractor.new { unshareable }
                 when 4
                   reassigned = 1
                   reassigned = 2
                   Ractor.new { reassigned }
                 else raise Failure, "unknown capture #{capture}"
                 end
        ractor.join
      end

      def self.observe(driver)
        aspect = driver.state.fetch(:aspect)
        classes = driver.state.fetch(:classes)
        modules = driver.state.fetch(:modules)
        {
          "alive" => driver.worker_alive?(WORKER),
          "slotSet" => [K, L].map { |t| slot_set?(classes[t], aspect) },
          "defined" => [K, L].map { |t| classes[t].method_defined?(:slot_method) },
          "included" => [K, L].map { |t| classes[t].include?(modules[t]) },
          "frozenClass" => [K, L].map { |t| classes[t].frozen? }
        }
      end

      def self.worker_setup(_id, setup_args)
        { aspect: setup_args.fetch(0), l: Class.new, module: Module.new }
      end

      def self.worker_announce(state)
        [state[:l], state[:module]]
      end

      def self.worker_call(state, op, args)
        env = { aspect: state[:aspect], classes: { K => IsolationK, L => state[:l] }, module: state[:module] }
        perform(op, args, env)
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end
    end

    # RactorLocal.tla: keys 1 and 2 map to :k1 and :k2 in each Ractor's store.
    module RactorLocal
      MAIN = 1
      WORKER = 2
      KEYS = { 1 => :k1, 2 => :k2 }.freeze

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless aspect == "local"
        driver.spawn_worker(WORKER, [])
      end

      def self.call(driver, op, args)
        caller = args.fetch(0)
        if caller == MAIN
          driver.classify { perform(op, args, driver.workers.fetch(WORKER)) }
        else
          driver.ask_worker(caller, op, args)
        end
      end

      def self.value_for(kind)
        kind == 1 ? 1 : [1]
      end

      def self.perform(op, args, other)
        key = KEYS.fetch(args.fetch(1))
        case op
        when "Store" then Ractor[key] = value_for(args.fetch(2))
        when "Fetch" then Ractor[key]
        when "StoreIfAbsent" then Ractor.store_if_absent(key) { value_for(args.fetch(2)) }
        when "FetchOther" then other[key]
        when "StoreOther" then other[key] = value_for(args.fetch(2))
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.own_store
        KEYS.keys.sort.map do |id|
          case Ractor[KEYS[id]]
          when nil then "unset"
          when Integer then "shareable"
          else "unshareable"
          end
        end
      end

      def self.observe(driver)
        { "stored" => [own_store, driver.observe_worker(WORKER).fetch("store")] }
      end

      def self.worker_setup(_id, _setup_args)
        {}
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_call(_state, op, args)
        perform(op, args, Ractor.main)
      end

      def self.worker_observe(_state)
        { "store" => own_store }
      end

      def self.worker_value(_state)
        :finished
      end
    end

    # RactorBlocking.tla: threads 1 and 2 are agents of main, 3 and 4 agents
    # of the worker.  Port 1 is the worker's default port; ports 2 and 3
    # belong to main.  Port :monitor monitors the worker (aspects exit, abort).
    module RactorBlocking
      SIGNATURE = true
      WORKER = 2
      D = 1
      P1 = 2
      P2 = 3
      ACTIVE = { "main" => [1, 2, 3], "worker" => [1, 3, 4], "exit" => [1, 2], "abort" => [1, 2] }.freeze
      PORT_NAMES = { D => "D", P1 => "P1", P2 => "P2" }.freeze
      LONG = 600
      SHORT = 0.05

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless ACTIVE.key?(aspect)
        driver.state[:aspect] = aspect
        driver.ports[P1] = Ractor::Port.new
        driver.ports[P2] = Ractor::Port.new
        driver.ports[:monitor] = Ractor::Port.new
        worker = driver.spawn_worker(WORKER, [driver.ports[P1], driver.ports[P2], ACTIVE[aspect].select { |t| t > 2 }])
        driver.ports[D] = worker.default_port
        if %w[exit abort].include?(aspect) && worker.monitor(driver.ports[:monitor]) != true
          raise Failure, "the monitor port was not registered"
        end
        driver.start_agents(ACTIVE[aspect].select { |t| t <= 2 })
      end

      def self.call(driver, op, args, expected)
        case op
        when "FinishWorker" then driver.classify_driver { driver.finish_worker(WORKER) }
        when "Finish" then driver.classify_driver { driver.finish_worker(WORKER, driver.state[:aspect] == "abort" ? :abort : :exit) }
        else driver.agent_step(args.fetch(0), op, args, expected)
        end
      end

      def self.main_thread_call(driver, _tid, op, args)
        worker = driver.workers.fetch(WORKER)
        port = ->(id) { driver.ports.fetch(id) }
        case op
        when "Send" then args[1] == D ? worker.send(MESSAGE) : port.(args[1]).send(MESSAGE)
        when "Receive" then port.(args[1]).receive
        when "ReceiveTimed" then port.(args[1]).receive(timeout: LONG)
        when "ReceiveShort" then port.(args[1]).receive(timeout: SHORT)
        when "Select" then Ractor.select(port.(P1), port.(P2))
        when "Close" then port.(args[1]).close
        when "Raise" then driver.agent_thread(args[1]).raise(RuntimeError, "interrupt")
        when "Wakeup" then driver.agent_thread(args[1]).wakeup
        when "Kill" then driver.agent_thread(args[1]).kill
        when "Join" then worker.join
        when "Value" then worker.value
        when "SelectRactor" then Ractor.select(worker)
        when "ReceiveToken" then port.(:monitor).receive
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.label(op, value, worker, port)
        case op
        when "Receive", "ReceiveTimed", "ReceiveShort" then value == MESSAGE ? "msg" : "unexpected"
        when "Select"
          name = PORT_NAMES.find { |id, _| value[0].equal?(port.(id)) }&.last
          name && value[1] == MESSAGE ? name : "unexpected"
        when "Join" then value.equal?(worker) ? "self" : "unexpected"
        when "Value" then value == :finished ? "finished" : "unexpected"
        when "SelectRactor" then value[0].equal?(worker) && value[1] == :finished ? "selected" : "unexpected"
        when "ReceiveToken"
          value[0].equal?(worker) && %i[exited aborted].include?(value[1]) ? value[1].to_s : "unexpected"
        else ""
        end
      end

      def self.main_label(driver, op, value)
        label(op, value, driver.workers.fetch(WORKER), ->(id) { driver.ports.fetch(id) })
      end

      def self.observe(driver)
        statuses = driver.agent_statuses
        {
          "alive" => driver.worker_alive?(WORKER),
          "portOpen" => [D, P1, P2].map { |id| !driver.ports.fetch(id).closed? },
          "ts" => (1..4).map { |tid| statuses.fetch(tid, "idle") }
        }
      end

      def self.worker_setup(_id, setup_args)
        p1, p2, agent_ids = setup_args
        { ports: { P1 => p1, P2 => p2 }, agent_ids: agent_ids }
      end

      def self.worker_agents(state)
        state.fetch(:agent_ids)
      end

      def self.worker_thread_call(state, _tid, op, args)
        sibling = -> { state.fetch(:agents).fetch(args[1]).thread }
        case op
        when "Send" then args[1] == D ? Ractor.current.send(MESSAGE) : state[:ports].fetch(args[1]).send(MESSAGE)
        when "Receive" then Ractor.receive
        when "ReceiveTimed" then Ractor.receive(timeout: LONG)
        when "ReceiveShort" then Ractor.receive(timeout: SHORT)
        when "Close" then Ractor.current.close
        when "Raise" then sibling.().raise(RuntimeError, "interrupt")
        when "Wakeup" then sibling.().wakeup
        when "Kill" then sibling.().kill
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.worker_label(state, op, value)
        label(op, value, Ractor.current, ->(id) { id == D ? Ractor.current.default_port : state[:ports].fetch(id) })
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end
    end

    # RactorOrder.tla: main (1) and workers 2 and 3 send [sender, n] arrays,
    # the n-th message of each sender, to main's ports 1 and 2.  Every receive
    # and select uses timeout: 0.
    module RactorOrder
      SIGNATURE = true
      MAIN = 1
      WORKERS = [2, 3].freeze
      NAMES = { 2 => "W1", 3 => "W2" }.freeze
      PORT_NAMES = { 1 => "P1", 2 => "P2" }.freeze

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless %w[fifo mixed].include?(aspect)
        driver.state[:sent] = 0
        driver.ports[1] = Ractor::Port.new
        driver.ports[2] = Ractor::Port.new
        WORKERS.each do |w|
          driver.spawn_worker(w, [w, driver.ports[1], driver.ports[2], aspect == "fifo" ? [w] : []])
        end
      end

      def self.call(driver, op, args, expected)
        case op
        when "Send" then args[0] == MAIN ? driver.main_step(op, args) : driver.agent_step(args[0], op, args, expected)
        when "Finish" then driver.classify_driver { driver.finish_worker(args[0]) }
        else driver.main_step(op, args)
        end
      end

      # The counter counts only messages that the port accepted.
      def self.send_numbered(port, sender, counter)
        port.send([sender, counter[:sent] + 1])
        counter[:sent] += 1
        port
      end

      def self.main_thread_call(driver, _tid, op, args)
        source = ->(arg) { arg[0] == "port" ? driver.ports.fetch(arg[1]) : driver.workers.fetch(arg[1]) }
        case op
        when "Send" then send_numbered(driver.ports.fetch(args[1]), MAIN, driver.state)
        when "Poll" then driver.ports.fetch(args[0]).receive(timeout: 0)
        when "Close" then driver.ports.fetch(args[0]).close
        when "Select" then Ractor.select(*args.map(&source), timeout: 0)
        when "Value" then driver.workers.fetch(args[0]).value
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.message_label(message)
        message.is_a?(Array) && message.size == 2 ? "m#{message[0]}.#{message[1]}" : "unexpected"
      end

      def self.main_label(driver, op, value)
        case op
        when "Poll" then message_label(value)
        when "Select"
          port_id = PORT_NAMES.keys.find { |id| value[0].equal?(driver.ports[id]) }
          worker_id = WORKERS.find { |id| value[0] == driver.workers[id] }
          if port_id then "#{PORT_NAMES[port_id]} #{message_label(value[1])}"
          elsif worker_id && value[1] == :finished then NAMES[worker_id]
          else "unexpected"
          end
        when "Value" then value == :finished ? "finished" : "unexpected"
        else ""
        end
      end

      def self.observe(driver)
        {
          "portOpen" => [1, 2].map { |id| !driver.ports.fetch(id).closed? },
          "alive" => [true] + WORKERS.map { |id| driver.worker_alive?(id) }
        }
      end

      def self.worker_setup(_id, setup_args)
        id, p1, p2, agent_ids = setup_args
        { id: id, ports: { 1 => p1, 2 => p2 }, agent_ids: agent_ids, sent: 0 }
      end

      def self.worker_agents(state)
        state.fetch(:agent_ids)
      end

      def self.worker_thread_call(state, _tid, op, args)
        case op
        when "Send" then send_numbered(state[:ports].fetch(args[1]), state[:id], state)
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.worker_label(_state, _op, _value)
        ""
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end
    end

    # RactorProcess.tla: thread 1 is an agent of main, thread 2 an agent of
    # the worker; both print "unwound N" when they die inside a call.  The
    # driver, main's main thread, makes the process-level calls.  The replay
    # runs in a child process that the harness supervises.
    module RactorProcess
      SIGNATURE = true
      SUPERVISED = true
      ANNOUNCE = true
      WORKER = 2
      ENDINGS = %w[MainReturn MainExit MainRaise MainExitBang WorkerExitBang].freeze

      def self.setup(driver, aspect)
        raise Failure, "unknown aspect #{aspect}" unless %w[exit fork signal].include?(aspect)
        driver.state[:side] = "parent"
        driver.state[:handled] = 0
        driver.ports[:p1] = Ractor::Port.new
        driver.spawn_worker(WORKER, [])
        Signal.trap(:USR1) { driver.state[:handled] += 1 } if aspect == "signal"
        driver.start_agents([1], announce: true)
      end

      def self.call(driver, op, args, expected)
        return end_process(driver, op) if ENDINGS.include?(op)
        case op
        when "Block", "Spin", "Kill", "ForkInWorker", "WorkerTrap" then driver.agent_step(args.fetch(0), op, args, expected)
        else driver.main_step(op, args)
        end
      end

      # Never returns: the process ends.
      def self.end_process(driver, op)
        case op
        when "MainReturn" then raise MainReturn
        when "MainExit" then exit 3
        when "MainRaise" then raise "main boom"
        when "MainExitBang" then exit! 4
        when "WorkerExitBang"
          driver.run_in_worker(WORKER, :exit_bang)
          sleep
        end
      end

      def self.main_thread_call(driver, _tid, op, _args)
        worker = driver.workers.fetch(WORKER)
        port = driver.ports.fetch(:p1)
        case op
        when "Block" then port.receive
        when "Kill" then Process.kill(:USR1, Process.pid)
        when "SendPort" then port.send(MESSAGE)
        when "Poll" then port.receive(timeout: 0)
        when "AtExit" then at_exit { $stdout.puts "at_exit"; $stdout.flush }
        when "WorkerExit" then worker_exit(driver, worker)
        when "FinishWorker" then driver.finish_worker(WORKER)
        when "Fork" then fork_replay(driver)
        when "Count" then Ractor.count
        when "SendWorker" then worker.send(MESSAGE)
        when "Join" then worker.join
        when "Value" then worker.value
        when "NewRactor" then Ractor.new { :fresh }.value
        when "Interrupted" then interrupted_receive(port)
        else raise Failure, "unknown op #{op}"
        end
      end

      # Returns the class of the cause of the RemoteError that value raises.
      def self.worker_exit(driver, worker)
        kind = driver.terminate_worker(WORKER, :exit)
        return "terminated #{kind}" unless kind == :aborted
        worker.value
        "value returned"
      rescue Ractor::RemoteError => e
        e.cause.class.name
      end

      # The child returns :child and goes on with the replay.  The parent waits
      # for it, checks that its own worker and threads are as before, and ends
      # with the child's exit status.
      def self.fork_replay(driver)
        before = driver.agent_statuses
        alive = driver.worker_alive?(WORKER)
        $stdout.flush
        pid = Process.fork
        if pid.nil?
          driver.state[:side] = "child"
          return :child
        end
        _, status = Process.wait2(pid)
        after = driver.agent_statuses
        problems = []
        problems << "the worker terminated" if alive && !driver.worker_alive?(WORKER)
        problems << "thread states changed from #{before.inspect} to #{after.inspect}" unless before == after
        unless problems.empty?
          $stderr.puts "parent after fork: #{problems.join("; ")}"
          exit!(1)
        end
        exit!(status.exitstatus || 1)
      end

      # Main's main thread waits in receive; a helper sends SIGINT once it sleeps.
      def self.interrupted_receive(port)
        main = Thread.current
        helper = Thread.new do
          sleep 0.0005 until main.status == "sleep"
          Process.kill(:INT, Process.pid)
        end
        begin
          port.receive
        ensure
          helper.join
        end
      end

      def self.main_label(driver, op, value)
        case op
        when "Block", "Poll" then value == MESSAGE ? "msg" : "unexpected"
        when "Fork" then value == :child ? "child" : "unexpected"
        when "Count", "WorkerExit" then value.to_s
        when "Join" then value == driver.workers.fetch(WORKER) ? "self" : "unexpected"
        when "Value" then value == :finished ? "finished" : "unexpected"
        when "NewRactor" then value == :fresh ? "fresh" : "unexpected"
        else ""
        end
      end

      def self.observe(driver)
        statuses = driver.agent_statuses
        {
          "side" => driver.state[:side],
          "alive" => driver.worker_alive?(WORKER),
          "ts" => [1, 2].map { |tid| statuses.fetch(tid) },
          "unwound" => [1, 2].map { |tid| driver.killed?(tid) },
          "handled" => driver.state[:handled]
        }
      end

      def self.worker_setup(_id, _setup_args)
        {}
      end

      def self.worker_agents(_state)
        [2]
      end

      def self.worker_thread_call(_state, _tid, op, args)
        case op
        when "Block" then Ractor.receive
        when "Spin" then loop { Thread.pass }
        when "Kill" then Process.kill(:USR1, Process.pid)
        when "ForkInWorker" then Process.fork { exit!(0) }
        when "WorkerTrap" then worker_trap(args[1])
        else raise Failure, "unknown op #{op}"
        end
      end

      def self.worker_trap(capture)
        return Signal.trap(:USR2) { nil } if capture == 1
        held = +"unshareable"
        Signal.trap(:USR2) { held }
      end

      def self.worker_label(_state, op, value)
        op == "Block" && value == MESSAGE ? "msg" : ""
      end

      def self.worker_run(_state, op)
        case op
        when :exit then exit
        when :exit_bang then exit!(5)
        else raise Failure, "unknown worker command #{op.inspect}"
        end
      end

      def self.worker_announce(_state)
        nil
      end

      def self.worker_observe(_state)
        {}
      end

      def self.worker_value(_state)
        :finished
      end
    end
  end

  MODULES = {
    "RactorPorts" => Modules::RactorPorts,
    "RactorTermination" => Modules::RactorTermination,
    "RactorTransfer" => Modules::RactorTransfer,
    "RactorIsolation" => Modules::RactorIsolation,
    "RactorLocal" => Modules::RactorLocal,
    "RactorBlocking" => Modules::RactorBlocking,
    "RactorOrder" => Modules::RactorOrder,
    "RactorProcess" => Modules::RactorProcess
  }.freeze

  # Runs the replay in a child process and compares its exit status, its
  # stdout lines (in any order), and its stderr with the model.  A path that
  # does not end the process ends with exit!, so only the lines of threads
  # unwound during the path and the PASS line appear.
  def self.supervise(module_name, path, spec)
    require "open3"
    require "rbconfig"
    ruby = ENV.fetch("RUBY", RbConfig.ruby)
    out, err, status = Open3.capture3(ruby, "--disable-gems", __FILE__, "--module=#{module_name}", "--path=#{path}", "--inner")
    steps = spec.fetch("steps")
    pass = "api_harness: PASS module=#{module_name} aspect=#{spec["aspect"]} steps=#{steps.size}"
    last = steps.last
    ending = last && !last.fetch("exit", []).empty? ? last["exit"] : nil
    unwound = last ? last.fetch("observe").fetch("unwound").each_with_index.filter_map { |u, i| "unwound #{i + 1}" if u } : []
    lines = out.lines(chomp: true)
    if ending
      expected_lines = unwound + (ending["atExit"] ? ["at_exit"] : [])
      expected_status = ending["status"]
      stderr_ok = ending["stderr"].empty? ? err.empty? : err.include?(ending["stderr"])
    else
      expected_lines = unwound + [pass]
      expected_status = 0
      stderr_ok = err.empty?
    end
    problems = []
    unless status.exitstatus == expected_status
      problems << "exit status #{status.exitstatus.inspect} (signal #{status.termsig.inspect}), model says #{expected_status}"
    end
    problems << "stdout #{lines.sort.inspect}, model says #{expected_lines.sort.inspect}" unless lines.sort == expected_lines.sort
    problems << "stderr does not match the model's #{ending ? ending["stderr"].inspect : '""'}" unless stderr_ok
    raise Failure, "supervised replay: #{problems.join("; ")}\nchild stderr:\n#{err}" unless problems.empty?
    puts pass
  end

  def self.main(argv)
    module_name = nil
    path = nil
    inner = false
    argv.each do |arg|
      case arg
      when /\A--module=([A-Za-z]+)\z/ then module_name = $1
      when /\A--path=(.+)\z/ then path = $1
      when "--inner" then inner = true
      else raise Failure, "unknown argument #{arg.inspect}"
      end
    end
    raise Failure, "--module and --path are required" unless module_name && path
    mod = MODULES.fetch(module_name) { raise Failure, "unknown module #{module_name}" }
    spec = JSON.parse(File.read(path))
    raise Failure, "path file names module #{spec["module"]}" unless spec["module"] == module_name
    return supervise(module_name, path, spec) if mod.const_defined?(:SUPERVISED, false) && !inner
    steps = spec.fetch("steps")
    begin
      result, index, observed = Driver.new(mod, spec.fetch("aspect")).run(steps)
    rescue MainReturn
      return
    end
    if result == :diverged
      puts "api_harness: DIVERGED module=#{module_name} aspect=#{spec["aspect"]} step=#{index} chose=#{JSON.generate(observed)}"
    else
      puts "api_harness: PASS module=#{module_name} aspect=#{spec["aspect"]} steps=#{steps.size}"
    end
    return unless inner
    $stdout.flush
    exit!(0)
  end
end

RactorAPIHarness.main(ARGV) if $PROGRAM_NAME == __FILE__
