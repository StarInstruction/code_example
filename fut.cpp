#include <mutex>
#include <condition_variable>
#include <optional> // C++17, or use boost::optional or a custom solution
#include <exception> // For std::exception_ptr
#include <stdexcept> // For std::runtime_error

template <typename T>
class SharedState {
public:
  SharedState() = default;

  // 禁止拷贝和赋值
  SharedState(const SharedState&) = delete;
  SharedState& operator=(const SharedState&) = delete;

  void set_value(T val) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_) {
      throw std::runtime_error("Promise already satisfied"); // 或者使用 std::future_error
    }
    value_ = std::move(val);
    ready_ = true;
    cv_.notify_one();
  }

  void set_exception(std::exception_ptr p) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_) {
      throw std::runtime_error("Promise already satisfied");
    }
    exception_ptr_ = p;
    ready_ = true;
    cv_.notify_one();
  }

  T get_value() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });
    if (exception_ptr_) {
      std::rethrow_exception(exception_ptr_);
    }
    // std::optional::value() 会在 optional 为空时抛出 std::bad_optional_access
    // 我们确保在 ready_ 为 true 且 exception_ptr_ 为空时 value_ 才有值
    if (value_) {
      return *value_;
    }
    // 理论上不应到达这里，如果 ready_ 但既没有值也没有异常
    throw std::runtime_error("Internal error: shared state ready but no value or exception");
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });
  }

private:
  mutable std::mutex mutex_; // mutable 因为 is_ready 是 const
  std::condition_variable cv_;
  std::optional<T> value_;
  std::exception_ptr exception_ptr_;
  bool ready_{false};
};




template <typename T>
class MyFuture {
public:
  // MyFuture 应该是只移的，但为了简单起见，这里允许拷贝（共享同一个 state）
  // 一个更完整的实现会处理移动语义和 .share() 来创建 shared_future
  MyFuture(std::shared_ptr<SharedState<T>> state) : state_(state) {}

  T get() {
    if (!state_) {
      throw std::runtime_error("Future has no associated state");
    }
    // 简单的实现：get() 之后使 future 失效，或者允许多次 get (如果 state 不变)
    // std::future 的 get() 会使 future 失效
    T value = state_->get_value();
    // state_.reset(); // 可选：使 future 在 get 后失效
    return value;
  }

  bool is_ready() const {
    if (!state_) {
      return false; // 或者抛出异常
    }
    return state_->is_ready();
  }

  void wait() const {
    if (!state_) {
      throw std::runtime_error("Future has no associated state");
    }
    state_->wait();
  }

  // 检查 future 是否有效 (即是否关联了共享状态)
  bool valid() const {
    return state_ != nullptr;
  }

private:
  std::shared_ptr<SharedState<T>> state_;
};







template <typename T>
class MyPromise {
public:
  MyPromise() : state_(std::make_shared<SharedState<T>>()), future_retrieved_(false) {}

  // 禁止拷贝
  MyPromise(const MyPromise&) = delete;
  MyPromise& operator=(const MyPromise&) = delete;

  // 允许移动
  MyPromise(MyPromise&& other) noexcept
    : state_(std::move(other.state_)), future_retrieved_(other.future_retrieved_) {
    other.future_retrieved_ = true; // 使移动后的 promise 不能再 get_future
  }
  MyPromise& operator=(MyPromise&& other) noexcept {
    if (this != &other) {
      state_ = std::move(other.state_);
      future_retrieved_ = other.future_retrieved_;
      other.future_retrieved_ = true;
    }
    return *this;
  }

  MyFuture<T> get_future() {
    if (!state_ || future_retrieved_) {
      // std::future_error(std::future_errc::future_already_retrieved 或者 no_state)
      throw std::runtime_error("Future already retrieved or no state");
    }
    future_retrieved_ = true;
    return MyFuture<T>(state_);
  }

  void set_value(const T& value) {
    if (!state_) {
      throw std::runtime_error("Promise has no state");
    }
    state_->set_value(value);
  }

  void set_value(T&& value) {
    if (!state_) {
      throw std::runtime_error("Promise has no state");
    }
    state_->set_value(std::move(value));
  }

  void set_exception(std::exception_ptr p) {
    if (!state_) {
      throw std::runtime_error("Promise has no state");
    }
    state_->set_exception(p);
  }

  ~MyPromise() {
    // 如果 promise 被销毁时，共享状态还未被满足 (ready_ == false)
    // 并且 future 已经被获取，那么应该设置一个 "broken promise" 异常
    if (state_ && !state_->is_ready() && future_retrieved_) {
      try {
        // std::future_errc::broken_promise
        state_->set_exception(std::make_exception_ptr(std::runtime_error("Broken promise")));
      }
      catch (...) {
        // 忽略在析构函数中设置异常时可能发生的异常
      }
    }
  }

private:
  std::shared_ptr<SharedState<T>> state_;
  bool future_retrieved_;
};

