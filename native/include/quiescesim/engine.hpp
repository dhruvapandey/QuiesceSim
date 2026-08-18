#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

namespace quiescesim {

using SignalId = std::uint32_t;
using ProcessId = std::uint32_t;

// Packed four-state scalar/word. An X or Z bit is represented by a set bit in
// the corresponding mask. Production will replace this uint64_t prototype
// representation with width-specialized SIMD-friendly vectors.
struct LogicWord {
  std::uint64_t bits{0};
  std::uint64_t x_mask{0};
  std::uint64_t z_mask{0};
  std::uint8_t width{1};

  static LogicWord known(std::uint64_t bits, std::uint8_t width);
  static LogicWord x(std::uint8_t width);
  static LogicWord z(std::uint8_t width);
  [[nodiscard]] bool operator==(const LogicWord& other) const = default;
  [[nodiscard]] bool is_known() const;
  [[nodiscard]] std::uint64_t as_u64() const;
};

// Four-state bitwise operators following SystemVerilog's bit-level truth
// tables. Z is treated as unknown for bitwise evaluation.
LogicWord bit_not(LogicWord value);
LogicWord bit_and(LogicWord left, LogicWord right);
LogicWord bit_or(LogicWord left, LogicWord right);
LogicWord bit_xor(LogicWord left, LogicWord right);

struct WaveChange {
  std::uint64_t time;
  SignalId signal;
  LogicWord old_value;
  LogicWord new_value;
};

class Engine {
 public:
  using Process = std::function<void(Engine&)>;
  using TimedCallback = std::function<void(Engine&)>;

  SignalId add_signal(std::string name, LogicWord initial);
  ProcessId add_process(std::string name, std::vector<SignalId> sensitivity, Process process);
  void schedule_active(ProcessId process);
  void schedule_at(std::uint64_t time, TimedCallback callback);

  [[nodiscard]] LogicWord read(SignalId signal) const;
  [[nodiscard]] const std::string& signal_name(SignalId signal) const;
  [[nodiscard]] std::uint64_t now() const { return time_; }
  [[nodiscard]] const std::vector<WaveChange>& waves() const { return waves_; }
  [[nodiscard]] std::uint64_t skipped_guarded_evaluations() const { return skipped_guarded_evaluations_; }
  // Emit the complete value-change history in standard VCD form. This is
  // intentionally exact-mode output; acceleration may later use replay to
  // reconstruct a requested interval before calling this method.
  void write_vcd(std::ostream& output) const;

  // Immediate update, used for combinational/process inputs. Any value change
  // wakes sensitive processes in the active queue.
  void write_now(SignalId signal, LogicWord value);
  // Nonblocking update. Writes commit only after the active region empties.
  // FIFO ordering gives the SystemVerilog last-assignment-wins behavior.
  void write_nba(SignalId signal, LogicWord value);
  // Used only after a semantic guard has established that a clocked process
  // cannot alter state at this edge. It is instrumentation today; later it is
  // the accounting source for region-level quiescence acceleration.
  void record_guarded_skip();
  void run();

 private:
  struct Signal { std::string name; LogicWord initial; LogicWord value; };
  struct Proc { std::string name; Process callback; };
  struct NbaWrite { SignalId signal; LogicWord value; };
  struct FutureEvent { std::uint64_t time; std::uint64_t order; TimedCallback callback; };
  struct FutureEarlier {
    bool operator()(const FutureEvent& lhs, const FutureEvent& rhs) const {
      return lhs.time != rhs.time ? lhs.time > rhs.time : lhs.order > rhs.order;
    }
  };

  void commit_now(SignalId signal, LogicWord value);
  void wake_sensitive(SignalId signal);
  std::vector<Signal> signals_;
  std::vector<Proc> processes_;
  std::unordered_map<SignalId, std::vector<ProcessId>> sensitivity_;
  std::queue<ProcessId> active_;
  std::vector<NbaWrite> nba_;
  std::priority_queue<FutureEvent, std::vector<FutureEvent>, FutureEarlier> future_;
  std::vector<WaveChange> waves_;
  std::uint64_t skipped_guarded_evaluations_{0};
  std::uint64_t time_{0};
  std::uint64_t next_order_{0};
};

}  // namespace quiescesim
