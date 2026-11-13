#pragma once
#include <chrono>
#include <concepts>
#include <functional>
#include "util_env.hpp"
#include "util_likely.hpp"

namespace dxmt {

template <typename F, typename context>
concept CommandWithContext = requires(F f, context &ctx) {
  { f(ctx) } -> std::same_as<void>;
};

namespace impl {
template <typename context> class CommandBase {
public:
  virtual void invoke(context &) = 0;
  virtual ~CommandBase() noexcept {};
  CommandBase<context> *next = nullptr;
};

template <typename context, typename F> class LambdaCommand final : public CommandBase<context> {
public:
  void
  invoke(context &ctx) final {
    std::invoke(func, ctx);
  };
  ~LambdaCommand() noexcept final = default;
  LambdaCommand(F &&ff) : CommandBase<context>(), func(std::forward<F>(ff)) {}
  LambdaCommand(const LambdaCommand &copy) = delete;
  LambdaCommand &operator=(const LambdaCommand &copy_assign) = delete;

private:
  F func;
};

template <typename context> class EmptyCommand final : public CommandBase<context> {
public:
  void invoke(context &ctx) final{/* nop */};
  ~EmptyCommand() noexcept = default;
};

template <typename value_t> struct LinkedListNode {
  value_t value;
  LinkedListNode *next;
};

} // namespace impl

template <typename Context> class CommandList {

  impl::EmptyCommand<Context> empty;
  impl::CommandBase<Context> *list_end;

public:
  CommandList() : list_end(&empty) {
    empty.next = nullptr;
  }
  ~CommandList() {
    reset();
  }

  void
  reset() {
    impl::CommandBase<Context> *cur = empty.next;
    while (cur) {
      auto next = cur->next;
      cur->~CommandBase<Context>(); // call destructor
      cur = next;
    }
    empty.next = nullptr;
    list_end = &empty;
  }

  CommandList(const CommandList &copy) = delete;
  CommandList(CommandList &&move) {
    this->reset();
    empty.next = move.empty.next;
    list_end = move.list_end;
    move.empty.next = nullptr;
    move.list_end = nullptr;
  }

  CommandList& operator=(CommandList&& move) {
    this->reset();
    empty.next = move.empty.next;
    list_end = move.list_end;
    move.empty.next = nullptr;
    move.list_end = nullptr;
    return *this;
  }

  template <CommandWithContext<Context> Fn>
  constexpr unsigned
  calculateCommandSize() {
    using command_t = impl::LambdaCommand<Context, Fn>;
    return sizeof(command_t);
  }

  template <CommandWithContext<Context> Fn>
  unsigned
  emit(Fn &&cmd, void *buffer) {
    using command_t = impl::LambdaCommand<Context, Fn>;
    auto cmd_h = new (buffer) command_t(std::forward<Fn>(cmd));
    list_end->next = cmd_h;
    list_end = cmd_h;
    return sizeof(command_t);
  }

  void append(CommandList &&list) {
    list_end->next = list.empty.next;
    list_end = list.list_end;
    list.empty.next = nullptr;
    list.list_end = nullptr;
  }

  void
  execute(Context &context) {
    if (unlikely(profileCommands())) {
      executeProfiled(context);
      return;
    }
    impl::CommandBase<Context> *cur = &empty;
    uint64_t n = 0;
    while (cur) {
      cur->invoke(context);
      cur = cur->next;
      n++;
    }
    last_command_count = n;
    last_slow_commands = 0;
    last_worst_command = {};
    last_worst_index = 0;
    last_worst_vptr = nullptr;
  };

  /* DXMT_PROFILE_COMMANDS=1 times every replayed command. Off by default: a
   * clock read is a syscall under Wine (~350 ns on an M4, two per command),
   * which on its own outweighed the replay work it was measuring. */
  static bool
  profileCommands() {
    static const bool enabled = env::getEnvVar("DXMT_PROFILE_COMMANDS") == "1";
    return enabled;
  }

  void
  executeProfiled(Context &context) {
    impl::CommandBase<Context> *cur = &empty;
    uint64_t n = 0;
    /* Replaying the recorded commands is where a stalled frame spends its
     * time, and the shape of that time is the whole question: 250 commands at
     * a millisecond each is a different bug from five commands at fifty. */
    uint64_t slow = 0;
    auto worst = std::chrono::steady_clock::duration::zero();
    while (cur) {
      auto t0 = std::chrono::steady_clock::now();
      cur->invoke(context);
      auto d = std::chrono::steady_clock::now() - t0;
      if (d > std::chrono::milliseconds(1)) slow++;
      /* Which command, not just how long: the present is recorded last, so an
       * index at the end of the list names it without needing the type back. */
      if (d > worst) {
        worst = d;
        last_worst_index = n;
        /* The commands are type-erased lambdas, so the only thing that names
         * one is its vtable: resolve this address against d3d11.dll's symbols
         * and the slow command has a name. Temporary -- goes once the stall
         * is identified. */
        last_worst_vptr = *reinterpret_cast<void *const *>(cur);
      }
      cur = cur->next;
      n++;
    }
    last_command_count = n;
    last_slow_commands = slow;
    last_worst_command = worst;
  };

  uint64_t last_command_count = 0;
  uint64_t last_slow_commands = 0;
  std::chrono::steady_clock::duration last_worst_command{};
  uint64_t last_worst_index = 0;
  void *last_worst_vptr = nullptr;
};

}; // namespace dxmt
