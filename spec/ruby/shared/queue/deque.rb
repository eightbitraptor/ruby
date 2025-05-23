describe :queue_deq, shared: true do
  it "removes an item from the queue" do
    q = @object.call
    q << Object.new
    q.size.should == 1
    q.send @method
    q.size.should == 0
  end

  it "returns items in the order they were added" do
    q = @object.call
    q << 1
    q << 2
    q.send(@method).should == 1
    q.send(@method).should == 2
  end

  it "blocks the thread until there are items in the queue" do
    q = @object.call
    v = 0

    th = Thread.new do
      q.send(@method)
      v = 1
    end

    v.should == 0
    q << Object.new
    th.join
    v.should == 1
  end

  it "removes an item from a closed queue" do
    q = @object.call
    q << 1
    q.close
    q.send(@method).should == 1
  end

  it "converts false-ish for non_blocking to boolean" do
    q = @object.call
    q << 1
    q << 2

    q.send(@method, false).should == 1
    q.send(@method, nil).should == 2
  end

  it "returns nil for a closed empty queue" do
    q = @object.call
    q.close
    q.send(@method).should == nil
  end

  it "returns nil for an empty queue that becomes closed" do
    q = @object.call

    t = Thread.new {
      q.send(@method).should == nil
    }

    Thread.pass until t.status == "sleep" && q.num_waiting == 1
    q.close
    t.join
  end

  describe "with a timeout" do
    it "returns an item if one is available in time" do
      q = @object.call

      t = Thread.new {
        q.send(@method, timeout: TIME_TOLERANCE).should == 1
      }
      Thread.pass until t.status == "sleep" && q.num_waiting == 1
      q << 1
      t.join
    end

    it "returns nil if no item is available in time" do
      q = @object.call

      Thread.new {
        q.send(@method, timeout: 0.001).should == nil
      }.join
    end

    it "does nothing if the timeout is nil" do
      q = @object.call
      t = Thread.new {
        q.send(@method, timeout: nil).should == 1
      }
      Thread.pass until t.status == "sleep" && q.num_waiting == 1
      q << 1
      t.join
    end

    it "immediately returns nil if no item is available and the timeout is 0" do
      q = @object.call
      q << 1
      q.send(@method, timeout: 0).should == 1
      q.send(@method, timeout: 0).should == nil
    end

    it "raise TypeError if timeout is not a valid numeric" do
      q = @object.call
      -> {
        q.send(@method, timeout: "1")
      }.should raise_error(TypeError, "no implicit conversion to float from string")

      -> {
        q.send(@method, timeout: false)
      }.should raise_error(TypeError, "no implicit conversion to float from false")
    end

    it "raise ArgumentError if non_block = true is passed too" do
      q = @object.call
      -> {
        q.send(@method, true, timeout: 1)
      }.should raise_error(ArgumentError, "can't set a timeout if non_block is enabled")
    end

    it "returns nil for a closed empty queue" do
      q = @object.call
      q.close
      q.send(@method, timeout: 0).should == nil
    end
  end

  describe "in non-blocking mode" do
    it "removes an item from the queue" do
      q = @object.call
      q << Object.new
      q.size.should == 1
      q.send(@method, true)
      q.size.should == 0
    end

    it "raises a ThreadError if the queue is empty" do
      q = @object.call
      -> { q.send(@method, true) }.should raise_error(ThreadError)
    end

    it "removes an item from a closed queue" do
      q = @object.call
      q << 1
      q.close
      q.send(@method, true).should == 1
    end

    it "raises a ThreadError for a closed empty queue" do
      q = @object.call
      q.close
      -> { q.send(@method, true) }.should raise_error(ThreadError)
    end

    it "converts true-ish non_blocking argument to true" do
      q = @object.call

      -> { q.send(@method, true) }.should raise_error(ThreadError)
      -> { q.send(@method, 1) }.should raise_error(ThreadError)
      -> { q.send(@method, "") }.should raise_error(ThreadError)
    end
  end
end
