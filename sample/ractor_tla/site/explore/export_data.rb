# frozen_string_literal: true

# Writes data/<Model>.js for the prototype pages in this directory: the
# merged state graph of each API model (TLC only, no replay) and the
# schedules of the worker-pool model.  build_site.rb writes the same files
# into the generated site.
#
#   TLA2TOOLS_JAR=/path/tla2tools.jar ruby export_data.rb

require "tmpdir"
require "fileutils"
require_relative "../build_site"

module RactorExplore
  OUT = File.join(__dir__, "data")

  module_function

  def write(name, data)
    FileUtils.mkdir_p(OUT)
    File.write(File.join(OUT, "#{name}.js"), RactorModelData.script(name, data))
  end

  def main
    jar = RactorTLA.check_jar
    java = RactorTLA.java_executable
    Dir.mktmpdir("ractor_explore") do |dir|
      RactorAPIVerifier::MODULES.each do |mod|
        spec = RactorSite.parse_spec(File.join(RactorSite::API_DIR, "#{mod.name}.tla"),
                                     File.join(RactorSite::API_DIR, "#{mod.name}.cfg"))
        states, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: spec.path,
                                        config: File.join(RactorSite::API_DIR, "#{mod.name}.cfg"), options: mod.options,
                                        scale: 1.0, timeout: 300, tag: "explore-#{mod.name}")
        graph = RactorTLA::Dot.parse(dot, variables: mod.variables, stutter: false, sinks: mod.sinks?)
        cores, transitions = RactorAPIVerifier.quotient(graph, mod.variables)
        write(mod.name, RactorModelData.api(mod, spec, RactorSite::SUMMARIES[mod.name], states, cores, transitions))
        puts "#{mod.name}: #{cores.size} states, #{transitions.size} transitions"
      end
      v = RactorTLAVerifier
      distinct, dot = RactorTLA.run_tlc(jar: jar, java: java, dir: dir, spec: v::SPEC_PATH, config: v::CONFIG_PATH,
                                        scale: 1.0, timeout: 300, tag: "explore-pool")
      schedules = v.parse_dot(dot, distinct)
      write("RactorPool", RactorModelData.pool(RactorSite::SUMMARIES["RactorPool"], distinct, schedules))
      puts "RactorPool: #{schedules.size} schedules"
    end
  end
end

RactorExplore.main if $PROGRAM_NAME == __FILE__
