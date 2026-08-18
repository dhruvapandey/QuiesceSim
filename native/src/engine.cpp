#include "quiescesim/engine.hpp"

#include <iomanip>
#include <ostream>
#include <stdexcept>

namespace quiescesim {

namespace {
std::uint64_t mask_for(std::uint8_t width) {
  if (width == 0 || width > 64) throw std::invalid_argument("LogicWord width must be 1..64");
  return width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
}
}

LogicWord LogicWord::known(std::uint64_t bits, std::uint8_t width) { return {bits & mask_for(width), 0, 0, width}; }
LogicWord LogicWord::x(std::uint8_t width) { return {0, mask_for(width), 0, width}; }
LogicWord LogicWord::z(std::uint8_t width) { return {0, 0, mask_for(width), width}; }
bool LogicWord::is_known() const { return x_mask == 0 && z_mask == 0; }
std::uint64_t LogicWord::as_u64() const { if (!is_known()) throw std::logic_error("value contains X or Z"); return bits; }

namespace {
void require_same_width(const LogicWord& left, const LogicWord& right) {
  if (left.width != right.width) throw std::invalid_argument("bitwise operands must have equal widths");
}

std::uint64_t unknown_mask(const LogicWord& value) { return value.x_mask | value.z_mask; }

LogicWord replace_slice(LogicWord original, LogicWord replacement, std::uint8_t msb, std::uint8_t lsb) {
  if (lsb > msb || msb >= original.width) throw std::invalid_argument("NBA part-select range is invalid");
  const auto width = static_cast<std::uint8_t>(msb - lsb + 1);
  if (replacement.width != width) throw std::invalid_argument("NBA part-select width mismatch");
  const auto field_mask = mask_for(width);
  const auto positioned_mask = field_mask << lsb;
  return {(original.bits & ~positioned_mask) | ((replacement.bits & field_mask) << lsb),
          (original.x_mask & ~positioned_mask) | ((replacement.x_mask & field_mask) << lsb),
          (original.z_mask & ~positioned_mask) | ((replacement.z_mask & field_mask) << lsb), original.width};
}
}

LogicWord bit_not(LogicWord value) {
  const auto mask = mask_for(value.width);
  const auto unknown = unknown_mask(value);
  return {~value.bits & ~unknown & mask, unknown, 0, value.width};
}

LogicWord bit_and(LogicWord left, LogicWord right) {
  require_same_width(left, right);
  const auto mask = mask_for(left.width);
  const auto left_unknown = unknown_mask(left);
  const auto right_unknown = unknown_mask(right);
  const auto left_known = ~left_unknown & mask;
  const auto right_known = ~right_unknown & mask;
  const auto known_zero = (left_known & ~left.bits) | (right_known & ~right.bits);
  const auto known_one = left_known & left.bits & right_known & right.bits;
  return {known_one, mask & ~(known_zero | known_one), 0, left.width};
}

LogicWord bit_or(LogicWord left, LogicWord right) {
  require_same_width(left, right);
  const auto mask = mask_for(left.width);
  const auto left_unknown = unknown_mask(left);
  const auto right_unknown = unknown_mask(right);
  const auto left_known = ~left_unknown & mask;
  const auto right_known = ~right_unknown & mask;
  const auto known_one = (left_known & left.bits) | (right_known & right.bits);
  const auto known_zero = left_known & ~left.bits & right_known & ~right.bits & mask;
  return {known_one, mask & ~(known_zero | known_one), 0, left.width};
}

LogicWord bit_xor(LogicWord left, LogicWord right) {
  require_same_width(left, right);
  const auto mask = mask_for(left.width);
  const auto unknown = unknown_mask(left) | unknown_mask(right);
  return {(left.bits ^ right.bits) & ~unknown & mask, unknown, 0, left.width};
}

namespace {
char vcd_bit(const LogicWord& value, std::uint8_t bit) {
  const auto mask = std::uint64_t{1} << bit;
  if ((value.x_mask & mask) != 0) return 'x';
  if ((value.z_mask & mask) != 0) return 'z';
  return (value.bits & mask) != 0 ? '1' : '0';
}

std::string vcd_value(const LogicWord& value) {
  if (value.width == 1) return std::string(1, vcd_bit(value, 0));
  std::string result{"b"};
  for (std::uint8_t bit = value.width; bit > 0; --bit) result += vcd_bit(value, bit - 1);
  return result;
}
}  // namespace

SignalId Engine::add_signal(std::string name, LogicWord initial) {
  signals_.push_back({std::move(name), initial, initial});
  return static_cast<SignalId>(signals_.size() - 1);
}

ProcessId Engine::add_process(std::string name, std::vector<SignalId> sensitivity, Process process) {
  const auto id = static_cast<ProcessId>(processes_.size());
  processes_.push_back({std::move(name), std::move(process)});
  for (const auto signal : sensitivity) sensitivity_[signal].push_back(id);
  return id;
}

void Engine::schedule_active(ProcessId process) { active_.push(process); }
void Engine::schedule_at(std::uint64_t time, TimedCallback callback) {
  if (time < time_) throw std::invalid_argument("cannot schedule event in the past");
  future_.push({time, next_order_++, std::move(callback)});
}
LogicWord Engine::read(SignalId signal) const { return signals_.at(signal).value; }
const std::string& Engine::signal_name(SignalId signal) const { return signals_.at(signal).name; }
void Engine::wake_sensitive(SignalId signal) { for (const auto process : sensitivity_[signal]) active_.push(process); }
void Engine::commit_now(SignalId signal, LogicWord value) {
  auto& target = signals_.at(signal);
  if (target.value == value) return;
  const auto old = target.value;
  target.value = value;
  waves_.push_back({time_, signal, old, value});
  wake_sensitive(signal);
}
void Engine::write_now(SignalId signal, LogicWord value) { commit_now(signal, value); }
void Engine::write_nba(SignalId signal, LogicWord value) { nba_.push_back({signal, value, std::nullopt}); }
void Engine::write_nba_slice(SignalId signal, LogicWord value, std::uint8_t msb, std::uint8_t lsb) {
  nba_.push_back({signal, value, std::pair{msb, lsb}});
}
void Engine::record_guarded_skip() { ++skipped_guarded_evaluations_; }

void Engine::write_vcd(std::ostream& output) const {
  output << "$version QuiesceSim native exact core $end\n"
         << "$timescale 1ns $end\n"
         << "$scope module dut $end\n";
  for (std::size_t id = 0; id < signals_.size(); ++id) {
    output << "$var wire " << static_cast<unsigned>(signals_[id].value.width) << " s" << id << ' ' << signals_[id].name << " $end\n";
  }
  output << "$upscope $end\n$enddefinitions $end\n#0\n";
  for (std::size_t id = 0; id < signals_.size(); ++id) {
    output << vcd_value(signals_[id].initial) << (signals_[id].initial.width == 1 ? "s" : " s") << id << '\n';
  }
  std::uint64_t last_time = 0;
  for (const auto& change : waves_) {
    if (change.time != last_time) { output << '#' << change.time << '\n'; last_time = change.time; }
    output << vcd_value(change.new_value) << (change.new_value.width == 1 ? "s" : " s") << change.signal << '\n';
  }
}

void Engine::run() {
  while (!active_.empty() || !nba_.empty() || !future_.empty()) {
    while (!active_.empty()) {
      const auto process = active_.front();
      active_.pop();
      processes_.at(process).callback(*this);
    }
    if (!nba_.empty()) {
      auto pending = std::move(nba_);
      nba_.clear();
      for (const auto& write : pending) {
        if (write.slice) {
          const auto [msb, lsb] = *write.slice;
          commit_now(write.signal, replace_slice(read(write.signal), write.value, msb, lsb));
        } else {
          commit_now(write.signal, write.value);
        }
      }
      continue;
    }
    if (!future_.empty()) {
      time_ = future_.top().time;
      while (!future_.empty() && future_.top().time == time_) {
        // priority_queue::top returns a const reference. Timed callbacks are
        // intentionally copyable, so copy before removing the queue element
        // rather than casting away constness to move from it.
        const auto event = future_.top();
        future_.pop();
        event.callback(*this);
      }
    }
  }
}

}  // namespace quiescesim
