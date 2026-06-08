# Measures major/full GC cost with a populated gen_iv table:
# a large long-lived set of Strings each carrying a gen-iv is kept alive
# while repeated full GCs scan/mark the gen_iv table.
old_object = Array.new(1_000_000){ String.new("x") }
old_object.each{|s| s.instance_variable_set(:@html_safe, true)}
100.times do
  GC.start
end