/*
// 特化 void 类型 (std::promise<void> 和 std::future<void>)
// SharedState<void> 会有所不同，它不存储值，只关心 ready 状态和异常
// MyPromise<void>::set_value() 不需要参数
// MyFuture<void>::get() 返回 void

template <>
class SharedState<void> {
public:
  SharedState() : ready_(false) {}
  SharedState(const SharedState&) = delete;
  SharedState& operator=(const SharedState&) = delete;

  void set_value() { // 注意：void 版本没有参数
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_) {
      throw std::runtime_error("Promise already satisfied");
    }
    ready_ = true;
    cv_.notify_one();
  }

  void set_exception(std::exception_ptr p) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_) {
      throw std::runtime_error("Promise already satisfied");
    }
    exception_ptr_ = p;
    ready_ = true;
    cv_.notify_one();
  }

  void get_value() { // 注意：void 版本返回 void
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });
    if (exception_ptr_) {
      std::rethrow_exception(exception_ptr_);
    }
  }

  bool is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
  }

  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::exception_ptr exception_ptr_;
  bool ready_;
};

// MyPromise<void> 和 MyFuture<void> 的 set_value() 和 get() 也需要相应特化
template <>
void MyPromise<void>::set_value() { // 无参数
  if (!state_) {
    throw std::runtime_error("Promise has no state");
  }
  // 需要 reinterpret_cast 来调用 SharedState<void> 的 set_value
  // 或者让 MyPromise<void> 直接持有 std::shared_ptr<SharedState<void>>
  // 为了简单，我们假设 MyPromise<T> 已经正确地使用了 SharedState<T>
  // 更优雅的解决方案是在 MyPromise 内部根据 T 是否为 void 来选择 SharedState 类型
  // 或者提供一个通用的 state_setter 接口。
  // 这里我们直接调用，假设编译器能正确处理。
  // 实际上，由于 MyPromise<T> 模板会实例化 SharedState<T>，
  // MyPromise<void> 会实例化 SharedState<void>，所以 state_->set_value() 会调用正确的版本。
  state_->set_value();
}

// MyFuture<void>::get() 已经通过 SharedState<void>::get_value() 返回 void 间接特化了


*/



#include <iostream>
#include <thread>
#include <chrono> // For std::chrono::seconds

void produce_data(MyPromise<int> promise) {
  try {
    std::cout << "Producer: Working..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
     promise.set_value(42);
    // 测试异常
    //throw std::runtime_error("Producer failed!");
    //std::cout << "Producer: Data set." << std::endl;
  }
  catch (...) {
    promise.set_exception(std::current_exception());
    std::cout << "Producer: Exception set." << std::endl;
  }
}

//void produce_data_void(MyPromise<void> promise) {
//  std::cout << "Producer (void): Working..." << std::endl;
//  std::this_thread::sleep_for(std::chrono::seconds(1));
//  promise.set_value(); // No argument for void promise
//  std::cout << "Producer (void): Signal sent." << std::endl;
//}


int main() {
  // 示例 1: 带有值的 Future
  MyPromise<int> prom;
  MyFuture<int> fut = prom.get_future();

  std::thread producer_thread(produce_data, std::move(prom)); // promise 应该是只移的

  std::cout << "Consumer: Waiting for data..." << std::endl;
  try {
    if (fut.is_ready()) {
      std::cout << "Consumer: Data is ready immediately!" << std::endl;
    }
    else {
      std::cout << "Consumer: Data not ready yet. Waiting..." << std::endl;
    }
    // fut.wait(); // 可以先 wait
    // std::cout << "Consumer: Waited. Data is now ready." << std::endl;

    int value = fut.get(); // 会阻塞直到值被设置或异常被抛出
    std::cout << "Consumer: Got value: " << value << std::endl;
  }
  catch (const std::exception& e) {
    std::cout << "Consumer: Caught exception: " << e.what() << std::endl;
  }

  producer_thread.join();
  std::cout << "--------------------------------" << std::endl;

  //// 示例 2: void Future
  //MyPromise<void> void_prom;
  //MyFuture<void> void_fut = void_prom.get_future();

  //std::thread void_producer_thread(produce_data_void, std::move(void_prom));

  //std::cout << "Consumer (void): Waiting for signal..." << std::endl;
  //try {
  //  void_fut.get(); // Blocks until promise is set
  //  std::cout << "Consumer (void): Signal received." << std::endl;
  //}
  //catch (const std::exception& e) {
  //  std::cout << "Consumer (void): Caught exception: " << e.what() << std::endl;
  //}

  //void_producer_thread.join();


  return 0;
}
