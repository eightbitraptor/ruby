# Compares the wall time of two Ruby binaries on interpreter-bound benchmarks.
#
#   ruby yarv/bench.rb BASELINE_RUBY CANDIDATE_RUBY [RUNS]
#
# Each benchmark runs RUNS times per binary, alternating binaries. The
# report shows the median time per binary and the ratio candidate/baseline.
# Set BENCHMARKS=name,name to run a subset.

BENCHMARKS = ENV.fetch("BENCHMARKS", %w[
  app_fib app_tak loop_whileloop so_nbody so_mandelbrot so_nsieve
  so_matrix so_array so_fannkuch so_spectralnorm app_aobench app_pentomino
].join(",")).split(",")

baseline, candidate, runs = ARGV[0], ARGV[1], Integer(ARGV[2] || 5)
abort "usage: #{$0} BASELINE_RUBY CANDIDATE_RUBY [RUNS]" unless baseline && candidate
dir = File.expand_path("../benchmark", __dir__)

# MEASURE=cycles reports CPU cycles from `/usr/bin/time -l` (macOS) in
# place of wall time. Cycles vary less when other processes load the
# machine.
MEASURE = ENV.fetch("MEASURE", "seconds")

def time_run(ruby, script)
  if MEASURE == "cycles"
    out = IO.popen(["/usr/bin/time", "-l", ruby, "--disable-gems", script, err: [:child, :out]], &:read)
    raise "#{ruby} #{script} failed" unless $?.success?
    return Float(out[/(\d+)\s+cycles elapsed/, 1]) / 1e9
  end
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  system(ruby, "--disable-gems", script, out: File::NULL, exception: true)
  Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
end

def median(xs) = xs.sort[xs.size / 2]

unit = MEASURE == "cycles" ? "Gcycles" : "s"
puts "| benchmark | baseline (#{unit}) | candidate (#{unit}) | candidate/baseline |"
puts "|---|---:|---:|---:|"
ratios = []
BENCHMARKS.each do |name|
  script = File.join(dir, "#{name}.rb")
  times = { baseline => [], candidate => [] }
  runs.times { [baseline, candidate].each { |ruby| times[ruby] << time_run(ruby, script) } }
  b, c = median(times[baseline]), median(times[candidate])
  ratios << c / b
  printf "| %s | %.3f | %.3f | %.3f |\n", name, b, c, c / b
end
printf "| geomean | | | %.3f |\n", ratios.sum { Math.log(_1) }.then { Math.exp(_1 / ratios.size) }
