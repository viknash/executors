A C++14 library for executors
=============================

This is a potential standard library proposal that covers:

* [Executors](#executors)
  * Executors and schedulers
  * Resumable functions / coroutines
  * A model for asynchronous operations
  * An alternative to `std::async()`
* [Timers](#timers)
* [Channels](#channels)

It has been tested with both g++ 4.9 (experimental) and clang 3.4, using the `-std=c++1y` compiler option.

<a name="executors"/> Executors
-------------------------------

The central concept of this library is the **executor**. An executor embodies a set of rules about where, when and how to run a function object. For example:

Type of executor | Where, when and how
---------------- | -------------------
System           | Any thread in the process.
Thread pool      | Any thread in the pool, and nowhere else.
Strand           | Not concurrent with any other function object sharing the strand, and in FIFO order.
Future / Promise | Any thread. Capture any exceptions thrown by the function object and store them in the promise.

To inject a function object into an executor, we can use a **post** operation:

    void f1()
    {
      std::cout << "Hello, world!\n";
    }

    // ...

    ex.post(f1);

This submits the function object for later execution, according to the rules of the executor:

Type of executor | Behaviour of post
---------------- | -----------------
System           | Adds the function object to a system thread pool's work queue.
Thread pool      | Adds the function object to the thread pool's work queue.
Strand           | Adds the function object to the strand's work queue.
Future / Promise | Wraps the function object in a try/catch block, and adds it to the system work queue.

Alternatively, we may want execute a function object according to the rules, but in the cheapest way possible. For this, we use a **dispatch** operation:

    ex.dispatch(f1);

By performing a dispatch operation, we are giving the executor the option of having `dispatch()` run the function object before it returns. Whether an executor does this depends on its rules:

Type of executor | Behaviour of dispatch
---------------- | ---------------------
System           | Always runs the function object before returning from `dispatch()`.
Thread pool      | If we're inside the thread pool, runs the function object before returning from `dispatch()`. Otherwise, adds to the thread pool's work queue.
Strand           | If we're inside the strand, or if the strand queue is empty, runs the function object before returning from `dispatch()`. Otherwise, adds to the strand's work queue.
Future / Promise | Wraps the function object in a try/catch block, and runs it before returning from `dispatch()`.

### Posting functions to a thread pool

As a simple example, let us consider how to implement the Active Object design pattern using the executors library. In the Active Object pattern, all operations associated with an object are run on its own private thread.

    class bank_account
    {
      int balance_ = 0;
      std::experimental::thread_pool pool_{1};
      mutable std::experimental::thread_pool::executor ex_ = std::experimental::make_executor(pool_);

    public:
      void deposit(int amount)
      {
        ex_.post([=]
          {
            balance_ += amount;
          });
      }

      void withdraw(int amount)
      {
        ex_.post([=]
          {
            if (balance_ >= amount)
              balance_ -= amount;
          });
      }
    };

First, we create a private thread pool with a single thread:

    std::experimental::thread_pool pool_{1};

A thread pool is an example of an **execution context**. An execution context represents a place where function objects will be executed. This is distinct from an executor which, as an embodiment of a set of rules, is intended to be a lightweight object that is cheap to copy and wrap for further adaptation.

Therefore, to inject function objects into thread pool, we must create an executor for it using the library function `make_executor()`::

    std::experimental::thread_pool::executor ex_ = std::experimental::make_executor(pool_);

To add the function to the queue, we then use a post operation:

    ex_.post([=]
      {
        if (balance_ >= amount)
          balance_ -= amount;
      });

### Waiting for function completion

When implementing the Active Object pattern, we will normally want to wait for the operation to complete. To do this we can reimplement our `bank_account` member functions using the free function `post()`. This function allows us to pass a **completion token**. A completion token specifies how we want to be notified when the function finishes. For example:

    void withdraw(int amount)
    {
      std::future<void> fut = std::experimental::post(ex_, [=]
        {
          if (balance_ >= amount)
            balance_ -= amount;
        },
        std::experimental::use_future);
      fut.get();
    }

Here, the `use_future` completion token is specified. When passed the `use_future` token, the free function `post()` returns the result via a `std::future`.

Other types of completion token include plain function objects (used as callbacks), resumable functions or coroutines, and even user-defined types. If we want our active object to accept any type of completion token, we simply change the member functions to accept the token as a template parameter:

    template <class CompletionToken>
    auto withdraw(int amount, CompletionToken&& token)
    {
      return std::experimental::post(ex_, [=]
        {
          if (balance_ >= amount)
            balance_ -= amount;
        },
        std::forward<CompletionToken>(token));
    }

The caller of this function can now choose how to receive the result of the operation, as opposed to having a single strategy hard-coded in the `bank_account` implementation. For example, the caller could choose to receive the result via a `std::future`:

    bank_account acct;
    // ...
    std::future<void> fut = acct.withdraw(10, std::experimental::use_future);
    fut.get();

or callback:

    acct.withdraw(10, []{ std::cout << "withdraw complete\n"; });

or any other type that meets the completion token requirements. This approach also works for functions that return a value:

    class bank_account
    {
      // ...  

      template <class CompletionToken>
      auto balance(CompletionToken&& token) const
      {
        return std::experimental::post(ex_, [=]
          {
            return balance_;
          },
          std::forward<CompletionToken>(token));
      }
    };

When using `use_future`, the future's value type is determined automatically from the executed function's return type:

    std::future<int> fut = acct.balance(std::experimental::use_future);
    std::cout << "balance is " << fut.get() << "\n";

Similarly, when using a callback, the function's result is passed as an argument:

    acct.balance([](int bal){ std::cout << "balance is " << bal << "\n"; });

### Limiting concurrency using strands

Clearly, having a private thread for each `bank_account` is not going to scale well to thousands or millions of objects. We may instead want all bank accounts to share a thread pool. The `system_executor` object provides access to a system thread pool which we can use for this purpose:

    std::experimental::system_executor ex;
    ex.post([]{ std::cout << "Hello, world!\n"; });

However, the system thread pool uses an unspecified number of threads, and the posted function could run on any of them. The original reason for using the Active Object pattern was to limit the `bank_account` object's internal logic to run on a single thread. Fortunately, we can also limit concurrency by using the `strand<>` template.

The `strand<>` template is an executor that acts as an adapter for other executors. In addition to the rules of the underlying executor, a strand adds a guarantee of non-concurrency. That is, it guarantees that no two function objects submitted to the strand will run in parallel.

We can convert the `bank_account` class to use a strand very simply:

    class bank_account
    {
      int balance_ = 0;
      mutable std::experimental::strand<std::experimental::system_executor> ex_;

    public:
      // ...
    };

### Lightweight, immediate execution using dispatch

As noted above, a post operation always submits a function object for later execution. This means that when we write:

    template <class CompletionToken>
    auto withdraw(int amount, CompletionToken&& token)
    {
      return std::experimental::post(ex_, [=]
        {
          if (balance_ >= amount)
            balance_ -= amount;
        },
        std::forward<CompletionToken>(token));
    }

we will always incur the cost of a context switch (plus an extra context switch if we wait for the result using a future). This cost can be avoided if we use a dispatch operation instead. The system executor's rules allow it to run a function object on any thread, so if we change the `withdraw` function to:

    template <class CompletionToken>
    auto withdraw(int amount, CompletionToken&& token)
    {
      return std::experimental::dispatch(ex_, [=]
        {
          if (balance_ >= amount)
            balance_ -= amount;
        },
        std::forward<CompletionToken>(token));
    }

then the enclosed function object can be executed before `dispatch()` returns. The only condition where it will run later is when the strand is already busy on another thread. In this case, in order to meet the strand's non-concurrency guarantee, the function object must be added to the strand's work queue. In the common case there is no contention on the strand and the cost is minimised.

### Composition using resumable functions

Let us now add a function to transfer balance from one bank account to another. To implement this function we must coordinate code on two distinct executors: the strands that belong to each of the bank accounts.

A first attempt at solving this might use a `std::future`:

    class bank_account
    {
      // ...  

      template <class CompletionToken>
      auto transfer(bank_account& to_acct, CompletionToken&& token)
      {
        return std::experimental::dispatch(ex_, [=, &to_acct]
          {
            if (balance_ >= amount)
            {
              balance_ -= amount;
              std::future<void> fut = to_acct.deposit(amount, std::experimental::use_future);
              fut.get();
            }
          },
          std::forward<CompletionToken>(token));
      }
    };

While correct, this approach has the side effect of blocking the thread until the future is ready. If the `to_acct` object's strand is busy running other function objects, this might take some time.

This executors library offers an alternative approach: resumable functions, or coroutines. These functions are identified by having a last argument of type `yield_context`:

      template <class CompletionToken>
      auto transfer(bank_account& to_acct, CompletionToken&& token)
      {
        return std::experimental::dispatch(ex_,
          [=, &to_acct](std::experimental::yield_context yield)
          {
            if (balance_ >= amount)
            {
              balance_ -= amount;
              to_acct.deposit(amount, yield);
            }
          },
          std::forward<CompletionToken>(token));
      }

The `yield` object is a completion token that means that, when the call out to the `to_acct` object is reached:

    to_acct.deposit(amount, yield);

the library implementation automatically suspends the current function. The thread is not blocked and remains available to process other function objects. Once the `deposit()` operation completes, the `transfer()` function resumes execution at the following statement.

These resumable functions are implemented entirely as a library construct, and require no alteration to the language. Consequently, they can utilise artbitrarily complex control flow constructs:

      template <class CompletionToken>
      auto transfer(std::vector<bank_account*> to_accts, CompletionToken&& token)
      {
        return std::experimental::dispatch(ex_,
          [=](std::experimental::yield_context yield)
          {
            if (balance_ >= amount)
            {
              balance_ -= amount;
              for (auto to_acct : to_accts)
                to_acct->deposit(amount, yield);
            }
          },
          std::forward<CompletionToken>(token));
      }

while still retaining concise, familiar C++ language use.

### Polymorphic executors

Up to this point, our `bank_account` class's executor has been a private implementation detail. However, rather than limit ourselves to the system executor, we will now alter the class to be able to specify the executor on a case-by-case basis.

Ultimately, executors are defined by a set of type requirements, and each of the executors we have used so far is a distinct type. For optimal performance we can use compile-time polymorphism, and specify the executor as a template parameter:

    template <class Executor>
    class bank_account
    {
      int balance_ = 0;
      mutable std::experimental::strand<Executor> ex_;

    public:
      // ...
    };

On the other hand, in many situations runtime polymorphism will be preferred. To support this, the library provides the `executor` class, a polymorphic wrapper:

    class bank_account
    {
      int balance_ = 0;
      mutable std::experimental::strand<std::experimental::executor> ex_;

    public:
      explicit bank_account(const std::experimental::executor& ex = std::experimental::system_executor())
        : ex_(ex)
      {
      }

      // ...
    };

The `bank_account` class can then be constructed using an explicitly-specified thread pool:

    std::experimental::thread_pool pool;
    auto ex = std::experimental::make_executor(pool);
    bank_account acct(ex);

or any other object that meets the executor type requirements.

<a name="timers"/> Timers
-------------------------

TBD.

<a name="channels"/> Channels
-----------------------------

TBD.
