# frozen_string_literal: true

# The data that the Ractor widgets read: for each model, one JavaScript file
# that sets window.RACTOR_MODELS[name].  An API model carries its merged state
# graph; the pool model carries its complete schedules.

require "json"
require "set"

module RactorModelData
  POOL_JOBS = { 1 => "share", 2 => "copy", 3 => "move" }.freeze

  module_function

  def plain(value)
    case value
    when Set then value.to_a.map { |v| plain(v) }.sort_by(&:to_s)
    when Array then value.map { |v| plain(v) }
    when Hash then value.transform_values { |v| plain(v) }
    else value
    end
  end

  def script(name, data)
    "window.RACTOR_MODELS = window.RACTOR_MODELS || {};\n" \
      "window.RACTOR_MODELS[#{name.to_json}] = #{JSON.generate(data)};\n"
  end

  # Paths of set-valued fields, such as "wait.*.ports": "*" stands for any
  # index of a tuple.  plain() turns sets into arrays, so readers need these
  # paths to print the values in TLA+ notation.
  def set_paths(value, path, found)
    case value
    when Set then found << path
    when Array then value.each { |v| set_paths(v, "#{path}.*", found) }
    when Hash then value.each { |k, v| set_paths(v, "#{path}.#{k}", found) }
    end
    found
  end

  # spec: a parsed specification (name, header, variables, invariants,
  # properties, actions, defs); cores and transitions: RactorAPIVerifier.quotient.
  def api(mod, spec, summary, states, cores, transitions)
    ids = cores.keys.each_with_index.to_h
    choice = RactorAPIVerifier.choices(transitions)
    sets = Set.new
    cores.each_value { |core| core.fields.each { |name, text| set_paths(RactorTLA::Value.parse(text), name, sets) } }
    {
      name: mod.name,
      summary: summary,
      header: spec.header,
      variables: spec.variables.map { |n, c| { name: n, comment: c } },
      sets: sets.to_a.sort,
      invariants: spec.invariants.map { |n| { name: n, comment: spec.defs[n]&.comment.to_s } },
      properties: spec.properties.map { |n| { name: n, comment: spec.defs[n]&.comment.to_s } },
      actions: spec.actions.to_h { |a| [a, { params: spec.defs[a].params, comment: spec.defs[a].comment }] },
      counts: mod.expected.merge(states: states),
      cores: cores.values.map do |core|
        { id: ids.fetch(core.key), initial: core.initial,
          fields: core.fields.transform_values { |text| plain(RactorTLA::Value.parse(text)) } }
      end,
      transitions: transitions.map do |t|
        step = plain(t.step)
        { from: ids.fetch(t.from), to: ids.fetch(t.to), op: step["op"], args: step["args"],
          outcome: step["outcome"], value: step.fetch("value", ""), wakes: step.fetch("wakes", []),
          exit: step["exit"].is_a?(Hash) ? step["exit"] : nil, choice: choice.key?(t) }
      end
    }
  end

  def pool(summary, states, schedules)
    { name: "RactorPool", summary: summary, states: states, jobs: POOL_JOBS, schedules: schedules.sort }
  end
end
