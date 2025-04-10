require_relative '../../spec_helper'
require_relative 'fixtures/classes'
require_relative 'shared/set_visibility'

describe "Module#private" do
  it_behaves_like :set_visibility, :private

  it "makes the target method uncallable from other types" do
    obj = Object.new
    class << obj
      def foo; true; end
    end

    obj.foo.should == true

    class << obj
      private :foo
    end

    -> { obj.foo }.should raise_error(NoMethodError)
  end

  it "makes a public Object instance method private in a new module" do
    m = Module.new do
      private :module_specs_public_method_on_object
    end

    m.should have_private_instance_method(:module_specs_public_method_on_object)

    # Ensure we did not change Object's method
    Object.should_not have_private_instance_method(:module_specs_public_method_on_object)
  end

  it "makes a public Object instance method private in Kernel" do
    Kernel.should have_private_instance_method(
                  :module_specs_public_method_on_object_for_kernel_private)
    Object.should_not have_private_instance_method(
                  :module_specs_public_method_on_object_for_kernel_private)
  end

  it "returns argument or arguments if given" do
    (class << Object.new; self; end).class_eval do
      def foo; end
      private(:foo).should equal(:foo)
      private([:foo, :foo]).should == [:foo, :foo]
      private(:foo, :foo).should == [:foo, :foo]
      private.should equal(nil)
    end
  end

  it "raises a NameError when given an undefined name" do
    -> do
      Module.new.send(:private, :undefined)
    end.should raise_error(NameError)
  end

  it "only makes the method private in the class it is called on" do
    base = Class.new do
      def wrapped
        1
      end
    end

    klass = Class.new(base) do
      def wrapped
        super + 1
      end
      private :wrapped
    end

    base.new.wrapped.should == 1
    -> do
      klass.new.wrapped
    end.should raise_error(NameError)
  end

  it "continues to allow a prepended module method to call super" do
    wrapper = Module.new do
      def wrapped
        super + 1
      end
    end

    klass = Class.new do
      prepend wrapper

      def wrapped
        1
      end
      private :wrapped
    end

    klass.new.wrapped.should == 2
  end
end
