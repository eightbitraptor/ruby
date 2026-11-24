# frozen_string_literal: true

# Ractor worker pool that mirrors sample/ractor_tla/RactorPool.tla.
#
# Two worker Ractors process three jobs that travel as a shared reference,
# a deep copy, and a moved graph.  Main dispatches through default ports,
# collects results through one main-owned result port per worker with
# Ractor.select, then stops and observes each worker.
#
#   ruby ractor_pool.rb --mode=free [--heap-objects=N]
#   ruby ractor_pool.rb --mode=replay --schedule=11,12,21,...
#
# Replay mode drives one 17-token schedule from the TLC state graph and
# releases every worker step through a private control port, so each
# modeled action happens at its modeled position.  Free mode lets both
# workers run and collect concurrently with large heaps.

Warning[:experimental] = false

module RactorPool
  WORKERS = [1, 2].freeze
  JOBS = [1, 2, 3].freeze
  MODES = { 1 => :share, 2 => :copy, 3 => :move }.freeze
  REPLAY_HEAP_OBJECTS = 32
  DEFAULT_HEAP_OBJECTS = 50_000
  SCHEDULE_LENGTH = 17
  TOKENS = [11, 12, 21, 22, 31, 32, 41, 42, 50, 61, 62, 71, 72].freeze
  PROCESS_COUNTS = %i[count minor_gc_count major_gc_count global_gc_count].freeze
  LOCAL_COUNTS = %i[count major_gc_count global_gc_count global_gc_invocation_count].freeze
  SHARED_LEAF = "shared-config"
  COPY_LEAF = "copy-leaf"
  MOVE_LEAF = "move-leaf"

  class Failure < StandardError; end

  class HeapCell
    attr_reader :value

    def initialize(value)
      @value = value
    end
  end

  class GarbageCell
    def initialize(value)
      @value = value
    end
  end

  def self.check(condition, message)
    raise Failure, message unless condition
  end

  def self.check_runtime!
    check(GC.config[:implementation] == "default", "the default GC is required")
    check(!GC.stress, "GC.stress must be disabled")
    check(GC.method(:start).parameters.include?([:key, :global]), "GC.start(global:) is required")
    check(GC.method(:stat).parameters.include?([:key, :process]), "GC.stat(process:) is required")
    stat = GC.stat
    %i[global_gc_count global_gc_invocation_count].each do |key|
      check(stat.key?(key), "GC.stat lacks #{key}")
    end
    GC.stat(process: true)
  end

  def self.content_checksum(job)
    case job
    when Hash then job.fetch(:id) + job.fetch(:config).fetch(:weights).sum
    when Array then job.fetch(1) + job.fetch(3).bytes.sum
    else raise Failure, "unexpected job #{job.inspect}"
    end
  end

  def self.cell_value(job_id, index)
    job_id * 1_000_000 + index
  end

  def self.retained_checksum(job_ids, heap_objects)
    job_ids.sum { |job_id| heap_objects.times.sum { |i| cell_value(job_id, i) } }
  end

  def self.build_shared_job
    Ractor.make_shareable({
      id: 1,
      mode: :share,
      config: { name: SHARED_LEAF, weights: [3, 5, 7], nested: { depth: 2, tags: ["a", "b"] } }
    })
  end

  # [:job, id, mode, leaf, child aliasing the leaf, shared child, self]
  def self.build_graph(job_id, leaf, shared_child)
    child = [leaf]
    root = [:job, job_id, MODES.fetch(job_id), leaf, child, shared_child]
    root << root
    root
  end

  def self.check_graph_shape(graph, job_id, shared_child)
    check(graph[0] == :job && graph[1] == job_id, "job #{job_id}: wrong graph header")
    check(graph[2] == MODES.fetch(job_id), "job #{job_id}: wrong mode")
    check(graph[3].equal?(graph[4][0]), "job #{job_id}: alias not preserved")
    check(graph[6].equal?(graph), "job #{job_id}: cycle not preserved")
    check(graph[5].equal?(shared_child), "job #{job_id}: shared child is not identical")
    check(!graph[3].frozen?, "job #{job_id}: leaf arrived frozen")
  end

  def self.worker_main(id, shared_job, events, results, heap_objects)
    GC.disable
    Thread.current.report_on_exception = false
    control = Ractor::Port.new
    Ractor[:retained] = {}
    received = {}
    checksums = {}
    processed = []
    begin
      events << [:ready, id, control]
      loop do
        verb, worker_id, job_id = control.receive
        check(worker_id == id, "worker #{id}: command addressed to #{worker_id.inspect}")
        case verb
        when :receive
          job = Ractor.receive
          checksums[job_id] = check_received_job(job, job_id, shared_job)
          received[job_id] = job
          events << [:received, id, job_id]
        when :process
          check(received.key?(job_id), "worker #{id}: process before receive of job #{job_id}")
          received.delete(job_id)
          deltas = process_job(job_id, heap_objects)
          processed << job_id
          results << [:result, job_id, MODES.fetch(job_id), checksums.fetch(job_id), deltas, retained_count]
          events << [:processed, id, job_id]
        when :stats
          events << [:stats, id, ObjectSpace.each_object(HeapCell).count, local_retained_checksum,
                     GC.stat.slice(*LOCAL_COUNTS)]
        when :stop
          Ractor.current.close
          events << [:closed, id, Ractor.current.default_port.closed?]
          break
        else
          raise Failure, "worker #{id}: unknown command #{verb.inspect}"
        end
      end
      [id, processed, retained_count]
    rescue Exception => e
      $stderr.puts "worker #{id} failed: #{e.class}: #{e.message}"
      events << [:fatal, id, e.class.name, e.message]
      raise
    end
  end

  def self.check_received_job(job, job_id, shared_job)
    case MODES.fetch(job_id)
    when :share
      check(job.equal?(shared_job), "shared job is not the constructor reference")
      check(job[:config].equal?(shared_job[:config]), "shared config is not identical")
      check(Ractor.shareable?(job), "shared job arrived unshareable")
      content_checksum(job)
    when :copy
      check_graph_shape(job, job_id, shared_job[:config])
      check(job[3] == COPY_LEAF, "copy did not keep the send-time snapshot: #{job[3].inspect}")
      checksum = content_checksum(job)
      job[3] << "-worker"
      checksum
    when :move
      check_graph_shape(job, job_id, shared_job[:config])
      check(job[3] == MOVE_LEAF, "moved leaf changed: #{job[3].inspect}")
      content_checksum(job)
    end
  end

  def self.process_job(job_id, heap_objects)
    retained = Array.new(heap_objects) { |i| HeapCell.new(cell_value(job_id, i)) }
    garbage = Array.new(heap_objects) { |i| GarbageCell.new(i) }
    Ractor[:retained][job_id] = retained
    check(garbage.size == heap_objects, "temporary heap is incomplete")
    garbage = nil
    before = GC.stat
    GC.start(full_mark: true, immediate_mark: true, immediate_sweep: true, global: false)
    after = GC.stat
    check(after[:count] - before[:count] == 1, "local collection changed count by #{after[:count] - before[:count]}")
    check(after[:major_gc_count] - before[:major_gc_count] == 1, "local collection was not one major collection")
    check(after[:global_gc_count] == before[:global_gc_count], "local collection changed global_gc_count")
    check(after[:global_gc_invocation_count] == before[:global_gc_invocation_count],
          "local collection changed global_gc_invocation_count")
    [after[:major_gc_count] - before[:major_gc_count], after[:global_gc_count] - before[:global_gc_count]]
  end

  def self.retained_count
    Ractor[:retained].sum { |_, cells| cells.size }
  end

  def self.local_retained_checksum
    Ractor[:retained].sum { |_, cells| cells.sum(&:value) }
  end

  class Driver
    def initialize(mode:, heap_objects:, schedule:)
      @mode = mode
      @heap_objects = heap_objects
      @schedule = schedule
      @pending = JOBS.dup
      @phase = WORKERS.to_h { |w| [w, :idle] }
      @assignment = WORKERS.to_h { |w| [w, nil] }
      @result = WORKERS.to_h { |w| [w, nil] }
      @processed_by = WORKERS.to_h { |w| [w, []] }
      @completed = []
      @global_collected = false
      @stashed_events = []
    end

    def run
      setup
      @mode == :replay ? replay : free_run
      finish
      probes if @mode == :free
      puts "ractor_pool: PASS mode=#{@mode} heap_objects=#{@heap_objects}" \
           "#{@mode == :replay ? " schedule=#{@schedule.join(",")}" : ""}"
    end

    private

    def check(condition, message)
      RactorPool.check(condition, message)
    end

    def setup
      @ractor_baseline = Ractor.count
      GC.disable
      @shared_job = RactorPool.build_shared_job
      check(Ractor.shareable?(@shared_job), "shared job must be shareable")
      @copy_leaf = +COPY_LEAF
      @copy_graph = RactorPool.build_graph(2, @copy_leaf, @shared_job[:config])
      @move_leaf = +MOVE_LEAF
      @move_graph = RactorPool.build_graph(3, @move_leaf, @shared_job[:config])
      @move_child = @move_graph[4]
      [@copy_graph, @move_graph].each do |graph|
        check(!Ractor.shareable?(graph), "mutable graph must be unshareable")
      end
      @jobs = { 1 => @shared_job, 2 => @copy_graph, 3 => @move_graph }
      @checksums = @jobs.transform_values { |job| RactorPool.content_checksum(job) }

      @events = Ractor::Port.new
      @results = WORKERS.to_h { |w| [w, Ractor::Port.new] }
      @monitor = Ractor::Port.new
      @workers = WORKERS.to_h do |w|
        worker = Ractor.new(w, @shared_job, @events, @results[w], @heap_objects, name: "tla-worker-#{w}") do |*args|
          RactorPool.worker_main(*args)
        end
        [w, worker]
      end
      @controls = {}
      WORKERS.each do
        verb, id, control = @events.receive
        check(verb == :ready && WORKERS.include?(id) && control.is_a?(Ractor::Port), "bad ready event")
        check(!@controls.key?(id), "worker #{id} announced twice")
        @controls[id] = control
      end
      WORKERS.each do |w|
        check(@workers[w].monitor(@monitor) == true, "monitor registration for worker #{w} failed")
      end
      @main_baseline = GC.stat
      @process_baseline = GC.stat(process: true)
    end

    def replay
      @schedule.each_with_index do |token, index|
        family, worker = token.divmod(10)
        case family
        when 1 then dispatch(worker)
        when 2 then receive(worker)
        when 3 then process(worker)
        when 4 then collect(worker)
        when 5 then global_collect
        when 6 then stop(worker)
        when 7 then observe(worker)
        else raise Failure, "schedule[#{index}]=#{token}: unknown token"
        end
      rescue Failure => e
        raise Failure, "schedule[#{index}]=#{token}: #{e.message}"
      end
      check(WORKERS.all? { |w| @phase[w] == :observed }, "schedule ended before both observations")
    end

    def free_run
      WORKERS.each { |w| command(w, :receive, w) }
      WORKERS.each { |w| dispatch(w) }
      WORKERS.each { |w| await_event([:received, w, w]) }
      WORKERS.each { |w| command(w, :process, w) }
      first = collect_any
      await_event([:processed, first, @processed_by[first].last])
      command(first, :receive, 3)
      dispatch(first)
      await_event([:received, first, 3])
      command(first, :process, 3)
      2.times do
        w = collect_any
        await_event([:processed, w, @processed_by[w].last])
      end
      global_collect
      WORKERS.each do |w|
        stop(w)
        observe(w)
      end
    end

    def dispatch_possible?
      !@pending.empty? && WORKERS.any? { |w| @phase[w] == :idle }
    end

    def precondition(condition, action)
      raise Failure, "#{action}: precondition is false" unless condition
    end

    def dispatch(w)
      precondition(!@pending.empty? && @phase[w] == :idle, "Dispatch(#{w})")
      job_id = @pending.shift
      job = @jobs.fetch(job_id)
      worker = @workers[w]
      case MODES.fetch(job_id)
      when :share
        worker.default_port.send(job, move: true)
        check(job[:config][:name] == SHARED_LEAF, "shared job unusable after move send")
      when :copy
        worker.send(job)
        @copy_leaf << "-main"
        check(job[3] == "#{COPY_LEAF}-main", "sender lost its own copy mutation")
      when :move
        worker.send(job, move: true)
        check(Ractor::MovedObject === job, "moved root is not a MovedObject")
        check(Ractor::MovedObject === @move_child, "moved child is not a MovedObject")
        begin
          job.size
          raise Failure, "moved root is still accessible"
        rescue Ractor::MovedError
        end
      end
      @assignment[w] = job_id
      @phase[w] = :queued
    end

    def receive(w)
      precondition(!dispatch_possible? && @phase[w] == :queued, "Receive(#{w})")
      command(w, :receive, @assignment[w])
      await_event([:received, w, @assignment[w]])
      @phase[w] = :received
    end

    def process(w)
      others_ready = WORKERS.any? { |v| v != w && @result[v] }
      precondition(!dispatch_possible? && @phase[w] == :received && !others_ready, "Process(#{w})")
      command(w, :process, @assignment[w])
      await_event([:processed, w, @assignment[w]])
      @result[w] = @assignment[w]
      @phase[w] = :ready
    end

    def collect(w)
      precondition(!dispatch_possible? && @phase[w] == :ready, "Collect(#{w})")
      port, message = Ractor.select(*@results.values)
      check(port.equal?(@results[w]), "Ractor.select returned #{port.inspect} instead of worker #{w}")
      accept_result(w, message)
    end

    def collect_any
      port, message = Ractor.select(*@results.values)
      w = @results.key(port)
      check(w, "Ractor.select returned an unknown port #{port.inspect}")
      accept_result(w, message)
      w
    end

    def accept_result(w, message)
      job_id = @assignment[w]
      check(job_id, "worker #{w} returned a result without an assignment")
      @processed_by[w] << job_id
      expected = [:result, job_id, MODES.fetch(job_id), @checksums.fetch(job_id), [1, 0],
                  @heap_objects * @processed_by[w].size]
      check(message == expected, "worker #{w} result #{message.inspect} differs from #{expected.inspect}")
      @completed << job_id
      @result[w] = nil
      @assignment[w] = nil
      @phase[w] = :idle
    end

    def global_collect
      precondition(@completed.sort == JOBS && WORKERS.all? { |w| @phase[w] == :idle } && !@global_collected,
                   "GlobalCollect")
      check(@copy_leaf == "#{COPY_LEAF}-main", "sender leaf changed to #{@copy_leaf.inspect}")
      [HeapCell, GarbageCell].each do |klass|
        count = ObjectSpace.each_object(klass).count
        check(count.zero?, "main sees #{count} #{klass} instances")
      end
      before_stats = WORKERS.to_h { |w| [w, query_stats(w)] }
      before_process = process_deltas
      check(before_process == { count: 3, major_gc_count: 3, global_gc_count: 0 },
            "process deltas before global collection: #{before_process.inspect}")
      main_before = GC.stat
      check(main_before[:count] == @main_baseline[:count], "main ran a collection before the global one")
      GC.start(full_mark: true, immediate_mark: true, immediate_sweep: true, global: true)
      main_after = GC.stat
      after_process = process_deltas
      check(after_process == { count: 4, major_gc_count: 3, global_gc_count: 1 },
            "process deltas after global collection: #{after_process.inspect}")
      check(main_after[:major_gc_count] == main_before[:major_gc_count], "global collection changed main major count")
      check(main_after[:global_gc_invocation_count] - main_before[:global_gc_invocation_count] == 1,
            "main did not record one global invocation")
      check(main_after[:global_gc_count] - main_before[:global_gc_count] == 1, "main did not record one global collection")
      check(main_after[:count] == @main_baseline[:count] + 1, "main count is #{main_after[:count]} after the global collection")
      WORKERS.each do |w|
        after = query_stats(w)
        before = before_stats[w]
        check(after[:major_gc_count] == before[:major_gc_count], "worker #{w} major count changed")
        check(after[:global_gc_count] - before[:global_gc_count] == 1, "worker #{w} global count delta is not one")
        check(after[:global_gc_invocation_count] == before[:global_gc_invocation_count],
              "worker #{w} recorded a global invocation")
      end
      @global_collected = true
    end

    def process_deltas
      now = GC.stat(process: true)
      deltas = { count: now[:count] - @process_baseline[:count],
                 major_gc_count: now[:major_gc_count] - @process_baseline[:major_gc_count],
                 global_gc_count: now[:global_gc_count] - @process_baseline[:global_gc_count] }
      check(now[:minor_gc_count] == @process_baseline[:minor_gc_count], "an unexpected minor collection ran")
      deltas
    end

    def query_stats(w)
      command(w, :stats, nil)
      verb, id, cell_count, checksum, stats = receive_event
      check(verb == :stats && id == w, "expected stats from worker #{w}, got #{[verb, id].inspect}")
      check(cell_count == @heap_objects * @processed_by[w].size, "worker #{w} holds #{cell_count} HeapCells")
      expected = RactorPool.retained_checksum(@processed_by[w], @heap_objects)
      check(checksum == expected, "worker #{w} retained checksum #{checksum} differs from #{expected}")
      stats
    end

    def stop(w)
      lower_observed = WORKERS.all? { |v| v >= w || @phase[v] == :observed }
      precondition(@global_collected && @phase[w] == :idle && lower_observed, "Stop(#{w})")
      command(w, :stop, nil)
      await_event([:closed, w, true])
      check(@workers[w].default_port.closed?, "worker #{w} default port is open after close")
      begin
        @workers[w].send(:late)
        raise Failure, "late send to worker #{w} did not raise"
      rescue Ractor::ClosedError
      end
      @phase[w] = :closing
    end

    def observe(w)
      precondition(@phase[w] == :closing, "Observe(#{w})")
      worker = @workers[w]
      token = @monitor.receive
      check(token == [worker, :exited], "monitor delivered #{token.inspect}")
      snapshot = GC.stat(process: true).slice(*PROCESS_COUNTS)
      check(worker.join.equal?(worker), "join did not return the worker")
      check(!worker.alive?, "worker #{w} is alive after join")
      value = worker.value
      expected = [w, @processed_by[w], @heap_objects * @processed_by[w].size]
      check(value == expected, "worker #{w} value #{value.inspect} differs from #{expected.inspect}")
      check(!value.frozen? && !Ractor.shareable?(value), "worker value arrived shareable")
      value << :absorbed
      begin
        worker.value
        raise Failure, "second value call did not raise"
      rescue Ractor::Error
      end
      check(snapshot == GC.stat(process: true).slice(*PROCESS_COUNTS), "value absorption changed process statistics")
      check(GC.stat(:count) == @main_baseline[:count] + 1, "value absorption changed main count")
      check(@controls[w].closed?, "control port of worker #{w} is open after termination")
      @phase[w] = :observed
    end

    def finish
      check(@stashed_events.empty?, "unconsumed events: #{@stashed_events.inspect}")
      check(Ractor.count == @ractor_baseline, "Ractor.count is #{Ractor.count}, baseline #{@ractor_baseline}")
      [@events, @monitor, *@results.values].each do |port|
        port.close
        check(port.closed?, "#{port.inspect} is open after close")
      end
    end

    def command(w, verb, job_id)
      @controls[w] << [verb, w, job_id]
    end

    def receive_event
      event = @events.receive
      raise Failure, "worker #{event[1]} failed: #{event[2]}: #{event[3]}" if event[0] == :fatal
      event
    end

    def await_event(expected)
      if (index = @stashed_events.index(expected))
        @stashed_events.delete_at(index)
        return
      end
      loop do
        event = receive_event
        return if event == expected
        raise Failure, "expected #{expected.inspect}, got #{event.inspect}" if @mode == :replay
        @stashed_events << event
      end
    end

    def probes
      queued_port_probe
      failure_probe
    end

    def queued_port_probe
      port = Ractor::Port.new
      port.send(:queued)
      port.close
      check(port.closed?, "queued port is not closed")
      begin
        port.send(:late)
        raise Failure, "send to a closed port did not raise"
      rescue Ractor::ClosedError
      end
      check(port.receive == :queued, "closed port lost its queued message")
      begin
        port.receive
        raise Failure, "drained closed port did not raise"
      rescue Ractor::ClosedError
      end
    end

    def failure_probe
      failing = Ractor.new(name: "tla-failing") do
        Thread.current.report_on_exception = false
        Ractor.receive
        raise RuntimeError, "expected failure"
      end
      monitor = Ractor::Port.new
      check(failing.monitor(monitor) == true, "failure monitor registration failed")
      failing.send(:raise)
      token = monitor.receive
      check(token == [failing, :aborted], "failure monitor delivered #{token.inspect}")
      check(failing.default_port.closed?, "aborted default port is open")
      begin
        failing.join
        raise Failure, "join of an aborted Ractor did not raise"
      rescue Ractor::RemoteError => e
        check(e.ractor.equal?(failing), "RemoteError names #{e.ractor.inspect}")
        check(e.cause.instance_of?(RuntimeError), "RemoteError cause is #{e.cause.class}")
        check(e.cause.message == "expected failure", "RemoteError cause message is #{e.cause.message.inspect}")
      end
      check(!failing.alive?, "aborted Ractor is alive")
      monitor.close
      check(Ractor.count == @ractor_baseline, "Ractor.count did not return to baseline after abort")
    end
  end

  def self.parse_options(argv)
    options = { mode: nil, heap_objects: nil, schedule: nil }
    set = lambda do |key, value|
      raise Failure, "--#{key.to_s.tr("_", "-")} was given twice" unless options[key].nil?
      options[key] = value
    end
    argv.each do |arg|
      case arg
      when /\A--mode=(free|replay)\z/
        set.call(:mode, $1.to_sym)
      when /\A--heap-objects=([1-9][0-9]*)\z/
        set.call(:heap_objects, Integer($1))
      when /\A--schedule=(\d+(?:,\d+)*)\z/
        set.call(:schedule, $1.split(",").map { |t| Integer(t) })
      else
        raise Failure, "unknown argument #{arg.inspect}"
      end
    end
    raise Failure, "--mode=free|replay is required" unless options[:mode]
    case options[:mode]
    when :replay
      schedule = options[:schedule]
      raise Failure, "--schedule is required in replay mode" unless schedule
      raise Failure, "schedule must have #{SCHEDULE_LENGTH} tokens" unless schedule.size == SCHEDULE_LENGTH
      raise Failure, "unknown schedule tokens" unless schedule.all? { |t| TOKENS.include?(t) }
      raise Failure, "--heap-objects is fixed at #{REPLAY_HEAP_OBJECTS} in replay mode" if options[:heap_objects]
      options[:heap_objects] = REPLAY_HEAP_OBJECTS
    when :free
      raise Failure, "--schedule is rejected in free mode" if options[:schedule]
      options[:heap_objects] ||= DEFAULT_HEAP_OBJECTS
    end
    options
  end

  def self.main(argv)
    options = parse_options(argv)
    check_runtime!
    Driver.new(**options).run
  end
end

RactorPool.main(ARGV) if $PROGRAM_NAME == __FILE__
