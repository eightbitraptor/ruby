# frozen_string_literal: true

module GCDisabledChecker
  def before_setup
    if @__gc_disabled__ = GC.enable # return true if GC is disabled
      GC.disable
    end

    if @__gc_major_disabled__ = GC.enable_major # return true if GC majors disabled
      GC.disable_major
    end

    super
  end

  def after_teardown
    super

    disabled = GC.enable
    disabled_major = GC.enable_major
    GC.disable if @__gc_disabled__
    GC.disable_major if @__gc_major_disabled__

    if @__gc_disabled__ != disabled
      label = {
        true => 'disabled',
        false => 'enabled',
      }
      raise "GC was #{label[@__gc_disabled__]}, but is #{label[disabled]} after the test."
    end

    if @__gc_major_disabled__ != disabled_major
      label = {
        true => 'disabled',
        false => 'enabled',
      }
      raise "GC Majors were #{label[@__gc_major_disabled__]}, but are #{label[disabled_major]} after the test."
    end
  end
end

module GCCompactChecker
  def after_teardown
    super
    GC.compact
  end
end

Test::Unit::TestCase.include GCDisabledChecker
Test::Unit::TestCase.include GCCompactChecker if ENV['RUBY_TEST_GC_COMPACT']
