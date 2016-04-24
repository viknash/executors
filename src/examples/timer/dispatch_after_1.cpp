#include <experimental/type_traits>
#include <experimental/future>
#include <experimental/timer>
#include <experimental/yield>
#include <experimental/executor>
#include <iostream>

using std::experimental::dispatch;
using std::experimental::dispatch_after;
using std::experimental::use_future;
using std::experimental::yield_context;
using std::experimental::basic_yield_context;
using std::experimental::executor;

int main()
{
  dispatch(
    [](basic_yield_context<executor> yield)
    {
      for (int i = 0; i < 10; ++i)
      {
        dispatch_after(std::chrono::seconds(1), yield);
        std::cout << i << std::endl;
      }
    }, use_future).get();
}
